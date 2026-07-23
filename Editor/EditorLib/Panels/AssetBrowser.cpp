#include "AssetBrowser.h"
#include "Asset/IO/Loader/SceneLoader.h"
#include "Boot/GameContext.h"
#include "DebugUI/DebugUIManager.h"
#include "EditorStrings.h"
#include "FileIconProvider.h"
#include "Preview/PreviewContext.h"
#include "Preview/PreviewManager.h"
#include "Preview/PreviewPBRRenderer.h"
#include "Preview/ThumbnailArray.h"
#include "Renderer/FrameResources/FrameScratchAllocator.h"
#include "Renderer/Material/MaterialManager.h"
#include "Renderer/RHI/Command/CommandManager.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Resource/AssetManager/AssetManager.h"
#include "Resource/GpuResourceManager.h"
#include "Resource/Pool/RenderTargetPool.h"
#include "Resource/Utils/PathUtils.h"
#include "Scene/SceneConstructor.h"
#include "Scheduler/FrameDriver.h"
#include "ThirdParty/imgui/imgui.h"
#include <DirectXMath.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <shellapi.h>
#include <shlobj.h>
#include <vector>

using namespace DX12Engine;
using namespace DX12Engine::Renderer;
using namespace DX12Engine::Resource;
using namespace DX12Engine::Scheduler;
using namespace DX12Engine::System::Resource::Utils;

EditorAssetManager::EditorAssetManager() = default;
EditorAssetManager::~EditorAssetManager() {}

void EditorAssetManager::SetContentRoot(const std::string &root) {
    m_contentRoot = PathUtils::Normalize(root);
    m_currentPath = m_contentRoot;
    ScanDirectory(m_currentPath);
}

void EditorAssetManager::ScanDirectory(const std::filesystem::path &dirPath) {
    m_entries.clear();
    if (!std::filesystem::exists(dirPath))
        return;
    for (const auto &entry : std::filesystem::directory_iterator(dirPath)) {
        DirEntry de;
        de.path = entry.path();
        de.name = de.path.filename().string();
        de.isDirectory = entry.is_directory();
        de.extension = de.path.extension().string();
        for (auto &c : de.extension)
            c = (char)tolower(c);
        m_entries.push_back(de);
    }
    std::sort(m_entries.begin(), m_entries.end(), [](const DirEntry &a, const DirEntry &b) {
        if (a.isDirectory != b.isDirectory)
            return a.isDirectory > b.isDirectory;
        return a.name < b.name;
    });
}

ImTextureID EditorAssetManager::GetIconTexture(const std::string &extension, bool isDirectory) {
    // 回退到文本图块模式，图标资源管理器后续实现
    (void)extension;
    (void)isDirectory;
    return (ImTextureID)0;
}

void EditorAssetManager::RegisterThumbnail(const std::string &filePath, D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle,
                                           uint32_t slice) {
    if (gpuHandle.ptr == 0)
        return;
    ThumbnailEntry entry;
    entry.gpuHandle = gpuHandle;
    entry.slice = slice;
    m_thumbnailMap[filePath] = entry;
}

void EditorAssetManager::RemoveThumbnailsBySlice(uint32_t slice) {
    if (slice == UINT32_MAX)
        return;
    for (auto it = m_thumbnailMap.begin(); it != m_thumbnailMap.end();) {
        if (it->second.slice == slice)
            it = m_thumbnailMap.erase(it);
        else
            ++it;
    }
}

// ========================================================================
// 加载磁盘缓存的缩略图
// ========================================================================

void EditorAssetManager::LoadThumbnailPack(DX12Engine::Renderer::D3D12DeviceContext *deviceCtx) {
    if (!m_thumbnailArray || !deviceCtx || m_contentRoot.empty())
        return;

    std::string packPath = (std::filesystem::path(m_contentRoot) / "Cache/Thumbnails/thumbnails.thumb").string();
    if (!std::filesystem::exists(packPath))
        return;

    std::ifstream file(packPath, std::ios::binary);
    if (!file.is_open())
        return;

    uint32_t count = 0;
    file.read(reinterpret_cast<char *>(&count), sizeof(count));

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t pathLen = 0;
        file.read(reinterpret_cast<char *>(&pathLen), sizeof(pathLen));
        std::string filePath(static_cast<size_t>(pathLen), '\0');
        file.read(filePath.data(), pathLen);

        uint32_t ddsLen = 0;
        file.read(reinterpret_cast<char *>(&ddsLen), sizeof(ddsLen));
        std::vector<uint8_t> ddsData(static_cast<size_t>(ddsLen));
        file.read(reinterpret_cast<char *>(ddsData.data()), ddsLen);

        constexpr uint32_t DDS_HEADER_SIZE = 128;
        if (ddsLen <= DDS_HEADER_SIZE)
            continue;
        const uint8_t *pixels = ddsData.data() + DDS_HEADER_SIZE;
        constexpr uint32_t THUMB_SIZE = 256;

        uint32_t slice = m_thumbnailArray->AllocSlice();
        if (slice == UINT32_MAX)
            continue;

        m_thumbnailArray->UploadToSlice(slice, THUMB_SIZE, THUMB_SIZE, pixels, deviceCtx);

        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = {};
        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = {};
        DebugUI::DebugUIManager::Get().AllocateSrvDescriptor(&cpuHandle, &gpuHandle);
        if (cpuHandle.ptr != 0) {
            m_thumbnailArray->CreateSliceSRV(slice, cpuHandle);
            // 建立文件路径 → 缩略图映射，供 DrawContentIcons 显示
            RegisterThumbnail(filePath, gpuHandle, slice);
        }
    }

    file.close();
}

