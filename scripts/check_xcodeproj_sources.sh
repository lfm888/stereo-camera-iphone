#!/bin/bash
# check_xcodeproj_sources.sh —— 确认"磁盘上的源文件"和"工程里参与编译的文件"一致
#
# 为什么需要它：CI 已经不依赖提交的 pbxproj，而是每次从 project.yml 生成。但生成器
# 只认 project.yml 里 sources 声明的目录。如果有人把 .swift 放进一个没被 sources 覆盖的
# 目录（或者在 project.yml 里写错路径），xcodebuild 会**成功**——只是那个文件根本没参与编译。
# 这个脚本让这种"静默漏文件"当场变成 CI 失败。
#
# 用法（CI 里、xcodegen generate 之后、xcodebuild 之前）：
#   bash scripts/check_xcodeproj_sources.sh
#
# 判据：
#   磁盘 = project.yml sources 覆盖的目录下所有 *.swift/*.mm/*.m/*.metal/*.cpp/*.c
#   工程 = 生成工程里所有 target 的 Sources build phase
#   两者必须逐项相同（按仓库相对路径）。
#
# 注意：比较用 File.realpath —— macOS 上 /tmp 是指向 /private/tmp 的软链，
# 只做字符串前缀裁剪会得出假的"全部不一致"。
set -euo pipefail

cd "$(dirname "$0")/.."
PROJECT="${PROJECT:-StereoVision.xcodeproj}"

if [ ! -d "$PROJECT" ]; then
    echo "找不到 $PROJECT —— 先运行 xcodegen generate --spec project.yml"
    exit 1
fi

DISK="$(mktemp)"
PROJ="$(mktemp)"
trap 'rm -f "$DISK" "$PROJ"' EXIT

# 1) 磁盘上"应该"参与编译的文件。
#    目录清单与 project.yml 的 sources 一一对应；core/tests 不在 sources 里，天然被排除。
{
    find StereoVision -type f \( -name '*.swift' -o -name '*.mm' -o -name '*.m' -o -name '*.metal' \)
    find core/src -type f \( -name '*.cpp' -o -name '*.c' \)
} | sed 's#^\./##' | LC_ALL=C sort -u > "$DISK"

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
puts names.uniq.sort
RUBY

# 两个清单都必须按**字节序**排：Ruby 的 String#sort 是字节序，而 macOS 的 sort
# 在 en_US.UTF-8 下是大小写不敏感排序（"core" 会排到 "StereoVision" 前面），
# 混用会得出"内容相同但顺序不同"的假失败。
LC_ALL=C sort -u -o "$PROJ" "$PROJ"
echo "== 磁盘上 $(wc -l < "$DISK" | tr -d ' ') 个源文件 / 工程里 $(wc -l < "$PROJ" | tr -d ' ') 个 =="

if diff -u "$DISK" "$PROJ"; then
    echo "OK: 工程覆盖了所有源文件"
    exit 0
fi

echo
echo "FAIL: 工程与磁盘不一致（'-' 只在磁盘上，'+' 只在工程里）"
echo "  '-' 说明文件没进工程：检查 project.yml 的 sources 是否覆盖了它所在目录"
echo "  '+' 说明工程引用了不存在的文件：多半是 project.yml 里的路径写错了"
exit 1
