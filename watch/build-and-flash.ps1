<#
  黄山派 (SF32LB52-ULP) -- the whole loop in one command:
     compile -> flash -> set the RTC from this PC's clock

  Usage:
    .\build-and-flash.ps1                 incremental build, flash, set time
    .\build-and-flash.ps1 -Clean          full rebuild first (~23 min)
    .\build-and-flash.ps1 -Port COM5      explicit port
    .\build-and-flash.ps1 -Erase          erase whole flash before writing
    .\build-and-flash.ps1 -NoTime         do not touch the RTC
    .\build-and-flash.ps1 -SkipBuild      flash only what is already built

  It stops at the first failing step and says which one it was, so a compile
  error never gets followed by a pointless flash attempt.

  ---------------------------------------------------------------------------
  ⚠⚠ 子脚本一律走**独立进程**（powershell.exe -File），不要用 `& .\child.ps1`

  2026-09-23 实测踩到：`& .\build.ps1` 明明编译成功（build_py.py 自己打印
  「编译成功，耗时 1 分 35 秒」并 sys.exit(0)），但回到本脚本时
  $LASTEXITCODE 还是那个哨兵值 99 —— 于是报了
      [STOP] build failed (exit 99). nothing was flashed.
  编译产物其实是好的，**只是烧录被静默跳过了**。
  （用 `& child.ps1` 的同进程调用，退出码在本机这个 PowerShell 宿主里不可靠；
    独立进程的 $LASTEXITCODE = 进程退出码，一定有。）

  所以：
    · 调子脚本 → Invoke-ChildScript（内部用 powershell.exe -File）
    · 万一退出码还是不对 → 再用**证据**交叉核对一遍构建（见下面的
      Test-BuildEvidence：build_py.log 末尾那行 ASCII 的
      `scons: done building targets` + main.bin 是不是真的被重写了）。
      宁可多查一次，也不要静默跳过烧录。

  ---------------------------------------------------------------------------
  ⚠⚠ 同一件事的两个**附加**坑（2026-09-23 第二次踩，症状一模一样）

  1. 函数返回值的污染 —— 真凶。
     `& powershell.exe @argv` 后面直接 `return $LASTEXITCODE`，子进程的输出
     会混进返回值，`$rc` 变成"字符串数组 + 退出码"，`$rc -ne 0` 恒真。
     症状：`[STOP] build failed (exit ). nothing was flashed.` —— exit 后面空着。
     修法：输出 `2>&1 | ForEach-Object { Write-Host $_ }` 吃掉。

  2. 本脚本是 UTF-8 **无 BOM** 时，脚本里的**中文会变乱码**。
     Windows PowerShell 5.1 读无 BOM 的 .ps1 用系统 ANSI 码页(GBK)，
     于是中文正则/中文字符串全部匹配不上、打印出来也是乱码。
     → 兜底逻辑一律用 ASCII 标记；并且已给本脚本补上 UTF-8 BOM。

  NOTE on splatting: a child script must be invoked with a HASHTABLE splat, not
  an array one, when calling it in-process. Invoke-ChildScript builds the argv
  itself, so the array/hashtable mixup cannot happen here either way.

      $p = @{ Port = 'COM3' } ; & .\flash.ps1 @p      # correct
      $a = @('-Port','COM3')  ; & .\flash.ps1 @a      # WRONG (positional)

  An array splat passes its elements POSITIONALLY, so the literal string
  "-Port" itself becomes the first positional argument, and the child blows up
  with a confusing "cannot convert COM3 to System.Int32".
