@echo off
chcp 65001 >nul
title SiFli watch 构建
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
echo.
pause
