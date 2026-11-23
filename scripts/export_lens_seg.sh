#!/bin/sh
# 导出 segzoo 训练好的分割模型为 CoreML（见 scripts/export_lens_seg.py 的说明）。
# 先准备 Python 环境（torch + coremltools + segzoo 的依赖），然后：
#   ./scripts/export_lens_seg.sh --ckpt /path/to/best.pth --segzoo /path/to/segzoo
set -e
PY="${PYTHON:-python}"
exec "$PY" "$(dirname "$0")/export_lens_seg.py" "$@"
