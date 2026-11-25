#!/bin/sh
# 把 GitHub Actions 最新一次成功的构建产物下载到本地仓库：
#   build/StereoVision-unsigned.ipa
#
# 用法：
#   ./scripts/fetch_ipa.sh            # 取最近一次成功的 ipa.yml（未签名 IPA）
#
# 需要 curl + unzip + node，以及 GitHub 凭据：优先用 $GITHUB_TOKEN，否则问本机 git 的 credential
# helper（Windows 上就是凭据管理器里那个，push 能成功就能下载）。
set -e
cd "$(dirname "$0")/.."

REPO="${REPO:-lfm888/stereo-camera-iphone}"
# 分割模型已删（见 test.md §15），现在只有一条出 IPA 的流水线
WORKFLOW="Build unsigned IPA"
OUT="build/StereoVision-unsigned.ipa"

TOKEN="${GITHUB_TOKEN:-}"
if [ -z "$TOKEN" ]; then
    TOKEN=$(printf 'protocol=https\nhost=github.com\n\n' | git credential fill 2>/dev/null | sed -n 's/^password=//p')
fi
[ -n "$TOKEN" ] || { echo "拿不到 GitHub token（设 GITHUB_TOKEN，或让 git credential 存好凭据）"; exit 1; }

CURL="curl -sf --retry 5 --retry-delay 2 --retry-all-errors"
api() { $CURL -H "Authorization: Bearer $TOKEN" "https://api.github.com$1"; }

echo "[fetch] 找最近一次成功的 \"$WORKFLOW\""
RUN=$(api "/repos/$REPO/actions/runs?per_page=20" | WORKFLOW="$WORKFLOW" node -e '
let s = "";
process.stdin.on("data", d => s += d).on("end", () => {
  const r = JSON.parse(s).workflow_runs.find(r => r.name === process.env.WORKFLOW && r.conclusion === "success");
  if (r) console.log(r.id, r.head_sha.slice(0, 7), r.created_at);
});')
[ -n "$RUN" ] || { echo "[fetch] 没有成功的运行"; exit 1; }
set -- $RUN
RUN_ID=$1; SHA=$2; WHEN=$3
echo "[fetch] run $RUN_ID（$SHA，$WHEN）"

ID=$(api "/repos/$REPO/actions/runs/$RUN_ID/artifacts" | node -e '
let s = "";
process.stdin.on("data", d => s += d).on("end", () => {
  const a = JSON.parse(s).artifacts.find(a => a.name === "StereoVision-unsigned-ipa" && !a.expired);
  if (a) console.log(a.id);
});')
[ -n "$ID" ] || { echo "[fetch] 这次运行里没有未过期的 IPA artifact"; exit 1; }

mkdir -p build/dl
echo "[fetch] 下载 artifact $ID"
curl -sfL --retry 5 --retry-delay 2 --retry-all-errors -H "Authorization: Bearer $TOKEN" \
    "https://api.github.com/repos/$REPO/actions/artifacts/$ID/zip" -o build/dl/ipa.zip
rm -rf build/dl/x && mkdir -p build/dl/x
unzip -oq build/dl/ipa.zip -d build/dl/x
IPA=$(find build/dl/x -name '*.ipa' | head -1)
[ -n "$IPA" ] || { echo "[fetch] artifact 里没有 .ipa"; exit 1; }
mv -f "$IPA" "$OUT"
rm -rf build/dl

echo "[fetch] -> $OUT（$(du -h "$OUT" | cut -f1)）"
sha256sum "$OUT" 2>/dev/null || shasum -a 256 "$OUT"