void EditorAssetManager::DrawBreadcrumb() {
    std::string relPath = m_currentPath.string();
    if (relPath.find(m_contentRoot) == 0)
        relPath = relPath.substr(m_contentRoot.size());
    std::vector<std::string> segs;
    std::string s;
    for (char c : relPath) {
        if (c == '/' || c == '\\') {
            if (!s.empty()) {
                segs.push_back(s);
                s.clear();
            }
        } else
            s += c;
    }
    if (!s.empty())
        segs.push_back(s);
    ImGui::BeginChild("##Breadcrumb", ImVec2(0, 24), false);
    if (ImGui::SmallButton("Content")) {
        m_currentPath = m_contentRoot;
        ScanDirectory(m_currentPath);
    }
    for (size_t i = 0; i < segs.size(); i++) {
        ImGui::SameLine();
        ImGui::TextDisabled(">");
        ImGui::SameLine();
        auto cp = m_contentRoot;
        for (size_t j = 0; j <= i; j++)
            cp = cp.append(segs[j]);
        if (i == segs.size() - 1) {
            ImGui::Text("%s", segs[i].c_str());
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.6f, 1.0f, 1.0f));
            if (ImGui::SmallButton(segs[i].c_str())) {
                m_currentPath = cp;
                ScanDirectory(m_currentPath);
            }
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndChild();
    ImGui::Separator();
}

void EditorAssetManager::DrawDirectoryTree(const std::filesystem::path &dirPath) {
    if (!std::filesystem::exists(dirPath))
        return;
    for (const auto &entry : std::filesystem::directory_iterator(dirPath)) {
        if (!entry.is_directory())
            continue;
        std::string name = entry.path().filename().string();
        bool hasSub = false;
        for (const auto &sub : std::filesystem::directory_iterator(entry.path())) {
            hasSub = true;
            break;
        }
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_None;
        if (!hasSub)
            flags |= ImGuiTreeNodeFlags_Leaf;
        if (entry.path() == m_currentPath)
            flags |= ImGuiTreeNodeFlags_Selected;
        bool open = ImGui::TreeNodeEx(name.c_str(), flags);
        if (ImGui::IsItemClicked() && entry.path() != m_currentPath) {
            m_currentPath = entry.path();
            ScanDirectory(m_currentPath);
        }
        if (open) {
            DrawDirectoryTree(entry.path());
            ImGui::TreePop();
        }
    }
}

