cmake_minimum_required(VERSION 3.20)
project(stereo_core LANGUAGES CXX)

# 跨平台立体匹配核心：Windows (MSVC) / Linux (gcc, clang) / iOS (clang)
# 目标：零第三方依赖，零 Apple 依赖，先把标量版跑对再谈 SIMD。

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (MSVC)
    # 源码是 UTF-8 无 BOM（clang/gcc 的默认），MSVC 在中文 locale 下默认按 GBK 读，
    # 会把中文注释按多字节解析——要么吞掉下一行，要么让字符串字面量变成“未闭合”。
    add_compile_options(/utf-8)
endif()

add_library(stereo_core STATIC
    src/sgm.cpp
    src/geometry.cpp
    src/rectify.cpp
    src/sample.cpp
    src/analyze.cpp
    src/cloud.cpp
    src/calibrate.cpp
    src/params.cpp
    src/stereo_c_api.cpp
)
target_include_directories(stereo_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# C 接口的单独检查：确认 C 头在纯 C 编译器下也能用
enable_language(C)
add_executable(c_api_smoke tests/c_api_smoke.c)
target_link_libraries(c_api_smoke PRIVATE stereo_core)

# 每个公开 C 头都必须**自包含**：生成一个只 include 它的 .c，用纯 C 编译器编一遍。
# 这是拆头（stereo_types.h / stereo_match.h / …）的回归保护：漏一个 #include 就当场失败。
set(STEREO_PUBLIC_HEADERS
    stereo_types.h
    stereo_context.h
    stereo_match.h
    stereo_rectify.h
    stereo_analyze.h
    stereo_export.h
    stereo_calibrate.h
    stereo_params.h
    stereo_pipeline.h
    stereo_c_api.h
)
set(STEREO_HEADER_SELFCHECK_SRCS)
foreach(hdr IN LISTS STEREO_PUBLIC_HEADERS)
    set(gen "${CMAKE_CURRENT_BINARY_DIR}/header_selfcheck/${hdr}.c")
    file(WRITE "${gen}" "#include <stereo/${hdr}>
")
    list(APPEND STEREO_HEADER_SELFCHECK_SRCS "${gen}")
endforeach()
add_library(stereo_header_selfcheck OBJECT ${STEREO_HEADER_SELFCHECK_SRCS})
target_include_directories(stereo_header_selfcheck PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
set_target_properties(stereo_header_selfcheck PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

if (MSVC)
    # 逐位一致的前提：不合并 FMA、不用快速数学
    target_compile_options(stereo_core PRIVATE /W4 /fp:precise)
else()
    target_compile_options(stereo_core PRIVATE -Wall -Wextra -ffp-contract=off)
endif()

add_executable(golden_test tests/golden_main.cpp)
target_link_libraries(golden_test PRIVATE stereo_core)

# 极线矫正 / 重采样 / 深度回转：不依赖外部金标准，用数学不变量与往返一致性检查。
# 这样在 Windows/Linux 上就能跑，不需要 Mac 或 Metal。
add_executable(geometry_test tests/geometry_test.cpp)
target_link_libraries(geometry_test PRIVATE stereo_core)

# 深度查询/分析：用独立写的朴素参考（暴力排序、逐块重建）对拍。
add_executable(analyze_test tests/analyze_test.cpp)
target_link_libraries(analyze_test PRIVATE stereo_core)

# 导出用的像素运算：点云 PLY / 深度裁剪 / 打包 / 拷贝。
add_executable(cloud_test tests/cloud_test.cpp)
target_link_libraries(cloud_test PRIVATE stereo_core)

# 自动标定搜索：注入已知的行偏移，看它能不能找回来。
add_executable(calibrate_test tests/calibrate_test.cpp)
target_link_libraries(calibrate_test PRIVATE stereo_core)

# 第 2..5 步的 CPU 全流水线 + 参数描述表。
add_executable(frame_test tests/frame_test.cpp)
target_link_libraries(frame_test PRIVATE stereo_core)

# 让 `ctest` / `cmake --build . --target golden_test` 之后能直接跑对拍
enable_testing()
add_test(NAME golden COMMAND golden_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/golden)
add_test(NAME c_api COMMAND c_api_smoke)
add_test(NAME geometry COMMAND geometry_test)
add_test(NAME analyze COMMAND analyze_test)
add_test(NAME cloud COMMAND cloud_test)
add_test(NAME calibrate COMMAND calibrate_test)
add_test(NAME frame COMMAND frame_test)
