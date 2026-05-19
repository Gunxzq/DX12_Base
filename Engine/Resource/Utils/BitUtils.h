#pragma once

#include <cassert>
#include <cstdint>

namespace DX12Engine {

/**
 * @brief 位操作工具类 (Header-Only Inline Implementation)
 * @details 专门用于资源句柄 (ResourceHandle) 的位打包与解包。
 *          配合 ResourceManager 中的 32-bit Handle 设计：
 *          - Index:     22 bits (Low)  -> Max 4,194,304 resources
 *          - Generation: 10 bits (High) -> Max 1,024 versions
 */
class BitUtils {
public:
    BitUtils() = delete;
    ~BitUtils() = delete;

    // --- 常量定义 ---
    static constexpr uint32_t INDEX_BITS = 22;
    static constexpr uint32_t GENERATION_BITS = 10;

    // 掩码:
    // INDEX_MASK = 0x003FFFFF (低22位为1)
    static constexpr uint32_t INDEX_MASK = (1u << INDEX_BITS) - 1;

    // GENERATION_MASK = 0xFFC00000 (高10位为1)
    // 注意：生成 Generation 值时，需要先移位再掩码，或者确保输入不溢出
    static constexpr uint32_t GENERATION_MASK = ~INDEX_MASK;

    // 最大有效值
    static constexpr uint32_t MAX_INDEX = INDEX_MASK;
    static constexpr uint32_t MAX_GENERATION = (1u << GENERATION_BITS) - 1;

    /**
     * @brief 将 Index 和 Generation 打包成一个 32 位句柄值
     * @param index 资源索引 (必须 <= 0x3FFFFF)
     * @param generation 版本号 (必须 <= 0x3FF)
     * @return 打包后的 32 位整数
     */
    [[nodiscard]] static inline constexpr uint32_t Pack(uint32_t index, uint32_t generation) {
        // 在 Debug 模式下检查边界，Release 模式下由 constexpr 保证或依赖调用者正确性
        assert(index <= MAX_INDEX && "Index out of bounds for 22-bit packing");
        assert(generation <= MAX_GENERATION && "Generation out of bounds for 10-bit packing");

        // Index 放在低 22 位
        // Generation 左移 22 位放在高 10 位
        return (index & INDEX_MASK) | ((generation << INDEX_BITS) & GENERATION_MASK);
    }

    /**
     * @brief 从 32 位句柄值中提取 Index
     * @param handle 打包后的句柄值
     * @return 资源索引 (低 22 位)
     */
    [[nodiscard]] static inline constexpr uint32_t UnpackIndex(uint32_t handle) { return handle & INDEX_MASK; }

    /**
     * @brief 从 32 位句柄值中提取 Generation
     * @param handle 打包后的句柄值
     * @return 版本号 (高 10 位)
     */
    [[nodiscard]] static inline constexpr uint32_t UnpackGeneration(uint32_t handle) { return handle >> INDEX_BITS; }

    /**
     * @brief 验证 Index 是否在合法范围内
     */
    [[nodiscard]] static inline constexpr bool IsValidIndex(uint32_t index) { return index <= MAX_INDEX; }

    /**
     * @brief 验证 Generation 是否在合法范围内
     */
    [[nodiscard]] static inline constexpr bool IsValidGeneration(uint32_t generation) {
        return generation <= MAX_GENERATION;
    }
};

} // namespace DX12Engine