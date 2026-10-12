<#
  probe.ps1 -- one-shot diagnostic session with the watch.

  The problem this solves: with the PM firmware the board blacks out after 10s of
  no input, and while suspended the console UART is dead -- you cannot send it
  anything at all. So a naive "open port and type" session spends all its time
  talking to a sleeping chip.

  Sequence:
    1. open, drain whatever stale bytes are sitting in the driver buffer
    2. pulse RTS (RESET on this board) and watch for a boot banner
    3. as soon as the console is up, send `keepawake` repeatedly until it takes.
       From then on sleep is off and the console stays alive.
    4. sample the RTC twice with a gap, using the exact same call the watchface
       uses, so we can tell whether the counter is advancing
    5. jump to the clock app so the watchface can be looked at

  Usage:
    .\probe.ps1 -Log probe.txt
    .\probe.ps1 -NoReset           # board is already awake, don't reset it
    .\probe.ps1 -RtcGap 20
#>
param(
    [string]$Port = "",
    [int]$Baud = 1000000,
    [int]$BootWaitMs = 15000,
    [int]$RtcGap = 15,
    [switch]$NoReset,
    [string]$Log = ""
)

$ErrorActionPreference = "Continue"

function Get-CandidatePorts {
    # .NET first -- see the long note in flash.ps1.  WMI's Win32_SerialPort does
    # not report this board's CH340 at all, so WMI-first made the list empty.
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
    $found = @()
    foreach ($com in ($names | Sort-Object)) {
        if ($lookup.ContainsKey($com)) { $found += [pscustomobject]@{ Port = $com; Name = $lookup[$com] } }
        else                           { $found += [pscustomobject]@{ Port = $com; Name = "(unknown device)" } }
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
$sp.DtrEnable = $false
$sp.RtsEnable = $false
$sp.ReadTimeout  = 200
$sp.WriteTimeout = 1000
$sp.NewLine   = "`r"
$sp.Encoding  = [System.Text.Encoding]::ASCII

$sb = New-Object System.Text.StringBuilder
function App([string]$s) { [void]$sb.Append($s + "`r`n") }

$script:mark = 0     # index in $sb after which we look for fresh output

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

function Send([string]$t) {
    App (">>> " + $t)
    $sp.Write($t + "`r")
    Pump 500
}

# PowerShell gotcha that bit this script once: calling a function as FOO() is a
# PARSER ERROR, not a call.  Functions are invoked bare (or with &).  Hence the
# void return + the plain "Get-Fresh" call sites below; do not re-add parens.
function Get-Fresh { return $sb.ToString().Substring([Math]::Min($script:mark, $sb.Length)) }

try { $sp.Open() } catch {
    Write-Host ("[ERROR] cannot open {0}: {1}" -f $use, $_.Exception.Message) -ForegroundColor Red
    exit 4
}

App ("=== probe on {0} @ {1} ===" -f $use, $Baud)

# ---- 1. drain stale bytes so the buffer only holds this session ------------
App "[1] draining stale buffer..."
Pump 1500
[void]$sb.Clear()
App "[1] buffer cleared"

if (-not $NoReset) {
    # ---- 2. reset via RTS ---------------------------------------------------
    App "[2] pulsing RTS (reset)..."
    $sp.RtsEnable = $true;  Start-Sleep -Milliseconds 120
    $sp.RtsEnable = $false
    $script:mark = $sb.Length
    App "[2] waiting for boot..."
    Pump $BootWaitMs

    $fresh = Get-Fresh
    if ($fresh -match 'SiFli|SFBL|msh') {
        App "[2] BOOT BANNER SEEN -- reset worked"
    } else {
        App "[2] !! no boot banner -- RTS pulse did NOT reset the chip"
    }
} else {
    $sp.Write("`r")
    Pump 800
    $script:mark = $sb.Length
}

# ---- 3. pin the board awake ------------------------------------------------
App "[3] trying to send keepawake..."
$ok = $false
for ($i = 0; $i -lt 22; $i++) {
    if ((Get-Fresh) -match 'idle screen-off') { $ok = $true; break }
    App (">>> keepawake   (attempt {0})" -f ($i + 1))
    $sp.Write("keepawake`r")
    Pump 900
}
if ($ok) { App "[3] keepawake ACTIVE -- sleep disabled, console will stay alive" }
else     { App "[3] !! keepawake never answered -- board is probably asleep already" }

# ---- 4. RTC sampling -------------------------------------------------------
App "[4] rtc sample 1"
Send "rtc_now"
$t1 = Get-Date
Pump ($RtcGap * 1000)
App ("[4] rtc sample 2  (after {0:N0}s)" -f ((Get-Date) - $t1).TotalSeconds)
Send "rtc_now"
Pump 1200

# ---- 5. switch to the watchface -------------------------------------------
App "[5] switching to the clock app"
Send "to_clock"
Pump 6000

App "[5] done, still listening 5s"
Pump 5000

$sp.Close()
App ""
App ("[closed] captured {0} chars" -f $sb.ToString().Length)

if ($Log -ne "") {
    [System.IO.File]::WriteAllText($Log, $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))
    Write-Host ("  log written: {0}" -f $Log) -ForegroundColor DarkGray
}

exit 0