void EditorAssetManager::DrawContentIcons() {
    float availWidth = ImGui::GetContentRegionAvail().x;
    float totalItemWidth = m_iconSize + m_iconSpacing * 2;
    int cols = std::max(1, (int)(availWidth / totalItemWidth));
    float labelHeight = 48.0f;           // 增大标签区域避免字符串重叠
    float cellWidth = availWidth / cols; // 均匀分摊，实现横向铺满
    float cellHeight = m_iconSize + 4 + labelHeight + m_iconSpacing;

    ImVec2 windowPos = ImGui::GetCursorScreenPos();

    for (size_t i = 0; i < m_entries.size(); i++) {
        const auto &entry = m_entries[i];
        int col = (int)(i % cols);
        int row = (int)(i / cols);

        // 在当前单元格内居中绘制图标
        float cellCenterX = windowPos.x + col * cellWidth;
        float iconOffsetX = (cellWidth - m_iconSize) * 0.5f;
        ImVec2 pos = ImVec2(cellCenterX + iconOffsetX, windowPos.y + row * cellHeight);

        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImVec2 iconMin = pos;
        ImVec2 iconMax = ImVec2(pos.x + m_iconSize, pos.y + m_iconSize);

        // 检查是否当前预览中的资产
        bool isPreviewing = !entry.isDirectory && !m_previewHighlightPath.empty() &&
                            entry.path == std::filesystem::path(m_previewHighlightPath);

        // 如果该资产正在预览，绘制高亮背景
        if (isPreviewing) {
            ImVec2 bgMin = ImVec2(cellCenterX, pos.y);
            ImVec2 bgMax = ImVec2(cellCenterX + cellWidth, pos.y + m_iconSize + 4 + labelHeight);
            dl->AddRectFilled(bgMin, bgMax, IM_COL32(60, 90, 140, 60), 4.0f);
            dl->AddRect(bgMin, bgMax, IM_COL32(80, 140, 220, 200), 4.0f, 0, 2.0f);
        }

        // 尝试缩略图（仅非目录文件）
        bool drewThumbnail = false;
        if (!entry.isDirectory && m_thumbnailArray && entry.extension != ".json" && entry.extension != ".material") {
            auto it = m_thumbnailMap.find(entry.path.string());
            if (it != m_thumbnailMap.end() && it->second.gpuHandle.ptr != 0) {
                ImGui::SetCursorScreenPos(pos);
                ImGui::Image((ImTextureID)it->second.gpuHandle.ptr, ImVec2(m_iconSize, m_iconSize));
                drewThumbnail = true;
            }
        }

        if (!drewThumbnail) {
            // 尝试 Windows 系统图标，回退到彩色方块+图标字体
            ImTextureID texId = GetIconTexture(entry.extension, entry.isDirectory);
            if (texId) {
                ImGui::SetCursorScreenPos(pos);
                ImGui::Image(texId, ImVec2(m_iconSize, m_iconSize));
            } else {
                FileIconInfo info = GetFileIconInfo(entry.extension, entry.isDirectory);
                dl->AddRectFilled(iconMin, iconMax, info.color, 6.0f);
                dl->AddRect(iconMin, iconMax, IM_COL32(140, 140, 160, 255), 6.0f);
                // 使用与色块一致的字号渲染图标/标签，居中显示
                const char *displayText = info.iconChar ? info.iconChar : info.label;
                float iconFontSize = m_iconSize * 0.5f;
                ImFont *font = ImGui::GetFont();
                ImVec2 txtSize = font->CalcTextSizeA(iconFontSize, FLT_MAX, 0.0f, displayText);
                ImVec2 txtPos =
                    ImVec2(pos.x + (m_iconSize - txtSize.x) * 0.5f, pos.y + (m_iconSize - txtSize.y) * 0.5f);
                dl->AddText(font, iconFontSize, txtPos, IM_COL32(220, 220, 240, 200), displayText);

                // 占位按钮（保持 ImGui 交互一致性）
                ImGui::SetCursorScreenPos(pos);
                ImGui::InvisibleButton(entry.name.c_str(), ImVec2(m_iconSize, m_iconSize));
            }
        }

        // 文件名标签
        ImGui::SetCursorScreenPos(ImVec2(cellCenterX, pos.y + m_iconSize + 4));
        ImGui::PushTextWrapPos(cellCenterX + cellWidth);
        ImGui::Text("%s", entry.name.c_str());
        ImGui::PopTextWrapPos();

        // 悬停提示
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("%s", entry.name.c_str());
            ImGui::TextDisabled("%s", entry.path.string().c_str());
            ImGui::EndTooltip();
        }

        // 双击进入目录 或 打开资产文件
        if (ImGui::IsMouseDoubleClicked(0) &&
            ImGui::IsMouseHoveringRect(ImVec2(cellCenterX, pos.y),
                                       ImVec2(cellCenterX + cellWidth, pos.y + m_iconSize + labelHeight))) {
            if (entry.isDirectory) {
                m_currentPath = entry.path;
                ScanDirectory(m_currentPath);
            } else if (m_onFileDoubleClick) {
                m_onFileDoubleClick(entry.path.string());
            }
        }
    }
    if (m_entries.empty())
        ImGui::TextDisabled("(empty)");
}

void EditorAssetManager::Draw(float /*deltaTime*/) {
    if (!m_visible)
        return;
    ImGui::Begin((std::string(EditorStrings::Get("asset_manager", "Asset Manager")) +
                  DockWindowIdToStr(DockWindowId::AssetManager))
                     .c_str(),
                 &m_visible);
    if (m_contentRoot.empty()) {
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.8f, 1.0f), "Content root not set");
        ImGui::End();
        return;
    }
    DrawBreadcrumb();
    float leftWidth = 200.0f;
    ImGui::BeginChild("##TreePanel", ImVec2(leftWidth, 0), true);
    ImGui::TextDisabled("Folders");
    ImGui::Separator();
    DrawDirectoryTree(m_contentRoot);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##ContentPanel", ImVec2(0, 0), false);
    ImGui::TextDisabled("%d items", (int)m_entries.size());
    ImGui::Separator();
    DrawContentIcons();
    ImGui::EndChild();
    ImGui::End();
}

// ========================================================================
// 设置预览渲染上下文（由 Editor 注入），注册渲染回调
// ========================================================================

void EditorAssetManager::SetPreviewContext(PreviewManager *previewMgr, PreviewPBRRenderer *previewRenderer,
                                           ThumbnailArray *thumbnailArray, FrameScratchAllocator *scratchAlloc,
                                           DX12Engine::Boot::GameContext *gameContext) {
    m_previewMgr = previewMgr;
    m_previewRenderer = previewRenderer;
    m_thumbnailArrayForRender = thumbnailArray;
    m_scratchAlloc = scratchAlloc;
    m_gameCtx = gameContext;

    if (!m_previewMgr || !m_previewRenderer || !m_gameCtx)
        return;

    // 注册预览渲染回调
    RegisterPreviewRenderCallback();

    // 注册资产双击回调（由 AssetBrowser 自管理，不从 Editor 注入）
    SetOnFileDoubleClick([this](const std::string &filePath) { OnFileDoubleClick(filePath); });
}

