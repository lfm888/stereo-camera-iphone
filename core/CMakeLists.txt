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
    src/stereo_api.cpp
)
target_include_directories(stereo_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# C 接口的单独检查：确认 C 头在纯 C 编译器下也能用
enable_language(C)
add_executable(c_api_smoke tests/c_api_smoke.c)
target_link_libraries(c_api_smoke PRIVATE stereo_core)

if (MSVC)
    # 逐位一致的前提：不合并 FMA、不用快速数学
    target_compile_options(stereo_core PRIVATE /W4 /fp:precise)
else()
    target_compile_options(stereo_core PRIVATE -Wall -Wextra -ffp-contract=off)
endif()

add_executable(golden_test tests/golden_main.cpp)
target_link_libraries(golden_test PRIVATE stereo_core)

# 让 `ctest` / `cmake --build . --target golden_test` 之后能直接跑对拍
enable_testing()
add_test(NAME golden COMMAND golden_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/golden)
add_test(NAME c_api COMMAND c_api_smoke)
