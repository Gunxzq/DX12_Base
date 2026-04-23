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
set(CMAKE_CXX_STANDARD 17)
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
find_package(imgui CONFIG REQUIRED)

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
    ${CMAKE_CURRENT_SOURCE_DIR}/Content
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
    assimp::assimp
    mimalloc-static
    Jolt::Jolt
    imgui::imgui
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

# 引擎库编译定义 (仅保留 UNICODE)
target_compile_definitions(DX12EngineCore PRIVATE
    UNICODE
    _UNICODE
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