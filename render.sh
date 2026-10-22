#!/bin/sh
# 一键构建 + 渲染仪表盘到 out/dashboard.png
# 用法: ./render.sh [输出路径]
set -e
cd "$(dirname "$0")"

OUT="${1:-out/dashboard.png}"

if [ ! -d build ]; then
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release > /dev/null
fi
cmake --build build -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" > /dev/null

mkdir -p "$(dirname "$OUT")"
./build/dash_sim "$OUT"
