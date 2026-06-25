#pragma once
#include <DirectXMath.h>
#include <cstdint>

#pragma pack(push, 16)

namespace DX12Engine {

namespace Renderer {

// ============================================================================
// 1. Pass Constants（每帧全局参数）
// ============================================================================

struct PassConstants {

    // 预计算,减少GPU计算量
    DirectX::XMFLOAT4X4 View;         // 视图矩阵
    DirectX::XMFLOAT4X4 Proj;         // 投影矩阵
    DirectX::XMFLOAT4X4 ViewProj;     // 视图投影矩阵
    DirectX::XMFLOAT4X4 InvView;      // 视图矩阵的逆矩阵
    DirectX::XMFLOAT4X4 InvProj;      // 投影矩阵的逆矩阵
    DirectX::XMFLOAT4X4 InvViewProj;  // 视图投影矩阵的逆矩阵
    DirectX::XMFLOAT4X4 PrevViewProj; // 上一帧视图投影矩阵
    DirectX::XMFLOAT3 CameraPos;      // 相机位置
    float TotalTime;                  // 总时间
    float DeltaTime;                  // 帧时间间隔
    float NearPlane;                  // 近裁剪平面
    float FarPlane;                   // 远裁剪平面
    float AspectRatio;                // 屏幕宽高比
    uint32_t FrameCount;              // 帧计数
    float Pad[3];
};

static_assert(sizeof(PassConstants) % 16 == 0,
              "PassConstants alignment error: size must be multiple of 16 bytes for HLSL cbuffer compatibility");

// ============================================================================
// 2. Object Constants（物体变换）处理独立/特殊物体
// ============================================================================

struct ObjectConstants {
    DirectX::XMFLOAT4X4 World;             // 物体变换矩阵
    DirectX::XMFLOAT4X4 WorldInvTranspose; // 物体变换矩阵的逆矩阵的转置矩阵
    DirectX::XMFLOAT4X4 PrevWorld;         // 上一帧的变换矩阵
    uint32_t MaterialIndex;                // 材质索引
    uint32_t ReceiveShadow;                // 是否接收阴影
    float ObjPad[2];
};

static_assert(sizeof(ObjectConstants) % 16 == 0,
              "ObjectConstants alignment error: size must be multiple of 16 bytes for HLSL cbuffer compatibility");

// ============================================================================
// 3. Skinning Constants（骨骼动画）
// ============================================================================

constexpr uint32_t MAX_BONE_COUNT = 256;

struct SkinningConstants {
    DirectX::XMFLOAT4X4 World;                          // 物体变换矩阵
    DirectX::XMFLOAT4X4 BoneTransforms[MAX_BONE_COUNT]; // 骨骼变换矩阵
};

// ============================================================================
// 4. Material Constants（材质参数）
// ============================================================================

struct MaterialConstants {
    DirectX::XMFLOAT4 BaseColor;            // 基础颜色
    float Metallic;                         // 元金属度
    float Roughness;                        // 粗糙度
    float Ambient;                          // 环境光遮蔽强度
    float Alpha;                            // 透明度
    DirectX::XMFLOAT4 Emissive;             // 自发光颜色
    float AlphaCutoff;                      // 透明度阈值
    uint32_t BaseColorTextureIndex;         // 基础颜色纹理索引
    uint32_t NormalTextureIndex;            // 法线纹理索引
    uint32_t MetallicRoughnessTextureIndex; // 元金属度和粗糙度纹理索引
    uint32_t EmissiveTextureIndex;          // 自发光颜色纹理索引
    uint32_t OcclusionTextureIndex;         // 遮挡纹理索引
    float MatPad[2];                        // 填充
};

static_assert(
    sizeof(MaterialConstants) % 16 == 0,
    "MaterialConstants alignment error: size must be multiple of 16 bytes for HLSL StructuredBuffer compatibility");

// ============================================================================
// 7. Fog / Atmosphere Constants
// ============================================================================

struct FogConstants {
    DirectX::XMFLOAT3 Color; // 雾颜色
    float Density;           // 雾密度
    float StartDistance;     // 雾开始距离
    float EndDistance;       // 雾结束距离
    float HeightFogStart;    // 高度雾开始距离
    float HeightFogEnd;      // 高度雾结束距离
    float HeightFogDensity;  // 高度雾密度
    float Pad;
};

// ============================================================================
// 8. Water Constants
// ============================================================================

struct WaterConstants {
    float Time;                      // 时间
    float WaveAmplitude;             // 波幅
    float WaveFrequency;             // 波频
    float WaveSpeed;                 // 波速
    float RefractionStrength;        // 折射强度
    float FresnelPower;              // 斯涅尔功率
    float FoamIntensity;             // 泡泡强度
    uint32_t ReflectionTextureIndex; // 反射纹理索引
    uint32_t RefractionTextureIndex; // 折射纹理索引
    uint32_t DepthTextureIndex;      // 深度纹理索引
    uint32_t NormalTextureIndex;     // 法线纹理索引
    float Pad;                       // 填充
};

static_assert(sizeof(WaterConstants) % 16 == 0,
              "WaterConstants alignment error: size must be multiple of 16 bytes for HLSL cbuffer compatibility");

// ============================================================================
// 9. Post Process Constants
// ============================================================================

struct PostProcessConstants {
    float Exposure;                // 曝光度
    float Gamma;                   // Gamma 值
    float BloomIntensity;          // 泛光强度
    float BloomThreshold;          // 泛光阈值
    float VignetteIntensity;       // 晕影强度
    float VignettePower;           // 晕影衰减指数
    float VignetteRadius;          // 晕影半径
    float Time;                    // 时间
    DirectX::XMFLOAT3 ColorFilter; // 颜色滤镜
    float Pad;                     // 填充
    uint32_t InputTextureIndex;    // 输入纹理索引
    uint32_t BloomTextureIndex;    // 模糊纹理索引
    uint32_t DepthTextureIndex;    // 深度纹理索引
    uint32_t OutputTextureIndex;   // 输出纹理索引
};

// ============================================================================
// 10. Particle Data（GPU 驱动）
// ============================================================================

struct Particle {
    DirectX::XMFLOAT3 Position; // 位置
    float Life;                 // 生命时间
    DirectX::XMFLOAT3 Velocity; // 速度
    float Size;                 // 尺寸
    DirectX::XMFLOAT4 Color;    // 颜色
    float Rotation;             // 旋转角度
    float Pad[3];
};

static_assert(sizeof(Particle) % 16 == 0,
              "Particle alignment error: size must be multiple of 16 bytes for HLSL StructuredBuffer compatibility");

// ============================================================================
// 11. Instance Data（GPU 实例化）处理批量/重复物体
// ============================================================================

struct InstanceData {
    DirectX::XMFLOAT4X4 World;             // 物体变换矩阵 (offset: 0)
    DirectX::XMFLOAT4X4 WorldInvTranspose; // 物体逆变换矩阵的转置 (offset: 64)
    uint32_t MaterialIndex;                // 材质索引-后续可改进为材质模板
    uint32_t ReceiveShadow;                // 是否接收阴影
    uint32_t ProbeIndex;                   // 反射探针索引 (UINT32_MAX = 无反射)
    float pad;                             // 16字节对齐填充
};

static_assert(
    sizeof(InstanceData) % 16 == 0,
    "InstanceData alignment error: size must be multiple of 16 bytes for HLSL StructuredBuffer compatibility");

// ============================================================================
// 12. Decal Constants
// ============================================================================

struct DecalConstants {
    DirectX::XMFLOAT4X4 World;             // 物体变换矩阵
    DirectX::XMFLOAT4X4 WorldInvTranspose; // 物体逆变换矩阵的转置
    DirectX::XMFLOAT3 Position;            // 位置
    float Size;                            // 尺寸
    DirectX::XMFLOAT3 Normal;              // 法线
    float Angle;                           // 角度
    uint32_t AlbedoTextureIndex;           // 漫反射纹理索引
    uint32_t NormalTextureIndex;           // 法线纹理索引
    uint32_t MaskTextureIndex;             // 掩码纹理索引
    float Opacity;                         // 透明度
};

static_assert(sizeof(DecalConstants) % 16 == 0,
              "DecalConstants alignment error: size must be multiple of 16 bytes for HLSL cbuffer compatibility");

} // namespace Renderer

} // namespace DX12Engine

// 恢复默认打包设置
#pragma pack(pop)
