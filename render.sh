#!/bin/sh
# 字体探针：把服务端下发的 binfont 渲染成 PNG
# 用法: ./render.sh --font <binfont> <out.png> ["示例文字"]
#
# 主题/布局预览已迁移到 Web 编辑器：启动服务端后打开 http://<server>:8080/
set -e
cd "$(dirname "$0")"

if [ $# -eq 0 ]; then
  echo "usage: $0 --font <binfont> <out.png> [\"text\"]" >&2
  echo "配色与布局预览请直接打开 Web 编辑器（服务端根路径）" >&2
  exit 2
fi

if [ ! -d build ]; then
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release > /dev/null
fi
cmake --build build -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" > /dev/null

./build/dash_sim "$@"
