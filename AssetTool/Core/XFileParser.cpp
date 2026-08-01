#include "XFileParser.h"
#include "XORCipher.h"
#include <algorithm>
#include <assimp/Importer.hpp>
#include <assimp/material.h>
#include <assimp/mesh.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <cfloat>
#include <cstring>
#include <fstream>
#include <vector>

namespace AssetTool {

// ── 默认导入标志 ──
// 注意：去掉 JoinIdenticalVertices 以匹配社区工具的顶点数（不焊接重复顶点）
//       ImproveCacheLocality 也会移动顶点，一并去掉
static constexpr unsigned int DEFAULT_FLAGS = aiProcess_Triangulate | aiProcess_GenNormals | aiProcess_SortByPType;

// ==========================================================================
// 辅助：assimp 矩阵/颜色 → 引擎格式
// ==========================================================================

static void AiColorToFloat4(const aiColor4D &src, float dst[4]) {
    dst[0] = src.r;
    dst[1] = src.g;
    dst[2] = src.b;
    dst[3] = src.a;
}

// ==========================================================================
// 加载一个 assimp aiMesh → XFileMesh
// ==========================================================================

static bool ConvertAiMesh(const aiMesh *aiMsh, XFileMesh &outMesh) {
    if (!aiMsh || aiMsh->mNumVertices == 0)
        return false;

    outMesh.name = aiMsh->mName.C_Str();

    uint32_t vc = aiMsh->mNumVertices;
    uint32_t fc = aiMsh->mNumFaces;

    // 顶点位置
    outMesh.positions.reserve(vc * 3);
    for (uint32_t i = 0; i < vc; ++i) {
        outMesh.positions.push_back(aiMsh->mVertices[i].x);
        outMesh.positions.push_back(aiMsh->mVertices[i].y);
        outMesh.positions.push_back(aiMsh->mVertices[i].z);
    }

    // 法线
    if (aiMsh->HasNormals()) {
        outMesh.normals.reserve(vc * 3);
        for (uint32_t i = 0; i < vc; ++i) {
            outMesh.normals.push_back(aiMsh->mNormals[i].x);
            outMesh.normals.push_back(aiMsh->mNormals[i].y);
            outMesh.normals.push_back(aiMsh->mNormals[i].z);
        }
    }

    // UV
    if (aiMsh->HasTextureCoords(0)) {
        outMesh.texcoords.reserve(vc * 2);
        for (uint32_t i = 0; i < vc; ++i) {
            outMesh.texcoords.push_back(aiMsh->mTextureCoords[0][i].x);
            outMesh.texcoords.push_back(aiMsh->mTextureCoords[0][i].y);
        }
    }

    // 索引
    outMesh.indices.reserve(fc * 3);
    for (uint32_t i = 0; i < fc; ++i) {
        const aiFace &face = aiMsh->mFaces[i];
        for (uint32_t j = 0; j < face.mNumIndices; ++j)
            outMesh.indices.push_back(face.mIndices[j]);
    }

    outMesh.ComputeBounds();
    return true;
}

// ==========================================================================
// 加载 assimp aiMaterial → XFileMaterial
// ==========================================================================

static void ConvertAiMaterial(const aiMaterial *aiMat, XFileMaterial &outMat) {
    if (!aiMat)
        return;

    // 漫反射颜色 (faceColor)
    aiColor4D diffuse(0.8f, 0.8f, 0.8f, 1.0f);
    aiGetMaterialColor(aiMat, AI_MATKEY_COLOR_DIFFUSE, &diffuse);
    AiColorToFloat4(diffuse, outMat.faceColor);

    // 高光指数 (power)
    float shininess = 10.0f;
    aiGetMaterialFloat(aiMat, AI_MATKEY_SHININESS, &shininess);
    outMat.power = shininess;

    // 高光颜色（assimp API 统一使用 aiColor4D）
    aiColor4D specular(0.0f, 0.0f, 0.0f, 1.0f);
    aiGetMaterialColor(aiMat, AI_MATKEY_COLOR_SPECULAR, &specular);
    outMat.specularColor[0] = specular.r;
    outMat.specularColor[1] = specular.g;
    outMat.specularColor[2] = specular.b;

    // 自发光颜色
    aiColor4D emissive(0.0f, 0.0f, 0.0f, 1.0f);
    aiGetMaterialColor(aiMat, AI_MATKEY_COLOR_EMISSIVE, &emissive);
    outMat.emissiveColor[0] = emissive.r;
    outMat.emissiveColor[1] = emissive.g;
    outMat.emissiveColor[2] = emissive.b;

    // 漫反射纹理文件名
    aiString texPath;
    if (aiMat->GetTexture(aiTextureType_DIFFUSE, 0, &texPath) == AI_SUCCESS) {
        outMat.textureFilename = texPath.C_Str();
    }
}

// ==========================================================================
// XFileParser 实现
// ==========================================================================

/// 检测缓冲区是否可能是 XOR 加密的 .x 文件
/// 明文 .x 文件头部为 "xof "，加密后基本不会是
static bool LooksEncrypted(const uint8_t *data, size_t size) { return size >= 4 && std::memcmp(data, "xof ", 4) != 0; }

bool XFileParser::TryImport(const uint8_t *data, size_t size, std::string &outError) {
    if (!m_importFlags)
        m_importFlags = DEFAULT_FLAGS;

    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFileFromMemory(data, size, m_importFlags, ".x");

    if (!scene) {
        outError = importer.GetErrorString();
        return false;
    }

    return Import(scene);
}

bool XFileParser::Parse(const uint8_t *data, size_t size) {
    m_meshes.clear();
    m_error.clear();

    // 第一次尝试：直接导入
    {
        std::string err;
        if (TryImport(data, size, err))
            return true;
        m_error = err;
    }

    // 自动解密重试：若数据看起来被 XOR 加密，用已知 key 逐个尝试
    if (m_autoDecrypt && LooksEncrypted(data, size) && !m_decryptKeys.empty()) {
        for (uint32_t key : m_decryptKeys) {
            // 解密副本
            std::vector<uint8_t> decrypted(data, data + size);
            XORCipher cipher(key);
            cipher.DecryptBuffer(decrypted.data(), decrypted.size());

            // 解密后仍然以 "xof " 开头才算有效
            if (!LooksEncrypted(decrypted.data(), decrypted.size())) {
                std::string err;
                if (TryImport(decrypted.data(), decrypted.size(), err)) {
                    m_error.clear();
                    return true;
                }
            }
        }
        // 所有 key 都试过仍不行，保留原始错误信息
    }

    return false;
}

bool XFileParser::ParseFile(const std::string &filepath) {
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file) {
        m_error = "Cannot open file: " + filepath;
        return false;
    }
    size_t size = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(size);
    file.read(reinterpret_cast<char *>(buffer.data()), size);
    file.close();

