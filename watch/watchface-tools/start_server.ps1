<#
  本地起一个静态服务，把「校时.html」用 http://localhost 打开。

  为什么需要它：Web Bluetooth 要求页面运行在安全上下文里。
  Chrome 把 file:// 也算安全上下文，多数情况直接双击 html 就能用；
  但一旦报 SecurityError，就必须走 http://localhost（localhost 一定算安全）。
  file:// 的另一个麻烦是不同浏览器策略不一致，这个脚本把它绕干净。

  用法：双击本文件，或
      .\start_server.ps1           默认端口 8765
      .\start_server.ps1 -Port 9000
  然后浏览器打开脚本打印出来的地址。关掉这个窗口就停了。
#>
param(
    [int]$Port = 8765
)

$ErrorActionPreference = "Stop"
$dir = $PSScriptRoot

# 优先用桌面环境里已有的 Python；没有就退到 .workbuddy 自带的
$cands = @(
    "C:\Users\18058\.workbuddy\binaries\python\envs\default\Scripts\python.exe",
    "C:\Users\18058\.workbuddy\binaries\python\versions\3.13.12\python.exe",
    "C:\Users\18058\AppData\Local\Programs\Python\Python313\python.exe"
)
$py = $null
foreach ($c in $cands) { if (Test-Path $c) { $py = $c; break } }

Write-Host ""
Write-Host "=== 手表校时 —— 本地服务 ===" -ForegroundColor Cyan
Write-Host "  目录 : $dir"
Write-Host "  端口 : $Port"
Write-Host ""
Write-Host "  在浏览器里打开：" -ForegroundColor Green
Write-Host "      http://localhost:$Port/校时.html" -ForegroundColor Green
Write-Host ""
Write-Host "  用 Chrome 或 Edge（Safari / iOS 不支持网页蓝牙）。" -ForegroundColor DarkGray
Write-Host "  按 Ctrl+C 停止。" -ForegroundColor DarkGray
Write-Host ""

if (-not $py) {
    Write-Host "[错误] 没找到 Python。可以直接双击「校时.html」试试，" -ForegroundColor Yellow
    Write-Host "       如果浏览器报 SecurityError 再回来装 Python。" -ForegroundColor Yellow
    exit 1
}

# 中文文件名的 URL 需要转义，这里直接把编码后的名字也打出来，省得手打
$enc = [System.Uri]::EscapeDataString("校时.html")
Write-Host "  （中文文件名如果打不开，用这个）" -ForegroundColor DarkGray
Write-Host "      http://localhost:$Port/$enc" -ForegroundColor DarkGray
Write-Host ""

Set-Location $dir
& $py -m http.server $Port --bind 127.0.0.1
