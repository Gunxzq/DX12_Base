#include "EditorStrings.h"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

EditorStrings &EditorStrings::Instance() {
    static EditorStrings inst;
    return inst;
}

void EditorStrings::Initialize(const std::string &configDir) {
    auto &inst = Instance();
    inst.m_configDir = configDir;
    inst.m_availableLocales.clear();
    inst.m_strings.clear();

    // 扫描所有 editor_strings_*.json 文件
    namespace fs = std::filesystem;
    if (!fs::exists(configDir))
        return;

    for (const auto &entry : fs::directory_iterator(configDir)) {
        auto filename = entry.path().filename().string();
        if (filename.rfind("editor_strings_", 0) != 0 || entry.path().extension() != ".json")
            continue;

        // 提取 locale 代码：editor_strings_zh-CN.json → zh-CN
        std::string locale = filename.substr(15);
        locale = locale.substr(0, locale.rfind(".json"));

        // 加载 JSON
        std::ifstream file(entry.path());
        if (!file.is_open())
            continue;
        try {
            nlohmann::json j;
            file >> j;
            auto &strings = inst.m_strings[locale];
            if (j.contains("strings")) {
                for (auto &[key, val] : j["strings"].items()) {
                    strings[key] = val.get<std::string>();
                }
            }
            inst.m_availableLocales.push_back(locale);
        } catch (...) {
            continue;
        }
    }

    // 默认使用第一个可用语言
    if (!inst.m_availableLocales.empty()) {
        inst.m_currentLocale = inst.m_availableLocales[0];
    }
}

bool EditorStrings::SetLocale(const std::string &locale) {
    auto &inst = Instance();
    if (inst.m_strings.find(locale) == inst.m_strings.end())
        return false;
    inst.m_currentLocale = locale;
    return true;
}

const char *EditorStrings::Get(const char *key, const char *fallback) {
    auto &inst = Instance();
    auto it = inst.m_strings.find(inst.m_currentLocale);
    if (it == inst.m_strings.end())
        return fallback ? fallback : key;
    auto kit = it->second.find(key);
    if (kit == it->second.end())
        return fallback ? fallback : key;
    return kit->second.c_str();
}