void EditorAssetManager::SetLayoutProxy(std::function<void(PreviewId)> onSetPreviewId,
                                        std::function<void()> onShowPreviewPanel) {
    m_onSetPreviewId = std::move(onSetPreviewId);
    m_onShowPreviewPanel = std::move(onShowPreviewPanel);
}

void EditorAssetManager::SetSceneSwitcher(std::function<bool(const std::string &, const std::filesystem::path &)> onSwitchScene) {
    m_onSwitchScene = std::move(onSwitchScene);
}

// ========================================================================
// 注册预览渲染回调
// ========================================================================

void EditorAssetManager::RegisterPreviewRenderCallback() {
    m_previewMgr->SetRenderCallback([this](PreviewId id, PreviewContext &pc) {
        if (!m_previewRenderer->IsInitialized())
            return;
        if (!pc.meshVB.IsValid() || !pc.meshIB.IsValid())
            return;

        using namespace DirectX;

        // 根据类型获取渲染目标和资源
        ID3D12Resource *rtRes = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = {};
        if (pc.type == PreviewType::Detail) {
            rtRes = DX12Engine::Resource::RenderTargetPool::GetInstance().GetResource(pc.renderTarget);
            rtvHandle = pc.rtvHandle;
        } else if (pc.type == PreviewType::Thumbnail && m_thumbnailArrayForRender &&
                   m_thumbnailArrayForRender->IsInitialized()) {
            rtRes = m_thumbnailArrayForRender->GetResource();
            rtvHandle = m_thumbnailArrayForRender->GetRtvHandle(pc.arraySlice);
        }
        if (!rtRes || rtvHandle.ptr == 0)
            return;

        auto *deviceCtx = m_gameCtx->DeviceContext;
        auto &cmdMgr = deviceCtx->GetCommandManager();
        uint64_t completedFence = m_gameCtx->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
        auto allocHandle = cmdMgr.AcquireAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
        auto *alloc = cmdMgr.GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocHandle);
        auto cmdHandle = cmdMgr.AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(alloc);
        auto cmdList = cmdMgr.GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdHandle);
        auto *d = cmdList.Get();

        // Barrier: COMMON → RENDER_TARGET
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = rtRes;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        d->ResourceBarrier(1, &b);

        // 清屏
        const float cc[] = {0.12f, 0.12f, 0.14f, 1.0f};
        d->ClearRenderTargetView(rtvHandle, cc, 0, nullptr);
        D3D12_VIEWPORT vp = {0, 0, (float)pc.width, (float)pc.height, 0.0f, 1.0f};
        D3D12_RECT sr = {0, 0, (LONG)pc.width, (LONG)pc.height};
        d->RSSetViewports(1, &vp);
        d->RSSetScissorRects(1, &sr);
        d->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

        // 绑定 CBV_SRV_UAV 描述符堆
        auto *descHeaps = m_gameCtx->DescriptorHeaps;
        if (descHeaps) {
            ID3D12DescriptorHeap *heaps[] = {
                descHeaps->GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, DX12Engine::Resource::HeapTag::Default)};
            d->SetDescriptorHeaps(1, heaps);
        }

        // 设置 PSO 和根签名（根据渲染模式切换）
        if (pc.renderMode == PreviewRenderMode::Unlit) {
            d->SetPipelineState(m_previewRenderer->GetUnlitPSO());
        } else {
            d->SetPipelineState(m_previewRenderer->GetPSO());
        }
        d->SetGraphicsRootSignature(m_previewRenderer->GetRootSignature());

        // CB 布局（256 bytes）
        struct PreviewCB {
            XMMATRIX worldViewProj;
            XMMATRIX world;
            XMFLOAT4 cameraPos;
            XMFLOAT4 lightDirection;
            XMFLOAT4 lightStrength;
            XMFLOAT4 baseColor;
            XMFLOAT4 emissive;
            XMFLOAT4 materialParams;
            uint32_t texIndices[4];
            XMFLOAT4 extra;
        };
        static_assert(sizeof(PreviewCB) == 256, "PreviewCB must be 256 bytes");

        XMMATRIX world = XMMatrixIdentity();
        XMVECTOR pos = XMLoadFloat3(&pc.position);
        XMVECTOR tgt = XMLoadFloat3(&pc.target);
        XMMATRIX view = XMMatrixLookAtLH(pos, tgt, XMVectorSet(0, 1, 0, 0));
        XMMATRIX proj = XMMatrixPerspectiveFovLH(pc.fov, (float)pc.width / pc.height, pc.nearPlane, pc.farPlane);

        PreviewCB cb;
        cb.worldViewProj = world * view * proj;
        cb.world = world;
        XMStoreFloat4(&cb.cameraPos, pos);
        cb.lightDirection = XMFLOAT4(pc.lightDirection.x, pc.lightDirection.y, pc.lightDirection.z, 0.0f);
        cb.lightStrength = XMFLOAT4(pc.lightStrength, pc.lightStrength, pc.lightStrength, 0.0f);

        // 材质数据
        cb.baseColor = XMFLOAT4(pc.baseColor.x, pc.baseColor.y, pc.baseColor.z, pc.baseColor.w);
        cb.emissive = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);

        float metallic = 0.5f, roughness = 0.5f, ambient = 1.0f, alpha = 1.0f;
        float alphaCutoff = 0.0f, normalStrength = 1.0f;
        if (pc.previewMaterial.IsValid() && m_gameCtx->MaterialMgr) {
            const auto *matData = m_gameCtx->MaterialMgr->GetMaterial(pc.previewMaterial);
            if (matData) {
                cb.baseColor =
                    XMFLOAT4(matData->baseColor.x, matData->baseColor.y, matData->baseColor.z, matData->baseColor.w);
                cb.emissive =
                    XMFLOAT4(matData->emissive.x, matData->emissive.y, matData->emissive.z, matData->emissive.w);
                metallic = matData->metallic;
                roughness = matData->roughness;
                ambient = matData->ambient;
                alpha = matData->alpha;
                alphaCutoff = matData->alphaCutoff;
                normalStrength = matData->normalIntensity;
            }
        }
        cb.materialParams = XMFLOAT4(metallic, roughness, ambient, alpha);

        bool hasTexture = pc.previewTexture.IsValid();
        cb.texIndices[0] = hasTexture ? 0u : 0xFFFFFFFFu;
        cb.texIndices[1] = 0xFFFFFFFFu;
        cb.texIndices[2] = 0xFFFFFFFFu;
        cb.texIndices[3] = 0xFFFFFFFFu;
        cb.extra = XMFLOAT4(alphaCutoff, normalStrength, -1.0f, 0.0f);

        auto sa = m_scratchAlloc->Allocate(sizeof(PreviewCB));
        if (!sa.cpuPtr)
            return;
        memcpy(sa.cpuPtr, &cb, sizeof(PreviewCB));
        d->SetGraphicsRootConstantBufferView(0, sa.gpuAddr);

        // 绑定纹理 SRV
        D3D12_GPU_DESCRIPTOR_HANDLE texSRV = {};
        if (hasTexture) {
            auto *texMgr = m_gameCtx->TextureMgr;
            if (texMgr) {
                texSRV = texMgr->GetSRV(pc.previewTexture);
            }
            if (texSRV.ptr != 0) {
                d->SetGraphicsRootDescriptorTable(1, texSRV);
            } else {
                cb.texIndices[0] = 0xFFFFFFFFu;
                memcpy(sa.cpuPtr, &cb, sizeof(PreviewCB));
                d->SetGraphicsRootConstantBufferView(0, sa.gpuAddr);
            }
        }

        ID3D12Resource *vb = DX12Engine::Resource::GpuResourceManager::GetInstance().GetResource(pc.meshVB);
        ID3D12Resource *ib = DX12Engine::Resource::GpuResourceManager::GetInstance().GetResource(pc.meshIB);
        D3D12_VERTEX_BUFFER_VIEW vbv = {};
        D3D12_INDEX_BUFFER_VIEW ibv = {};
        if (vb && ib) {
            vbv = {vb->GetGPUVirtualAddress(), pc.vertexCount * pc.vertexStride, pc.vertexStride};
            d->IASetVertexBuffers(0, 1, &vbv);
            ibv = {ib->GetGPUVirtualAddress(), pc.indexCount * (pc.indexFormat == DXGI_FORMAT_R16_UINT ? 2 : 4),
                   pc.indexFormat};
            d->IASetIndexBuffer(&ibv);
            d->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            d->DrawIndexedInstanced(pc.indexCount, 1, 0, 0, 0);
        }

        // ── 缩略图渲染 ──
        if (pc.arraySlice != UINT32_MAX && m_thumbnailArrayForRender && m_thumbnailArrayForRender->IsInitialized()) {
            ID3D12Resource *thumbRes = m_thumbnailArrayForRender->GetResource();
            D3D12_CPU_DESCRIPTOR_HANDLE thumbRtv = m_thumbnailArrayForRender->GetRtvHandle(pc.arraySlice);
            D3D12_RESOURCE_BARRIER tb = {};
            tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            tb.Transition.pResource = thumbRes;
            tb.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            tb.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            tb.Transition.Subresource = pc.arraySlice;
            d->ResourceBarrier(1, &tb);
            const float thumbClear[] = {0.12f, 0.12f, 0.14f, 1.0f};
            d->ClearRenderTargetView(thumbRtv, thumbClear, 0, nullptr);
            D3D12_VIEWPORT thumbVp = {0, 0, 256.0f, 256.0f, 0.0f, 1.0f};
            D3D12_RECT thumbSr = {0, 0, 256, 256};
            d->RSSetViewports(1, &thumbVp);
            d->RSSetScissorRects(1, &thumbSr);
            d->OMSetRenderTargets(1, &thumbRtv, FALSE, nullptr);
            if (pc.renderMode == PreviewRenderMode::Unlit) {
                d->SetPipelineState(m_previewRenderer->GetUnlitPSO());
            } else {
                d->SetPipelineState(m_previewRenderer->GetPSO());
            }
            d->SetGraphicsRootSignature(m_previewRenderer->GetRootSignature());
            d->SetGraphicsRootConstantBufferView(0, sa.gpuAddr);
            if (hasTexture && texSRV.ptr != 0) {
                d->SetGraphicsRootDescriptorTable(1, texSRV);
            }
            d->IASetVertexBuffers(0, 1, &vbv);
            d->IASetIndexBuffer(&ibv);
            d->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            d->DrawIndexedInstanced(pc.indexCount, 1, 0, 0, 0);
            // 出口屏障
            D3D12_RESOURCE_BARRIER tbOut = tb;
            tbOut.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            tbOut.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
            d->ResourceBarrier(1, &tbOut);
        }

        // 出口屏障
        D3D12_RESOURCE_BARRIER bOut = b;
        bOut.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bOut.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        d->ResourceBarrier(1, &bOut);
        cmdList.Close();

        m_gameCtx->FrameDriver->SubmitRenderCommand(RenderPhase::PostProcess, cmdHandle);
        uint64_t seq = m_gameCtx->GetNextSequence();
        cmdMgr.ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocHandle, seq);
    });
}