#>
param(
    [string]$Port = "",
    [switch]$Clean,
    [switch]$Erase,
    [switch]$NoTime,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Continue"
$ROOT = $PSScriptRoot
$SENTINEL = 99        # "the child never reported an exit code"
$BUILD_OUT = Join-Path $ROOT "lvgl\watch\project\build_sf32lb52-lchspi-ulp_hcpu"
$MAIN_BIN = Join-Path $BUILD_OUT "main.bin"
$BUILD_LOG = Join-Path $ROOT "build_py.log"

function Step($n, $total, $title) {
    Write-Host ""
    Write-Host ("########## step {0}/{1}  {2} ##########" -f $n, $total, $title) -ForegroundColor Magenta
}

<#
  调子脚本：起一个独立 powershell 进程，返回它的退出码。
  参数用哈希表：switch/布尔 -> 只在 $true 时加 "-Name"；其它 -> "-Name" "value"。

  ⚠⚠ 子进程的输出必须在本函数里被**吃掉**（接进管道 -> Write-Host），
  绝不能让它在函数体里裸奔。2026-09-23 实测踩到：

      & powershell.exe @argv
      return $LASTEXITCODE        # ← 这样写是错的

  这样 `return` 出去的其实是 [子进程的全部输出..., 退出码] 这个**混合数组**，
  于是 `$rc -ne 0` 恒真（数组和标量 0 比较永远"不相等"）→ 每一次都被判成
  "构建失败" → `[STOP] ... nothing was flashed.`，而 build_py.log 里明明是
  「scons: done building targets. / 退出码 0」。
  最好认的症状：报错里 exit 后面**空着** —— `[STOP] build failed (exit ).`
  （把 $rc 拼进字符串时，数组被展开成一堆无意义的文本，空的那段就是它）。

  正确姿势：输出接到 Write-Host（Information 流），它不参与函数返回值。
#>
function Invoke-ChildScript([string]$Script, [hashtable]$ChildArgs) {
    $argv = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $Script)
    if ($ChildArgs) {
        foreach ($k in $ChildArgs.Keys) {
            $v = $ChildArgs[$k]
            if ($v -is [switch] -or $v -is [bool]) {
                if ($v) { $argv += "-$k" }
            } else {
                $argv += "-$k"
                $argv += [string]$v
            }
        }
    }
    & powershell.exe @argv 2>&1 | ForEach-Object { Write-Host $_ }
    if ($null -eq $LASTEXITCODE) { return $SENTINEL }   # 子进程压根没起来
    return [int]$LASTEXITCODE
}

<#
  退出码不可信时的兜底：用**证据**判断这次构建到底有没有成功。

  ⚠⚠ 这里的匹配串必须**纯 ASCII**。2026-09-23 踩过：原来写的是中文字面量
  「退出码 0」，而本脚本存的是 UTF-8 **无 BOM** —— Windows PowerShell 5.1
  读无 BOM 的 .ps1 时按系统 ANSI(本机 GBK) 解码，脚本里的中文**全变乱码**，
  那个正则于是永远匹配不上日志里正确的 UTF-8 中文 → logSaysOk 恒为 False
  → 又是一次「编译明明好好的、烧录被静默跳过」。
  改用 build_py.py 自己打的 ASCII 行 `scons: done building targets.` ——
  纯 ASCII，无论按 GBK 还是 UTF-8 解都一样，从根上免疫。

  （另外本脚本末尾已被补上 UTF-8 BOM；但别指望靠它 —— 兜底逻辑就该不依赖编码。）

  两条证据都要满足：
    ① build_py.log 末尾 20 行里有 `scons: done building targets`
    ② main.bin 的修改时间晚于本次构建开始的时间（产物真的被重写了）
  注意 ① 单独用不行：日志可能是上一轮留下的。
#>
function Test-BuildEvidence([datetime]$Since) {
    $logSaysOk = $false
    if (Test-Path $BUILD_LOG) {
        # 按**字节**读、用 Latin-1 解码（一字节一字符，不丢信息），再找 ASCII 标记。
        # 这样彻底不用管日志到底是什么编码。
        $bytes = [System.IO.File]::ReadAllBytes($BUILD_LOG)
        $text  = [System.Text.Encoding]::GetEncoding(28591).GetString($bytes)
        $tail  = ($text -split "`n" | Select-Object -Last 20) -join "`n"
        $logSaysOk = ($tail -match "scons: done building targets")
    }
    $binFresh = $false
    if (Test-Path $MAIN_BIN) {
        $binFresh = ((Get-Item $MAIN_BIN).LastWriteTime -ge $Since)
    }
    [pscustomobject]@{
        LogSaysOk = $logSaysOk
        BinFresh  = $binFresh
        Ok        = ($logSaysOk -and $binFresh)
    }
}

