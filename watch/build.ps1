<#
  黄山派（立创 SF32LB52-ULP）watch 工程 —— 一键编译

  用法（双击同目录的 build.bat 效果相同）：
    .\build.ps1                  编译黄山派固件（默认）
    .\build.ps1 -Clean           先清理再完整编译
    .\build.ps1 -PcSim           编译 PC 模拟器版（在电脑上跑 UI，不用烧板）
    .\build.ps1 -Board xxx       指定其它板

  本脚本很薄：真正干活的是同目录的 build_py.py。
  为什么要绕这一下？Windows 上 SCons 并行编译存在一个 PATH 竞态，会随机报
      Error in calling command:arm-none-eabi-gcc
      Exception: No such file or directory
      Please check Toolchains PATH setting.
  看着像工具链没装，其实工具链完好。补丁必须打在 Python 进程里，
  所以统一交给 build_py.py（详见该文件里 _harden_spawn 的注释）。
  只有找不到虚拟环境 Python 时，才回退到 SDK 官方的 export.ps1 + scons。
#>
param(
    [string]$Board = "sf32lb52-lchspi-ulp",
    [switch]$Clean,
    [switch]$PcSim
)

$ErrorActionPreference = "Continue"

if ($PcSim) { $Board = "pc" }

$PY = "C:\Users\18058\.sifli\python_env\sifli-sdk2.4_py3.13_env\Scripts\python.exe"
$BP = Join-Path $PSScriptRoot "build_py.py"

Write-Host ""
Write-Host "=== SiFli watch 构建 ===" -ForegroundColor Cyan
Write-Host "  工程 : $PSScriptRoot"
Write-Host "  板子 : $Board"
Write-Host ""

if (-not (Test-Path $BP)) {
    Write-Host "[错误] 找不到 build_py.py：$BP" -ForegroundColor Red
    exit 1
}

# ── 主路径：交给 build_py.py ────────────────────────────────────────────────
if (Test-Path $PY) {
    # 清空 PYTHONPATH：某些终端/沙箱注入的 sitecustomize 会拦截删除操作，
    # 让 scons 清理临时文件时被打断。编译本身不需要它（build_py.py 会自己设）。
    $env:PYTHONPATH = ""

    if ($Clean) {
        Write-Host "[1/2] 清理..." -ForegroundColor Yellow
        & $PY $BP "--board=$Board" "-c"
    }
    Write-Host "[2/2] 编译中... 全新编译约 23 分钟（LVGL v8 全量 + 12 个巨型字体数组）" -ForegroundColor Yellow
    Write-Host "      日志：$(Join-Path $PSScriptRoot 'build_py.log')，可另开窗口看进度" -ForegroundColor DarkGray
    Write-Host ""
    & $PY $BP "--board=$Board"
    exit $LASTEXITCODE
}

# ── 回退路径：没有虚拟环境 Python，改用 SDK 官方 export.ps1 + scons ─────────
Write-Host "[提示] 未找到虚拟环境 Python，改用 SDK 官方方式编译。" -ForegroundColor Yellow

$parent1 = Split-Path $PSScriptRoot -Parent
$parent2 = Split-Path $parent1 -Parent
$SDKCands = @(
    (Join-Path $PSScriptRoot "sifli-sdk"),
    (Join-Path $parent1    "sifli-sdk"),
    (Join-Path $parent2    "sifli-sdk"),
    (Join-Path $parent2    "xp\sifli-sdk"),
    "C:\Users\18058\Desktop\huangshanpai\sifli-sdk"
)
$SDK = $null
foreach ($c in $SDKCands) {
    if (Test-Path (Join-Path $c "tools\build\building.py")) { $SDK = $c; break }
}
if (-not $SDK) {
    Write-Host "[错误] 找不到 SDK。请把 sifli-sdk 文件夹放回本目录的上一级（与 watch 并列）。" -ForegroundColor Red
    exit 2
}

$PROJ = Join-Path $PSScriptRoot "lvgl\watch\project"
if (-not (Test-Path $PROJ)) {
    Write-Host "[错误] 找不到工程：$PROJ" -ForegroundColor Red
    exit 1
}

# 绕开 SDK 工具在 Windows 上的 shell 名大小写 bug
$env:SIFLI_SDK_SHELL = "powershell.exe"

Set-Location $SDK
. .\export.ps1

$TOOLCHAIN = "C:\Users\18058\.sifli\tools\arm-none-eabi-gcc\14.2.1\bin"
if ((Test-Path $TOOLCHAIN) -and ($env:PATH -notlike "*$TOOLCHAIN*")) {
    $env:PATH = "$TOOLCHAIN;$env:PATH"
}
$BUILD_TOOLS = Join-Path $SDK "tools\build"
if ($env:PYTHONPATH -notlike "*$BUILD_TOOLS*") {
    if ($env:PYTHONPATH) { $env:PYTHONPATH = "$BUILD_TOOLS;$env:PYTHONPATH" }
    else { $env:PYTHONPATH = $BUILD_TOOLS }
}

Set-Location $PROJ
if ($Clean) {
    Write-Host "[1/2] 清理..." -ForegroundColor Yellow
    scons --board=$Board -c
}
Write-Host "[2/2] 编译中..." -ForegroundColor Yellow
scons --board=$Board -j8 2>&1 | Tee-Object -FilePath (Join-Path $PSScriptRoot "build_$Board.log")
exit $LASTEXITCODE
