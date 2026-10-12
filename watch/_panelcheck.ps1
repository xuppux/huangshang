param(
    [string]$Port = "COM3",
    [int]$Seconds = 55,
    [string]$Log = "C:\Users\18058\Desktop\huangshanpai\watch\_panelcheck.txt"
)

# 硬复位 -> 先 keepawake 钉住板子（不然一息屏串口就哑）-> 再 to_clock 把时钟 App 叫出来
# 面板（含 panel: 自检打印）是在时钟 App 建页面时构建的，所以必须走到这一步。

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

# hard reset
$sp.RtsEnable = $true
Start-Sleep -Milliseconds 120
$sp.RtsEnable = $false

$sb = New-Object System.Text.StringBuilder
$t0 = Get-Date
$sent1 = $false
$sent2 = $false

while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    try {
        $chunk = $sp.ReadExisting()
        if ($chunk -and $chunk.Length -gt 0) { [void]$sb.Append($chunk) }
    } catch { }
    $el = ((Get-Date) - $t0).TotalMilliseconds
    if (-not $sent1 -and $el -ge 4500) {
        $sp.Write("keepawake`r")
        [void]$sb.Append("`r`n### SENT [keepawake] at +" + [int]$el + "ms`r`n")
        $sent1 = $true
    }
    if (-not $sent2 -and $el -ge 22000) {
        $sp.Write("to_clock`r")
        [void]$sb.Append("`r`n### SENT [to_clock] at +" + [int]$el + "ms`r`n")
        $sent2 = $true
    }
    Start-Sleep -Milliseconds 40
}

$sp.Close()
Set-Content -Path $Log -Value $sb.ToString() -Encoding UTF8
exit 0
