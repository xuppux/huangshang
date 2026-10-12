<#
  黄山派 (SF32LB52-ULP) watch project -- one-click UART flash

  Usage:
    .\flash.ps1                  auto-detect COM port, then flash
    .\flash.ps1 -Port COM5       use an explicit port
    .\flash.ps1 -Erase           erase whole flash first (use after changing partitions)
    .\flash.ps1 -List            only list serial ports, do not flash
    .\flash.ps1 -Baud 921600     slower link if 1M is unstable

  Why UART only: J-Link is not installed on this machine, so SWD is unavailable.
  The chip is put into download mode by sftool itself (DTR/RTS reset), so the
  normal flash inside the chip is never needed.

  WARNING: do NOT let a separate USB-serial module feed power into the board
  while flashing. Power it from its own USB port only.
#>
param(
    [string]$Port = "",
    [int]$Baud = 1000000,
    [switch]$Erase,
    [switch]$List,
    [switch]$Force
)

$ErrorActionPreference = "Continue"

$ROOT   = $PSScriptRoot
$BUILD  = Join-Path $ROOT "lvgl\watch\project\build_sf32lb52-lchspi-ulp_hcpu"
$SRC    = Join-Path $ROOT "lvgl\watch\src"
$SFTOOL = "C:\Users\18058\.sifli\tools\sftool\0.1.16\sftool.exe"

function Say($msg, $color) {
    if ($color) { Write-Host $msg -ForegroundColor $color } else { Write-Host $msg }
}

Write-Host ""
Say "=== SiFli watch -- UART flash ===" Cyan
Write-Host ""

# ---------------------------------------------------------------- 0. tool check
if (-not (Test-Path $SFTOOL)) {
    # fall back to whatever the SDK put on PATH
    $c = Get-Command sftool -ErrorAction SilentlyContinue
    if ($c) { $SFTOOL = $c.Source }
    else {
        Say "[ERROR] sftool not found." Red
        Say "        expected: $SFTOOL" DarkGray
        Say "        run the SDK export script once to install it, or fix the path above." DarkGray
        exit 2
    }
}

# ------------------------------------------------------- 1. enumerate COM ports
function Get-SerialPorts {
    # Order matters here, and the old order was wrong.
    #
    # WMI's Win32_SerialPort only reports ports that the driver exposes as a
    # "serial port" device class.  On this machine the on-board CH340 USART
    # bridge does NOT show up there -- Win32_SerialPort returns nothing at all,
    # even though the board is plugged in and Device Manager lists it under
    # "USB-SERIAL CH340 (COM3)".  Asking WMI first therefore made the port list
    # come up empty and the flash aborted with "no serial port found".
    #
    # .NET's SerialPort.GetPortNames() reads the actual registry port table and
    # is the reliable primary source.  WMI/PnP are only used to decorate it with
    # a human-readable name and the VID/PID.
    $names = @()
    try { $names = @([System.IO.Ports.SerialPort]::GetPortNames()) } catch { }

    # best-effort: descriptive name + PNPDeviceID, keyed by COMx
    $info = @{}
    try {
        Get-CimInstance Win32_PnPEntity -ErrorAction Stop |
            Where-Object { $_.Name -match '\((COM\d+)\)' } |
            ForEach-Object {
                # re-match inside the loop: $Matches from Where-Object is not
                # guaranteed to still be in scope here
                if ($_.Name -match '\((COM\d+)\)') {
                    $com = $Matches[1]
                    if (-not $info.ContainsKey($com)) {
                        $info[$com] = [pscustomobject]@{ Name = $_.Name; Id = $_.PNPDeviceID }
                    }
                }
            }
    } catch { }

    if ($names.Count -eq 0) {
        # last resort -- WMI, which at least covers legacy COM ports
        try {
            Get-CimInstance Win32_SerialPort -ErrorAction Stop | ForEach-Object {
                $names += $_.DeviceID
                if (-not $info.ContainsKey($_.DeviceID)) {
                    $info[$_.DeviceID] = [pscustomobject]@{ Name = $_.Description; Id = $_.PNPDeviceID }
                }
            }
        } catch { }
    }

    $found = @()
    foreach ($com in ($names | Sort-Object)) {
        $rec = $info[$com]
        if ($rec) {
            $found += [pscustomobject]@{ Port = $com; Name = $rec.Name;           Id = $rec.Id }
        } else {
            $found += [pscustomobject]@{ Port = $com; Name = "(unknown device)";  Id = "" }
        }
    }
    return $found
}

