#pragma once

#include <string>
#include <unordered_map>
#include <vector>

// ========================================================================
// EditorStrings — 编辑器多语言字符串管理器
//
// 从 Editor/Config/editor_strings_<locale>.json 加载语言包
// 通过 Locale() 静态方法获取当前语言字符串
// ========================================================================

class EditorStrings {
public:
    /// 初始化：加载所有可用语言包
    static void Initialize(const std::string &configDir);

    /// 获取当前语言下的字符串
    static const char *Get(const char *key, const char *fallback = nullptr);

    /// 切换语言
    static bool SetLocale(const std::string &locale);

    /// 获取当前语言代码
    static const std::string &GetLocale() { return Instance().m_currentLocale; }

    /// 获取所有可用语言代码列表
    static const std::vector<std::string> &GetAvailableLocales() { return Instance().m_availableLocales; }

private:
    EditorStrings() = default;
    static EditorStrings &Instance();

    std::string m_configDir;
    std::string m_currentLocale = "zh-CN";
    std::vector<std::string> m_availableLocales;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> m_strings; // locale → key → value
};