<#
  clockcheck.ps1 -- 时钟体检：一次跑完，回答「表盘时间为什么不走」。

  为什么需要专门的脚本，而不是直接敲命令：

    1. 固件里的 clkdiag 自检只在开机后头 60 秒打印，晚了就没了。
       所以顺序必须钉死：先闷头听 75 秒，再开口问。serial.ps1 是先发后听，
       顺序正好相反，抓不到自检。
    2. 板子没有 RTC 备份电池，冷启动 RTC 读到 0。要判断「走不走」，
       必须先用电脑的时间把 RTC 校一遍，再隔一段时间回读比对秒数。
    3. 这台机器上 WMI 的 Win32_SerialPort 认不出板子的 CH340，
       必须走 .NET 的 GetPortNames（原因详见 flash.ps1 里的长注释）。

  跑法：
    .\clockcheck.ps1                      听 75 秒自检，然后校时并回读
    .\clockcheck.ps1 -Listen 90
    .\clockcheck.ps1 -SkipListen          板子已经跑过自检了，只校时+回读
    .\clockcheck.ps1 -Log clockcheck.txt
#>
param(
    [string]$Port = "",
    [int]$Baud = 1000000,
    [int]$Listen = 75,
    [int]$CompareGap = 20,
    [switch]$SkipListen,
    [string]$Log = ""
)

$ErrorActionPreference = "Continue"

# ------------------------------------------------------------- resolve port
$names = @()
try { $names = @([System.IO.Ports.SerialPort]::GetPortNames()) } catch { }
$lookup = @{}
try {
    Get-CimInstance Win32_PnPEntity -ErrorAction Stop |
        Where-Object { $_.Name -match '\((COM\d+)\)' } |
        ForEach-Object {
            if ($_.Name -match '\((COM\d+)\)') {
                $com = $Matches[1]
                if (-not $lookup.ContainsKey($com)) { $lookup[$com] = $_.Name }
            }
        }
} catch { }

$use = $null
if ($Port -ne "") { $use = $Port }
else {
    foreach ($com in ($names | Sort-Object)) {
        $nm = ""
        if ($lookup.ContainsKey($com)) { $nm = $lookup[$com] }
        foreach ($k in @("CH34", "CP21", "FT23", "USB-SERIAL", "USB Serial", "WCH", "SiFli")) {
            if ($nm -like "*$k*") { $use = $com; break }
        }
        if ($use) { break }
    }
    if (-not $use -and $names.Count -eq 1) { $use = $names[0] }
}
if (-not $use) {
    Write-Host "[NOT CONNECTED] 找不到可用串口。" -ForegroundColor Yellow
    exit 3
}

# --------------------------------------------------------------- open port
$sp = New-Object System.IO.Ports.SerialPort
$sp.PortName  = $use
$sp.BaudRate  = $Baud
$sp.Parity    = [System.IO.Ports.Parity]::None
$sp.DataBits  = 8
$sp.StopBits  = [System.IO.Ports.StopBits]::One
$sp.Handshake = [System.IO.Ports.Handshake]::None
$sp.DtrEnable = $false      # 置高会把芯片拖进 BOOT，别动
$sp.RtsEnable = $false
$sp.ReadTimeout  = 200
$sp.WriteTimeout = 1000
$sp.NewLine   = "`r"
$sp.Encoding  = [System.Text.Encoding]::ASCII

$sb = New-Object System.Text.StringBuilder
function App([string]$s) { [void]$sb.Append($s + "`r`n") }

function Pump([int]$ms) {
    $end = (Get-Date).AddMilliseconds($ms)
    while ((Get-Date) -lt $end) {
        try {
            $chunk = $sp.ReadExisting()
            if ($chunk -and $chunk.Length -gt 0) { [void]$sb.Append($chunk) }
            else { Start-Sleep -Milliseconds 25 }
        } catch { Start-Sleep -Milliseconds 25 }
    }
}

function Send([string]$t) {
    App (">>> " + $t)
    $sp.Write($t + "`r")
    Pump 800
}

try { $sp.Open() } catch {
    Write-Host ("[ERROR] 打不开 {0}：{1}" -f $use, $_.Exception.Message) -ForegroundColor Red
    Write-Host "        先把别的串口监视器 / putty / IDE 终端关掉。" -ForegroundColor DarkGray
    exit 4
}

App ("=== clockcheck on {0} @ {1}  {2:yyyy-MM-dd HH:mm:ss} ===" -f $use, $Baud, (Get-Date))

# ---- 1. 闷头听：抓开机的 clkdiag 自检 ---------------------------------------
if (-not $SkipListen) {
    App ("[1] 静听 {0} 秒，等 clkdiag 自检（只在开机头 60 秒有）..." -f $Listen)
    Pump ($Listen * 1000)
}

# ---- 2. 校时：拿这台电脑的钟去写 RTC ----------------------------------------
$n = Get-Date
$setCmd = "show_date {0} {1} {2} {3} {4} {5}" -f $n.Year, $n.Month, $n.Day, $n.Hour, $n.Minute, $n.Second
App ""
App ("[2] 用电脑时间校 RTC：{0:yyyy-MM-dd HH:mm:ss}" -f $n)
Send $setCmd
Send "rtc_now"          # 走表盘那条路径回读，确认写入生效
Send "show_date"        # SDK 自带命令，交叉验证

# ---- 3. 隔一段时间再回读，看秒数有没有往前走 --------------------------------
App ""
App ("[3] 等 {0} 秒后回读，比对秒数" -f $CompareGap)
Pump ($CompareGap * 1000)
Send "rtc_now"

# ---- 4. 切到表盘 ------------------------------------------------------------
App ""
App "[4] 切到表盘（也可按 KEY1）"
Send "to_clock"
Pump 4000

$sp.Close()
App ""
App ("[closed] 共捕获 {0} 字符" -f $sb.ToString().Length)

if ($Log -ne "") {
    [System.IO.File]::WriteAllText($Log, $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))
    Write-Host ("  日志已写：{0}" -f $Log) -ForegroundColor DarkGray
}

exit 0
