param(
    [string]$Port = "COM3",
    [int]$Seconds = 60,
    [string]$Log = "C:\Users\18058\Desktop\huangshanpai\watch\_bootcheck.txt"
)

# 抓「开机第一个画面是什么 App」的完整证据链。
#
# 为什么要抢在烧录后立刻接管串口：
#   开机横幅 + 时钟 App 的 init 只在开头那十几秒出现一次，
#   serial.ps1 是"先发后听"，正好错过。
#
# 判据（看日志里的这几行）：
#   clock: N watchface(s) registered, in order: [0]xxx   -> 表盘列表只剩几个
#   STATE_ACTIVE: clock[band] init / resume              -> 开机直接进表盘（没发 to_clock 也出现）
#   Main 相关日志（app_mainmenu）                         -> 不该出现；出现说明还是先进主菜单
#
# 复位手段用 DTR+RTS 一起抬（只抬 RTS 拿到的是驱动缓冲里的陈旧日志，会被骗）。

$sp = New-Object System.IO.Ports.SerialPort
$sp.PortName = $Port
$sp.BaudRate = 1000000
$sp.Parity = [System.IO.Ports.Parity]::None
$sp.DataBits = 8
$sp.StopBits = [System.IO.Ports.StopBits]::One
$sp.Handshake = [System.IO.Ports.Handshake]::None
$sp.DtrEnable = $false
$sp.RtsEnable = $false
$sp.ReadTimeout = 200
$sp.Encoding = [System.Text.Encoding]::ASCII

$sp.Open()

# --- 冷复位：DTR + RTS 一起抬 ---
$sp.DtrEnable = $true
$sp.RtsEnable = $true
Start-Sleep -Milliseconds 250
$sp.DtrEnable = $false
$sp.RtsEnable = $false
Start-Sleep -Milliseconds 300

$sb = New-Object System.Text.StringBuilder
$t0 = Get-Date
$sentKeep = $false

while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    try {
        $chunk = $sp.ReadExisting()
        if ($chunk -and $chunk.Length -gt 0) { [void]$sb.Append($chunk) }
    } catch { }

    $el = ((Get-Date) - $t0).TotalMilliseconds
    # 板子 10 秒就息屏，息屏后串口哑掉，所以过了启动期就把 keepawake 钉上
    if (-not $sentKeep -and $el -ge 24000) {
        $sp.Write("keepawake`r")
        [void]$sb.Append("`r`n### SENT [keepawake] at +" + [int]$el + "ms`r`n")
        $sentKeep = $true
    }
    Start-Sleep -Milliseconds 30
}

$sp.Close()
Set-Content -Path $Log -Value $sb.ToString() -Encoding UTF8
exit 0
