#!/bin/bash
# ci_diff_xcodeproj.sh —— **临时诊断脚本**（任务书第 1.2 步）
#
# 回答一个问题：仓库里提交的 StereoVision.xcodeproj，和 project.yml 用 xcodegen
# 生成出来的工程，到底差在哪。
#
# 三条设计约束（第一版踩过的坑）：
#   1. 生成失败时**必须显式报出来**，不能拿"文件和自己比"得出空 diff 的假象。
#   2. 对比要在**完整仓库副本**上做：只拷 .xcodeproj 会导致 Config/Base.xcconfig
#      找不到，于是 PRODUCT_BUNDLE_IDENTIFIER 之类的差异全是假的。
#   3. 诊断不能改变构建输入：跑完必须把仓库恢复原状（project.yml + .xcodeproj）。
#
# 另外加一个**探针**：如果 `xcodegen generate` 因为 spec 写错而失败，就临时把
# 已知的错误写法换成正确写法再生成一次，只为拿到"如果修好会差在哪"的数据。
# 探针**不改仓库**（跑完恢复），也不代表我建议怎么改。
#
# 永远不会让 CI 失败：整体不 set -e，每一步 || true。
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT="$PWD"
OUT="ci-artifacts"
TARGET="StereoVision"

mkdir -p "$OUT"
rm -f "$OUT"/*.txt "$OUT"/*.diff "$OUT"/*.log

echo "== 1) 工具版本 =="
command -v xcodegen >/dev/null 2>&1 || { echo "xcodegen 未安装，brew install"; brew install xcodegen || true; }
xcodegen --version 2>&1 | tee "$OUT/xcodegen_version.txt" || true
xcodebuild -version 2>&1 | tee "$OUT/xcodebuild_version.txt" || true

if [ ! -d StereoVision.xcodeproj ]; then
    echo "仓库里没有提交 StereoVision.xcodeproj，没什么可对比的"
    echo "no committed xcodeproj" > "$OUT/README.txt"
    exit 0
fi

echo "== 2) 备份完整仓库（排除 build/.git），用于语义对比 =="
rm -rf /tmp/committed_repo
mkdir -p /tmp/committed_repo
( command -v rsync >/dev/null && rsync -a --exclude build --exclude .git ./ /tmp/committed_repo/ ) \
    || ( cp -R . /tmp/committed_repo/ 2>/dev/null || true )
ls /tmp/committed_repo | head -20

echo "== 3) 用仓库里的 project.yml 生成 =="
if xcodegen generate --spec project.yml > "$OUT/xcodegen_generate.log" 2>&1; then
    echo "OK" > "$OUT/generation_status.txt"
    echo "生成成功"
else
    echo "FAILED" > "$OUT/generation_status.txt"
    echo "！！！ xcodegen generate 失败，下面的 diff 不可信 ！！！"
    cat "$OUT/xcodegen_generate.log"
    # 探针：临时换掉已知的错误 product type，再试一次
    echo "== 3b) 探针：type: static-library -> type: library.static =="
    cp project.yml /tmp/project.yml.orig
    sed -i.bak 's/^\( *\)type: *static-library *$/\1type: library.static/' project.yml || true
    if xcodegen generate --spec project.yml > "$OUT/xcodegen_generate_probe.log" 2>&1; then
        echo "OK" > "$OUT/generation_probe_status.txt"
        echo "探针生成成功"
    else
        echo "FAILED" > "$OUT/generation_probe_status.txt"
        cat "$OUT/xcodegen_generate_probe.log"
    fi
    # 记录探针用的 spec，便于复现
    diff -u /tmp/project.yml.orig project.yml > "$OUT/project_yml_probe.diff" || true
    cp /tmp/project.yml.orig project.yml
    rm -f project.yml.bak   # sed -i.bak 的残留
fi

if [ ! -s "$OUT/generation_status.txt" ] || ! grep -q OK "$OUT/generation_probe_status.txt" 2>/dev/null; then
    :
fi
GENERATED_OK=0
grep -q OK "$OUT/generation_status.txt" 2>/dev/null && GENERATED_OK=1
grep -q OK "$OUT/generation_probe_status.txt" 2>/dev/null && GENERATED_OK=1

echo "== 4) 参与编译的源文件清单对比（所有 target）=="
if ! gem list -i xcodeproj >/dev/null 2>&1; then
    gem install xcodeproj --no-document || true
fi
cat > /tmp/list_sources.rb <<'RUBY'
require "xcodeproj"
proj = Xcodeproj::Project.open(ARGV[0])
proj.targets.sort_by(&:name).each do |t|
    puts "### target: #{t.name} (#{t.product_type})"
    files = t.source_build_phase.files.map { |f| f.file_ref&.path.to_s }.reject(&:empty?).sort
    files.each { |f| puts "  #{f}" }
end
RUBY
ruby /tmp/list_sources.rb /tmp/committed_repo/StereoVision.xcodeproj > "$OUT/src_committed.txt" 2>/dev/null || true
ruby /tmp/list_sources.rb StereoVision.xcodeproj > "$OUT/src_generated.txt" 2>/dev/null || true
diff -u "$OUT/src_committed.txt" "$OUT/src_generated.txt" > "$OUT/sources.diff" 2>/dev/null || true

echo "== 5) build settings 语义对比（抹掉路径与命令行回显的噪声）=="
norm() { grep -v ' -showBuildSettings' "$1" | sed "s#${2}#REPO#g" | sort; }
xcodebuild -project /tmp/committed_repo/StereoVision.xcodeproj -target "$TARGET" \
           -configuration Release -showBuildSettings 2>/dev/null > /tmp/bs_c.txt || true
xcodebuild -project StereoVision.xcodeproj -target "$TARGET" \
           -configuration Release -showBuildSettings 2>/dev/null > /tmp/bs_g.txt || true
norm /tmp/bs_c.txt /tmp/committed_repo > "$OUT/bs_committed.txt" || true
norm /tmp/bs_g.txt "$ROOT" > "$OUT/bs_generated.txt" || true
diff -u "$OUT/bs_committed.txt" "$OUT/bs_generated.txt" > "$OUT/buildsettings.diff" 2>/dev/null || true

echo "== 6) scheme / target 列表 =="
xcodebuild -list -project /tmp/committed_repo/StereoVision.xcodeproj 2>&1 > "$OUT/xcodebuild_list_committed.txt" || true
xcodebuild -list -project StereoVision.xcodeproj 2>&1 > "$OUT/xcodebuild_list_generated.txt" || true

echo "== 7) 磁盘上的源文件（供 1.4 的 check 脚本参考）=="
{
    find StereoVision -type f \( -name '*.swift' -o -name '*.mm' -o -name '*.m' -o -name '*.metal' \) | sort
    find core/src -type f \( -name '*.cpp' -o -name '*.c' \) | sort
} > "$OUT/files_on_disk.txt" || true

echo "== 8) 恢复仓库原状（诊断不能改变构建输入）=="
rm -rf StereoVision.xcodeproj
cp -R /tmp/committed_repo/StereoVision.xcodeproj ./ || true
git status --short | tee "$OUT/git_status_after.txt" || true

echo "== 9) 摘要 =="
{
    echo "generation_status : $(cat "$OUT/generation_status.txt" 2>/dev/null)"
    echo "probe_status      : $(cat "$OUT/generation_probe_status.txt" 2>/dev/null)"
    echo "sources.diff      : $(wc -l < "$OUT/sources.diff" 2>/dev/null) 行"
    echo "buildsettings.diff: $(wc -l < "$OUT/buildsettings.diff" 2>/dev/null) 行"
} | tee "$OUT/SUMMARY.txt"
ls -la "$OUT" || true
exit 0
