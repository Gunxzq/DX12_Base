#include "Core/InputActionId.h"
#include "Core/InputBinding.h"
#include "InputKeyParser.h"
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace DX12Engine {
namespace Input {

// 内部辅助函数：解析单个 BindingSource
static void ParseBindingSource(const json &sourceItem, BindingSource &outSource) {
    // 1. 解析主键 (Key)
    if (sourceItem.contains("key")) {
        std::string keyStr = sourceItem["key"];
        outSource.KeyCode = ParseKeyCode(keyStr);
    }

    // 2. 解析修饰键 (Modifier)
    if (sourceItem.contains("modifier")) {
        std::string modStr = sourceItem["modifier"];
        outSource.ModifierKey = ParseKeyCode(modStr);
    } else if (sourceItem.contains("requires")) {
        // 兼容之前的 "requires": "W" 写法，视为修饰键逻辑的一部分
        std::string reqStr = sourceItem["requires"];
        // 注意：这里简化处理，实际运行时可能需要检查两个键同时按下
        // 暂时存入 ModifierKey 或单独字段，这里存入 ModifierKey 示意
        outSource.ModifierKey = ParseKeyCode(reqStr);
    }

    // 3. 解析轴向映射 (Axis Map)
    // 格式: "axis_map": { "W": "Y+", "S": "Y-", "A": "X-", "D": "X+" }
    if (sourceItem.contains("axis_map") && sourceItem["axis_map"].is_object()) {
        const auto &axisMap = sourceItem["axis_map"];

        // 注意：axis_map 通常对应多个 Key，但在这里我们是在解析单个 sourceItem。
        // 如果 JSON 结构是 { "keys": ["W","S"], "axis_map": {...} }，
        // 则需要在外部循环处理 keys，并为每个 key 查找对应的 axis_map 条目。
        // 下面的逻辑假设 sourceItem 已经针对单个 Key 进行了预处理，或者我们只处理简单的单键轴。

        // 更 robust 的做法：如果 sourceItem 有 "keys" 数组，应该在外部展开。
        // 这里我们处理一种常见情况：sourceItem 代表一个具体的键位定义。
        // 如果 axis_map 存在，我们需要知道当前处理的是哪个键。
        // 由于 JSON 结构限制，通常 axis_map 是和 keys 数组并列的。
        // 因此，这个函数最好由外部调用者传入 "当前正在处理的键名"。
    }
}

// 内部辅助函数：从包含 "keys" 和 "axis_map" 的对象中提取所有 Sources
static std::vector<BindingSource> ExtractSourcesFromComplexItem(const json &item) {
    std::vector<BindingSource> sources;

    // 情况 A: 简单的单键绑定 { "key": "Space" }
    if (item.contains("key") && !item.contains("keys")) {
        BindingSource src;
        ParseBindingSource(item, src);
        sources.push_back(src);
        return sources;
    }

    if (item.contains("keys") && item["keys"].is_array()) {
        const auto &keysArray = item["keys"];
        const bool hasAxisMap = item.contains("axis_map") && item["axis_map"].is_object();
        const json &axisMap = hasAxisMap ? item["axis_map"] : json::object();

        for (const auto &keyVal : keysArray) {
            std::string keyName = keyVal.get<std::string>();
            EKeyCode code = ParseKeyCode(keyName);
            if (code == EKeyCode::None)
                continue;

            BindingSource src;
            src.KeyCode = code;

            if (hasAxisMap && axisMap.contains(keyName)) {
                std::string dirStr = axisMap[keyName].get<std::string>();
                if (dirStr == "X+") {
                    src.Axis = BindingSource::AxisType::X;
                    src.AxisScale = 1.0f;
                } else if (dirStr == "X-") {
                    src.Axis = BindingSource::AxisType::X;
                    src.AxisScale = -1.0f;
                } else if (dirStr == "Y+") {
                    src.Axis = BindingSource::AxisType::Y;
                    src.AxisScale = 1.0f;
                } else if (dirStr == "Y-") {
                    src.Axis = BindingSource::AxisType::Y;
                    src.AxisScale = -1.0f;
                }
            }

            if (item.contains("modifier")) {
                src.ModifierKey = ParseKeyCode(item["modifier"]);
            }
            sources.push_back(src);
        }
    }

    // 情况 C: 手柄摇杆 { "device": "Gamepad", "stick": "LeftStick" }
    if (item.contains("device")) {
        std::string device = item["device"];

        // --- Gamepad ---
        if (device == "Gamepad") {
            if (item.contains("stick")) {
                std::string stickName = item["stick"];
                if (stickName == "LeftStick") {
                    sources.push_back({EKeyCode::Axis_LeftStick_X, EKeyCode::None, BindingSource::AxisType::X, 1.0f});
                    sources.push_back({EKeyCode::Axis_LeftStick_Y, EKeyCode::None, BindingSource::AxisType::Y, 1.0f});
                } else if (stickName == "RightStick") {
                    sources.push_back({EKeyCode::Axis_RightStick_X, EKeyCode::None, BindingSource::AxisType::X, 1.0f});
                    sources.push_back({EKeyCode::Axis_RightStick_Y, EKeyCode::None, BindingSource::AxisType::Y, 1.0f});
                }
            } else if (item.contains("button")) {
                std::string btnName = "Gamepad_" + item["button"].get<std::string>();
                BindingSource src;
                src.KeyCode = ParseKeyCode(btnName);
                if (item.contains("threshold"))
                    src.Threshold = item["threshold"];
                sources.push_back(src);
            }
        }

        // --- Mouse ---
        else if (device == "Mouse") {
            if (item.contains("axis") && item["axis"] == "Delta") {
                // 鼠标移动通常作为特殊的 Axis 处理，这里映射到自定义的 Axis 码
                // 注意：你需要确保 InputKeyCodes.h 中有对应的 Axis_Mouse_Delta_X/Y 或者类似定义
                // 暂时复用 Axis_Mouse_X/Y
                sources.push_back({EKeyCode::Axis_Mouse_X, EKeyCode::None, BindingSource::AxisType::X, 1.0f});
                sources.push_back({EKeyCode::Axis_Mouse_Y, EKeyCode::None, BindingSource::AxisType::Y, 1.0f});
            } else if (item.contains("button")) {
                std::string btnName = "Mouse_" + item["button"].get<std::string>(); // e.g., Mouse_Left
                BindingSource src;
                src.KeyCode = ParseKeyCode(btnName);
                sources.push_back(src);
            }
        }
    }
    return sources;
}

class InputConfigLoader {
public:
    static bool LoadConfig(const std::string &filePath, std::unordered_map<ActionId, ActionBinding> &outBindings,
                           std::unordered_map<std::string, InputContextConfig> &outContexts) {
        std::ifstream file(filePath);
        if (!file.is_open()) {
            std::cerr << "[Input] Error: Cannot open file " << filePath << std::endl;
            return false;
        }

        try {
            json j = json::parse(file);

            // --- 1. 解析全局 Bindings ---
            if (j.contains("bindings")) {
                for (auto &[actionName, bindingData] : j["bindings"].items()) {
                    ActionId actionId = HashString(actionName);
                    ActionBinding binding;
                    binding.Id = actionId;

                    if (bindingData.is_array()) {
                        for (const auto &item : bindingData) {
                            auto sources = ExtractSourcesFromComplexItem(item);
                            binding.Sources.insert(binding.Sources.end(), sources.begin(), sources.end());
                        }
                    } else if (bindingData.is_object()) {
                        auto sources = ExtractSourcesFromComplexItem(bindingData);
                        binding.Sources.insert(binding.Sources.end(), sources.begin(), sources.end());
                    }

                    outBindings[actionId] = binding;
                }
            }

            // --- 2. 解析 Contexts ---
            if (j.contains("contexts")) {
                for (auto &[ctxName, ctxData] : j["contexts"].items()) {
                    InputContextConfig ctx;
                    ctx.Name = ctxName;
                    if (ctxData.contains("priority"))
                        ctx.Priority = ctxData["priority"];

                    // 2.1 启用的动作列表
                    if (ctxData.contains("enabled_actions") && ctxData["enabled_actions"].is_array()) {
                        for (const auto &actName : ctxData["enabled_actions"]) {
                            ctx.EnabledActions.push_back(HashString(actName.get<std::string>()));
                        }
                    }

                    // 2.2 局部覆盖 (Overrides)
                    if (ctxData.contains("overrides") && ctxData["overrides"].is_object()) {
                        for (auto &[overActionName, overData] : ctxData["overrides"].items()) {
                            ActionId overId = HashString(overActionName);
                            ActionBinding overBinding;
                            overBinding.Id = overId;

                            if (overData.is_array()) {
                                for (const auto &item : overData) {
                                    auto sources = ExtractSourcesFromComplexItem(item);
                                    overBinding.Sources.insert(overBinding.Sources.end(), sources.begin(),
                                                               sources.end());
                                }
                            }

                            ctx.Overrides[overId] = overBinding;
                        }
                    }

                    outContexts[ctxName] = ctx;
                }
            }

            std::cout << "[Input] Successfully loaded config from: " << filePath << std::endl;
            return true;

        } catch (const std::exception &e) {
            std::cerr << "[Input] JSON Parse Error: " << e.what() << std::endl;
            return false;
        }
    }
};

} // namespace Input
} // namespace DX12Engine