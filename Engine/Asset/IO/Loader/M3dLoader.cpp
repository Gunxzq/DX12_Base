#include "M3dLoader.h"

#include <fstream>
#include <string>

using namespace DirectX;

namespace DX12Engine::Resource {

// ============================================================================
// 内部解析辅助
// ============================================================================
namespace {

/// 读取一个带标签的单值：标签 + 值
template <typename T>
static void ReadTaggedValue(std::ifstream &fin, T &outValue) {
    std::string tag;
    fin >> tag >> outValue;
}

/// 读取 4x4 矩阵（16个浮点数），行优先，跳过前导标签（如 BoneOffset0）
static void ReadMatrix(std::ifstream &fin, XMFLOAT4X4 &outMatrix) {
    std::string ignore;
    fin >> ignore >>
        outMatrix(0, 0) >> outMatrix(0, 1) >> outMatrix(0, 2) >> outMatrix(0, 3) >>
        outMatrix(1, 0) >> outMatrix(1, 1) >> outMatrix(1, 2) >> outMatrix(1, 3) >>
        outMatrix(2, 0) >> outMatrix(2, 1) >> outMatrix(2, 2) >> outMatrix(2, 3) >>
        outMatrix(3, 0) >> outMatrix(3, 1) >> outMatrix(3, 2) >> outMatrix(3, 3);
}

} // anonymous namespace

// ============================================================================
// LoadFromFile — 完整解析 .m3d 文本格式
// ============================================================================
bool M3dLoader::LoadFromFile(const std::string &filepath, M3dMeshData &outData) {
    std::ifstream fin(filepath);
    if (!fin) {
        return false;
    }

    std::string ignore;

    // ========================================================================
    // 1. Header
    //    ***************m3d-File-Header***************
    //    #Materials <N>
    //    #Vertices <N>
    //    #Triangles <N>
    //    #Bones <N>
    //    #AnimationClips <N>
    // ========================================================================
    uint32_t numMaterials = 0, numVertices = 0, numTriangles = 0, numBones = 0, numAnimationClips = 0;
    fin >> ignore; // "***************m3d-File-Header***************"
    ReadTaggedValue(fin, numMaterials);
    ReadTaggedValue(fin, numVertices);
    ReadTaggedValue(fin, numTriangles);
    ReadTaggedValue(fin, numBones);
    ReadTaggedValue(fin, numAnimationClips);

    if (numVertices == 0 || numTriangles == 0) {
        return false;
    }

    // ========================================================================
    // 2. Materials
    //    ***************Materials*********************
    //    Name: <name>
    //    Diffuse: r g b
    //    Fresnel0: r g b
    //    Roughness: <val>
    //    AlphaClip: <0/1>
    //    MaterialTypeName: <name>
    //    DiffuseMap: <filename>
    //    NormalMap: <filename>
    // ========================================================================
    outData.materials.resize(numMaterials);
    fin >> ignore; // "***************Materials*********************"
    for (uint32_t i = 0; i < numMaterials; ++i) {
        auto &mat = outData.materials[i];

        fin >> ignore; // "Name:"
        fin >> ignore; // material name (not stored, only for skipping)

        fin >> ignore >> mat.DiffuseAlbedo.x >> mat.DiffuseAlbedo.y >> mat.DiffuseAlbedo.z;
        mat.DiffuseAlbedo.w = 1.0f; // .m3d stores only RGB

        fin >> ignore >> mat.FresnelR0.x >> mat.FresnelR0.y >> mat.FresnelR0.z;

        fin >> ignore >> mat.Roughness;

        fin >> ignore >> mat.AlphaClip;

        fin >> ignore >> mat.EffectTypeName;

        fin >> ignore >> mat.DiffuseMapName;

        fin >> ignore >> mat.NormalMapName;
    }

    // ========================================================================
    // 3. SubsetTable
    //    ***************SubsetTable*******************
    //    SubsetID: <id> VertexStart: <v> VertexCount: <v> FaceStart: <f> FaceCount: <f>
    // ========================================================================
    outData.subsets.resize(numMaterials);
    fin >> ignore; // "***************SubsetTable*******************"
    for (uint32_t i = 0; i < numMaterials; ++i) {
        auto &sub = outData.subsets[i];
        fin >> ignore >> sub.materialIndex; // "SubsetID:" <id>
        fin >> ignore >> sub.vertexStart;   // "VertexStart:" <v>
        fin >> ignore >> sub.vertexCount;   // "VertexCount:" <v>
        fin >> ignore >> sub.faceStart;     // "FaceStart:" <f>
        fin >> ignore >> sub.faceCount;     // "FaceCount:" <f>
    }

    // ========================================================================
    // 4. Vertices
    //    ***************Vertices**********************
    //    Position: x y z
    //    Tangent: x y z w
    //    Normal: x y z
    //    Tex-Coords: u v
    //    BlendWeights: w0 w1 w2 w3
    //    BlendIndices: i0 i1 i2 i3
    // ========================================================================
    outData.vertices.resize(numVertices);
    fin >> ignore; // "***************Vertices**********************"
    for (uint32_t i = 0; i < numVertices; ++i) {
        auto &v = outData.vertices[i];
        float weights[4];
        float indices[4];

        fin >> ignore >> v.Pos.x >> v.Pos.y >> v.Pos.z;                    // "Position:" x y z
        fin >> ignore >> v.TangentU.x >> v.TangentU.y >> v.TangentU.z >> ignore; // "Tangent:" x y z w (w discarded)
        fin >> ignore >> v.Normal.x >> v.Normal.y >> v.Normal.z;            // "Normal:" x y z
        fin >> ignore >> v.TexC.x >> v.TexC.y;                              // "Tex-Coords:" u v
        fin >> ignore >> weights[0] >> weights[1] >> weights[2] >> weights[3]; // "BlendWeights:" w0 w1 w2 w3
        fin >> ignore >> indices[0] >> indices[1] >> indices[2] >> indices[3]; // "BlendIndices:" i0 i1 i2 i3

        v.BoneWeights = XMFLOAT4(weights[0], weights[1], weights[2], weights[3]);
        v.BoneIndices[0] = static_cast<uint8_t>(indices[0]);
        v.BoneIndices[1] = static_cast<uint8_t>(indices[1]);
        v.BoneIndices[2] = static_cast<uint8_t>(indices[2]);
        v.BoneIndices[3] = static_cast<uint8_t>(indices[3]);
    }

    // ========================================================================
    // 5. Triangles
    //    ***************Triangles*********************
    //    i0 i1 i2  (每行一个三角形)
    // ========================================================================
    outData.indices.resize(numTriangles * 3);
    fin >> ignore; // "***************Triangles*********************"
    for (uint32_t i = 0; i < numTriangles * 3; i += 3) {
        uint32_t a, b, c;
        fin >> a >> b >> c;
        outData.indices[i + 0] = a;
        outData.indices[i + 1] = b;
        outData.indices[i + 2] = c;
    }

    // ========================================================================
    // 6. BoneOffsets
    //    ***************BoneOffsets*******************
    //    BoneOffset<N> m00 m01 m02 m03 m10 m11 m12 m13 m20 m21 m22 m23 m30 m31 m32 m33
    // ========================================================================
    if (numBones > 0) {
        outData.boneOffsets.resize(numBones);
        fin >> ignore; // "***************BoneOffsets*******************"
        for (uint32_t i = 0; i < numBones; ++i) {
            ReadMatrix(fin, outData.boneOffsets[i]);
        }

        // ========================================================================
        // 7. BoneHierarchy
        //    ***************BoneHierarchy*****************
        //    ParentIndexOfBone<N>: <parentIndex>
        // ========================================================================
        outData.boneHierarchy.resize(numBones);
        fin >> ignore; // "***************BoneHierarchy*****************"
        for (uint32_t i = 0; i < numBones; ++i) {
            fin >> ignore >> outData.boneHierarchy[i];
        }

        // ========================================================================
        // 8. BoneNames（该文件格式通常不含此部分）
        // ========================================================================
        outData.boneNames.clear();

        // ========================================================================
        // 9. AnimationClips
        //    ***************AnimationClips****************
        //    AnimationClip <name>
        //    {
        //        Bone<N> #Keyframes: <N>
        //        {
        //            Time: <t> Pos: x y z Scale: x y z Quat: x y z w
        //            ...
        //        }
        //        ...
        //    }
        // ========================================================================
        if (numAnimationClips > 0) {
            fin >> ignore; // "***************AnimationClips****************"
            for (uint32_t clipIdx = 0; clipIdx < numAnimationClips; ++clipIdx) {
                std::string clipName;
                fin >> ignore >> clipName; // "AnimationClip" <name>

                AnimationClip clip;
                clip.BoneAnimations.resize(numBones);
                fin >> ignore; // "{"

                for (uint32_t boneIdx = 0; boneIdx < numBones; ++boneIdx) {
                    uint32_t numKeyframes = 0;
                    fin >> ignore >> ignore >> numKeyframes; // "Bone<N>" "#Keyframes:" <N>
                    fin >> ignore; // "{"

                    BoneAnimation &boneAnim = clip.BoneAnimations[boneIdx];
                    boneAnim.Keyframes.resize(numKeyframes);
                    for (uint32_t kf = 0; kf < numKeyframes; ++kf) {
                        Keyframe &key = boneAnim.Keyframes[kf];
                        fin >> ignore >> key.TimePos;                         // "Time:" t
                        fin >> ignore >> key.Translation.x >> key.Translation.y >> key.Translation.z; // "Pos:" x y z
                        fin >> ignore >> key.Scale.x >> key.Scale.y >> key.Scale.z;                   // "Scale:" x y z
                        fin >> ignore >> key.RotationQuat.x >> key.RotationQuat.y >> key.RotationQuat.z >> key.RotationQuat.w; // "Quat:" x y z w
                    }
                    fin >> ignore; // "}"
                }
                fin >> ignore; // "}"

                outData.animations[clipName] = std::move(clip);
            }
        }
    }

    return true;
}

} // namespace DX12Engine::Resource
