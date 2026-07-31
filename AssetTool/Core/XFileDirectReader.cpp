#include "XFileDirectReader.h"
#include <algorithm>
#include <cfloat>
#include <cstring>
#include <fstream>
#include <vector>

namespace AssetTool {

// ── 已知 GUID（小端序） ──
static const uint8_t GUID_MESH[] = {
    0x44, 0xab, 0x82, 0x3d, 0xda, 0x62, 0xcf, 0x11,
    0xab, 0x39, 0x00, 0x20, 0xaf, 0x71, 0xe4, 0x33
};
static const uint8_t GUID_MESH_FACE[] = {
    0x5f, 0xab, 0x82, 0x3d, 0xda, 0x62, 0xcf, 0x11,
    0xab, 0x39, 0x00, 0x20, 0xaf, 0x71, 0xe4, 0x33
};
static const uint8_t GUID_MESH_NORMALS[] = {
    0x43, 0xf4, 0xf6, 0xf6, 0xda, 0x47, 0xd2, 0x11,
    0x8f, 0x52, 0x00, 0x40, 0x33, 0x94, 0xa3, 0x04
};
static const uint8_t GUID_MESH_TEXCOORDS[] = {
    0x40, 0xf4, 0xf6, 0xf6, 0xda, 0x47, 0xd2, 0x11,
    0x8f, 0x52, 0x00, 0x40, 0x33, 0x94, 0xa3, 0x04
};
static const uint8_t GUID_MATERIAL[] = {
    0x4d, 0xab, 0x82, 0x3d, 0xda, 0x62, 0xcf, 0x11,
    0xab, 0x39, 0x00, 0x20, 0xaf, 0x71, 0xe4, 0x33
};
static const uint8_t GUID_TEXTURE_FILENAME[] = {
    0xe1, 0x90, 0x27, 0xa4, 0x10, 0x78, 0xcf, 0x11,
    0x8f, 0x52, 0x00, 0x40, 0x33, 0x94, 0xa3, 0x04
};

// ── 简单的流式读取器 ──
class BinaryReader {
    const uint8_t *m_data;
    size_t m_size;
    size_t m_pos;
public:
    BinaryReader(const uint8_t *d, size_t s) : m_data(d), m_size(s), m_pos(0) {}

    bool AtEnd() const { return m_pos >= m_size; }

    uint32_t ReadDword() {
        if (m_pos + 4 > m_size) return 0;
        uint32_t v;
        memcpy(&v, m_data + m_pos, 4);
        m_pos += 4;
        return v;
    }

    float ReadFloat() {
        uint32_t d = ReadDword();
        float f;
        memcpy(&f, &d, 4);
        return f;
    }

    std::string ReadString() {
        std::string s;
        while (m_pos < m_size && m_data[m_pos] != 0) {
            s += static_cast<char>(m_data[m_pos]);
            m_pos++;
        }
        m_pos++; // skip null
        // align to 4 bytes
        m_pos = (m_pos + 3) & ~3;
        return s;
    }

    void Skip(size_t n) { m_pos += n; }

    size_t Pos() const { return m_pos; }
};

static bool FindGUID(const uint8_t *data, size_t size, size_t &pos, const uint8_t *guid) {
    while (pos + 16 <= size) {
        if (memcmp(data + pos, guid, 16) == 0) return true;
        pos += 4; // 4-byte search step
    }
    return false;
}

bool XFileDirectReader::ParseFile(const std::string &filepath) {
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file) { m_error = "Cannot open: " + filepath; return false; }
    size_t size = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(size);
    file.read(reinterpret_cast<char *>(buffer.data()), size);
    file.close();
    return Parse(buffer.data(), size);
}

bool XFileDirectReader::Parse(const uint8_t *data, size_t size) {
    m_meshes.clear();
    m_error.clear();

    // 检查头部
    if (size < 16 || memcmp(data, "xof ", 4) != 0) {
        m_error = "Not a valid .x file";
        return false;
    }

    // 检查是二进制格式
    if (memcmp(data + 4, "0303bin", 7) != 0 && memcmp(data + 4, "0302bin", 7) != 0) {
        // 尝试文本格式（简化处理）
        return ReadText(data, size);
    }

    return ReadBinary(data, size);
}

