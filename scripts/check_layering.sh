#!/usr/bin/env bash
# check_layering.sh — 把 ARCHITECTURE.md 里的"规矩"变成会失败的检查。
#
# 用法（CI 在构建前跑；本地也可以跑）：
#   bash scripts/check_layering.sh
#
# 检查项（每条失败都会打印命中的文件:行）：
#   1. StereoVision/UI/       不许 import AVFoundation/MetalKit，不许出现 StereoEngine/SVStereoCore
#   2. StereoVision/Contracts/ 不许 import SwiftUI/AVFoundation
#   3. 旧类名零残留（含注释）：CameraController / CalibrationSource / Analyzer / Renderer
#   4. Swift 里不许有逐像素循环（ARCHITECTURE §6 的启发式）
#   5. core/ 不许 include Apple 框架头（跨平台库的唯一硬约束）
#   6. core/src/capi/ 边界层不许有逐像素循环；每个文件 <= 250 行（阶段 2 起生效）
#   7. struct stereo_context 只在一个地方定义（阶段 2 起生效）
#   8. 总头 stereo_c_api.h 只 #include 子头（阶段 2 起生效）
#
# 说明：`grep -w` 用来做"整词"匹配，GNU grep 与 macOS/BSD grep 都支持；
# 不用 `\b`，避免 BSD grep 的正则方言差异。
set -uo pipefail
cd "$(dirname "$0")/.."

fail=0
total=0

ok()   { total=$((total + 1)); printf 'OK   %s\n' "$1"; }
bad()  { total=$((total + 1)); fail=1; printf 'FAIL %s\n' "$1"; shift; [ $# -gt 0 ] && printf '%s\n' "$@" | sed 's/^/       /'; }

# grep 命中即失败
no_matches() {
    local desc="$1" pattern="$2"
    shift 2
    local out
    out=$(grep -rnE "$pattern" "$@" 2>/dev/null || true)
    if [ -n "$out" ]; then bad "$desc" "$out"; else ok "$desc"; fi
}

# grep 命中整词即失败（参数：描述，然后是要禁的词，最后是要搜索的路径）
no_words() {
    local desc="$1" path="$2"
    shift 2
    local args=()
    for w in "$@"; do args+=(-e "$w"); done
    local out
    out=$(grep -rnw "${args[@]}" "$path" 2>/dev/null || true)
    if [ -n "$out" ]; then bad "$desc" "$out"; else ok "$desc"; fi
}

echo "== Swift 目录红线 =="
no_matches "UI/ 不 import AVFoundation/MetalKit、不出现 StereoEngine/SVStereoCore" \
    'import (AVFoundation|MetalKit)|StereoEngine|SVStereoCore' StereoVision/UI
no_matches "Contracts/ 不 import SwiftUI/AVFoundation" \
    'import (SwiftUI|AVFoundation)' StereoVision/Contracts
no_words "旧类名零残留（CameraController / CalibrationSource / Analyzer / Renderer）" \
    StereoVision CameraController CalibrationSource Analyzer Renderer
no_matches "Swift 里没有逐像素循环（for … in 0..<）" \
    'for .* in 0\.\.<' --include='*.swift' StereoVision

echo "== core/ 跨平台约束 =="
no_matches "core/ 不 include Apple 框架头" \
    '#(include|import)[[:space:]]*[<"](Foundation|UIKit|AVFoundation|CoreVideo|CoreGraphics|CoreImage|Metal|MetalKit|QuartzCore|Photos|ImageIO|UniformTypeIdentifiers)[/>"]' \
    core/include core/src

echo "== C API 边界层（阶段 2 起生效） =="
if [ -d core/src/capi ]; then
    no_matches "capi/ 里没有逐像素循环（逐行 memcpy 拷贝不算；见脚本头注释）" \
        'for[[:space:]]*\([[:space:]]*int[[:space:]]+x[[:space:]]*=' core/src/capi
    long=$(find core/src/capi -name '*.cpp' -exec wc -l {} + 2>/dev/null | awk '$1 > 250 && $2 != "total" { print }')
    if [ -n "$long" ]; then bad "capi/ 每个 .cpp <= 250 行" "$long"; else ok "capi/ 每个 .cpp <= 250 行"; fi
    defs=$(grep -rln '^struct stereo_context' core/src core/include 2>/dev/null || true)
    if [ "$(printf '%s\n' "$defs" | sed '/^$/d' | wc -l)" -eq 1 ] && printf '%s' "$defs" | grep -q 'capi_internal.hpp'; then
        ok "struct stereo_context 只在 capi_internal.hpp 定义"
    else
        bad "struct stereo_context 只在 capi_internal.hpp 定义" "${defs:-（没有找到定义）}"
    fi
else
    echo "SKIP capi/ 检查（core/src/capi 还不存在）"
fi

if [ -f core/include/stereo/stereo_types.h ]; then
    includes=$(grep -c '#include "stereo/' core/include/stereo/stereo_c_api.h 2>/dev/null || true)
    if [ "${includes:-0}" -ge 2 ]; then
        ok "总头 stereo_c_api.h 只做 #include 子头"
    else
        bad "总头 stereo_c_api.h 只做 #include 子头" "只找到 ${includes:-0} 个子头 include"
    fi
else
    echo "SKIP 总头检查（还没有 stereo_types.h）"
fi

echo
if [ "$fail" -eq 0 ]; then
    echo "全部 $total 项检查通过"
    exit 0
fi
echo "$total 项里有失败项（见上面的 FAIL）"
exit 1
