#include "ShaderUtils.h"
#include <d3dcompiler.h>
#include <filesystem>

namespace DX12Engine::Renderer {

namespace {
std::string s_shaderRoot;
}

void SetShaderRoot(const std::string &root) { s_shaderRoot = root; }

const std::string &GetShaderRoot() { return s_shaderRoot; }

HRESULT CompileShaderFromFile(const std::wstring &fileName, const std::string &entryPoint, const std::string &target,
                              UINT flags, Microsoft::WRL::ComPtr<ID3DBlob> &outBlob,
                              Microsoft::WRL::ComPtr<ID3DBlob> *outErrors) {

    // 基于 ShaderRoot 解析完整路径
    std::wstring fullPath = std::filesystem::path(std::wstring(s_shaderRoot.begin(), s_shaderRoot.end())) / fileName;

    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompileFromFile(fullPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entryPoint.c_str(),
                                    target.c_str(), flags, 0, outBlob.GetAddressOf(), errors.GetAddressOf());

    if (FAILED(hr)) {
        if (errors) {
            const char *errMsg = reinterpret_cast<const char *>(errors->GetBufferPointer());
            OutputDebugStringA("\n=== SHADER COMPILATION ERROR ===\n");
            OutputDebugStringA(errMsg);
            OutputDebugStringA("===============================\n");
        }
        if (outErrors) {
            *outErrors = std::move(errors);
        }
    }

    return hr;
}

} // namespace DX12Engine::Renderer
