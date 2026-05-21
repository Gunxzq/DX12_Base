cmake_minimum_required(VERSION 3.14)

# ========================================================================
# vcpkg 工具链配置 (必须在 project() 之前)
# ========================================================================
# 如果环境变量中有 VCPKG_ROOT，则使用它
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
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

# ========================================================================
# 3. 构建引擎核心库 (Static Library)
# ========================================================================
# 收集 Engine 目录下的所有源文件和头文件
file(GLOB_RECURSE ENGINE_SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/Engine/**/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Engine/**/*.h"
)

# 创建静态库
add_library(DX12EngineCore STATIC ${ENGINE_SOURCES})

# 设置包含目录 (PUBLIC 使得链接此库的目标也能找到头文件)
target_include_directories(DX12EngineCore PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine
    ${CMAKE_CURRENT_SOURCE_DIR}/Content
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/ThirdParty/imgui
)


# 确保引擎内部的所有代码都使用 mimalloc 的重载操作符
target_compile_definitions(DX12EngineCore PRIVATE
    UNICODE
    _UNICODE
    MI_OVERRIDE=ON
    IMGUI_DEFINE_MATH_OPERATORS   
)


# 允许编译器同时使用多个核心进行编译，显著加快大型项目的构建速度
target_compile_options(DX12EngineCore PRIVATE /MP)


# 指定 Common.h 为预编译头，因为它是引擎中最基础且被广泛包含的头文件
# 这将大幅减少每个 cpp文件重复解析 Windows.h, DirectXMath, STL 等重型头文件的时间
target_precompile_headers(DX12EngineCore PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/Engine/Common/Common.h"
)

# 链接依赖库
target_link_libraries(DX12EngineCore PUBLIC 
    nlohmann_json::nlohmann_json
    concurrentqueue::concurrentqueue
    EnTT::EnTT
    spdlog::spdlog
    d3d12.lib
    dxgi.lib
    dxguid.lib
    d3dcompiler.lib
    assimp::assimp
    mimalloc-static
    Jolt::Jolt
    Taskflow::Taskflow
    GameNetworkingSockets::static
    protobuf::libprotobuf
)


# 如果只在 Runtime 使用，也可以只加在 Runtime 上
target_compile_definitions(DX12EngineCore PUBLIC
    STEAMNETWORKINGSOCKETS_STATIC_LINK
)

# ========================================================================
# 4. 构建主程序 (Executable)
# ========================================================================
# 收集 Runtime 目录下的源文件
file(GLOB_RECURSE RUNTIME_SOURCES 
   "${CMAKE_CURRENT_SOURCE_DIR}/Runtime/**/*.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/Runtime/**/*.h"
)



set(RUNTIME_RESOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Content/DX12_Base.rc")

# 【关键修改】添加 WIN32 关键字
# 这告诉链接器这是一个 Windows GUI 程序，入口点是 wWinMain/WinMain，而不是 main
add_executable(${PROJECT_NAME} WIN32 ${RUNTIME_SOURCES} ${RUNTIME_RESOURCES})





# 主程序编译定义
# 【关键】MI_OVERRIDE=ON 必须定义在最终可执行文件目标上
# 静态库不保留宏定义，会导致 mimalloc 无法替换全局 new/malloc
target_compile_definitions(${PROJECT_NAME} PRIVATE
    UNICODE
    _UNICODE
    MI_OVERRIDE=ON
)

# 【新增优化 3】为主程序也启用 /MP 和 PCH
# 虽然主程序代码较少，但保持一致性并加速 Runtime 部分的编译
target_compile_options(${PROJECT_NAME} PRIVATE /MP)
target_precompile_headers(${PROJECT_NAME} REUSE_FROM DX12EngineCore)

# 引擎库编译定义 (与 EXE 保持一致，确保 mimalloc 正确注入)
target_compile_definitions(DX12EngineCore PRIVATE
    UNICODE
    _UNICODE
    MI_OVERRIDE=ON
)


set_source_files_properties(${RUNTIME_RESOURCES} PROPERTIES 
    RC_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/Content"
)

# 主程序链接引擎核心库
target_link_libraries(${PROJECT_NAME} PRIVATE 
    DX12EngineCore
)

# 设置包含目录
target_include_directories(${PROJECT_NAME} PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/Runtime  
)


# 【建议】显式链接 Windows 基础库，防止某些环境下链接缺失
target_link_libraries(${PROJECT_NAME} PRIVATE
    user32.lib
    gdi32.lib
    shell32.lib
    ole32.lib
    oleaut32.lib
    uuid.lib
)

# 为 RC 编译器添加包含目录，使其能找到 resource.h 和 targetver.h
target_include_directories(${PROJECT_NAME} PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/Content
)

# 复制 Content 文件夹到输出目录
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/Content
    $<TARGET_FILE_DIR:${PROJECT_NAME}>/Content
)

# 复制 Config 文件夹到输出目录
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/Config
    $<TARGET_FILE_DIR:${PROJECT_NAME}>/Config
)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/Shaders
    $<TARGET_FILE_DIR:${PROJECT_NAME}>/Shaders
)

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

        # 链接 GTest 主库 和 引擎核心库
        target_link_libraries(DX12Tests PRIVATE 
            GTest::gtest_main 
            DX12EngineCore
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
    "${CMAKE_CURRENT_SOURCE_DIR}/Runtime/NetworkTest.cpp"
)

# 复用引擎核心的预编译头以加速编译
target_precompile_headers(NetworkTest REUSE_FROM DX12EngineCore)

# 链接引擎核心库 (其中已包含 GameNetworkingSockets 等依赖)
target_link_libraries(NetworkTest PRIVATE 
    DX12EngineCore
)

# 设置包含目录，确保能找到 "Network/..." 头文件
target_include_directories(NetworkTest PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine
    ${CMAKE_CURRENT_SOURCE_DIR}/Runtime
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