# chips commonly used on these boards, best candidate first
$PREFERRED = @("CH34", "CP21", "CP210", "FT23", "FTDI", "USB-SERIAL", "USB Serial",
               "WCH", "Silicon Labs", "Prolific", "SiFli", "SF32")

$ports = @(Get-SerialPorts)

Write-Host "--- serial ports ---" -ForegroundColor DarkCyan
if ($ports.Count -eq 0) {
    Write-Host "  (none)" -ForegroundColor Yellow
} else {
    foreach ($p in $ports) {
        $tag = ""
        foreach ($k in $PREFERRED) { if ($p.Name -like "*$k*") { $tag = "  <-- likely the board"; break } }
        Write-Host ("  {0,-8} {1}{2}" -f $p.Port, $p.Name, $tag)
    }
}

if ($List) { Write-Host ""; exit 0 }

# --------------------------------------------------------- 2. choose the port
# Deliberately written with plain foreach loops and @() guards everywhere.
# A pipeline expression such as
#     $ranked = $ports | Where-Object { ... }
# collapses to a bare scalar when it matches exactly one item, and the
# Where-Object scriptblock runs in its own scope, so a local flag assigned
# inside its inner foreach is not something to bet the whole script on.
$chosen = $null
$ranked = @()

if ($Port -ne "") {
    foreach ($p in $ports) { if ($p.Port -ieq $Port) { $ranked += $p } }
    if ($ranked.Count -eq 0) {
        Say ""
        Say "[ERROR] $Port is not present." Red
        Say "        unplug/replug the board, or run: .\flash.ps1 -List" DarkGray
        exit 3
    }
    $chosen = $ranked[0]
} else {
    foreach ($p in $ports) {
        $n = [string]$p.Name
        foreach ($k in $PREFERRED) {
            if ($n -like "*$k*") { $ranked += $p; break }
        }
    }
    if ($ranked.Count -ge 1) {
        $chosen = $ranked[0]
    } elseif ($ports.Count -eq 1) {
        $chosen = $ports[0]
    } elseif ($ports.Count -gt 1) {
        Say ""
        Say "[ERROR] several ports and none looks like the board -- pick one explicitly:" Yellow
        foreach ($p in $ports) { Say "          .\flash.ps1 -Port $($p.Port)" DarkGray }
        exit 3
    }
}

