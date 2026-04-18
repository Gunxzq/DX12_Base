cmake_minimum_required(VERSION 3.14)
project(DX12_Base LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 1. 设置输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Bin/x64/${CMAKE_BUILD_TYPE})

# 2. 查找依赖
find_package(nlohmann_json CONFIG REQUIRED)
find_package(DirectXTK12 CONFIG REQUIRED)
find_package(concurrentqueue CONFIG REQUIRED)
find_package(EnTT CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

# 3. 自动收集源文件
# 收集 Engine/Src 和 Runtime 目录下的所有 .cpp 和 .h 文件
file(GLOB_RECURSE PROJECT_SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/Engine/Src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Engine/Include/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Runtime/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Runtime/*.h"
)



# 定义可执行文件
add_executable(${PROJECT_NAME} ${PROJECT_SOURCES})

# 4. 包含目录
target_include_directories(${PROJECT_NAME} PRIVATE 
    # 推荐：将 Include 目录作为根，这样代码中可以写 #include "Core/Logger.h"
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Include
    
 
)

# 5. 链接库
# 5. 链接库
target_link_libraries(${PROJECT_NAME} PRIVATE 
    nlohmann_json::nlohmann_json
    Microsoft::DirectXTK12
    concurrentqueue::concurrentqueue
    EnTT::EnTT
    spdlog::spdlog
    d3d12.lib
    dxgi.lib
    dxguid.lib
)

# 6. 复制 Content 文件夹
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/Content
    $<TARGET_FILE_DIR:${PROJECT_NAME}>/Content
)