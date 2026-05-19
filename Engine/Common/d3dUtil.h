//***************************************************************************************
// d3dUtil.h by Frank Luna (C) 2015 All Rights Reserved.
//
// General helper code.
//***************************************************************************************

#pragma once

#include "Common/MathHelper.h"
#include "Common/ThrowHelper.h"
#include "Renderer/Utils/DDSTextureLoader.h"
#include "d3dx12.h"
#include <D3Dcompiler.h>
#include <DirectXCollision.h>
#include <DirectXColors.h>
#include <DirectXMath.h>
#include <DirectXPackedVector.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl.h>

// ---------- 保留必要的类型别名 ----------
using Microsoft::WRL::ComPtr;

extern const int gNumFrameResources;

inline std::wstring AnsiToWString(const std::string &str) {
    WCHAR buffer[512];
    MultiByteToWideChar(CP_ACP, 0, str.c_str(), -1, buffer, 512);
    return std::wstring(buffer);
}

class d3dUtil {
public:
    static bool IsKeyDown(int vkeyCode);
    static std::string ToString(HRESULT hr);

    static UINT CalcConstantBufferByteSize(UINT byteSize) { return (byteSize + 255) & ~255; }

    static ComPtr<ID3DBlob> LoadBinary(const std::wstring &filename);
    static ComPtr<ID3D12Resource> CreateDefaultBuffer(ID3D12Device *device, ID3D12GraphicsCommandList *cmdList,
                                                      const void *initData, UINT64 byteSize,
                                                      ComPtr<ID3D12Resource> &uploadBuffer);
    static ComPtr<ID3DBlob> CompileShader(const std::wstring &filename, const D3D_SHADER_MACRO *defines,
                                          const std::string &entrypoint, const std::string &target);

    /**
     * @brief 创建上传堆资源（Upload Heap Resource）
     * @param device D3D12 设备指针
     * @param byteSize 资源大小（字节）
     * @param resourceState 资源状态
     * @param outResource 输出的资源指针
     * @return HRESULT 创建结果
     */
    static HRESULT CreateUploadBuffer(ID3D12Device *device, UINT64 byteSize, D3D12_RESOURCE_STATES resourceState,
                                      ID3D12Resource **outResource);

    /**
     * @brief 创建默认堆资源（Default Heap Resource）
     * @param device D3D12 设备指针
     * @param byteSize 资源大小（字节）
     * @param resourceState 资源状态
     * @param outResource 输出的资源指针
     * @return HRESULT 创建结果
     */
    static HRESULT CreateDefaultBuffer(ID3D12Device *device, UINT64 byteSize, D3D12_RESOURCE_STATES resourceState,
                                       ID3D12Resource **outResource);
};