    // 检测是否为 XOR 加密（.x 文件魔数应为 "xof "）
    if (size >= 4 && std::memcmp(buffer.data(), "xof ", 4) == 0) {
        return Parse(buffer.data(), size);
    }

    // 尝试 XOR 解密
    {
        XORCipher cipher(0x0B7E7759);
        cipher.DecryptBuffer(buffer.data(), size);
        if (size >= 4 && std::memcmp(buffer.data(), "xof ", 4) == 0)
            return Parse(buffer.data(), size);
    }

    // 尝试其他 key
    uint32_t altKeys[] = {
        0x95127634, 0x19870430, 0xAC510B91, 0x13322366, 0xEF452301, 0x33333323, 0x33322166,
    };
    for (uint32_t altKey : altKeys) {
        if (altKey == 0x0B7E7759)
            continue;
        std::vector<uint8_t> altBuf(buffer);
        XORCipher altCipher(altKey);
        altCipher.DecryptBuffer(altBuf.data(), altBuf.size());
        if (altBuf.size() >= 4 && std::memcmp(altBuf.data(), "xof ", 4) == 0)
            return Parse(altBuf.data(), altBuf.size());
    }

    m_error =
        "Cannot parse .x file (tried plain + " + std::to_string(1 + sizeof(altKeys) / sizeof(altKeys[0])) + " keys)";
    return false;
}

bool XFileParser::Import(const aiScene *scene) {
    if (!scene || !scene->HasMeshes()) {
        m_error = "No meshes found in .x file";
        return false;
    }

    for (uint32_t mi = 0; mi < scene->mNumMeshes; ++mi) {
        XFileMesh mesh;

        // 几何体
        if (!ConvertAiMesh(scene->mMeshes[mi], mesh))
            continue;

        // 材质
        uint32_t matIdx = scene->mMeshes[mi]->mMaterialIndex;
        if (matIdx < scene->mNumMaterials) {
            ConvertAiMaterial(scene->mMaterials[matIdx], mesh.material);
        }

        // 若 mesh 无名，用文件级默认名
        if (mesh.name.empty()) {
            mesh.name = "mesh_" + std::to_string(mi);
        }

        m_meshes.push_back(std::move(mesh));
    }

    return !m_meshes.empty();
}

// ==========================================================================
// XFileMesh 方法
// ==========================================================================

