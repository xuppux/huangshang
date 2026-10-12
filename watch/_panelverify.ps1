param(
    [string]$Port = "COM3",
    [int]$Seconds = 75,
    [string]$Log = "C:\Users\18058\Desktop\huangshanpai\watch\_panelverify.txt"
)

# 硬复位 -> keepawake（不然一息屏串口就哑）-> to_clock（面板是时钟 App 建页面时建的）
# -> 依次敲 bright / calib，把面板上「亮度」「手动校时」两个按钮**背后的同一个函数**
#    从串口走一遍。
#
# 为什么绕这一圈：屏幕上有没有变暗、面板有没有弹出来，我这边看不到（没有截图通道）。
# 但命令走的是和按钮完全相同的入口函数，所以串口这边通了，按钮那边的差别就只剩
# "触摸事件有没有派发到 btnm_event_handler" —— 而那部分有 panel: btn 那行日志兜着。

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

# bright 连敲 4 次：100 -> 60 -> 30 -> 10 -> 100，正好走一圈又回到全亮，
# 不会把用户的表留在暗档上。
$plan = @(
    @{ at =  4500; cmd = "keepawake"    },
    @{ at = 22000; cmd = "to_clock"     },
    @{ at = 30000; cmd = "bright"       },
    @{ at = 33000; cmd = "bright"       },
    @{ at = 36000; cmd = "bright"       },
    @{ at = 39000; cmd = "bright"       },
    @{ at = 42000; cmd = "calib check"  },
    @{ at = 45000; cmd = "calib open"   },
    @{ at = 50000; cmd = "calib close"  },
    @{ at = 54000; cmd = "ble"          }
)

$i = 0
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    try {
        $chunk = $sp.ReadExisting()
        if ($chunk -and $chunk.Length -gt 0) { [void]$sb.Append($chunk) }
    } catch { }

    $el = ((Get-Date) - $t0).TotalMilliseconds
    while ($i -lt $plan.Count -and $el -ge $plan[$i].at) {
        $sp.Write($plan[$i].cmd + "`r")
        [void]$sb.Append("`r`n### SENT [" + $plan[$i].cmd + "] at +" + [int]$el + "ms`r`n")
        $i++
    }
    Start-Sleep -Milliseconds 40
}

$sp.Close()
Set-Content -Path $Log -Value $sb.ToString() -Encoding UTF8
exit 0
