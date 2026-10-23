# ESP8266 emulator GUI from Windows: builds and runs the emulator inside WSL, opens http://127.0.0.1:8266/.
# Usage:  .\run-gui.ps1 [-Bin path\firmware.bin] [-Elf path\firmware.elf] [-ProfileFile path\gpio.json] [-Port 8266]
#                       [-Distro Ubuntu] [-Offline | -Internet] [-NetProfile file] [-NetForward 8080:80,...]
#                       [-NetHost name=ip,...] [-NetCapture file] [-Workdir dir] [-NoBrowser]
#         .\run-gui.ps1 -Install     adds the esp8266emu command (runs emulator.cmd from any terminal)
# Without -Bin the page opens empty: drop a firmware.bin (with its .elf) on it.
# A .bin dropped with its .elf gets the experimental virtual network when WSL has libslirp: local only, every name
# leads to this PC. -Internet lets it reach the internet, -Offline turns the network off.
# -NetProfile instead runs only the images a network profile binds; its paths are WSL paths if they start with /.
param(
    [string]$Bin,
    [string]$Elf,
    [string]$ProfileFile,
    [ValidateRange(1, 65535)][int]$Port = 8266,
    [string]$Distro = 'Ubuntu',
    [switch]$Offline,
    [switch]$Internet,
    [string]$NetProfile,
    [string[]]$NetForward,
    [string[]]$NetHost,
    [string]$NetCapture,
    [string]$Workdir,
    [switch]$NoBrowser,
    [switch]$Install
)
$ErrorActionPreference = 'Stop'

if ($Install) {
    $apps = Join-Path $env:LOCALAPPDATA 'Microsoft\WindowsApps'
    $shim = Join-Path $apps 'esp8266emu.cmd'
    Set-Content -LiteralPath $shim -Encoding ascii -Value @('@echo off', "call `"$PSScriptRoot\emulator.cmd`" %*")
    Write-Host "Installed $shim"
    if (($env:Path -split ';') -notcontains $apps) {
        Write-Host "$apps is not on your PATH; add it, or start emulator.cmd directly."
    } else {
        Write-Host 'Open a new terminal and type esp8266emu to start the emulator.'
    }
    exit 0
}

function To-Wsl([string]$p) {
    $full = (Resolve-Path -LiteralPath $p).Path -replace '\\', '/'
    $out = & wsl.exe -d $Distro -- wslpath -a "$full"
    if ($LASTEXITCODE -ne 0 -or -not $out) { throw "wslpath failed for $p" }
    return ($out | Select-Object -First 1).Trim()
}

# Network files may live in WSL already (a path that starts with /); an output file need not exist yet.
function To-WslNet([string]$p, [switch]$Output) {
    if ($p.StartsWith('/')) { return $p }
    if (-not $Output) { return To-Wsl $p }
    $dir = Split-Path -Parent $p
    if (-not $dir) { $dir = '.' }
    return (To-Wsl $dir) + '/' + (Split-Path -Leaf $p)
}

# One option per value; through emulator.cmd a list arrives as one comma-separated string: -NetForward 8080:80,8081:81
function Split-List([string[]]$values, [string]$pattern, [string]$example) {
    $items = @($values | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    foreach ($item in $items) { if ($item -notmatch $pattern) { throw "invalid value '$item': expected $example" } }
    return $items
}

function Test-PortInUse([int]$p) {
    $client = New-Object System.Net.Sockets.TcpClient
    try { $client.Connect('127.0.0.1', $p); return $true } catch { return $false } finally { $client.Dispose() }
}

$url = "http://127.0.0.1:$Port/"
if (Test-PortInUse $Port) {
    $isGui = $false
    try { $isGui = (Invoke-WebRequest -UseBasicParsing -Uri ($url + 'api/state') -TimeoutSec 2).StatusCode -eq 200 } catch { }
    if ($isGui -and -not $Bin) {
        Write-Host "The emulator GUI is already running at $url"
        if (-not $NoBrowser) { Start-Process $url }
        exit 0
    }
    Write-Host "Port $Port is already in use$(if ($isGui) { ' by another emulator GUI' })."
    Write-Host "Start this one on another port, for example:  emulator.cmd -Port 8276"
    exit 1
}

$here = To-Wsl $PSScriptRoot
$argv = @("$here/run-gui.sh", '--port', "$Port")
if ($Bin) { $argv += @('--bin', (To-Wsl $Bin)) }
if ($Elf) { $argv += @('--elf', (To-Wsl $Elf)) }
if ($ProfileFile) { $argv += @('--profile-file', (To-Wsl $ProfileFile)) }
if ($Workdir) { $argv += @('--workdir', (To-WslNet $Workdir -Output)) }
if ($Offline) { $argv += '--offline' }
if ($Internet) { $argv += '--internet' }
if ($NetProfile) { $argv += @('--net-profile', (To-WslNet $NetProfile)) }
foreach ($f in (Split-List $NetForward '^\d{1,5}:\d{1,5}$' 'HOST_PORT:GUEST_PORT')) { $argv += @('--net-forward', $f) }
foreach ($h in (Split-List $NetHost '^[^=]+=[0-9.]+$' 'NAME=IP')) { $argv += @('--net-host', $h) }
if ($NetCapture) { $argv += @('--net-capture', (To-WslNet $NetCapture -Output)) }

Write-Host "Starting the emulator in WSL ($Distro); the GUI opens at $url. Ctrl+C stops it."
# open the browser once the server answers
$browserJob = $null
if (-not $NoBrowser) {
    $browserJob = Start-Job -ScriptBlock {
        param($u)
        for ($i = 0; $i -lt 240; $i++) {
            try { Invoke-WebRequest -UseBasicParsing -Uri ($u + 'api/state') -TimeoutSec 1 | Out-Null; Start-Process $u; return } catch { Start-Sleep -Milliseconds 500 }
        }
    } -ArgumentList $url
}
$emulatorExitCode = 1
try {
    & wsl.exe -d $Distro -- bash @argv
    $emulatorExitCode = $LASTEXITCODE
} finally {
    if ($browserJob) {
        Stop-Job -Job $browserJob -ErrorAction SilentlyContinue
        Remove-Job -Job $browserJob -Force -ErrorAction SilentlyContinue
    }
}
exit $emulatorExitCode
