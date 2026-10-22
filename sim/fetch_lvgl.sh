#!/bin/sh
# 拉取 PC 模拟器所需的 LVGL 源码到仓库根目录的 lvgl/
# 固件侧不需要它：idf_component.yml 会由 ESP-IDF 组件管理器自动下载 LVGL 9.6
set -e
cd "$(dirname "$0")/.."

VERSION="${LVGL_VERSION:-v9.3.0}"

if [ -d lvgl/src ]; then
    echo "lvgl/ 已存在，跳过（版本需为 ${VERSION}）"
    exit 0
fi

echo "clone LVGL ${VERSION} -> lvgl/"
git clone --depth 1 --branch "$VERSION" https://github.com/lvgl/lvgl.git lvgl
echo "完成，现在可以运行 ./render.sh out/dashboard.png"
