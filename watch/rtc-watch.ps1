<#
  rtc-watch.ps1 -- poll `show_date` repeatedly and see whether the RTC advances.

  Why a loop instead of two readings: with the PM firmware the board blacks out
  after IDLE_TIME_LIMIT (10s) and the console goes silent while it is suspended.
  A single pair of readings can easily land in the dead window and tell you
  nothing. Polling every few seconds shows BOTH things at once:

    * whether the RTC counter is actually advancing (compare the seconds)
    * exactly when the console dies (i.e. when the board suspended)

  Usage:
    .\rtc-watch.ps1                       # reset, then poll for 45s
    .\rtc-watch.ps1 -NoReset -Seconds 60
    .\rtc-watch.ps1 -Log rtc.txt
#>
param(
    [string]$Port = "",
    [int]$Baud = 1000000,
    [int]$Seconds = 45,
    [int]$IntervalMs = 3000,
    [int]$BootWaitMs = 13000,
    [switch]$NoReset,
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
if ($Port -ne "") { $use = $Port }
else {
    $pref = @("CH34", "CP21", "FT23", "USB-SERIAL", "USB Serial", "WCH", "Prolific", "SiFli")
    foreach ($k in $pref) {
        $m = $cands | Where-Object { $_.Name -like "*$k*" } | Select-Object -First 1
        if ($m) { $use = $m.Port; break }
    }
    if (-not $use -and $cands.Count -eq 1) { $use = $cands[0].Port }
}
if (-not $use) {
    Write-Host "[NOT CONNECTED] no usable serial port." -ForegroundColor Yellow
    exit 3
}

$sp = New-Object System.IO.Ports.SerialPort
$sp.PortName  = $use
$sp.BaudRate  = $Baud
$sp.Parity    = [System.IO.Ports.Parity]::None
$sp.DataBits  = 8
$sp.StopBits  = [System.IO.Ports.StopBits]::One
$sp.Handshake = [System.IO.Ports.Handshake]::None
# De-assert the handshake lines and never touch them again: on this board they
# are wired to BOOT/RESET, so a stray transition reboots the chip mid-test.
$sp.DtrEnable = $false
$sp.RtsEnable = $false
$sp.ReadTimeout  = 200
$sp.WriteTimeout = 1000
$sp.NewLine   = "`r"
$sp.Encoding  = [System.Text.Encoding]::ASCII

$sb  = New-Object System.Text.StringBuilder
# The function is called App(...) rather than Log(...) on purpose: $Log is a
# parameter, and having a function and a variable share a name is a trap.
function App([string]$s) { [void]$sb.Append($s + "`r`n") }

function Pump([int]$ms) {
    $end = (Get-Date).AddMilliseconds($ms)
    while ((Get-Date) -lt $end) {
        try {
            $chunk = $sp.ReadExisting()
            if ($chunk -and $chunk.Length -gt 0) { [void]$sb.Append($chunk) }
            else { Start-Sleep -Milliseconds 30 }
        } catch { Start-Sleep -Milliseconds 30 }
    }
}

try { $sp.Open() } catch {
    Write-Host ("[ERROR] cannot open {0}: {1}" -f $use, $_.Exception.Message) -ForegroundColor Red
    exit 4
}

App ("=== rtc-watch on {0} @ {1} ===" -f $use, $Baud)

if (-not $NoReset) {
    # RTS is RESET on this board's CH340 (that is how sftool forces download
    # mode). Pulse it once so we start from a known state, DTR stays low so the
    # chip boots normally instead of entering download mode.
    $sp.RtsEnable = $true;  Start-Sleep -Milliseconds 120
    $sp.RtsEnable = $false
    App "[reset pulse sent on RTS]"
    Pump $BootWaitMs
} else {
    $sp.Write("`r")
    Pump 800
}

$t0 = Get-Date
$next = 0
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    if (((Get-Date) - $t0).TotalSeconds -ge $next) {
        App ("--- t={0:N1}s  >>> show_date" -f ((Get-Date) - $t0).TotalSeconds)
        $sp.Write("show_date`r")
        $next += ($IntervalMs / 1000.0)
    }
    Pump 200
}

$sp.Close()
App ""
App ("[closed] captured {0} chars" -f $sb.ToString().Length)

if ($Log -ne "") {
    [System.IO.File]::WriteAllText($Log, $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))
    Write-Host ("  log written: {0}" -f $Log) -ForegroundColor DarkGray
}

exit 0
