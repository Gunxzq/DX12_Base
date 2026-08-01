// ========================================================================
// RobotMergerUtil.cpp — RobotMerger 跨文件共享辅助函数实现
// 拆分自 RobotMerger.cpp（2026-08-01，适度拆分以缓解 C1060 编译堆不足）
// ========================================================================

#include "RobotMergerUtil.h"

#include "ANIParser.h"
#include "HODParser.h"

#include <assimp/material.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

using namespace AssetTool;

// ==========================================================================
// IsRenderBone — 筛选可渲染部件（排除武装/特效/辅助节点）
// 拆分自 RobotMerger.cpp（2026-08-01）
// ==========================================================================

bool RobotMerger::IsRenderBone(const std::string &name) {
    std::string lower;
    for (char c : name)
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.find("hit") != std::string::npos)
        return false;
    if (lower.find("root") != std::string::npos)
        return false;
    if (lower.find("weapon_point") != std::string::npos)
        return false;
    if (lower.find("fannel") != std::string::npos)
        return false;
    if (lower.find("gun") != std::string::npos)
        return false;
    if (lower.find("sword") != std::string::npos)
        return false;
    if (lower.find("shield") != std::string::npos)
        return false;
    if (lower.find("missile") != std::string::npos)
        return false;
    return true;
}

// ==========================================================================
// CreatePartMaterial — FBX 颜色材质（无纹理）
// 纹理导出暂缓：assimp 6.0.4 FBX 导出器 bug（FBXExporter.cpp:1769 对空
// tpath_by_image 解引用 end() 迭代器崩溃，社区 PR #6405 修复未合入）。
// UKW 贴图本身很少（DX9 时代），后续可在 Blender 内补充。
// ==========================================================================

aiMaterial *AssetTool::CreatePartMaterial(const std::string &stem, const std::string &meshName,
                                          const XFileMaterial &xf) {
    aiMaterial *mat = new aiMaterial();
    aiString matNameStr(stem + "_" + meshName + "_mat");
    mat->AddProperty(&matNameStr, AI_MATKEY_NAME);
    // faceColor 为 RGBA（AiColorToFloat4 写入 r,g,b,a），直接按序映射，勿再 ARGB 错位
    aiColor4D diffuse(xf.faceColor[0], xf.faceColor[1], xf.faceColor[2], xf.faceColor[3]);
    // Emissive 合入 diffuse：assimp 6.0.4 FBX 导出器 legacy 路径写 "Emissive"/Vector3D
    // （FBXExporter.cpp L1516），而 Blender 5.2 导入器只认 "EmissiveColor"/Color（import_fbx.py L2106-2107）。
    // 二进制 FBX 下无法像 ASCII 那样文本替换属性名，故把 emissive 叠加进 baseColor，
    // 保证自发光部件（faceColor 黑 + emissive 黄绿）导入 Blender 后可见而非全黑。
    for (int i = 0; i < 3; ++i)
        diffuse[i] = std::min(1.0f, diffuse[i] + xf.emissiveColor[i]);
    mat->AddProperty(&diffuse, 1, AI_MATKEY_COLOR_DIFFUSE);
    aiColor4D specular(xf.specularColor[0], xf.specularColor[1], xf.specularColor[2], 1.0f);
    mat->AddProperty(&specular, 1, AI_MATKEY_COLOR_SPECULAR);
    aiColor4D emissive(xf.emissiveColor[0], xf.emissiveColor[1], xf.emissiveColor[2], 1.0f);
    mat->AddProperty(&emissive, 1, AI_MATKEY_COLOR_EMISSIVE);
    float shininess = (xf.power > 0.0f) ? xf.power : 10.0f;
    mat->AddProperty(&shininess, 1, AI_MATKEY_SHININESS);
    return mat;
}

// ==========================================================================
// SameMaterial — 材质等效判断
// 用于部件合并：同骨骼 + 同材质才合并顶点，不同材质保留独立 PartData（材质不丢失）
// ==========================================================================

bool AssetTool::SameMaterial(const XFileMaterial &a, const XFileMaterial &b) {
    for (int i = 0; i < 4; ++i)
        if (std::fabs(a.faceColor[i] - b.faceColor[i]) > 1e-6f)
            return false;
    if (std::fabs(a.power - b.power) > 1e-6f)
        return false;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(a.specularColor[i] - b.specularColor[i]) > 1e-6f)
            return false;
        if (std::fabs(a.emissiveColor[i] - b.emissiveColor[i]) > 1e-6f)
            return false;
    }
    return a.textureFilename == b.textureFilename;
}

// ==========================================================================
// ParseFrameToLocalMatrices — 动画帧 → 每骨骼局部矩阵（按 hod 骨骼索引对齐）
// frameData: 帧原始字节（HOD 魔术起 9847B 或 HD2 魔术起帧块）
// hod: 基准骨架（层级/命名）；outLocal: 输出每骨骼局部矩阵（TRS → 4×4）
// ==========================================================================

