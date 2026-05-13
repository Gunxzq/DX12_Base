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
};
