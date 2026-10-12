<#
  黄山派 (SF32LB52-ULP) -- talk to the board's serial console (uart1 @ 1000000)

  Two jobs, both of which need the board running normally (NOT in download mode):

    1) monitor boot log       .\serial.ps1
    2) set the RTC once       .\serial.ps1 -SetTime
       -> sends  show_date <y> <m> <d> <h> <min> <sec>  using THIS PC's clock,
          then sends  show_date  again and prints the chip's answer so you can
          see whether it really took.

    .\serial.ps1 -Send "help"           send any finsh command
    .\serial.ps1 -Seconds 15 -Log out.txt

  Why the RTC must be set at least once: this board has no backup cell for the
  RTC domain, so the clock stops when the board is fully unplugged. A reset, a
  re-flash or an MCU sleep all keep it, but pulling power loses it.
  DTR/RTS are left de-asserted on purpose -- raising them would reset the chip.
#>
param(
    [string]$Port = "",
    [int]$Baud = 1000000,
    [switch]$SetTime,
    [string[]]$Send = @(),
    [int]$SendGapMs = 800,
    [int]$Seconds = 6,
    [int]$WaitBoot = 12000,
    [string]$Log = ""
)

$ErrorActionPreference = "Continue"

# ------------------------------------------------------------- resolve port
function Get-CandidatePorts {
    $found = @()
    try {
        $sp = Get-CimInstance Win32_SerialPort -ErrorAction Stop
        foreach ($p in $sp) { $found += [pscustomobject]@{ Port = $p.DeviceID; Name = $p.Description } }
    } catch { }
    if ($found.Count -eq 0) {
        try {
            $pnp = Get-CimInstance Win32_PnPEntity -ErrorAction Stop |
                   Where-Object { $_.Name -match '\((COM\d+)\)' }
            foreach ($p in $pnp) {
                if ($p.Name -match '\((COM\d+)\)') { $found += [pscustomobject]@{ Port = $Matches[1]; Name = $p.Name } }
            }
        } catch { }
    }
    return $found
}

$cands = Get-CandidatePorts
$use = $null
if ($Port -ne "") {
    $use = $Port
} else {
    $pref = @("CH34", "CP21", "FT23", "USB-SERIAL", "USB Serial", "WCH", "Prolific", "SiFli")
    foreach ($k in $pref) {
        $m = $cands | Where-Object { $_.Name -like "*$k*" } | Select-Object -First 1
        if ($m) { $use = $m.Port; break }
    }
    if (-not $use -and $cands.Count -eq 1) { $use = $cands[0].Port }
}

if (-not $use) {
    Write-Host ""
    Write-Host "[NOT CONNECTED] no usable serial port." -ForegroundColor Yellow
    if ($cands.Count -gt 1) {
        Write-Host "  several are present, pick one:" -ForegroundColor DarkGray
        foreach ($c in $cands) { Write-Host ("    -Port {0}   ({1})" -f $c.Port, $c.Name) -ForegroundColor DarkGray }
    } else {
        Write-Host "  plug in the board, then retry. (.\flash.ps1 -List shows what is there)" -ForegroundColor DarkGray
    }
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
$sp.DtrEnable = $false      # keep these low: asserting them resets the chip
$sp.RtsEnable = $false
$sp.ReadTimeout  = 200
$sp.WriteTimeout = 1000
$sp.NewLine   = "`r"
$sp.Encoding  = [System.Text.Encoding]::ASCII

Write-Host ""
Write-Host ("=== serial $use @ $Baud (8N1) ===") -ForegroundColor Cyan
Write-Host ""

try {
    $sp.Open()
} catch {
    Write-Host ("[ERROR] cannot open {0}: {1}" -f $use, $_.Exception.Message) -ForegroundColor Red
    Write-Host "        close any serial monitor / putty / IDE terminal first." -ForegroundColor DarkGray
    exit 4
}

$sb = New-Object System.Text.StringBuilder

function Pump-Serial([int]$ms) {
    $end = (Get-Date).AddMilliseconds($ms)
    while ((Get-Date) -lt $end) {
        try {
            $chunk = $sp.ReadExisting()
            if ($chunk -and $chunk.Length -gt 0) {
                [void]$sb.Append($chunk)
                Write-Host $chunk -NoNewline
            } else {
                Start-Sleep -Milliseconds 40
            }
        } catch {
            Start-Sleep -Milliseconds 40
        }
    }
}

function Write-Cmd([string]$text) {
    Write-Host ""
    Write-Host ("> " + $text) -ForegroundColor Yellow
    $sp.Write($text + "`r")
}

# Waiting for the finsh prompt instead of sleeping a fixed amount: opening this
# port resets the chip, and the "msh />" prompt only shows up ~3s into boot.
# Sending earlier just loses the command silently.
function Wait-Prompt([int]$maxMs) {
    $end = (Get-Date).AddMilliseconds($maxMs)
    $seenAt = $null
    while ((Get-Date) -lt $end) {
        try {
            $chunk = $sp.ReadExisting()
            if ($chunk -and $chunk.Length -gt 0) {
                [void]$sb.Append($chunk)
                Write-Host $chunk -NoNewline
                if ($sb.ToString() -match 'msh\s*/>|finsh') { $seenAt = Get-Date; break }
            } else {
                Start-Sleep -Milliseconds 40
            }
        } catch {
            Start-Sleep -Milliseconds 40
        }
    }
    if ($seenAt) {
        Write-Host ""
        Write-Host "[console ready]" -ForegroundColor Green
        Start-Sleep -Milliseconds 150     # let the prompt settle before typing
        return $true
    }
    Write-Host ""
    Write-Host ("[warn] no prompt within {0}ms -- sending anyway" -f $maxMs) -ForegroundColor Yellow
    return $false
}

Write-Host "--- listening ---" -ForegroundColor DarkGray

# A bare CR makes finsh repaint its prompt. Without this, attaching to a board
# that is ALREADY running never shows the prompt (it was printed at boot, long
# before we connected), and we would wait out the whole timeout every time.
$sp.Write("`r")
Start-Sleep -Milliseconds 150

Wait-Prompt $WaitBoot | Out-Null

if ($SetTime) {
    $n = Get-Date
    $cmd = "show_date {0} {1} {2} {3} {4} {5}" -f $n.Year, $n.Month, $n.Day, $n.Hour, $n.Minute, $n.Second
    Write-Cmd $cmd
    Pump-Serial 700
    # read the chip's own answer back, so the user can see it actually took
    Write-Cmd "show_date"
    Pump-Serial 1500
}

foreach ($c in $Send) {
    Write-Cmd $c
    Pump-Serial $SendGapMs
}

$remain = $Seconds * 1000
if ($remain -gt 0) { Pump-Serial $remain }

Write-Host ""
Write-Host ""
Write-Host "[closed]" -ForegroundColor DarkGray
$sp.Close()

if ($Log -ne "") {
    Set-Content -Path $Log -Value $sb.ToString() -Encoding UTF8
    Write-Host ("  log written: {0}" -f $Log) -ForegroundColor DarkGray
}

# Explicit exit code: callers (build-and-flash.ps1) treat a missing code as a
# failure, because a parameter-binding error would otherwise leave a stale 0.
exit 0
