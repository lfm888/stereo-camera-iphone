# 仓库根构建入口：core + 第二个前端（CLI）。
#
# 两个入口并存是有意的：
#   * `cmake -S core -B build/core`   只构建/测试 C++ 核心（CI 的老路径，保持可用）；
#   * `cmake -S .    -B build/all`    额外构建 apps/cli（CLI 用总头调库，验证跨平台面）。
#
# iOS 不走这里：project.yml 把 core/src 直接编进 App target。
cmake_minimum_required(VERSION 3.20)
project(stereo_workspace LANGUAGES C CXX)

add_subdirectory(core)
add_subdirectory(apps/cli)
