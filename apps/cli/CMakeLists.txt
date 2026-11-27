# apps/cli — 第二个前端（只 include 公开 C 头）
#
# 由仓库根的 CMakeLists 引入（core 作为子目录），也可以单独作为子目录加进别的工程。

add_executable(stereo_cli
    main.cpp
    pnm.cpp
    calib.cpp
)

target_link_libraries(stereo_cli PRIVATE stereo_core)

# 与 core 相同的语言/编码约定：源码是 UTF-8 无 BOM，MSVC 需要 /utf-8。
set_target_properties(stereo_cli PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
if (MSVC)
    # fopen/fscanf 是标准 C，不是“不安全 API”；CLI 要保持零第三方依赖
    target_compile_definitions(stereo_cli PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(stereo_cli PRIVATE /utf-8 /W4)
else()
    target_compile_options(stereo_cli PRIVATE -Wall -Wextra)
endif()
