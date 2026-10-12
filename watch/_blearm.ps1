param(
    [string]$Port = "COM3",
    [string]$Cmd = "ble on",
    [int]$SendAtMs = 4000,
    [int]$Seconds = 40,
    [string]$Log = "C:\Users\18058\Desktop\huangshanpai\watch\_blearm.txt"
)

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
$sent = $false

while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    try {
        $chunk = $sp.ReadExisting()
        if ($chunk -and $chunk.Length -gt 0) { [void]$sb.Append($chunk) }
    } catch { }
    if (-not $sent -and ((Get-Date) - $t0).TotalMilliseconds -ge $SendAtMs) {
        $sp.Write($Cmd + "`r")
        [void]$sb.Append("`r`n### SENT [$Cmd] at +" + [int]((Get-Date) - $t0).TotalMilliseconds + "ms`r`n")
        $sent = $true
    }
    Start-Sleep -Milliseconds 40
}

$sp.Close()
Set-Content -Path $Log -Value $sb.ToString() -Encoding UTF8
exit 0