bool AssetTool::ParseFrameToLocalMatrices(const std::vector<uint8_t> &frameData, const HODData &hod,
                                          std::vector<RobotMerger::Mat4x4> &outLocal) {
    outLocal.assign(hod.BoneCount(), RobotMerger::Mat4x4::Identity());
    if (frameData.size() < 4)
        return false;

    const bool isHod = (frameData[0] == 'H' && frameData[1] == 'O' && frameData[2] == 'D');
    const bool isHd2 = (frameData[0] == 'H' && frameData[1] == 'D' && frameData[2] == '2');

    if (isHod) {
        // 1.008：标准 HOD，HODParser 直解
        HODParser frameParser;
        if (!frameParser.Parse(frameData.data(), frameData.size()))
            return false;
        const auto &frameHod = frameParser.GetResult();
        size_t n = std::min(frameHod.BoneCount(), hod.BoneCount());
        for (size_t bi = 0; bi < n; ++bi) {
            for (int j = 0; j < 16; ++j)
                outLocal[bi].m[j] = static_cast<float>(frameHod.bones[bi].transform[j]);
            // Body_d 偏移修正（与静态 HOD 组装 Merge 一致：帧数据 Y 同样含 +1.30 偏移）
            if (frameHod.bones[bi].name == "Body_d.x")
                outLocal[bi].m[13] -= 1.30f;
        }
        return true;
    }

    if (isHd2) {
        // PUK：HD2 帧块 = 19B 头（HD2+类型+部件数+0+1）+ 每部件 179B（171B TRS + 8B A/B）
        // TRS 布局：f0-3 = quat(x,y,z,w)，f4-6 = scale，f7-9 = position
        size_t pos = 19;
        for (size_t bi = 0; bi < hod.BoneCount(); ++bi) {
            if (pos + 179 > frameData.size())
                break;
            const uint8_t *p = frameData.data() + pos;
            float f[10];
            for (int i = 0; i < 10; ++i)
                std::memcpy(&f[i], p + i * 4, 4);

            // quat(x,y,z,w) → 旋转矩阵（行主序）
            float x = f[0], y = f[1], z = f[2], w = f[3];
            float sx = f[4], sy = f[5], sz = f[6];
            float px = f[7], py = f[8], pz = f[9];
            // 归一化四元数
            float len = std::sqrt(x * x + y * y + z * z + w * w);
            if (len > 1e-6f) {
                x /= len;
                y /= len;
                z /= len;
                w /= len;
            }
            float m[16] = {1 - 2 * (y * y + z * z),
                           2 * (x * y - z * w),
                           2 * (x * z + y * w),
                           0,
                           2 * (x * y + z * w),
                           1 - 2 * (x * x + z * z),
                           2 * (y * z - x * w),
                           0,
                           2 * (x * z - y * w),
                           2 * (y * z + x * w),
                           1 - 2 * (x * x + y * y),
                           0,
                           px,
                           py,
                           pz,
                           1};
            // scale
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    m[r * 4 + c] *= (c == 0 ? sx : (c == 1 ? sy : sz));
            for (int j = 0; j < 16; ++j)
                outLocal[bi].m[j] = m[j];
            // Body_d 偏移修正（与静态 HOD 组装 Merge 一致：帧数据 Y 同样含 +1.30 偏移）
            if (hod.bones[bi].name == "Body_d.x")
                outLocal[bi].m[13] -= 1.30f;
            pos += 179;
        }
        return true;
    }

    return false;
}

// ==========================================================================
// WriteFirstFrameHOD — 从 ANI 首帧提取 HOD 骨架（2026-08-01 用户定案：
// 骨骼信息只从 ANI 来，不依赖同目录 Robo.hod）
// ANI 帧数据即标准 HOD 9847B（HOD 魔术起），连续可靠；输出临时 .hod 供
// MergeFromANI / ExportAnimationsFBX 复用（矩阵/层级与帧数据完全一致）。
// ==========================================================================

std::string AssetTool::WriteFirstFrameHOD(const std::string &aniPath, const std::string &stem, std::string &err) {
    err.clear();
    if (aniPath.empty()) {
        err = "ANI path is empty";
        return "";
    }

    ANIParser ani;
    if (!ani.ParseFile(aniPath)) {
        err = "ANI parse failed: " + ani.GetError();
        return "";
    }
    const auto &groups = ani.GetGroups();
    if (groups.empty() || groups[0].frames.empty()) {
        err = "ANI has no frames";
        return "";
    }

    const auto &frame = groups[0].frames[0];
    if (frame.data.size() < 4 || frame.data[0] != 'H' || frame.data[1] != 'O' || frame.data[2] != 'D') {
        err = "first frame is not HOD format";
        return "";
    }

    // 校验首帧 HOD 可解析（骨骼数 > 0）
    HODParser check;
    if (!check.Parse(frame.data.data(), frame.data.size())) {
        err = "first frame HOD parse failed: " + check.GetError();
        return "";
    }
    if (check.GetResult().BoneCount() == 0) {
        err = "first frame HOD has no bones";
        return "";
    }

    // 写入 {aniDir}/{stem}_frame0.hod
    fs::path aniFsPath(aniPath);
    std::string outPath = (aniFsPath.parent_path() / (stem + "_frame0.hod")).string();
    std::ofstream ofs(outPath, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        err = "cannot write " + outPath;
        return "";
    }
    ofs.write(reinterpret_cast<const char *>(frame.data.data()), static_cast<std::streamsize>(frame.data.size()));
    ofs.close();
    return outPath;
}
