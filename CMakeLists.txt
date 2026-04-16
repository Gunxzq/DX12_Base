cmake_minimum_required(VERSION 3.14)
project(DX12_Base LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17) # 建议升级到 17
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 1. 设置输出目录 (让 exe 跑到 Build/Bin/x64/Debug 下，方便管理)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Bin/x64/${CMAKE_BUILD_TYPE})

# 2. 查找依赖
find_package(nlohmann_json CONFIG REQUIRED)

# 3. 定义可执行文件
add_executable(${PROJECT_NAME}
    Runtime/DX12_Base.cpp
    # 添加更多源文件...
)

# 4. 包含目录
# 链接 nlohmann_json 会自动包含它的头文件
# 这里添加你自己的公共头文件目录
target_include_directories(${PROJECT_NAME} PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/Include 
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine
)

# 5. 链接库
target_link_libraries(${PROJECT_NAME} PRIVATE 
    nlohmann_json::nlohmann_json
    d3d12.lib
    dxgi.lib
    dxguid.lib
)

# 6. (进阶) 如果你需要复制 Content 文件夹到输出目录
# 这样你的 exe 才能找到模型和贴图
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/Content
    $<TARGET_FILE_DIR:${PROJECT_NAME}>/Content
)