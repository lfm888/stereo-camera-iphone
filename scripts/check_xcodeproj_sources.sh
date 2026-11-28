#!/bin/bash
# check_xcodeproj_sources.sh —— 确认"仓库里的源文件"和"工程里参与编译的文件"一致
#
# 为什么需要它：CI 不再依赖提交的 pbxproj，每次从 project.yml 生成。生成器只认
# project.yml 里 sources 声明的目录。一旦有源文件没被覆盖，xcodebuild 会**成功**——
# 只是那个文件根本没参与编译。这个脚本让"静默漏文件"当场变成 CI 失败。
#
# 用法（CI 里、xcodegen generate 之后、xcodebuild 之前）：
#   bash scripts/check_xcodeproj_sources.sh
#
# 判据（两个方向都要成立）：
#   仓库 —— 除"豁免目录"外的所有 *.swift/*.mm/*.m/*.cpp/*.c/*.metal
#   工程 —— 生成工程里所有 target 的 Sources build phase
#   两者必须逐项相同。文件出现在未声明的目录里（例如新建了一个顶层目录）也会失败。
#
# 注意两件事：
#   1. 比较用 File.realpath —— macOS 上 /tmp 是指向 /private/tmp 的软链，
#      只做字符串前缀裁剪会得出假的"全部不一致"。
#   2. 两个清单都必须按**字节序**排序 —— Ruby 的 String#sort 是字节序，而 macOS 的
#      sort 在 en_US.UTF-8 下大小写不敏感（"core/" 会排到 "StereoVision/" 前面），
#      混用会得出"内容相同但顺序不同"的假失败。
set -euo pipefail

cd "$(dirname "$0")/.."
PROJECT="${PROJECT:-StereoVision.xcodeproj}"

# 参与 iOS 编译的目录（必须与 project.yml 的 sources 对应）
COMPILED_DIRS="StereoVision StereoVisionTests core/src"
# 明确不参与 iOS 编译的目录（C++ 测试、CLI、脚本、工具、构建产物、CI 配置）
EXEMPT_DIRS="core/tests apps scripts tools docs build .github Config"

if [ ! -d "$PROJECT" ]; then
    echo "找不到 $PROJECT —— 先运行 xcodegen generate --spec project.yml"
    exit 1
fi

DISK="$(mktemp)"
PROJ="$(mktemp)"
trap 'rm -f "$DISK" "$PROJ"' EXIT

echo "编译目录: $COMPILED_DIRS"
echo "豁免目录: $EXEMPT_DIRS"

# 0) 声明的编译目录必须真的在 project.yml 的 sources 里
for d in $COMPILED_DIRS; do
    if ! grep -q "path: *$d\$" project.yml; then
        echo "FAIL: project.yml 的 sources 里没有声明 $d"
        exit 1
    fi
done

# 1) 仓库里所有"应该参与编译"的源文件
{
    find . -type f \( -name '*.swift' -o -name '*.mm' -o -name '*.m' \
                      -o -name '*.cpp' -o -name '*.c' -o -name '*.metal' \) \
        -not -path './.git/*'
} | sed 's#^\./##' | while IFS= read -r f; do
    keep=1
    for d in $EXEMPT_DIRS; do
        case "$f" in "$d"/*) keep=0 ;; esac
    done
    # 用 if 而不是 `[ ... ] && printf`：keep=0 时后者返回 1，
    # 配合 set -o pipefail 会让整个管道失败（find 的顺序一换就踩到）。
    if [ "$keep" = 1 ]; then printf '%s\n' "$f"; fi
done | LC_ALL=C sort -u > "$DISK"

# 2) 生成工程里实际参与编译的文件（所有 target）
ruby - "$PROJECT" > "$PROJ" <<'RUBY'
require "xcodeproj"
root = File.realpath(Dir.pwd)
proj = Xcodeproj::Project.open(ARGV[0])
names = []
proj.targets.each do |t|
    t.source_build_phase.files.each do |f|
        ref = f.file_ref
        next if ref.nil?
        path = begin
            File.realpath(ref.real_path.to_s)
        rescue StandardError
            ref.real_path.to_s
        end
        names << (path.start_with?("#{root}/") ? path[(root.length + 1)..] : path)
    end
end
names = names.uniq.sort
# 只报告仓库内的文件（指向 SDK / 系统头的一律忽略）
puts names.select { |n| !n.start_with?("/") }
RUBY
LC_ALL=C sort -u -o "$PROJ" "$PROJ"

echo "== 仓库 $(wc -l < "$DISK" | tr -d ' ') 个源文件 / 工程里 $(wc -l < "$PROJ" | tr -d ' ') 个 =="

if diff -u "$DISK" "$PROJ"; then
    echo "OK: 工程覆盖了所有源文件"
    exit 0
fi

echo
echo "FAIL: 仓库与工程不一致（'-' 只在仓库里，'+' 只在工程里）"
echo "  '-' 说明文件没参与编译：要么它在未声明的目录里（把目录加进 project.yml 的 sources"
echo "      并同步更新本脚本的 COMPILED_DIRS），要么 project.yml 里的路径写错了"
echo "  '+' 说明工程引用了仓库里不存在的文件：多半是 project.yml 里的路径写错了"
exit 1
