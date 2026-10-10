@echo off
chcp 936 >nul
cd /d "%~dp0"
echo ==================================================
echo   SOLO-AI eyes  烧录面板 (字库 / 按钮 / 程序 / 视频)
echo   地址: http://127.0.0.1:8767/
echo ==================================================
rem 本文件必须保持 GBK + CRLF + 无 BOM; 用编辑器另存成 UTF-8 会让 cmd 解析错乱
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0启动烧录面板.ps1"
if errorlevel 1 pause
