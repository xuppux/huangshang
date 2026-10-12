<#
  rtc-check.ps1 -- is the RTC actually counting?

  Opens the console ONCE, prints the boot banner if it is still coming, then
  sends `show_date` twice with a gap in between. Two readings are all you need:
  if the seconds advance, the RTC hardware is alive and the clock source (LXT)
  started fine; if the two readings are identical, the RTC has stopped and no
  amount of "setting the time" will help.

  Usage:
    .\rtc-check.ps1                     # auto port, 15s gap
    .\rtc-check.ps1 -Port COM3 -GapSeconds 30
    .\rtc-check.ps1 -Log rtc.txt

  DTR/RTS are left de-asserted so opening the port does not reset the chip.
  Pass -Reset to drive DTR low->high once if you WANT a reboot and a fresh log.
#>
param(
    [string]$Port = "",
    [int]$Baud = 1000000,
    [int]$GapSeconds = 15,
    [int]$BootWaitMs = 12000,
    [switch]$Reset,
    [string]$Log = ""
)

$ErrorActionPreference = "Continue"

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
    Write-Host "[NOT CONNECTED] no usable serial port." -ForegroundColor Yellow
    foreach ($c in $cands) { Write-Host ("  -Port {0}  ({1})" -f $c.Port, $c.Name) -ForegroundColor DarkGray }
    exit 3
}

$sp = New-Object System.IO.Ports.SerialPort
$sp.PortName  = $use
$sp.BaudRate  = $Baud
$sp.Parity    = [System.IO.Ports.Parity]::None
$sp.DataBits  = 8
$sp.StopBits  = [System.IO.Ports.StopBits]::One
$sp.Handshake = [System.IO.Ports.Handshake]::None
$sp.DtrEnable = $false
$sp.RtsEnable = $false
$sp.ReadTimeout  = 200
$sp.WriteTimeout = 1000
$sp.NewLine   = "`r"
$sp.Encoding  = [System.Text.Encoding]::ASCII

$sb = New-Object System.Text.StringBuilder

function Pump([int]$ms) {
    $end = (Get-Date).AddMilliseconds($ms)
    while ((Get-Date) -lt $end) {
        try {
            $chunk = $sp.ReadExisting()
            if ($chunk -and $chunk.Length -gt 0) { [void]$sb.Append($chunk) }
            else { Start-Sleep -Milliseconds 40 }
        } catch { Start-Sleep -Milliseconds 40 }
    }
}

function Send([string]$t) {
    [void]$sb.Append("`r`n>>> " + $t + "`r`n")
    $sp.Write($t + "`r")
    Start-Sleep -Milliseconds 400
}

try { $sp.Open() } catch {
    Write-Host ("[ERROR] cannot open {0}: {1}" -f $use, $_.Exception.Message) -ForegroundColor Red
    exit 4
}

Write-Host ("=== rtc-check on {0} @ {1} ===" -f $use, $Baud) -ForegroundColor Cyan

if ($Reset) {
    # On this board's CH340 the two handshake lines drive the chip's BOOT and
    # RESET pins (that is how sftool auto-enters download mode). Pulse RTS only,
    # and keep DTR de-asserted, so we get a plain reboot instead of download mode.
    $sp.RtsEnable = $true;  Start-Sleep -Milliseconds 120
    $sp.RtsEnable = $false; Start-Sleep -Milliseconds 400
    Write-Host "[reset pulse sent on RTS]" -ForegroundColor DarkGray
    Pump $BootWaitMs
} else {
    # A bare CR makes finsh repaint the prompt when we attach to a board that is
    # already running (its prompt was printed at boot, long before we got here).
    $sp.Write("`r")
    Pump 600
}

Send "show_date"
Pump 1200
$first = $sb.ToString()

Send ("echo waiting " + $GapSeconds + "s ...")
Pump ($GapSeconds * 1000)

Send "show_date"
Pump 1500
$second = $sb.ToString()

$sp.Close()

Write-Host ""
Write-Host ("[closed] captured {0} chars" -f $sb.ToString().Length) -ForegroundColor DarkGray

if ($Log -ne "") {
    # WriteAllText, not Set-Content: the console output is raw bytes and must not
    # be re-encoded line by line.
    [System.IO.File]::WriteAllText($Log, $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))
    Write-Host ("  log written: {0}" -f $Log) -ForegroundColor DarkGray
}

exit 0
