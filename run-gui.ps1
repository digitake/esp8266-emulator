# ESP8266 emulator GUI from Windows: builds and runs the emulator inside WSL, opens http://127.0.0.1:8266/.
# Usage:  .\run-gui.ps1 [-Bin path\firmware.bin] [-Elf path\firmware.elf] [-ProfileFile path\gpio.json] [-Port 8266] [-Distro Ubuntu]
#                       [-NetProfile profile.txt [-NetForward 8080:80,8081:81] [-NetHost NAME=IP,...] [-NetCapture net.pcap]]
# Without -Bin the page opens empty: drop a firmware.bin on it.
# -NetProfile (experimental virtual network) takes a profile made in WSL by scripts/network_profile.py; a path that
# starts with / is used as a WSL path. It needs libslirp-dev, libglib2.0-dev and pkg-config in that distribution.
param(
    [string]$Bin,
    [string]$Elf,
    [string]$ProfileFile,
    [ValidateRange(1, 65535)][int]$Port = 8266,
    [string]$Distro = 'Ubuntu',
    [string]$NetProfile,
    [ValidatePattern('^\d{1,5}:\d{1,5}$')][string[]]$NetForward,
    [ValidatePattern('^[^=]+=[0-9.]+$')][string[]]$NetHost,
    [string]$NetCapture
)
$ErrorActionPreference = 'Stop'

function To-Wsl([string]$p) {
    $full = (Resolve-Path -LiteralPath $p).Path -replace '\\', '/'
    $out = & wsl.exe -d $Distro -- wslpath -a "$full"
    if ($LASTEXITCODE -ne 0 -or -not $out) { throw "wslpath failed for $p" }
    return ($out | Select-Object -First 1).Trim()
}

# Network files may live in WSL already; an output file need not exist yet.
function To-WslNet([string]$p, [switch]$Output) {
    if ($p.StartsWith('/')) { return $p }
    if (-not $Output) { return To-Wsl $p }
    $dir = Split-Path -Parent $p
    if (-not $dir) { $dir = '.' }
    return (To-Wsl $dir) + '/' + (Split-Path -Leaf $p)
}

$here = To-Wsl $PSScriptRoot
$argv = @("$here/run-gui.sh", '--port', "$Port")
if ($Bin) { $argv += @('--bin', (To-Wsl $Bin)) }
if ($Elf) { $argv += @('--elf', (To-Wsl $Elf)) }
if ($ProfileFile) { $argv += @('--profile-file', (To-Wsl $ProfileFile)) }
if ($NetProfile) { $argv += @('--net-profile', (To-WslNet $NetProfile)) }
foreach ($f in $NetForward) { $argv += @('--net-forward', $f) }
foreach ($h in $NetHost) { $argv += @('--net-host', $h) }
if ($NetCapture) { $argv += @('--net-capture', (To-WslNet $NetCapture -Output)) }

$url = "http://127.0.0.1:$Port/"
Write-Host "Starting the emulator in WSL ($Distro); the GUI opens at $url. Ctrl+C stops it."
# open the browser once the server answers
$browserJob = Start-Job -ScriptBlock {
    param($u)
    for ($i = 0; $i -lt 60; $i++) {
        try { Invoke-WebRequest -UseBasicParsing -Uri ($u + 'api/state') -TimeoutSec 1 | Out-Null; Start-Process $u; return } catch { Start-Sleep -Milliseconds 500 }
    }
} -ArgumentList $url
$emulatorExitCode = 1
try {
    & wsl.exe -d $Distro -- bash @argv
    $emulatorExitCode = $LASTEXITCODE
} finally {
    Stop-Job -Job $browserJob -ErrorAction SilentlyContinue
    Remove-Job -Job $browserJob -Force -ErrorAction SilentlyContinue
}
exit $emulatorExitCode