// ========================================================================
// 资产文件双击回调（由 AssetBrowser 自管理，替代从 Editor 注入）
// ========================================================================

void EditorAssetManager::LoadSceneDescription(const DX12Engine::Resource::SceneDescription &desc) {
    if (!m_gameCtx)
        return;

    m_gameCtx->Logging->Info("[AssetBrowser] Loading scene description: {}", desc.metadata.name);

    // 调用场景切换回调（释放旧场景资源）
    if (m_onSwitchScene) {
        m_onSwitchScene(desc.metadata.name, "");
    }

    // SceneConstructor 是 EditorAssetManager 的值成员，编辑器生命周期，复用避免悬空回调
    m_sceneCtor.LoadScene(desc, m_gameCtx, DX12Engine::Resource::HeapTag::EditorViewport,
        [this, desc](bool success) {
            m_gameCtx->Logging->Info("[AssetBrowser] Default scene '{}' load {}", desc.metadata.name, success ? "succeeded" : "failed");
        });
}

// ========================================================================
// 从文件路径异步加载场景（不触发 SwitchScene 回调，供 Tab 切换使用）
// ========================================================================

void EditorAssetManager::LoadSceneFromFile(const std::filesystem::path &sceneFilePath) {
    if (!m_gameCtx)
        return;

    m_gameCtx->Logging->Info("[AssetBrowser] Loading scene from file: {}", sceneFilePath.string());

    try {
        DX12Engine::Resource::SceneDescription desc =
            DX12Engine::Resource::SceneLoader::LoadFromFile(sceneFilePath);

        // 直接通过 SceneConstructor 加载，不触发 SwitchScene 回调
        // （Tab 切换时 SwitchScene 已在 ProcessPendingTabSwitch 中完成）
        m_sceneCtor.LoadScene(desc, m_gameCtx, DX12Engine::Resource::HeapTag::EditorViewport,
            [this](bool success) {
                m_gameCtx->Logging->Info("[AssetBrowser] Scene load from file {}", success ? "succeeded" : "failed");
            },
            sceneFilePath.string());  // 传入文件路径用于 Tab 匹配
    } catch (const std::exception &e) {
        m_gameCtx->Logging->Error("[AssetBrowser] Failed to load scene from file '{}': {}", sceneFilePath.string(), e.what());
    }
}

