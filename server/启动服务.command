#!/bin/bash
cd "$(dirname "$0")" || exit 1
if ! command -v python3 >/dev/null 2>&1; then
  echo "未找到 python3，请先安装 Python 3.10+。"
  read -n 1 -s -r -p "按任意键退出…"
  exit 1
fi
python3 launcher.py
echo
read -n 1 -s -r -p "按任意键关闭窗口…"