$total = 1                          # flash always runs
if (-not $SkipBuild) { $total++ }
if (-not $NoTime)    { $total++ }

$idx = 1

# ------------------------------------------------------------------ 1. build
if (-not $SkipBuild) {
    Step $idx $total "compile"
    $idx++

    $t0 = Get-Date
    $rc = Invoke-ChildScript (Join-Path $ROOT "build.ps1") @{ Clean = [bool]$Clean }

    if ($rc -eq $SENTINEL -or $rc -ne 0) {
        $ev = Test-BuildEvidence $t0
        if ($ev.Ok) {
            Write-Host ""
            Write-Host ("[WARN] build.ps1 reported exit {0}, but the evidence says it worked:" -f $rc) -ForegroundColor Yellow
            Write-Host "        build_py.log ends with '退出码 0' AND main.bin was rewritten." -ForegroundColor DarkGray
            Write-Host "        (exit-code propagation through child .ps1 is unreliable here - see the header)" -ForegroundColor DarkGray
        } else {
            Write-Host ""
            Write-Host ("[STOP] build failed (exit {0}). nothing was flashed." -f $rc) -ForegroundColor Red
            Write-Host ("       evidence: logSaysOk={0} binFresh={1}" -f $ev.LogSaysOk, $ev.BinFresh) -ForegroundColor DarkGray
            Write-Host "       full log: build_py.log" -ForegroundColor DarkGray
            exit 1
        }
    }
    Write-Host ""
    if (Test-Path $MAIN_BIN) {
        Write-Host ("[ok] build finished  main.bin {0:n0} bytes  {1}" -f `
                    (Get-Item $MAIN_BIN).Length, (Get-Item $MAIN_BIN).LastWriteTime) -ForegroundColor Green
    } else {
        Write-Host "[ok] build finished" -ForegroundColor Green
    }
}

# ------------------------------------------------------------------ 2. flash
Step $idx $total "flash"
$idx++

$flashArgs = @{}
if ($Port -ne "") { $flashArgs['Port'] = $Port }
if ($Erase)       { $flashArgs['Erase'] = $true }

$rc = Invoke-ChildScript (Join-Path $ROOT "flash.ps1") $flashArgs

if ($rc -eq $SENTINEL -or $rc -ne 0) {
    Write-Host ""
    Write-Host ("[STOP] flash failed (exit {0})." -f $rc) -ForegroundColor Red
    exit 2
}
Write-Host ""
Write-Host "[ok] flashed" -ForegroundColor Green

# --------------------------------------------------------------- 3. set time
if (-not $NoTime) {
    Step $idx $total "set RTC from this PC's clock"
    Start-Sleep -Seconds 3      # let the app boot and bring the console up

    $serialArgs = @{ SetTime = $true; Seconds = 3 }
    if ($Port -ne "") { $serialArgs['Port'] = $Port }

    $rc = Invoke-ChildScript (Join-Path $ROOT "serial.ps1") $serialArgs

    if ($rc -eq $SENTINEL -or $rc -ne 0) {
        Write-Host ""
        Write-Host ("[WARN] could not talk to the console (exit {0})." -f $rc) -ForegroundColor Yellow
        Write-Host "       the firmware IS flashed -- only the RTC is not set." -ForegroundColor DarkGray
        Write-Host "       retry later:  .\serial.ps1 -SetTime" -ForegroundColor DarkGray
    } else {
        Write-Host ""
        Write-Host "[ok] RTC command sent -- check the echoed answer above." -ForegroundColor Green
    }
}

Write-Host ""
Write-Host "=== done ===" -ForegroundColor Cyan
exit 0
