#!/bin/bash
# ci_diff_xcodeproj.sh —— **临时诊断脚本**（任务书第 1.2 步）
#
# 目的：回答一个问题——仓库里提交的 StereoVision.xcodeproj，和 project.yml 用 xcodegen
# 生成出来的工程，到底差在哪。结论用来决定第 1.4 步能不能直接切换成"CI 里生成"。
#
# 这个脚本**永远不会让 CI 失败**：每一步都 || true，整体也不 set -e。
# 第 1.4 步完成、check_xcodeproj_sources.sh 上线之后，本脚本可以删掉。
#
# 只在 macOS runner 上跑（需要 xcodegen / xcodebuild / ruby）。
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT="$PWD"
OUT="ci-artifacts"
TARGET="StereoVision"

mkdir -p "$OUT"
rm -f "$OUT"/*.txt "$OUT"/*.diff "$OUT"/*.log

echo "== 1) 工具版本 =="
if ! command -v xcodegen >/dev/null 2>&1; then
    echo "xcodegen 未安装，尝试 brew install"
    brew install xcodegen || true
fi
xcodegen --version 2>&1 | tee "$OUT/xcodegen_version.txt" || true
xcodebuild -version 2>&1 | tee "$OUT/xcodebuild_version.txt" || true

echo "== 2) 备份提交进仓库的工程 =="
rm -rf /tmp/committed
mkdir -p /tmp/committed
cp -R StereoVision.xcodeproj /tmp/committed/StereoVision.xcodeproj 2>/dev/null || {
    echo "仓库里没有提交 StereoVision.xcodeproj，没什么可对比的"
    echo "no committed xcodeproj" > "$OUT/README.txt"
    exit 0
}

echo "== 3) 用 project.yml 生成工程 =="
xcodegen generate --spec project.yml 2>&1 | tee "$OUT/xcodegen_generate.log" || true

echo "== 4) pbxproj 文本 diff（对象 ID 不同，噪声大，仅作参考）=="
diff -u /tmp/committed/StereoVision.xcodeproj/project.pbxproj \
        StereoVision.xcodeproj/project.pbxproj > "$OUT/pbxproj.diff" 2>/dev/null || true
wc -l "$OUT/pbxproj.diff" || true

echo "== 5) build settings 语义对比（把 /tmp 路径抹掉以消除路径噪声）=="
xcodebuild -project /tmp/committed/StereoVision.xcodeproj -target "$TARGET" \
           -configuration Release -showBuildSettings 2>/dev/null \
    | sed "s#/tmp/committed#$ROOT#g" | sort > "$OUT/bs_committed.txt" || true
xcodebuild -project StereoVision.xcodeproj -target "$TARGET" \
           -configuration Release -showBuildSettings 2>/dev/null | sort > "$OUT/bs_generated.txt" || true
diff -u "$OUT/bs_committed.txt" "$OUT/bs_generated.txt" > "$OUT/buildsettings.diff" 2>/dev/null || true
wc -l "$OUT/buildsettings.diff" || true

echo "== 6) 参与编译的源文件清单对比 =="
if ! gem list -i xcodeproj >/dev/null 2>&1; then
    echo "xcodeproj gem 未安装，尝试 gem install"
    gem install xcodeproj --no-document || true
fi
cat > /tmp/list_sources.rb <<'RUBY'
require "xcodeproj"
proj = Xcodeproj::Project.open(ARGV[0])
name = ARGV[1]
proj.targets.each do |t|
    puts "### target: #{t.name} (#{t.product_type})"
    next unless t.name == name
    files = t.source_build_phase.files.map { |f| f.file_ref&.path.to_s }.reject(&:empty?).sort
    files.each { |f| puts f }
end
RUBY
ruby /tmp/list_sources.rb /tmp/committed/StereoVision.xcodeproj "$TARGET" \
    > "$OUT/src_committed.txt" 2>/dev/null || true
ruby /tmp/list_sources.rb StereoVision.xcodeproj "$TARGET" \
    > "$OUT/src_generated.txt" 2>/dev/null || true
diff -u "$OUT/src_committed.txt" "$OUT/src_generated.txt" > "$OUT/sources.diff" 2>/dev/null || true
wc -l "$OUT/sources.diff" || true

echo "== 7) scheme 与 target 列表（生成后的工程）=="
xcodebuild -list -project StereoVision.xcodeproj 2>&1 | tee "$OUT/xcodebuild_list_generated.txt" || true
xcodebuild -list -project /tmp/committed/StereoVision.xcodeproj 2>&1 \
    | tee "$OUT/xcodebuild_list_committed.txt" || true

echo "== 8) 磁盘上的源文件 vs 生成工程里的编译清单（供 1.4 的脚本参考）=="
{
    find StereoVision -type f \( -name '*.swift' -o -name '*.mm' -o -name '*.m' -o -name '*.metal' \) | sort
    find core/src -type f \( -name '*.cpp' -o -name '*.c' \) | sort
} > "$OUT/files_on_disk.txt" || true
wc -l "$OUT/files_on_disk.txt" || true

echo "== 诊断完成，产物在 $OUT/ =="
ls -la "$OUT" || true
exit 0
