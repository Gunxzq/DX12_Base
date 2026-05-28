// ========== ThrowHelper.h ==========
#pragma once
#include "WindowsPlatform.h"
#include <comdef.h>
#include <iostream>
#include <sstream>

/**
 * @brief 将 HRESULT 转换为可读的错误字符串
 */
inline std::wstring HResultToString(HRESULT hr) {
    _com_error err(hr);
    return std::wstring(err.ErrorMessage());
}

/**
 * @brief D3D 异常类
 */
class DxException {
public:
    DxException() = default;
    DxException(HRESULT hr, const std::wstring &functionName, const std::wstring &filename, int lineNumber)
        : ErrorCode(hr), FunctionName(functionName), Filename(filename), LineNumber(lineNumber) {}

    std::wstring ToString() const {
        std::wstringstream ss;
        ss << L"[DX12 ERROR] " << FunctionName << L" failed at " << Filename << L":" << LineNumber
           << L"\nError Code: 0x" << std::hex << ErrorCode << L"\nMessage: " << HResultToString(ErrorCode);
        return ss.str();
    }

    HRESULT ErrorCode = S_OK;
    std::wstring FunctionName;
    std::wstring Filename;
    int LineNumber = -1;
};

/**
 * @brief COM 对象释放宏
 * 用法: ReleaseCom(ptr);
 */
#ifndef ReleaseCom
#define ReleaseCom(x)                                                                                                  \
    do {                                                                                                               \
        if (x) {                                                                                                       \
            x->Release();                                                                                              \
            x = nullptr;                                                                                               \
        }                                                                                                              \
    } while (0)
#endif

/**
 * @brief 检查 HRESULT 并在失败时抛出异常
 * 用法: ThrowIfFailed(device->CreateXXX(...));
 */
#ifndef ThrowIfFailed
#define ThrowIfFailed(x)                                                                                               \
    do {                                                                                                               \
        HRESULT hr__ = (x);                                                                                            \
        if (FAILED(hr__)) {                                                                                            \
            throw DxException(hr__, L#x, __FILEW__, __LINE__);                                                         \
        }                                                                                                              \
    } while (0)
#endif
