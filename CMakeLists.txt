cmake_minimum_required(VERSION 3.14)
project(DX12_Base LANGUAGES CXX)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 1. 设置输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Bin/x64/${CMAKE_BUILD_TYPE})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Lib/x64/${CMAKE_BUILD_TYPE})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Lib/x64/${CMAKE_BUILD_TYPE})

# 2. 查找第三方依赖 (通过 vcpkg)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(DirectXTK12 CONFIG REQUIRED)
find_package(concurrentqueue CONFIG REQUIRED)
find_package(EnTT CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

# ========================================================================
# 3. 构建引擎核心库 (Static Library)
# ========================================================================
# 收集 Engine 目录下的所有源文件和头文件
file(GLOB_RECURSE ENGINE_SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/Engine/Src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Engine/Include/*.h"
)

# 创建静态库
add_library(DX12EngineCore STATIC ${ENGINE_SOURCES})

# 设置包含目录 (PUBLIC 使得链接此库的目标也能找到头文件)
target_include_directories(DX12EngineCore PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Include
)

# 链接依赖库
target_link_libraries(DX12EngineCore PUBLIC 
    nlohmann_json::nlohmann_json
    Microsoft::DirectXTK12
    concurrentqueue::concurrentqueue
    EnTT::EnTT
    spdlog::spdlog
    d3d12.lib
    dxgi.lib
    dxguid.lib
)

# ========================================================================
# 4. 构建主程序 (Executable)
# ========================================================================
# 收集 Runtime 目录下的源文件
file(GLOB_RECURSE RUNTIME_SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/Runtime/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Runtime/*.h"
)

# 收集资源文件 (.rc)
# 注意：.rc 文件通常不需要 GLOB_RECURSE，直接指定即可，或者确保它在 RUNTIME_SOURCES 中被包含
# 这里我们显式添加 .rc 文件，确保它被编译
set(RUNTIME_RESOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Runtime/DX12_Base.rc")

# 【关键修改】添加 WIN32 关键字
# 这告诉链接器这是一个 Windows GUI 程序，入口点是 wWinMain/WinMain，而不是 main
add_executable(${PROJECT_NAME} WIN32 ${RUNTIME_SOURCES} ${RUNTIME_RESOURCES})

# 主程序链接引擎核心库
target_link_libraries(${PROJECT_NAME} PRIVATE 
    DX12EngineCore
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

# 复制 Content 文件夹到输出目录
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/Content
    $<TARGET_FILE_DIR:${PROJECT_NAME}>/Content
)

# ========================================================================
# 5. 集成 Google Test (FetchContent)
# ========================================================================
include(FetchContent)

# 声明 Google Test
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.14.0 # 使用稳定版本，可根据需要更新
)

# 对于 Windows，强制使用共享 CRT 以匹配默认项目设置
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

# 下载并配置 Google Test
FetchContent_MakeAvailable(googletest)

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