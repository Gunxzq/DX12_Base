#pragma once
#include <DirectXMath.h>
#include <cstdint>

namespace DX12Engine::Renderer {

// ============================================================================
// 1. Pass Constants（每帧全局参数）
// ============================================================================

struct PassConstants {

    // 预计算,减少GPU计算量
    DirectX::XMFLOAT4X4 View;        // 视图矩阵
    DirectX::XMFLOAT4X4 Proj;        // 投影矩阵
    DirectX::XMFLOAT4X4 ViewProj;    // 视图投影矩阵
    DirectX::XMFLOAT4X4 InvView;     // 视图矩阵的逆矩阵
    DirectX::XMFLOAT4X4 InvProj;     // 投影矩阵的逆矩阵
    DirectX::XMFLOAT4X4 InvViewProj; // 视图投影矩阵的逆矩阵
    DirectX::XMFLOAT3 CameraPos;     // 相机位置
    float TotalTime;                 // 总时间
    float DeltaTime;                 // 帧时间间隔
    float NearPlane;                 // 近裁剪平面
    float FarPlane;                  // 远裁剪平面
    float AspectRatio;               // 屏幕宽高比
    uint32_t FrameCount;             // 帧计数
    uint32_t LightCount;             // 光源计数
    float AmbientIntensity;          // 环境光强度
    float Pad[3];
};

// ============================================================================
// 2. Object Constants（物体变换）
// ============================================================================

struct ObjectConstants {
    DirectX::XMFLOAT4X4 World;             // 物体变换矩阵
    DirectX::XMFLOAT4X4 WorldInvTranspose; // 物体变换矩阵的逆矩阵的转置矩阵
    DirectX::XMFLOAT4X4 PrevWorld;         // 上一帧的变换矩阵
    uint32_t MaterialIndex;                // 材质索引
    uint32_t ReceiveShadow;                // 是否接收阴影
    float Pad[2];
};

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
    DirectX::XMFLOAT4 Emissive;             // 自发光颜色
    float Metallic;                         // 元金属度
    float Roughness;                        // 粗糙度
    float Alpha;                            // 透明度
    float AlphaCutoff;                      // 透明度阈值
    uint32_t BaseColorTextureIndex;         // 基础颜色纹理索引
    uint32_t NormalTextureIndex;            // 法线纹理索引
    uint32_t MetallicRoughnessTextureIndex; // 元金属度和粗糙度纹理索引
    uint32_t EmissiveTextureIndex;          // 自发光颜色纹理索引
    uint32_t OcclusionTextureIndex;         // 阴挡纹理索引
    float Pad[3];
};

// ============================================================================
// 5. Light Constants（光源）
// ============================================================================

enum class LightType : uint32_t { Directional, Point, Spot };

struct Light {
    LightType Type;
    float Intensity;
    DirectX::XMFLOAT3 Color;

    uint32_t CastShadow;
    float ShadowBias;
    float ShadowMapIndex;

    union {
        // 方向光
        struct {
            DirectX::XMFLOAT3 Direction;
        } Dir;

        // 点光源
        struct {
            DirectX::XMFLOAT3 Position;
            float Range;
            float Falloff;
        } Point;

        // 聚光灯
        struct {
            DirectX::XMFLOAT3 Position;
            float Range;
            DirectX::XMFLOAT3 Direction;
            float SpotAngle;
            float Falloff;
        } Spot;
    };
};

struct LightConstants {
    Light Lights[256];              // 光源数组
    uint32_t DirectionalLightCount; // 方向光计数
    uint32_t PointLightCount;       // 点光源计数
    uint32_t SpotLightCount;        // 聚光灯计数
    float AmbientIntensity;         // 环境光强度
    DirectX::XMFLOAT3 AmbientColor; // 环境光颜色
    float Pad;
};

// ============================================================================
// 6. Shadow Constants（阴影）
// ============================================================================

struct ShadowConstants {
    DirectX::XMFLOAT4X4 LightViewProj; // 光源视图投影矩阵
    DirectX::XMFLOAT3 LightDir;        // 光源方向
    float ShadowMapSize;               // 阴影贴图大小
    float ShadowBias;                  // 阴影偏移
    float NormalBias;                  // 法线偏移
    float CascadeSplit0;               // 级联分割0
    float CascadeSplit1;               // 级联分割1
    float CascadeSplit2;               // 级联分割2
    uint32_t CascadeCount;             // 级联计数
    float Pad[3];
};

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
    DirectX::XMFLOAT4X4 World;             // 物体变换矩阵
    DirectX::XMFLOAT4X4 WorldInvTranspose; // 物体逆变换矩阵的转置
    float Time;                            // 时间
    float WaveAmplitude;                   // 波幅
    float WaveFrequency;                   // 波频
    float WaveSpeed;                       // 波速
    float RefractionStrength;              // 折射强度
    float FresnelPower;                    // 斯涅尔功率
    float FoamIntensity;                   // 泡泡强度
    float Pad;                             // 填充
    uint32_t ReflectionTextureIndex;       // 反射纹理索引
    uint32_t RefractionTextureIndex;       // 折射纹理索引
    uint32_t DepthTextureIndex;            // 深度纹理索引
    uint32_t NormalTextureIndex;           // 法线纹理索引
};

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

// ============================================================================
// 11. Instance Data（GPU 实例化）
// ============================================================================

struct InstanceData {
    DirectX::XMFLOAT4X4 World;             // 物体变换矩阵
    DirectX::XMFLOAT4X4 WorldInvTranspose; // 物体逆变换矩阵的转置
    uint32_t MaterialIndex;                // 材质索引
    uint32_t ReceiveShadow;                // 是否接收阴影
    float Pad[2];
};

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
    float Pad[3];
};

} // namespace DX12Engine::Renderer