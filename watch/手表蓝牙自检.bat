@echo off
chcp 65001 >nul
setlocal

REM ─────────────────────────────────────────────────────────────────────
REM  手表 <-> 电脑 蓝牙通道 —— 双击这个就行
REM
REM  想做别的，改下面 MODE 那一行：
REM      test   = 自检（默认，会说 PASS / FAIL）
REM      chat   = 聊天模式，打字发给手表
REM      scan   = 只扫描，确认电脑看得见表
REM      probe  = 把蓝牙服务树打出来（排查用）
REM
REM  前提：手表已经烧好带 ble_ai 的固件、开机等够 30~60 秒、电脑蓝牙开着。
REM ─────────────────────────────────────────────────────────────────────

set MODE=test

set PY=C:\Users\18058\.workbuddy\binaries\python\envs\default\Scripts\python.exe
set SCRIPT=%~dp0ble_ai_pc.py

if not exist "%PY%" (
    echo [错误] 找不到 Python：%PY%
    pause
    exit /b 1
)
if not exist "%SCRIPT%" (
    echo [错误] 找不到脚本：%SCRIPT%
    pause
    exit /b 1
)

"%PY%" "%SCRIPT%" %MODE% %*

echo.
echo ────────────────────────────────────────────────
echo  结束。按任意键关闭窗口。
pause >nul