Say ("  [sel] {0} port(s), {1} look like the board, using: {2}" -f `
        $ports.Count, $ranked.Count, $(if ($chosen) { $chosen.Port } else { "(none)" })) DarkGray

if (-not $chosen) {
    Say ""
    Say "[NOT CONNECTED] no serial port found." Yellow
    Say "  Plug the board's own USB port into the PC, then run this again." DarkGray
    Say "  If the port still does not appear, the USB-serial driver is missing:" DarkGray
    Say "    CH340/CH343 -> CH341SER driver      CP210x -> Silicon Labs VCP driver" DarkGray
    Say "    FT232       -> FTDI VCP driver      (Device Manager -> Ports)" DarkGray
    exit 3
}

Say ""
Say ("  port : {0}  ({1})" -f $chosen.Port, $chosen.Name) Green

# ------------------------------------------------- 2b. pre-flight port check
# Cheapest possible failure detector: if the port cannot be opened right now,
# something else owns it and sftool will die halfway through with a message
# that does not say so.  Catch it here instead.
try {
    $probe = New-Object System.IO.Ports.SerialPort $chosen.Port, 9600
    $probe.ReadTimeout = 200
    $probe.Open()
    $probe.Close()
    $probe.Dispose()
} catch {
    Say ""
    Say ("[ERROR] {0} exists but cannot be opened -- another program holds it." -f $chosen.Port) Red
    Say ("        {0}" -f $_.Exception.Message) DarkGray
    Say "        close any serial monitor / PuTTY / Arduino IDE / this project's" DarkGray
    Say "        own serial.ps1, then run this again." DarkGray
    exit 6
}

# ------------------------------------------------------ 3. firmware freshness
$bins = @(
    (Join-Path $BUILD "main.bin"),
    (Join-Path $BUILD "bootloader\bootloader.bin"),
    (Join-Path $BUILD "ftab\ftab.bin")
)
$missing = @()
foreach ($b in $bins) { if (-not (Test-Path $b)) { $missing += (Split-Path $b -Leaf) } }
if ($missing.Count -gt 0) {
    Say ""
    Say ("[ERROR] firmware not built yet, missing: {0}" -f ($missing -join ", ")) Red
    Say "        run first:  .\build.ps1" DarkGray
    exit 4
}

$mb = Get-Item $bins[0]
$newer = Get-ChildItem $SRC -Recurse -Include *.c,*.h,*.png -ErrorAction SilentlyContinue |
         Where-Object { $_.LastWriteTime -gt $mb.LastWriteTime }
Say ("  image: main.bin  {0} bytes  built {1}" -f $mb.Length, $mb.LastWriteTime)
if ($newer.Count -gt 0 -and -not $Force) {
    Say ""
    Say ("[STALE] {0} source file(s) changed after this image was built." -f $newer.Count) Yellow
    $newer | Sort-Object LastWriteTime -Descending | Select-Object -First 5 |
        ForEach-Object { Say ("          {0}  {1}" -f $_.LastWriteTime, $_.Name) DarkGray }
    Say "        flashing this will NOT include those edits." Yellow
    Say "        rebuild first:  .\build.ps1     (or pass -Force to flash anyway)" DarkGray
    exit 5
}

# ------------------------------------------------------------ 4. flash it
Push-Location $BUILD
try {
    $argList = @("-p", $chosen.Port, "-c", "SF32LB52", "-m", "nor")
    if ($Baud -ne 1000000) { $argList += @("-b", "$Baud") }
    $argList += @("write_flash")
    if ($Erase) { $argList += "-e" }
    $argList += @(
        "bootloader\bootloader.bin@0x12010000",
        "main.bin@0x12020000",
        "ftab\ftab.bin@0x12000000"
    )

    Say ""
    Say ("  cmd : sftool {0}" -f ($argList -join " ")) DarkGray
    Say ""
    Say "  flashing... do not unplug the board." Yellow
    Write-Host ""

    & $SFTOOL @argList
    $code = $LASTEXITCODE
} finally {
    Pop-Location
}

Write-Host ""
if ($code -eq 0) {
    Say "[OK] flash finished, the device has been reset and should boot now." Green
    Write-Host ""
    Say "  next: set the RTC once so the watch face shows real time" DarkGray
    Say ("        .\serial.ps1 -SetTime        (uses $($chosen.Port), 1000000 baud)") DarkGray
} else {
    Say ("[FAILED] sftool exit code = {0}" -f $code) Red
    Write-Host ""
    Say "  usual causes:" DarkGray
    Say "    - another program holds the port (close any serial monitor first)" DarkGray
    Say "    - the board is not in download mode: hold BOOT, tap RESET, release BOOT" DarkGray
    Say "      (sftool normally does this on its own via DTR/RTS)" DarkGray
    Say "    - a USB-serial module is also feeding power into the board" DarkGray
    Say "    - try a lower speed:  .\flash.ps1 -Baud 921600" DarkGray
}
exit $code
