cmake_minimum_required(VERSION 3.14)

# ========================================================================
# vcpkg 工具链配置 (必须在 project() 之前)
# ========================================================================
# 如果环境变量中有 VCPKG_ROOT，则使用它
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()

# vcpkg overlay ports：assimp FBX 导出器补丁（EmissiveColor/EmissiveFactor，
# 修复 X→FBX 链路发光材质丢失）。见 vcpkg-overlays/assimp/fbx_emissive_export.patch
if(NOT DEFINED VCPKG_OVERLAY_PORTS)
    set(VCPKG_OVERLAY_PORTS "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg-overlays" CACHE STRING "" FORCE)
endif()

project(DX12_Base LANGUAGES CXX)


add_compile_options("/utf-8")



# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 1. 设置输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Bin/x64/${CMAKE_BUILD_TYPE})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Lib/x64/${CMAKE_BUILD_TYPE})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Lib/x64/${CMAKE_BUILD_TYPE})

# ========================================================================
# 2. 查找第三方依赖 (通过 vcpkg)
# ========================================================================
find_package(nlohmann_json CONFIG REQUIRED)
find_package(concurrentqueue CONFIG REQUIRED)
find_package(EnTT CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(assimp CONFIG REQUIRED)
find_package(mimalloc REQUIRED)
find_package(Jolt CONFIG REQUIRED)
find_package(Taskflow CONFIG REQUIRED)
find_package(GameNetworkingSockets CONFIG REQUIRED)
find_package(protobuf CONFIG REQUIRED)
find_package(Stb REQUIRED)

# ========================================================================
# 3. 拆分引擎模块 (Static Libraries)
#    按目录划分，改一个模块不影响其他模块的重编译
# ========================================================================

set(ENGINE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Engine)
set(FIDELITYFX_DIR ${ENGINE_DIR}/ThirdParty/FidelityFX)

# FidelityFX 排他规则（所有模块共用）
set(FX_EXCLUDES
    ".*/FidelityFX/framegeneration/.*"
    ".*/ffx_fsr2.*" ".*/ffx_provider_fsr2.*"
    ".*/ffx_frameinterpolation.*" ".*/ffx_opticalflow.*"
    ".*/ffx_fsr3upscaler_shaderblobs.cpp" ".*/ffx_fsr3upscaler.cpp"
    ".*/ffx_provider_fsr3upscale.cpp" ".*/ffx_provider_registry.cpp"
    ".*/ffx_api.cpp"
)

macro(filter_fx SOURCES_LIST)
    foreach(pattern ${FX_EXCLUDES})
        list(FILTER ${SOURCES_LIST} EXCLUDE REGEX "${pattern}")
    endforeach()
endmacro()

# ========================================================================
# 3.1a DX12ECS — ECS 核心层（分离为 DLL，减少 ECS 改动牵连）
# ========================================================================
file(GLOB_RECURSE ECS_SOURCES
    "${ENGINE_DIR}/ECS/*.cpp"
    "${ENGINE_DIR}/ECS/*.h"
)

add_library(DX12ECS SHARED ${ECS_SOURCES})

target_include_directories(DX12ECS PUBLIC
    ${ENGINE_DIR}
)

target_compile_definitions(DX12ECS PRIVATE DX12ECS_BUILD)

target_compile_options(DX12ECS PRIVATE /MP)

target_link_libraries(DX12ECS PUBLIC
    EnTT::EnTT
    spdlog::spdlog
)

set_target_properties(DX12ECS PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Bin/x64/Debug/Editor"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Bin/x64/Release/Editor"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/Bin/x64/RelWithDebInfo/Editor"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZELER "${CMAKE_BINARY_DIR}/Bin/x64/MinSizeRel/Editor"
)

# ========================================================================
# 3.1 DX12Core — 稳定基础层（Common/Core/Event/Math/Platform/Scene）
# ========================================================================
file(GLOB_RECURSE CORE_SOURCES
    "${ENGINE_DIR}/Common/*.cpp"    "${ENGINE_DIR}/Common/*.h"
    "${ENGINE_DIR}/Boot/*.cpp"      "${ENGINE_DIR}/Boot/*.h"
    "${ENGINE_DIR}/Core/*.cpp"      "${ENGINE_DIR}/Core/*.h"
    "${ENGINE_DIR}/Event/*.cpp"     "${ENGINE_DIR}/Event/*.h"
    "${ENGINE_DIR}/Framework/*.cpp" "${ENGINE_DIR}/Framework/*.h"
    "${ENGINE_DIR}/Logger/*.cpp"    "${ENGINE_DIR}/Logger/*.h"
    "${ENGINE_DIR}/Math/*.cpp"      "${ENGINE_DIR}/Math/*.h"
    "${ENGINE_DIR}/Platform/*.cpp"  "${ENGINE_DIR}/Platform/*.h"
    "${ENGINE_DIR}/Scene/*.cpp"     "${ENGINE_DIR}/Scene/*.h"
    "${ENGINE_DIR}/Scheduler/*.cpp" "${ENGINE_DIR}/Scheduler/*.h"
)

add_library(DX12Core STATIC ${CORE_SOURCES})

target_include_directories(DX12Core PUBLIC
    ${ENGINE_DIR}
    ${ENGINE_DIR}/ThirdParty/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/Game/Application
)

target_compile_definitions(DX12Core PUBLIC
    UNICODE _UNICODE
    MI_OVERRIDE=ON
    IMGUI_DEFINE_MATH_OPERATORS
)

target_compile_options(DX12Core PRIVATE /MP)
target_precompile_headers(DX12Core PRIVATE "${ENGINE_DIR}/Common/Common.h")

target_link_libraries(DX12Core PUBLIC
    DX12ECS
    nlohmann_json::nlohmann_json
    concurrentqueue::concurrentqueue
    spdlog::spdlog
    Jolt::Jolt
    mimalloc-static
    GameNetworkingSockets::static
    protobuf::libprotobuf
)

# ========================================================================
# 3.2 DX12Resource — 资源管理（GPU资源/资产加载/格式定义）
# ========================================================================
file(GLOB_RECURSE RESOURCE_SOURCES
    "${ENGINE_DIR}/Resource/*.cpp" "${ENGINE_DIR}/Resource/*.h"
    "${ENGINE_DIR}/Asset/Definitions/*.cpp" "${ENGINE_DIR}/Asset/Definitions/*.h"
    "${ENGINE_DIR}/Asset/IO/*.cpp" "${ENGINE_DIR}/Asset/IO/*.h"
)

add_library(DX12Resource STATIC ${RESOURCE_SOURCES})

target_include_directories(DX12Resource PUBLIC
    ${ENGINE_DIR}
)

target_compile_definitions(DX12Resource PUBLIC
    UNICODE _UNICODE
    MI_OVERRIDE=ON
)

target_compile_options(DX12Resource PRIVATE /MP)
target_precompile_headers(DX12Resource REUSE_FROM DX12Core)

target_link_libraries(DX12Resource PUBLIC
    DX12Core
    d3d12.lib dxgi.lib dxguid.lib d3dcompiler.lib
)

# ========================================================================
# 3.3 DX12Renderer — 渲染管线（最大模块，改动最频繁）
# ========================================================================
file(GLOB_RECURSE RENDERER_SOURCES
    "${ENGINE_DIR}/Renderer/*.cpp" "${ENGINE_DIR}/Renderer/*.h"
    "${ENGINE_DIR}/DebugUI/*.cpp"  "${ENGINE_DIR}/DebugUI/*.h"
    # FidelityFX 源码（与渲染器绑定的第三方库）
    "${ENGINE_DIR}/ThirdParty/FidelityFX/**/*.cpp"
    "${ENGINE_DIR}/ThirdParty/FidelityFX/**/*.h"
    # imgui 源码
    "${ENGINE_DIR}/ThirdParty/imgui/*.cpp"
    "${ENGINE_DIR}/ThirdParty/imgui/*.h"
)

filter_fx(RENDERER_SOURCES)

add_library(DX12Renderer STATIC ${RENDERER_SOURCES})

target_include_directories(DX12Renderer PUBLIC
    ${ENGINE_DIR}
    ${ENGINE_DIR}/ThirdParty/imgui
    ${FIDELITYFX_DIR}/api/include
    ${FIDELITYFX_DIR}/api/include/dx12
    ${FIDELITYFX_DIR}/api/internal
    ${FIDELITYFX_DIR}/backend/dx12
    ${FIDELITYFX_DIR}/upscalers/include
    ${FIDELITYFX_DIR}/upscalers/fsr3/include
    ${FIDELITYFX_DIR}/upscalers/fsr3/internal
)

target_compile_definitions(DX12Renderer PUBLIC
    UNICODE _UNICODE
    MI_OVERRIDE=ON
    IMGUI_DEFINE_MATH_OPERATORS
    FFX_API_ENABLE_DX12=1
)

target_compile_options(DX12Renderer PRIVATE /MP)
target_precompile_headers(DX12Renderer REUSE_FROM DX12Core)

target_link_libraries(DX12Renderer PUBLIC
    DX12Resource
    DX12Core
    ${FIDELITYFX_DIR}/signedbin/amd_fidelityfx_loader_dx12.lib
)

# ========================================================================
# 3.4 DX12Background — 后台异步任务
# ========================================================================
file(GLOB_RECURSE BACKGROUND_SOURCES
    "${ENGINE_DIR}/Background/*.cpp" "${ENGINE_DIR}/Background/*.h"
)

add_library(DX12Background STATIC ${BACKGROUND_SOURCES})

target_include_directories(DX12Background PUBLIC
    ${ENGINE_DIR}
)

target_compile_definitions(DX12Background PUBLIC
    UNICODE _UNICODE
    MI_OVERRIDE=ON
)

target_compile_options(DX12Background PRIVATE /MP)
target_precompile_headers(DX12Background REUSE_FROM DX12Core)

target_link_libraries(DX12Background PUBLIC
    DX12Resource
    DX12Core
    Taskflow::Taskflow
)

# ========================================================================
# 3.5 DX12Network — 网络模块
# ========================================================================
file(GLOB_RECURSE NETWORK_SOURCES
    "${ENGINE_DIR}/Network/*.cpp" "${ENGINE_DIR}/Network/*.h"
)

add_library(DX12Network STATIC ${NETWORK_SOURCES})

target_include_directories(DX12Network PUBLIC
    ${ENGINE_DIR}
)

target_compile_definitions(DX12Network PUBLIC
    UNICODE _UNICODE
    MI_OVERRIDE=ON
    STEAMNETWORKINGSOCKETS_STATIC_LINK
)

target_compile_options(DX12Network PRIVATE /MP)
target_precompile_headers(DX12Network REUSE_FROM DX12Core)

target_link_libraries(DX12Network PUBLIC
    DX12Core
    GameNetworkingSockets::static
    protobuf::libprotobuf
)



# ========================================================================
# 4. 构建主程序 (Executable)
# ========================================================================
# 收集 Runtime 目录下的源文件
file(GLOB_RECURSE GAME_SOURCES 
   "${CMAKE_CURRENT_SOURCE_DIR}/Game/**/*.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/Game/**/*.h"
)



set(GAME_RESOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Game/Application/DX12_Base.rc")

# 【关键修改】添加 WIN32 关键字
# 这告诉链接器这是一个 Windows GUI 程序，入口点是 wWinMain/WinMain，而不是 main
add_executable(DX12_Game WIN32 ${GAME_SOURCES} ${GAME_RESOURCES})





# 主程序编译定义
# 【关键】MI_OVERRIDE=ON 必须定义在最终可执行文件目标上
# 静态库不保留宏定义，会导致 mimalloc 无法替换全局 new/malloc
target_compile_definitions(DX12_Game PRIVATE
    UNICODE
    _UNICODE
    MI_OVERRIDE=ON
)

# 【新增优化 3】为主程序也启用 /MP 和 PCH
# 虽然主程序代码较少，但保持一致性并加速 Runtime 部分的编译
target_compile_options(DX12_Game PRIVATE /MP)
target_precompile_headers(DX12_Game REUSE_FROM DX12Core)

set_source_files_properties(${GAME_RESOURCES} PROPERTIES
    RC_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/Game/Application"
)

# 主程序链接所有引擎模块库（链接顺序：底层在前）
target_link_libraries(DX12_Game PRIVATE
    DX12Renderer
    DX12Background
    DX12Network
    DX12Resource
    DX12Core
)

# 设置独立输出目录
set_target_properties(DX12_Game PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Bin/x64/Debug/Game"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Bin/x64/Release/Game"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/Bin/x64/RelWithDebInfo/Game"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/Bin/x64/MinSizeRel/Game"
)

# 设置包含目录
target_include_directories(DX12_Game PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/Game/Game
    ${CMAKE_CURRENT_SOURCE_DIR}/Game/Application
)


# 【建议】显式链接 Windows 基础库
target_link_libraries(DX12_Game PRIVATE
    user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib
)


# FSR 和第三方依赖 DLL 由 vcpkg 和 FidelityFX 自动复制，无需额外 POST_BUILD
# Content/Config/Shaders 通过 .dxproject.ini 在运行时动态定位，不复制的输出目录

# 复制 FSR DLL 到输出目录（仅当 DLL 存在于 signedbin 时）
if(EXISTS "${FIDELITYFX_DIR}/signedbin/amd_fidelityfx_loader_dx12.dll")
    add_custom_command(TARGET DX12_Game POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${FIDELITYFX_DIR}/signedbin/amd_fidelityfx_loader_dx12.dll
            $<TARGET_FILE_DIR:DX12_Game>
        COMMENT "Copying FSR loader DLL..."
    )
endif()
# 生成 ProjectConfig 头文件（编译时常量，替代运行时 ProjectConfig.ini）
set(PROJECT_TYPE "game")
set(PROJECT_CONFIG_ROOT "Game/Config")
configure_file(
    "${CMAKE_SOURCE_DIR}/Engine/Core/ProjectConfigGenerated.h.in"
    "${CMAKE_BINARY_DIR}/Generated/Game/ProjectConfigGenerated.h"
)

target_include_directories(DX12_Game PRIVATE
    "${CMAKE_BINARY_DIR}/Generated/Game"
)

# 复制 Game 配置文件到输出目录
add_custom_command(TARGET DX12_Game POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/Game/Config
    $<TARGET_FILE_DIR:DX12_Game>/Config
)

# ========================================================================
# 5. 构建编辑器
# ========================================================================

# 5.1 DX12EditorLib — 编辑器专用逻辑库
file(GLOB_RECURSE EDITORLIB_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Editor/EditorLib/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Editor/EditorLib/*.h"
)

# 显式添加新文件（GLOB_RECURSE 在 configure 时求值，新增文件需手动加入直到下次 reconfigure）
list(APPEND EDITORLIB_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Editor/EditorLib/Properties/Editors/TransformEditor.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Editor/EditorLib/Properties/Editors/LightEditor.cpp"
)

add_library(DX12EditorLib STATIC ${EDITORLIB_SOURCES})

target_include_directories(DX12EditorLib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/Editor/EditorLib/Core
    ${CMAKE_CURRENT_SOURCE_DIR}/Editor/EditorLib/Panels
    ${CMAKE_CURRENT_SOURCE_DIR}/Editor/EditorLib
    ${CMAKE_CURRENT_SOURCE_DIR}/Editor/EditorLib/Input
    ${CMAKE_CURRENT_SOURCE_DIR}/Editor/EditorLib/Scene
    ${CMAKE_CURRENT_SOURCE_DIR}/Editor/EditorLib/Log
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/ThirdParty/imguizmo
)

target_compile_definitions(DX12EditorLib PUBLIC
    UNICODE _UNICODE
    MI_OVERRIDE=ON
    IMGUI_DEFINE_MATH_OPERATORS
    WITH_EDITOR
)

target_compile_options(DX12EditorLib PRIVATE /MP)
target_precompile_headers(DX12EditorLib REUSE_FROM DX12Core)

target_link_libraries(DX12EditorLib PUBLIC
    DX12Renderer
    DX12Background
    DX12Resource
    DX12Core
)

# 5.2 DX12_Editor 主程序
file(GLOB_RECURSE EDITOR_APP_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Editor/Application/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Editor/Application/*.h"
)

set(EDITOR_RESOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Editor/Application/DX12_Editor.rc")

add_executable(DX12_Editor WIN32 ${EDITOR_APP_SOURCES} ${EDITOR_RESOURCES}
    # ImGuizmo 源码编译（仅 Editor 可执行文件使用，不污染 Engine Core）
    "${CMAKE_CURRENT_SOURCE_DIR}/Engine/ThirdParty/imguizmo/ImGuizmo.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Engine/ThirdParty/imguizmo/ImSequencer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Engine/ThirdParty/imguizmo/ImCurveEdit.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Engine/ThirdParty/imguizmo/ImGradient.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Engine/ThirdParty/imguizmo/GraphEditor.cpp"
)

target_compile_definitions(DX12_Editor PRIVATE
    UNICODE _UNICODE
    MI_OVERRIDE=ON
    WITH_EDITOR
)

target_compile_options(DX12_Editor PRIVATE /MP)
target_precompile_headers(DX12_Editor REUSE_FROM DX12Core)

set_source_files_properties(${EDITOR_RESOURCES} PROPERTIES
    RC_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/Editor/Application"
)

# 生成 ProjectConfig 头文件（编译时常量，替代运行时 ProjectConfig.ini）
set(PROJECT_TYPE "editor")
set(PROJECT_CONFIG_ROOT "Editor/Config")
configure_file(
    "${CMAKE_SOURCE_DIR}/Engine/Core/ProjectConfigGenerated.h.in"
    "${CMAKE_BINARY_DIR}/Generated/Editor/ProjectConfigGenerated.h"
)

target_include_directories(DX12_Editor PRIVATE
    "${CMAKE_BINARY_DIR}/Generated/Editor"
    ${CMAKE_CURRENT_SOURCE_DIR}/Editor/Application
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/ThirdParty/imguizmo
)

# 复制 Editor 配置文件到输出目录
add_custom_command(TARGET DX12_Editor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/Editor/Config
    $<TARGET_FILE_DIR:DX12_Editor>/Config
)

# 复制 Shaders 到输出目录
add_custom_command(TARGET DX12_Editor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/Shaders
    $<TARGET_FILE_DIR:DX12_Editor>/Shaders
)

set_target_properties(DX12_Editor PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Bin/x64/Debug/Editor"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Bin/x64/Release/Editor"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/Bin/x64/RelWithDebInfo/Editor"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/Bin/x64/MinSizeRel/Editor"
)

target_link_libraries(DX12_Editor PRIVATE
    DX12EditorLib
    DX12Renderer
    DX12Background
    DX12Resource
    DX12Core
    user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib
)
if(EXISTS "${FIDELITYFX_DIR}/signedbin/amd_fidelityfx_upscaler_dx12.dll")
    add_custom_command(TARGET DX12_Game POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${FIDELITYFX_DIR}/signedbin/amd_fidelityfx_upscaler_dx12.dll
            $<TARGET_FILE_DIR:DX12_Game>
        COMMENT "Copying FSR upscaler DLL..."
    )
endif()


# ========================================================================
# 5. 集成 Google Test (通过 vcpkg) - 默认启用测试
# ========================================================================
option(BUILD_TESTS "Build test programs" ON)

if(BUILD_TESTS)
    find_package(GTest CONFIG REQUIRED)

    # ========================================================================
    # 6. 构建测试程序
    # ========================================================================
    enable_testing()

    # 收集 Tests 目录下的测试源文件
    file(GLOB_RECURSE TEST_SOURCES 
        "${CMAKE_CURRENT_SOURCE_DIR}/Tests/*.cpp"
    )

    if(TEST_SOURCES)
        add_executable(DX12Tests ${TEST_SOURCES})

        # 链接 GTest 主库 和 引擎模块
        target_link_libraries(DX12Tests PRIVATE 
            GTest::gtest_main 
            DX12Core DX12Resource
        )

        # 自动发现测试用例，以便 VS Test Explorer 识别
        include(GoogleTest)
        gtest_discover_tests(DX12Tests)
    endif()
    endif()

# ========================================================================
# 7. 构建网络测试程序 (Standalone Executable)
# ========================================================================
# 注意：NetworkTest.cpp 有独立的 main 函数，必须单独编译为一个 exe

add_executable(NetworkTest 
    "${CMAKE_CURRENT_SOURCE_DIR}/Tests/NetworkTest.cpp"
)

# 复用引擎核心的预编译头以加速编译
target_precompile_headers(NetworkTest REUSE_FROM DX12Core)

# 链接引擎模块库 (Network 已依赖 DX12Core)
target_link_libraries(NetworkTest PRIVATE 
    DX12Network DX12Core
)

# 设置包含目录，确保能找到 "Network/..." 头文件
target_include_directories(NetworkTest PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine
    ${CMAKE_CURRENT_SOURCE_DIR}/Game
)

# 复制 Content/Config 等到 NetworkTest 的输出目录，确保运行时能找到资源（如果需要）
add_custom_command(TARGET NetworkTest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/Content
    $<TARGET_FILE_DIR:NetworkTest>/Content
)

add_custom_command(TARGET NetworkTest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/Config
    $<TARGET_FILE_DIR:NetworkTest>/Config
)


# 添加网络测试程序
add_executable(NetworkP2PTest 
    "${CMAKE_CURRENT_SOURCE_DIR}/Tests/NetworkP2PTest.cpp"
)

target_link_libraries(NetworkP2PTest PRIVATE 
    DX12Network DX12Core
)

target_include_directories(NetworkP2PTest PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine
    ${CMAKE_CURRENT_SOURCE_DIR}/Game
)

# ========================================================================
# 8. 构建 AssetTool — UKW PowerUp Kit 离线资产转换工具
#
# 目录结构：
#   Core/   — 共享解析器 + RobotMerger 合并核心
#   CLI/    — 控制台版本入口
#   GUI/    — Win32 图形界面版本
# ========================================================================

set(ASSET_TOOL_SHARED_SOURCES
    # Core: 解析器
    "${CMAKE_CURRENT_SOURCE_DIR}/AssetTool/Core/XORCipher.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AssetTool/Core/XORCipher.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/AssetTool/Core/XFileParser.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AssetTool/Core/XFileParser.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/AssetTool/Core/XFileDirectReader.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AssetTool/Core/XFileDirectReader.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/AssetTool/Core/ObjParser.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AssetTool/Core/ObjParser.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/AssetTool/Core/HODParser.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AssetTool/Core/HODParser.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/AssetTool/Core/ANIParser.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AssetTool/Core/ANIParser.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/AssetTool/Core/MPDParser.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AssetTool/Core/MPDParser.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/AssetTool/Core/MPDSceneParser.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AssetTool/Core/MPDSceneParser.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/AssetTool/Core/ScriptSPTParser.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AssetTool/Core/ScriptSPTParser.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/AssetTool/Core/MapSceneConverter.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AssetTool/Core/MapSceneConverter.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/AssetTool/Core/TextureConverter.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AssetTool/Core/TextureConverter.h"
    # Core: 合并核心
    "${CMAKE_CURRENT_SOURCE_DIR}/AssetTool/Core/RobotMerger.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AssetTool/Core/RobotMerger.h"
    # Core: 合并核心拆分（2026-08-01 适度拆分，缓解 C1060 编译堆不足）
    "${CMAKE_CURRENT_SOURCE_DIR}/AssetTool/Core/RobotMergerMath.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AssetTool/Core/RobotMergerUtil.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AssetTool/Core/RobotMergerUtil.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/AssetTool/Core/RobotMergerFBX.cpp"
    # RobotMergerX.cpp（每帧 .x 导出）已废弃并删除（2026-08-01 用户定案：不输出 x，FBX 为主载体）
    # Core: IK（B 方案：FABRIK 离线验证）
    "${CMAKE_CURRENT_SOURCE_DIR}/AssetTool/Core/IKSolver.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AssetTool/Core/IKSolver.h"
    # Core: FBX → 引擎资产（fbxs2dxmesh，2026-08-01 路线定案：FBX 唯一来源）
    "${CMAKE_CURRENT_SOURCE_DIR}/AssetTool/Core/FbxMeshConverter.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AssetTool/Core/FbxMeshConverter.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/AssetTool/Core/AnimClipConverter.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AssetTool/Core/AnimClipConverter.h"
)

# ── AssetTool (CLI) ──
add_executable(AssetTool
    ${ASSET_TOOL_SHARED_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/AssetTool/CLI/main.cpp"
)

target_include_directories(AssetTool PRIVATE ${CMAKE_SOURCE_DIR}/Engine ${Stb_INCLUDE_DIR}
    "${CMAKE_CURRENT_SOURCE_DIR}/AssetTool")
target_link_libraries(AssetTool PRIVATE DX12Resource DX12Core nlohmann_json::nlohmann_json assimp::assimp)
target_compile_options(AssetTool PRIVATE /MP)
target_compile_definitions(AssetTool PRIVATE UNICODE _UNICODE)

set_target_properties(AssetTool PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_DEBUG    "${CMAKE_BINARY_DIR}/Bin/x64/Debug/AssetTool"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE  "${CMAKE_BINARY_DIR}/Bin/x64/Release/AssetTool"
)

# ── AssetToolGUI (Win32) ──
add_executable(AssetToolGUI WIN32
    ${ASSET_TOOL_SHARED_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/AssetTool/GUI/AssetToolGUI.cpp"
)

target_include_directories(AssetToolGUI PRIVATE ${CMAKE_SOURCE_DIR}/Engine ${Stb_INCLUDE_DIR}
    "${CMAKE_CURRENT_SOURCE_DIR}/AssetTool")
target_link_libraries(AssetToolGUI PRIVATE
    DX12Resource DX12Core nlohmann_json::nlohmann_json assimp::assimp
    comctl32.lib ole32.lib shlwapi.lib
)
target_compile_options(AssetToolGUI PRIVATE /MP)
target_compile_definitions(AssetToolGUI PRIVATE UNICODE _UNICODE)

set_target_properties(AssetToolGUI PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_DEBUG    "${CMAKE_BINARY_DIR}/Bin/x64/Debug/AssetTool"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE  "${CMAKE_BINARY_DIR}/Bin/x64/Release/AssetTool"
)