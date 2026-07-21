#include "FileBrowser.h"
#include "EditorStrings.h"
#include "Resource/Utils/PathUtils.h"
#include "ThirdParty/imgui/imgui.h"
#include <algorithm>
#include <cctype>
#include <set>

using namespace DX12Engine::System::Resource::Utils;

EditorFileManager::EditorFileManager() = default;

void EditorFileManager::SetContentRoot(const std::string &root) {
    std::string normalized = PathUtils::Normalize(root);
    if (m_contentRoot != normalized) {
        m_contentRoot = normalized;
        ScanContentDirectory();
    }
}

void EditorFileManager::ScanContentDirectory() {
    m_rootEntries.clear();
    m_allFiles.clear();

    if (m_contentRoot.empty() || !std::filesystem::exists(m_contentRoot))
        return;

    for (const auto &entry : std::filesystem::directory_iterator(m_contentRoot)) {
        FileEntry fe;
        fe.path = entry.path();
        fe.filename = fe.path.filename().string();
        fe.normalizedPath = PathUtils::Normalize(fe.path.string());
        fe.extension = fe.path.extension().string();
        fe.isDirectory = entry.is_directory();
        m_rootEntries.push_back(fe);
    }

    for (const auto &entry : std::filesystem::recursive_directory_iterator(m_contentRoot)) {
        if (!entry.is_regular_file())
            continue;
        FileEntry fe;
        fe.path = entry.path();
        fe.filename = fe.path.filename().string();
        fe.normalizedPath = PathUtils::Normalize(fe.path.string());
        fe.extension = fe.path.extension().string();
        fe.isDirectory = false;
        m_allFiles.push_back(fe);
    }

    auto sortDirFirst = [](const FileEntry &a, const FileEntry &b) {
        if (a.isDirectory != b.isDirectory)
            return a.isDirectory > b.isDirectory;
        return a.filename < b.filename;
    };
    std::sort(m_rootEntries.begin(), m_rootEntries.end(), sortDirFirst);
    std::sort(m_allFiles.begin(), m_allFiles.end(),
              [](const FileEntry &a, const FileEntry &b) { return a.filename < b.filename; });
}

bool EditorFileManager::MatchesSearch(const std::string &filename, const std::string &search) {
    if (search.empty())
        return true;
    auto it = std::search(filename.begin(), filename.end(), search.begin(), search.end(),
                          [](char c1, char c2) { return tolower(c1) == tolower(c2); });
    return it != filename.end();
}

const char *EditorFileManager::GetFileTypeCategory(const std::string &ext) {
    std::string extLow = ext;
    for (auto &c : extLow)
        c = (char)tolower(c);

    if (extLow == ".dxmesh" || extLow == ".obj" || extLow == ".fbx" || extLow == ".gltf" || extLow == ".glb")
        return "Models";
    if (extLow == ".dds" || extLow == ".png" || extLow == ".jpg" || extLow == ".jpeg" || extLow == ".tga" ||
        extLow == ".bmp")
        return "Textures";
    if (extLow == ".json" || extLow == ".material")
        return "Materials";
    if (extLow == ".scene")
        return "Scenes";
    return "Other";
}