void XFileMesh::ComputeBounds() {
    boundsMin[0] = boundsMin[1] = boundsMin[2] = FLT_MAX;
    boundsMax[0] = boundsMax[1] = boundsMax[2] = -FLT_MAX;
    for (size_t i = 0; i + 2 < positions.size(); i += 3) {
        boundsMin[0] = std::min(boundsMin[0], positions[i + 0]);
        boundsMin[1] = std::min(boundsMin[1], positions[i + 1]);
        boundsMin[2] = std::min(boundsMin[2], positions[i + 2]);
        boundsMax[0] = std::max(boundsMax[0], positions[i + 0]);
        boundsMax[1] = std::max(boundsMax[1], positions[i + 1]);
        boundsMax[2] = std::max(boundsMax[2], positions[i + 2]);
    }
}

bool XFileMesh::WriteDxMesh(const std::string &outputPath) const {
    // 注意：DxMeshHeader / DxMeshStaticVertex / DxMeshLOD 在全局命名空间
    DxMeshHeader header = {};
    std::memcpy(header.magic, DX_MESH_MAGIC, 8);
    header.version = DX_MESH_VERSION;
    header.flags = 0; // 静态网格
    header.indexSize = 4;

    uint32_t vCount = static_cast<uint32_t>(VertexCount());
    uint32_t iCount = static_cast<uint32_t>(indices.size());
    if (vCount == 0)
        return false;

    header.vertexCount = vCount;
    header.indexCount = iCount;
    header.vertexStride = sizeof(DxMeshStaticVertex);
    header.boundsMin[0] = boundsMin[0];
    header.boundsMin[1] = boundsMin[1];
    header.boundsMin[2] = boundsMin[2];
    header.boundsMax[0] = boundsMax[0];
    header.boundsMax[1] = boundsMax[1];
    header.boundsMax[2] = boundsMax[2];
    header.lodCount = 1;

    // 构建顶点
    std::vector<DxMeshStaticVertex> vertices(vCount);
    for (uint32_t i = 0; i < vCount; ++i) {
        vertices[i].position[0] = positions[i * 3 + 0];
        vertices[i].position[1] = positions[i * 3 + 1];
        vertices[i].position[2] = positions[i * 3 + 2];

        if (HasNormals()) {
            vertices[i].normal[0] = normals[i * 3 + 0];
            vertices[i].normal[1] = normals[i * 3 + 1];
            vertices[i].normal[2] = normals[i * 3 + 2];
        } else {
            vertices[i].normal[0] = 0.0f;
            vertices[i].normal[1] = 1.0f;
            vertices[i].normal[2] = 0.0f;
        }
        vertices[i].tangentU[0] = 1.0f;
        vertices[i].tangentU[1] = 0.0f;
        vertices[i].tangentU[2] = 0.0f;

        if (HasTexcoords()) {
            vertices[i].texC[0] = texcoords[i * 2 + 0];
            vertices[i].texC[1] = texcoords[i * 2 + 1];
        } else {
            vertices[i].texC[0] = 0.0f;
            vertices[i].texC[1] = 0.0f;
        }
    }

    DxMeshLOD lod = {};
    lod.vertexOffset = 0;
    lod.vertexCount = vCount;
    lod.indexOffset = 0;
    lod.indexCount = iCount;
    lod.errorMetric = 0.0f;

    uint32_t vertexSize = vCount * header.vertexStride;
    uint32_t indexSize = iCount * header.indexSize;
    header.lodOffset = static_cast<uint32_t>(sizeof(header) + vertexSize + indexSize);

    std::ofstream ofs(outputPath, std::ios::binary);
    if (!ofs)
        return false;

    ofs.write(reinterpret_cast<const char *>(&header), sizeof(header));
    ofs.write(reinterpret_cast<const char *>(vertices.data()), vertexSize);
    ofs.write(reinterpret_cast<const char *>(indices.data()), indexSize);
    ofs.write(reinterpret_cast<const char *>(&lod), sizeof(lod));
    return true;
}

// ==========================================================================
// XFileMaterial → MaterialDesc
// ==========================================================================

DX12Engine::Resource::MaterialDesc XFileMaterial::ToMaterialDesc() const {
    DX12Engine::Resource::MaterialDesc desc;
    desc.shader = "PBR/Standard";

    // ColorRGBA: faceColor 按 RGBA 存储（AiColorToFloat4 写入 r,g,b,a）
    desc.params.baseColor[0] = faceColor[0]; // R
    desc.params.baseColor[1] = faceColor[1]; // G
    desc.params.baseColor[2] = faceColor[2]; // B
    desc.params.baseColor[3] = faceColor[3]; // A

    float p = (power > 0.0f) ? power : 10.0f;
    desc.params.roughness = std::max(0.05f, std::min(0.95f, 1.0f / (p + 1.0f)));

    float specIntensity = (specularColor[0] + specularColor[1] + specularColor[2]) / 3.0f;
    desc.params.metallic = std::max(0.0f, std::min(1.0f, specIntensity));
    desc.params.ao = 1.0f;

    if (!textureFilename.empty()) {
        desc.textures.baseColor = textureFilename;
    }
    return desc;
}

} // namespace AssetTool
