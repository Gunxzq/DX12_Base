#pragma once
// ========================================================================
// XORCipher — UKW PowerUp Kit XOR 对称加解密
//
// UKW 使用 XOR 逐 int（4 字节块）方式加密/解密文件。
// 加密与解密是同一操作（对称）。
//
// 已知 key:
//   PowerUp Kit / 原版  → 0x0B7E7759
//   SeedMod             → 0x95127634
//   MSV_MOD             → 0x19870430
//   ...
//
// 适用范围: .hod / .ani / .mpd / .sdt / .fx / .dds
// .x 和 .spt 不在 XOR 加密范围内（明文）。
// ========================================================================

#include <cstdint>
#include <string>
#include <vector>

namespace AssetTool {

class XORCipher {
public:
    /// 支持的 MOD 版本 key
    enum class Version : uint32_t {
        PowerUpKit = 0x0B7E7759,
        SeedMod = 0x95127634,
        MSVMod = 0x19870430,
        SeedMod205 = 0xAC510B91,
        UnitedMod = 0x13322366,
        RaidMod = 0xEF452301,
        TheEpicOfWar = 0x33333323,
        UNEvo = 0x33322166,
    };

    explicit XORCipher(uint32_t key = static_cast<uint32_t>(Version::PowerUpKit));

    /// 对缓冲区进行 XOR 解密（就地修改）
    void DecryptBuffer(uint8_t *data, size_t size) const;

    /// 返回解密后的副本
    std::vector<uint8_t> DecryptCopy(const uint8_t *data, size_t size) const;

    /// 判断文件扩展名是否需要解密
    static bool NeedsDecrypt(const std::string &extension);

    /// 获取当前 key
    uint32_t GetKey() const { return m_key; }

    /// 设置 key（支持多 MOD 兼容）
    void SetKey(uint32_t key) { m_key = key; }

private:
    uint32_t m_key;
};

} // namespace AssetTool
