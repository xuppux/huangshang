param(
    [int]$Seconds = 110,
    [string]$Log = "C:\Users\18058\Desktop\huangshanpai\watch\_blecap.txt",
    [string]$Port = "COM3",
    [switch]$Reset
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

if ($Reset) {
    $sp.RtsEnable = $true
    Start-Sleep -Milliseconds 120
    $sp.RtsEnable = $false
    Start-Sleep -Milliseconds 80
}

$sb = New-Object System.Text.StringBuilder
[void]$sb.Append("### WALL START " + (Get-Date).ToString("HH:mm:ss.fff") + "`r`n")
$end = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $end) {
    try {
        $chunk = $sp.ReadExisting()
        if ($chunk -and $chunk.Length -gt 0) { [void]$sb.Append($chunk) }
        else { Start-Sleep -Milliseconds 50 }
    } catch { Start-Sleep -Milliseconds 50 }
}

[void]$sb.Append("`r`n### WALL END " + (Get-Date).ToString("HH:mm:ss.fff") + "`r`n")
$sp.Write("ble`r")
Start-Sleep -Milliseconds 1500
try { [void]$sb.Append($sp.ReadExisting()) } catch {}

$sp.Close()
Set-Content -Path $Log -Value $sb.ToString() -Encoding UTF8
exit 0
