@echo off
chcp 65001 >nul
cd /d "%~dp0"
where python >nul 2>nul
if errorlevel 1 (
  echo 未找到 Python，请先安装 Python 3.10+（安装时勾选 Add Python to PATH）。
  pause
  exit /b 1
)
where pythonw >nul 2>nul
if errorlevel 1 (
  start "" python launcher.py
) else (
  start "" pythonw launcher.py
)
