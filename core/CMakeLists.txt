cmake_minimum_required(VERSION 3.20)
project(stereo_core LANGUAGES CXX)

# 跨平台立体匹配核心：Windows (MSVC) / Linux (gcc, clang) / iOS (clang)
# 目标：零第三方依赖，零 Apple 依赖，先把标量版跑对再谈 SIMD。

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(stereo_core STATIC
    src/sgm.cpp
)
target_include_directories(stereo_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if (MSVC)
    # 逐位一致的前提：不合并 FMA、不用快速数学
    # /utf-8：源码是 UTF-8 无 BOM（clang/gcc 的默认），MSVC 在中文 locale 下
    # 默认按 GBK 读，会把中文注释按多字节解析并吞掉下一行
    target_compile_options(stereo_core PRIVATE /W4 /fp:precise /utf-8)
else()
    target_compile_options(stereo_core PRIVATE -Wall -Wextra -ffp-contract=off)
endif()

add_executable(golden_test tests/golden_main.cpp)
target_link_libraries(golden_test PRIVATE stereo_core)
if (MSVC)
    target_compile_options(golden_test PRIVATE /utf-8)
endif()

# 让 `ctest` / `cmake --build . --target golden_test` 之后能直接跑对拍
enable_testing()
add_test(NAME golden COMMAND golden_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/golden)
