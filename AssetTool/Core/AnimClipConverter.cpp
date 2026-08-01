#include "AnimClipConverter.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>

namespace AssetTool {

namespace {

/// 去 _bone 后缀（与 FbxMeshConverter 一致）
std::string StripBoneSuffix(const std::string &name) {
    if (name.size() >= 5 && name.compare(name.size() - 5, 5, "_bone") == 0)
        return name.substr(0, name.size() - 5);
    return name;
}

/// 是否为 _end 末端节点（Blender 辅助节点，引擎不需要）
bool IsEndNode(const std::string &name) { return name.size() >= 4 && name.compare(name.size() - 4, 4, "_end") == 0; }

/// 键帧输出结构（JSON 序列化用）
struct KeyframeOut {
    double t = 0.0;
    float pos[3] = {0.0f, 0.0f, 0.0f};
    float quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float scale[3] = {1.0f, 1.0f, 1.0f};
};

/// 合并 position/rotation/scale 键帧到统一时间轴
/// 三组键帧时间并集为时间轴；某分量在 t 处无键帧时取"≤t 最近键帧"（不插值，插值是引擎职责）
void MergeKeys(const aiNodeAnim *chan, std::vector<KeyframeOut> &out) {
    if (!chan)
        return;

    // 收集唯一时间点
    std::set<double> times;
    for (unsigned int i = 0; i < chan->mNumPositionKeys; ++i)
        times.insert(chan->mPositionKeys[i].mTime);
    for (unsigned int i = 0; i < chan->mNumRotationKeys; ++i)
        times.insert(chan->mRotationKeys[i].mTime);
    for (unsigned int i = 0; i < chan->mNumScalingKeys; ++i)
        times.insert(chan->mScalingKeys[i].mTime);
    if (times.empty())
        return;

    auto findPos = [&](double t, aiVector3D &v) -> bool {
        for (unsigned int i = 0; i < chan->mNumPositionKeys; ++i) {
            if (chan->mPositionKeys[i].mTime <= t + 1e-6) {
                v = chan->mPositionKeys[i].mValue;
                if (chan->mPositionKeys[i].mTime >= t - 1e-6)
                    return true; // 精确命中
            } else {
                break;
            }
        }
        return chan->mNumPositionKeys > 0;
    };
    auto findRot = [&](double t, aiQuaternion &q) -> bool {
        for (unsigned int i = 0; i < chan->mNumRotationKeys; ++i) {
            if (chan->mRotationKeys[i].mTime <= t + 1e-6) {
                q = chan->mRotationKeys[i].mValue;
                if (chan->mRotationKeys[i].mTime >= t - 1e-6)
                    return true;
            } else {
                break;
            }
        }
        return chan->mNumRotationKeys > 0;
    };
    auto findScl = [&](double t, aiVector3D &v) -> bool {
        for (unsigned int i = 0; i < chan->mNumScalingKeys; ++i) {
            if (chan->mScalingKeys[i].mTime <= t + 1e-6) {
                v = chan->mScalingKeys[i].mValue;
                if (chan->mScalingKeys[i].mTime >= t - 1e-6)
                    return true;
            } else {
                break;
            }
        }
        return chan->mNumScalingKeys > 0;
    };

    for (double t : times) {
        KeyframeOut kf;
        kf.t = t;
        aiVector3D p, s;
        aiQuaternion q;
        if (findPos(t, p)) {
            kf.pos[0] = p.x;
            kf.pos[1] = p.y;
            kf.pos[2] = p.z;
        }
        if (findRot(t, q)) {
            kf.quat[0] = q.x;
            kf.quat[1] = q.y;
            kf.quat[2] = q.z;
            kf.quat[3] = q.w;
        }
        if (findScl(t, s)) {
            kf.scale[0] = s.x;
            kf.scale[1] = s.y;
            kf.scale[2] = s.z;
        }
        out.push_back(kf);
    }
}

/// 文件名消毒：Windows 非法字符（<>:"/\|?*）替换为 _，避免 ofstream 打开失败
/// （Blender 导出的 AnimStack 名可能带管道符，如 "Armature|Armature|动作名"）
std::string SanitizeFileName(const std::string &name) {
    std::string out = name;
    for (auto &c : out) {
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*')
            c = '_';
    }
    return out;
}

/// 生成唯一输出文件名（重名追加序号，参考 ani2anim 的 {name}_{n} 规则；先做文件名消毒）
std::string MakeUniqueName(const std::string &name, const std::set<std::string> &used) {
    std::string base = SanitizeFileName(name);
    if (used.count(base) == 0)
        return base;
    for (int i = 2;; ++i) {
        std::string cand = base + "_" + std::to_string(i);
        if (used.count(cand) == 0)
            return cand;
    }
}

} // namespace

AnimClipResult AnimClipConverter::Convert(const std::string &fbxPath, const std::string &outputDir,
                                          const AnimClipOptions &options) {
    AnimClipResult result;
    if (fbxPath.empty() || outputDir.empty()) {
        result.error = "fbxPath / outputDir is empty";
        return result;
    }

    std::filesystem::path fbxFsPath(fbxPath);
    result.stem = fbxFsPath.stem().string();

    // ── 1. assimp 读取 FBX（动画不需要三角化/法线）──
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(fbxPath, 0);
    if (!scene) {
        result.error = "Failed to read FBX: " + std::string(importer.GetErrorString());
        return result;
    }
    if (scene->mNumAnimations == 0) {
        result.error = "FBX contains no animations (AnimStack count = 0)";
        return result;
    }

    std::filesystem::create_directories(std::filesystem::path(outputDir));
    std::set<std::string> usedNames;

    // ── 2. 遍历 AnimStack → 每剪辑一个 .anim ──
    for (unsigned int ai = 0; ai < scene->mNumAnimations; ++ai) {
        const aiAnimation *anim = scene->mAnimations[ai];
        if (!anim)
            continue;

        std::string clipName = anim->mName.length ? anim->mName.C_Str() : ("anim_" + std::to_string(ai + 1));

        // 命名规约（2026-08-01 用户定案）：Blender AnimStack 名形如 "Armature|Armature|动作名_序号"，
        // 三段名只保留最后一段具体动作名（文件名与 .anim 的 name 字段均用此名）。
        {
            size_t bar = clipName.rfind('|');
            if (bar != std::string::npos)
                clipName = clipName.substr(bar + 1);
        }

        // 名字过滤
        if (!options.clipFilter.empty()) {
            bool matched = false;
            for (const auto &f : options.clipFilter) {
                if (clipName == f || clipName.find(f) != std::string::npos) {
                    matched = true;
                    break;
                }
            }
            if (!matched)
                continue;
        }

        // fps：mTicksPerSecond 合理值（10~240）直接采用，否则 30 回退
        double ticksPerSec = anim->mTicksPerSecond;
        double fps = (ticksPerSec >= 1.0 && ticksPerSec <= 240.0) ? ticksPerSec : 30.0;
        double duration = (anim->mDuration > 0.0 && ticksPerSec > 0.0) ? (anim->mDuration / ticksPerSec) : 0.0;

        // 通道
        nlohmann::json channels = nlohmann::json::array();
        for (unsigned int ci = 0; ci < anim->mNumChannels; ++ci) {
            const aiNodeAnim *chan = anim->mChannels[ci];
            if (!chan)
                continue;
            std::string nodeName = chan->mNodeName.length ? chan->mNodeName.C_Str() : "";
            if (nodeName.empty() || IsEndNode(nodeName))
                continue;

            std::vector<KeyframeOut> keys;
            MergeKeys(chan, keys);
            if (keys.empty())
                continue;

            nlohmann::json keyframes = nlohmann::json::array();
            for (const auto &kf : keys) {
                nlohmann::json jk;
                jk["t"] = kf.t;
                jk["position"] = {kf.pos[0], kf.pos[1], kf.pos[2]};
                jk["rotation"] = {kf.quat[0], kf.quat[1], kf.quat[2], kf.quat[3]};
                jk["scale"] = {kf.scale[0], kf.scale[1], kf.scale[2]};
                keyframes.push_back(std::move(jk));
            }

            nlohmann::json jc;
            jc["bone"] = StripBoneSuffix(nodeName);
            jc["keyframes"] = std::move(keyframes);
            channels.push_back(std::move(jc));
            result.channelCount++;
            result.totalKeyframes += static_cast<int>(keys.size());
        }

        if (channels.empty())
            continue;

        // ── 3. 写 {name}.anim ──
        std::string uniqueName = MakeUniqueName(clipName, usedNames);
        usedNames.insert(uniqueName);

        nlohmann::json doc;
        doc["version"] = 1;
        doc["name"] = clipName;
        doc["duration"] = duration;
        doc["fps"] = fps;
        doc["loop"] = options.loop;
        doc["channels"] = std::move(channels);

        std::filesystem::path outPath = std::filesystem::path(outputDir) / (uniqueName + ".anim");
        std::ofstream ofs(outPath);
        if (!ofs) {
            result.error = "Failed to write: " + outPath.string();
            return result;
        }
        ofs << doc.dump(2);
        result.outputFiles.push_back(outPath.string());
        result.clipCount++;
        std::cout << "[anim2clip] " << uniqueName << ".anim (" << duration << "s, " << fps << "fps, " << channels.size()
                  << " channels)\n";
    }

    if (result.clipCount == 0) {
        result.error = "No clips matched (0 exported)";
        return result;
    }

    result.success = true;
    return result;
}

} // namespace AssetTool