void EditorAssetManager::OnFileDoubleClick(const std::string &filePath) {
    if (filePath == m_previewFilePath)
        return;

    std::string ext;
    auto dot = filePath.find_last_of('.');
    if (dot != std::string::npos) {
        for (char c : filePath.substr(dot))
            ext += (char)tolower(c);
    }

    bool isMeshFile = (ext == ".dxmesh" || ext == ".obj");
    bool isTextureFile =
        (ext == ".dds" || ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga");
    bool isMaterialFile = (ext == ".mat");
    bool isSceneFile = (ext == ".json");

    if (isSceneFile) {
        m_gameCtx->Logging->Info("[AssetBrowser] Scene file double-clicked: {}", filePath);

        // Step 0: 调用场景切换回调（释放旧场景资源，为新场景腾出空间）
        // 若返回 false 表示场景已存在 Tab 中，跳过加载
        if (m_onSwitchScene && !m_onSwitchScene("", filePath)) {
            m_gameCtx->Logging->Info("[AssetBrowser] Scene already open, skipping load: {}", filePath);
            return;
        }

        // Step 1: SceneLoader 解析 JSON → SceneDescription
        DX12Engine::Resource::SceneDescription desc;
        try {
            desc = DX12Engine::Resource::SceneLoader::LoadFromFile(filePath);
        } catch (const std::exception &e) {
            m_gameCtx->Logging->Error("[AssetBrowser] Failed to load scene file: {}", e.what());
            return;
        }

        // Step 2: SceneConstructor 异步加载依赖
        // SceneConstructor 是 EditorAssetManager 的值成员，编辑器生命周期，复用避免悬空回调
        m_sceneCtor.LoadScene(desc, m_gameCtx, DX12Engine::Resource::HeapTag::EditorViewport,
            [this](bool success) {
                m_gameCtx->Logging->Info("[AssetBrowser] Scene load {}", success ? "succeeded" : "failed");
            },
            filePath);  // 传入文件路径用于 Tab 匹配
        return;
    }

    if (!isMeshFile && !isTextureFile && !isMaterialFile) {
        m_gameCtx->Logging->Info("[AssetBrowser] Preview not supported for: {}", filePath);
        return;
    }

    auto *existingCtx = m_previewMgr->GetContext(m_detailPreviewId);
    if (existingCtx) {
        // 释放旧纹理
        if (existingCtx->previewTexture.IsValid() && m_gameCtx->TextureMgr) {
            m_gameCtx->TextureMgr->Release(existingCtx->previewTexture, UINT64_MAX);
            existingCtx->previewTexture = Resource::TextureHandle::Invalid();
        }
        // 释放旧几何体
        if (existingCtx->geometryHandle.IsValid() && m_gameCtx->GeometryResourceManager) {
            m_gameCtx->GeometryResourceManager->Release(existingCtx->geometryHandle, UINT64_MAX);
            existingCtx->geometryHandle = {};
        }
        // 释放旧材质
        if (existingCtx->previewMaterial.IsValid() && m_gameCtx->MaterialMgr) {
            m_gameCtx->MaterialMgr->ReleaseMaterial(existingCtx->previewMaterial);
            existingCtx->previewMaterial = {};
        }
    }

    m_gameCtx->Logging->Info("[AssetBrowser] Asset double-clicked: {}", filePath);

    PreviewId newId = m_previewMgr->AcquirePreview(m_detailPreviewId, PreviewType::Detail, 1024, 1024);
    if (newId == 0) {
        m_gameCtx->Logging->Error("[AssetBrowser] Failed to acquire preview for: {}", filePath);
        return;
    }

    PreviewContext *ctx = m_previewMgr->GetContext(newId);
    if (!ctx) {
        m_gameCtx->Logging->Error("[AssetBrowser] Acquired preview context is null");
        return;
    }

    uint64_t seq = ++m_previewLoadSequence;
    ctx->loadSequence = seq;
    ctx->pendingLoadSequence = UINT64_MAX;
    ctx->needsRender = true;
    m_detailPreviewId = newId;
    m_previewFilePath = filePath;
    SetPreviewHighlightPath(filePath);

    if (m_onSetPreviewId)
        m_onSetPreviewId(newId);
    if (m_onShowPreviewPanel)
        m_onShowPreviewPanel();

    m_gameCtx->Logging->Info("[AssetBrowser] Loading preview for: {} (id={}, seq={})", filePath, newId, seq);

    if (isMeshFile) {
        ctx->renderMode = PreviewRenderMode::PBR;
        DX12Engine::Resource::AssetManager::GetInstance().Load(
            filePath, DX12Engine::Resource::AssetType::Mesh,
            [this, newId, seq](const DX12Engine::Resource::AssetResult &result) {
                if (!result.success) {
                    m_gameCtx->Logging->Warn("[AssetBrowser] Preview load failed: {}", result.path);
                    return;
                }
                PreviewContext *pc = m_previewMgr->GetContext(newId);
                if (!pc || pc->loadSequence != seq) {
                    if (result.geometryHandle.IsValid() && m_gameCtx->GeometryResourceManager)
                        m_gameCtx->GeometryResourceManager->Release(result.geometryHandle, UINT64_MAX);
                    return;
                }
                if (result.geometryHandle.IsValid() && m_gameCtx->GeometryResourceManager) {
                    const auto *mesh =
                        m_gameCtx->GeometryResourceManager->GetGeometry<DX12Engine::Resource::TriangleMesh>(
                            result.geometryHandle);
                    if (mesh && mesh->isGpuReady) {
                        pc->meshVB = mesh->vertexBufferHandle;
                        pc->meshIB = mesh->indexBufferHandle;
                        pc->vertexCount = mesh->vertexCount;
                        pc->indexCount = mesh->indexCount;
                        pc->vertexStride = mesh->vertexStride;
                        pc->indexFormat = mesh->indexFormat;
                        pc->geometryHandle = result.geometryHandle;
                        pc->pendingLoadSequence = seq;
                        pc->needsRender = true;
                        if (m_thumbnailArrayForRender) {
                            uint32_t slice = m_thumbnailArrayForRender->AllocSlice();
                            if (slice != UINT32_MAX) {
                                pc->arraySlice = slice;
                                m_needsThumbnailCache = true;
                            }
                        }
                        m_gameCtx->Logging->Info("[AssetBrowser] Preview mesh loaded: {} (verts={}, indices={})",
                                                 result.path, pc->vertexCount, pc->indexCount);
                    } else {
                        pc->geometryHandle = result.geometryHandle;
                        pc->pendingLoadSequence = seq + 1;
                    }
                }
            });
    } else if (isTextureFile) {
        ctx->renderMode = PreviewRenderMode::Unlit;
        DX12Engine::Resource::AssetManager::GetInstance().Load(
            filePath, DX12Engine::Resource::AssetType::Texture,
            [this, newId, seq](const DX12Engine::Resource::AssetResult &result) {
                if (!result.success) {
                    m_gameCtx->Logging->Warn("[AssetBrowser] Texture preview load failed: {}", result.path);
                    return;
                }
                PreviewContext *pc = m_previewMgr->GetContext(newId);
                if (!pc || pc->loadSequence != seq) {
                    if (result.textureHandle.IsValid() && m_gameCtx->TextureMgr)
                        m_gameCtx->TextureMgr->Release(result.textureHandle, UINT64_MAX);
                    return;
                }
                if (result.textureHandle.IsValid()) {
                    const auto *sphere = m_previewRenderer ? m_previewRenderer->GetPreviewSphere() : nullptr;
                    if (sphere) {
                        pc->meshVB = sphere->vertexBufferHandle;
                        pc->meshIB = sphere->indexBufferHandle;
                        pc->vertexCount = sphere->vertexCount;
                        pc->indexCount = sphere->indexCount;
                        pc->vertexStride = sphere->vertexStride;
                        pc->indexFormat = sphere->indexFormat;
                    }
                    m_gameCtx->TextureMgr->Retain(result.textureHandle);
                    pc->previewTexture = result.textureHandle;
                    pc->pendingLoadSequence = seq;
                    pc->needsRender = true;
                    if (m_thumbnailArrayForRender) {
                        uint32_t slice = m_thumbnailArrayForRender->AllocSlice();
                        if (slice != UINT32_MAX) {
                            pc->arraySlice = slice;
                            m_needsThumbnailCache = true;
                        }
                    }
                    m_gameCtx->Logging->Info("[AssetBrowser] Texture preview loaded: {} (texHandle={})", result.path,
                                             (uint32_t)result.textureHandle);
                }
            });
    } else if (isMaterialFile) {
        ctx->renderMode = PreviewRenderMode::PBR;
        DX12Engine::Resource::AssetManager::GetInstance().Load(
            filePath, DX12Engine::Resource::AssetType::Material,
            [this, newId, seq](const DX12Engine::Resource::AssetResult &result) {
                if (!result.success) {
                    m_gameCtx->Logging->Warn("[AssetBrowser] Material preview load failed: {}", result.path);
                    return;
                }
                PreviewContext *pc = m_previewMgr->GetContext(newId);
                if (!pc || pc->loadSequence != seq) {
                    if (result.materialHandle.IsValid() && m_gameCtx->MaterialMgr)
                        m_gameCtx->MaterialMgr->ReleaseMaterial(result.materialHandle);
                    return;
                }
                if (result.materialHandle.IsValid() && m_gameCtx->MaterialMgr) {
                    const auto *matData = m_gameCtx->MaterialMgr->GetMaterial(result.materialHandle);
                    if (!matData)
                        return;
                    const auto *sphere = m_previewRenderer ? m_previewRenderer->GetPreviewSphere() : nullptr;
                    if (sphere) {
                        pc->meshVB = sphere->vertexBufferHandle;
                        pc->meshIB = sphere->indexBufferHandle;
                        pc->vertexCount = sphere->vertexCount;
                        pc->indexCount = sphere->indexCount;
                        pc->vertexStride = sphere->vertexStride;
                        pc->indexFormat = sphere->indexFormat;
                    }
                    pc->baseColor = matData->baseColor;
                    pc->previewMaterial = result.materialHandle;
                    pc->pendingLoadSequence = seq;
                    pc->needsRender = true;
                    if (m_thumbnailArrayForRender) {
                        uint32_t slice = m_thumbnailArrayForRender->AllocSlice();
                        if (slice != UINT32_MAX) {
                            pc->arraySlice = slice;
                            m_needsThumbnailCache = true;
                        }
                    }
                    m_gameCtx->Logging->Info("[AssetBrowser] Material preview loaded: {} (matHandle={})", result.path,
                                             (uint32_t)result.materialHandle);
                }
            });
    }
}