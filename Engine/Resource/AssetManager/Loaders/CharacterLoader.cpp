#include "Asset/IO/Loader/CharacterLoader.h"
#include "Resource/AssetManager/AssetManager.h"
#include "Resource/Manager/SkeletonManager.h"

namespace DX12Engine::Resource {

// ========================================================================
// Character Loader — .character → Character（复合资产）
// 按资产类型拆文件（2026-08-02，参照属性卡 Register*Editor 模式）
//
// 聚合逻辑独立（联动快照阶段 3）：解析 .character → 依赖清单
// （骨架同步，其余异步计数）→ 组装 CharacterData
// 错误处理：依赖失败 → 仍递减计数 → 组装失败回调（data->IsValid() 守卫）
// ========================================================================

void AssetManager::RegisterCharacterLoader() {
    RegisterLoader(".character", AssetType::Character, [this](const std::string &p, AssetCallback cb, uint8_t pr) {
        return LoadCharacterImpl(p, std::move(cb), pr);
    });
}

uint32_t AssetManager::LoadCharacterImpl(const std::string &path, AssetCallback onComplete, uint8_t priority) {
    uint32_t id = m_nextRequestId++;
    auto sharedPath = std::make_shared<std::string>(path);
    auto callback = std::move(onComplete);

    CharacterParseResult parsed;
    if (!ParseCharacterFile(path, parsed)) {
        if (callback) {
            AssetResult res;
            res.type = AssetType::Character;
            res.path = *sharedPath;
            res.success = false;
            callback(res);
        }
        return id;
    }

    auto data = std::make_shared<Resource::CharacterData>();
    auto boneNames = std::make_shared<std::vector<std::string>>();

    // ── 骨架：同步加载（.anim 需要 boneNames，须先就绪） ──
    // SkeletonManager::LoadFromJSON 为同步纯 CPU 加载（文件小、无 GPU 依赖）
    for (const auto &dep : parsed.dependencies) {
        if (dep.type == AssetType::Skeleton && m_skeletonMgr) {
            data->skeleton = m_skeletonMgr->LoadFromJSON(dep.path);
            if (data->skeleton.IsValid()) {
                if (const auto *skel = m_skeletonMgr->GetSkeleton(data->skeleton))
                    *boneNames = skel->BoneNames;
            }
            break;
        }
    }

    // ── 其余依赖（mesh/materials/clips）异步加载，计数聚合 ──
    // 骨架同步完成，从 dependencies 中剔除 Skeleton 后计数
    size_t asyncCount = 0;
    for (const auto &dep : parsed.dependencies)
        if (dep.type != AssetType::Skeleton)
            ++asyncCount;
    auto pending = std::make_shared<std::atomic<int>>(static_cast<int>(asyncCount));

    auto onDep = [this, sharedPath, callback, data, pending,
                  parsed](const AssetResult &res, const CharacterDependency &dep, const std::string &clipName) mutable {
        if (res.success) {
            switch (dep.type) {
            case AssetType::Mesh:
                data->mesh = res.geometryHandle;
                break;
            case AssetType::Material:
                if (dep.materialSlot >= 0) {
                    if (static_cast<int>(data->materials.size()) <= dep.materialSlot)
                        data->materials.resize(dep.materialSlot + 1);
                    data->materials[dep.materialSlot] = res.materialHandle;
                }
                break;
            case AssetType::Animation:
                if (!clipName.empty())
                    data->clips[clipName] = res.clipHandle;
                break;
            default:
                break;
            }
        }

        if (pending->fetch_sub(1) == 1) {
            // 全部异步依赖完成 → 组装完成回调
            AssetResult out;
            out.type = AssetType::Character;
            out.path = *sharedPath;
            out.success = data->IsValid();
            out.characterData = *data;
            if (!parsed.defaultClip.empty())
                out.characterData.defaultClip = parsed.defaultClip;
            m_cache[*sharedPath] = out;
            if (callback)
                callback(out);
        }
    };

    size_t clipIdx = 0;
    for (const auto &dep : parsed.dependencies) {
        if (dep.type == AssetType::Skeleton)
            continue; // 已同步加载
        if (dep.type == AssetType::Animation) {
            std::string clipName = clipIdx < parsed.clipNames.size() ? parsed.clipNames[clipIdx] : "";
            ++clipIdx;
            LoadAnimation(dep.path, *boneNames,
                          [onDep, dep, clipName](const AssetResult &res) mutable { onDep(res, dep, clipName); });
        } else {
            Load(dep.path, [onDep, dep](const AssetResult &res) mutable { onDep(res, dep, ""); });
        }
    }
    return id;
}

} // namespace DX12Engine::Resource