void EditorFileManager::DrawDirectoryNode(const std::filesystem::path &dirPath, const std::string &searchStr) {
    std::vector<FileEntry> entries;

    for (const auto &entry : std::filesystem::directory_iterator(dirPath)) {
        FileEntry fe;
        fe.path = entry.path();
        fe.filename = fe.path.filename().string();
        fe.normalizedPath = PathUtils::Normalize(fe.path.string());
        fe.isDirectory = entry.is_directory();
        entries.push_back(fe);
    }

    std::sort(entries.begin(), entries.end(), [](const FileEntry &a, const FileEntry &b) {
        if (a.isDirectory != b.isDirectory)
            return a.isDirectory > b.isDirectory;
        return a.filename < b.filename;
    });

    for (const auto &fe : entries) {
        if (fe.isDirectory) {
            if (ImGui::TreeNodeEx(fe.filename.c_str(), ImGuiTreeNodeFlags_None)) {
                DrawDirectoryNode(fe.path, searchStr);
                ImGui::TreePop();
            }
        } else {
            if (!searchStr.empty() && !MatchesSearch(fe.filename, searchStr))
                continue;
            ImGui::BulletText("%s", fe.filename.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", fe.normalizedPath.c_str());
        }
    }
}

void EditorFileManager::DrawTypeView(const std::string &searchStr) {
    struct TypeGroup {
        const char *name;
        std::vector<const FileEntry *> files;
    };
    std::vector<TypeGroup> groups;
    std::set<std::string> catOrder = {"Models", "Textures", "Materials", "Scenes", "Other"};

    for (const auto &fe : m_allFiles) {
        if (!searchStr.empty() && !MatchesSearch(fe.filename, searchStr))
            continue;
        const char *cat = GetFileTypeCategory(fe.extension);
        auto it =
            std::find_if(groups.begin(), groups.end(), [cat](const TypeGroup &g) { return strcmp(g.name, cat) == 0; });
        if (it == groups.end()) {
            groups.push_back({cat, {}});
            it = groups.end() - 1;
        }
        it->files.push_back(&fe);
    }

    std::sort(groups.begin(), groups.end(), [&](const TypeGroup &a, const TypeGroup &b) {
        auto ia = catOrder.find(a.name);
        auto ib = catOrder.find(b.name);
        if (ia != catOrder.end() && ib != catOrder.end())
            return *ia < *ib;
        if (ia != catOrder.end())
            return true;
        if (ib != catOrder.end())
            return false;
        return strcmp(a.name, b.name) < 0;
    });

    for (const auto &group : groups) {
        if (group.files.empty())
            continue;
        if (ImGui::TreeNodeEx(group.name, ImGuiTreeNodeFlags_DefaultOpen)) {
            for (const auto *fe : group.files) {
                ImGui::BulletText("%s", fe->filename.c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", fe->normalizedPath.c_str());
            }
            ImGui::TreePop();
        }
    }
}

void EditorFileManager::Draw(float /*deltaTime*/) {
    if (!m_visible)
        return;

    ImGui::Begin((std::string(EditorStrings::Get("content_browser", "Content Browser")) + DockWindowIdToStr(DockWindowId::ContentBrowser)).c_str(),
                 &m_visible);

    if (m_contentRoot.empty()) {
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.8f, 1.0f), "Content root not set");
        ImGui::End();
        return;
    }

    // ── 工具栏 ──
    // 视图模式切换按钮（Folder / Type 互斥）
    bool isFolder = (m_viewMode == ViewMode::Folder);
    const char *modeLabel = isFolder ? "Folder" : "Type";
    if (ImGui::SmallButton(modeLabel)) {
        m_viewMode = isFolder ? ViewMode::Type : ViewMode::Folder;
        m_searchBuffer[0] = '\0';
    }
    ImGui::SameLine();
    ImGui::TextDisabled(isFolder ? ">" : "<");

    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputTextWithHint("##Search", EditorStrings::Get("search", "Search..."), m_searchBuffer,
                             sizeof(m_searchBuffer));

    ImGui::SameLine();
    if (ImGui::SmallButton("R")) {
        m_searchBuffer[0] = '\0';
        ScanContentDirectory();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(EditorStrings::Get("refresh", "Refresh"));

    ImGui::Separator();

    // ── 内容（可滚动区域） ──
    ImGui::BeginChild("##Content", ImVec2(0, 0), false);
    std::string searchStr(m_searchBuffer);
    bool hasSearch = !searchStr.empty();

    if (hasSearch) {
        // 搜索模式：扁平结果，最多 20 条
        int count = 0;
        for (const auto &fe : m_allFiles) {
            if (count >= 20)
                break;
            if (!MatchesSearch(fe.filename, searchStr))
                continue;
            ImGui::BulletText("%s", fe.filename.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", fe.normalizedPath.c_str());
            count++;
        }
        if (count == 0) {
            ImGui::TextDisabled("No results found");
        } else if (count >= 20) {
            ImGui::TextDisabled("... and more (limit 20)");
        }
    } else if (m_viewMode == ViewMode::Folder) {
        // 文件夹视图
        if (ImGui::TreeNodeEx("Content", ImGuiTreeNodeFlags_DefaultOpen)) {
            for (const auto &fe : m_rootEntries) {
                if (fe.isDirectory) {
                    if (ImGui::TreeNodeEx(fe.filename.c_str(), ImGuiTreeNodeFlags_None)) {
                        DrawDirectoryNode(fe.path, "");
                        ImGui::TreePop();
                    }
                } else {
                    ImGui::BulletText("%s", fe.filename.c_str());
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", fe.normalizedPath.c_str());
                }
            }
            ImGui::TreePop();
        }
    } else {
        // 文件类型视图
        DrawTypeView("");
    }

    ImGui::EndChild();
    ImGui::End();
}