bool XFileDirectReader::ReadBinary(const uint8_t *data, size_t size) {
    // 跳过头部
    size_t pos = 16;
    // 头部的"0032"后的空格/换行
    while (pos < size && (data[pos] == ' ' || data[pos] == '\r' || data[pos] == '\n' || data[pos] == 0))
        pos++;

    XFileMesh mesh;
    bool hasMesh = false, hasNormals = false, hasTexcoords = false, hasMaterial = false;

    // 搜索 Mesh GUID
    size_t searchPos = pos;
    while (searchPos < size) {
        size_t guidPos = searchPos;
        if (memcmp(data + guidPos, GUID_MESH, 16) == 0) {
            // 找到 Mesh，跳过 GUID 和名称
            BinaryReader br(data, size);
            br.Skip(guidPos + 16);
            // 读取模板名称（整数 + 名称字符串）
            uint32_t nameLen = br.ReadDword();
            br.Skip(nameLen + ((4 - (nameLen % 4)) % 4)); // align

            // 读取 Mesh 数据
            uint32_t vertexCount = br.ReadDword();
            mesh.positions.reserve(vertexCount * 3);
            for (uint32_t i = 0; i < vertexCount; ++i) {
                mesh.positions.push_back(br.ReadFloat()); // x
                mesh.positions.push_back(br.ReadFloat()); // y
                mesh.positions.push_back(br.ReadFloat()); // z
            }

            uint32_t faceCount = br.ReadDword();
            mesh.indices.reserve(faceCount * 3);
            for (uint32_t i = 0; i < faceCount; ++i) {
                uint32_t nIndices = br.ReadDword();
                for (uint32_t j = 0; j < nIndices; ++j)
                    mesh.indices.push_back(br.ReadDword());
            }

            mesh.ComputeBounds();
            hasMesh = true;
            searchPos = br.Pos();

            // 继续在同一区域搜索 Normals
            size_t subSearch = searchPos;
            while (subSearch < size && subSearch < searchPos + 1024) {
                size_t ns = subSearch;
                if (memcmp(data + ns, GUID_MESH_NORMALS, 16) == 0) {
                    BinaryReader nr(data, size);
                    nr.Skip(ns + 16);
                    uint32_t nl = nr.ReadDword();
                    nr.Skip(nl + ((4 - (nl % 4)) % 4));
                    uint32_t nCount = nr.ReadDword();
                    for (uint32_t i = 0; i < nCount; ++i) {
                        mesh.normals.push_back(nr.ReadFloat());
                        mesh.normals.push_back(nr.ReadFloat());
                        mesh.normals.push_back(nr.ReadFloat());
                    }
                    hasNormals = true;
                    break;
                }
                subSearch += 4;
            }

            // 搜索 TexCoords
            subSearch = searchPos;
            while (subSearch < size && subSearch < searchPos + 1024) {
                size_t ts = subSearch;
                if (memcmp(data + ts, GUID_MESH_TEXCOORDS, 16) == 0) {
                    BinaryReader tr(data, size);
                    tr.Skip(ts + 16);
                    uint32_t tl = tr.ReadDword();
                    tr.Skip(tl + ((4 - (tl % 4)) % 4));
                    uint32_t tCount = tr.ReadDword();
                    for (uint32_t i = 0; i < tCount; ++i) {
                        mesh.texcoords.push_back(tr.ReadFloat());
                        mesh.texcoords.push_back(tr.ReadFloat());
                    }
                    hasTexcoords = true;
                    break;
                }
                subSearch += 4;
            }

            // 搜索 Material
            subSearch = searchPos;
            while (subSearch < size && subSearch < searchPos + 2048) {
                size_t ms = subSearch;
                if (memcmp(data + ms, GUID_MATERIAL, 16) == 0) {
                    BinaryReader mr(data, size);
                    mr.Skip(ms + 16);
                    uint32_t ml = mr.ReadDword();
                    mr.Skip(ml + ((4 - (ml % 4)) % 4));

                    // faceColor (4 floats)
                    for (int i = 0; i < 4; ++i) mesh.material.faceColor[i] = mr.ReadFloat();
                    mesh.material.power = mr.ReadFloat();
                    for (int i = 0; i < 3; ++i) mesh.material.specularColor[i] = mr.ReadFloat();
                    for (int i = 0; i < 3; ++i) mesh.material.emissiveColor[i] = mr.ReadFloat();

                    // 搜索 TextureFilename
                    size_t texSearch = mr.Pos();
                    while (texSearch < size && texSearch < mr.Pos() + 512) {
                        if (memcmp(data + texSearch, GUID_TEXTURE_FILENAME, 16) == 0) {
                            BinaryReader txr(data, size);
                            txr.Skip(texSearch + 16);
                            uint32_t txl = txr.ReadDword();
                            txr.Skip(txl + ((4 - (txl % 4)) % 4));
                            mesh.material.textureFilename = txr.ReadString();
                            break;
                        }
                        texSearch += 4;
                    }

                    hasMaterial = true;
                    break;
                }
                subSearch += 4;
            }

            break;
        }
        searchPos += 4;
    }

    if (!hasMesh) { m_error = "No Mesh found in .x file"; return false; }
    m_meshes.push_back(std::move(mesh));
    return true;
}

// ── 简单文本 .x 解析器 ──
bool XFileDirectReader::ReadText(const uint8_t *data, size_t size) {
    std::string text(reinterpret_cast<const char *>(data), size);
    XFileMesh mesh;
    bool inMesh = false, inNormals = false, inTexCoords = false;
    // 仅做占位——社区工具用的是二进制格式
    (void)inMesh; (void)inNormals; (void)inTexCoords;
    m_error = "Text format .x not fully supported by DirectReader";
    return false;
}

} // namespace AssetTool
