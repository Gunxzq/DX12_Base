#pragma once
#include <d3d11.h>
#include <d3dcompiler.h>
#include <string>
#include <wrl/client.h>

namespace DX12Engine::Renderer {

// ========================================================================
// ShaderUtils — 着色器编译工具

// 设置着色器根目录（由 Bootstrap 在初始化时调用一次，仅开发模式）
void SetShaderRoot(const std::string &root);

/// 获取着色器根目录
const std::string &GetShaderRoot();

/**
 * @brief 从文件编译着色器
 * @param fileName 着色器文件名
 * @param entryPoint 入口点
 * @param target 目标
 * @param flags 编译标志
 * @param outBlob 输出着色器二进制
 * @param outErrors 输出错误信息
 * @return HRESULT 编译结果码
 * @date 2026-07-07
 */
HRESULT CompileShaderFromFile(const std::wstring &fileName, const std::string &entryPoint, const std::string &target,
                              UINT flags, Microsoft::WRL::ComPtr<ID3DBlob> &outBlob,
                              Microsoft::WRL::ComPtr<ID3DBlob> *outErrors = nullptr);

} // namespace DX12Engine::Renderer
