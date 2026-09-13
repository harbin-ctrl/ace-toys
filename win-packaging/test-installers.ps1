# Tests the toys' installers on this machine: install, upgrade while running,
# uninstall, and the x64 build. Run after `make inno` at the repository
# root:
#
#   powershell -ExecutionPolicy Bypass -File win-packaging\test-installers.ps1 [-Toys poingo,splat]
#
# Each toy tested is left installed and stopped.
param(
    [string[]]$Toys = @('poingo', 'splat', 'balloons')
)
$ErrorActionPreference = 'Stop'

# -File passes "poingo,splat" as one string.
$Toys = @($Toys -split ',' | Where-Object { $_ })
$unknown = $Toys | Where-Object { $_ -notin 'poingo', 'splat', 'balloons' }
if ($unknown) {
    throw "Unknown toy: $($unknown -join ', '). Use poingo, splat or balloons."
}

$repo = Split-Path -Parent $PSScriptRoot
$work = Join-Path $env:TEMP 'win-toys-test'
$iscc = "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
$names = @{ poingo = 'Poingo'; splat = 'Splat'; balloons = 'Balloons' }
$native = if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64' } else { 'x64' }
$script:failures = 0

function Check([string]$what, [bool]$ok) {
    if ($ok) { "  ok    $what" } else { "  FAIL  $what"; $script:failures++ }
}

# arm64 or x64, from the PE header.
function Machine([string]$file) {
    $bytes = [IO.File]::ReadAllBytes($file)
    $pe = [BitConverter]::ToInt32($bytes, 0x3C)
    switch ([BitConverter]::ToUInt16($bytes, $pe + 4)) {
        0xAA64 { 'arm64' }
        0x8664 { 'x64' }
        default { 'other' }
    }
}

function Wait-Until([scriptblock]$condition, [int]$seconds = 20) {
    $deadline = (Get-Date).AddSeconds($seconds)
    do {
        if (& $condition) { return $true }
        Start-Sleep -Milliseconds 250
    } while ((Get-Date) -lt $deadline)
    return $false
}

function App([string]$toy) { Get-Process -Name $toy -ErrorAction SilentlyContinue }

function Stop-App([string]$toy) {
    App $toy | Stop-Process -Force -ErrorAction SilentlyContinue
    $null = Wait-Until { -not (App $toy) } 5
}

# Waits for the setup program alone; the toy its last page launches keeps running.
function Invoke-Setup([string]$exe) {
    $process = Start-Process -FilePath $exe -PassThru `
        -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/CLOSEAPPLICATIONS'
    $process.WaitForExit()
    $process.ExitCode
}

function Get-UninstallEntry([string]$name) {
    Get-ChildItem HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall |
        Get-ItemProperty -ErrorAction SilentlyContinue |
        Where-Object { $_.DisplayName -eq $name -and $_.InstallLocation -like '*\Ace\*' }
}

function Test-Toy([string]$toy) {
    $toy
    $name = $names[$toy]
    $setup = Get-ChildItem "$repo\installer\$toy-*-setup.exe" -ErrorAction SilentlyContinue |
             Sort-Object LastWriteTime | Select-Object -Last 1
    if (-not $setup) {
        Check 'installer built' $false
        return
    }
    $dir = "$env:LOCALAPPDATA\Programs\Ace\$toy"
    $exe = "$dir\$toy.exe"
    $menu = Join-Path ([Environment]::GetFolderPath('Programs')) "Ace\$name.lnk"

    Stop-App $toy
    Check 'installer exits 0' ((Invoke-Setup $setup.FullName) -eq 0)
    Check 'installer launches it' (Wait-Until { [bool](App $toy) })
    Check "installed program is $native" ((Machine $exe) -eq $native)
    Check 'Start menu shortcut in Ace' (Test-Path -LiteralPath $menu)
    Check 'listed in installed apps' ([bool](Get-UninstallEntry $name))

    $old = @(App $toy).Id
    Check 'upgrade while running exits 0' ((Invoke-Setup $setup.FullName) -eq 0)
    Check 'upgrade replaces the running toy' (Wait-Until {
        $now = @(App $toy).Id
        $now.Count -gt 0 -and -not ($now | Where-Object { $_ -in $old })
    })

    Stop-App $toy
    Check 'uninstaller exits 0' ((Invoke-Setup "$dir\unins000.exe") -eq 0)
    Check 'program removed' (Wait-Until { -not (Test-Path $dir) })
    Check 'shortcut removed' (-not (Test-Path -LiteralPath $menu))
    Check 'removed from installed apps' (-not (Get-UninstallEntry $name))

    # The x64 build, whatever this machine is.
    $version = $setup.BaseName -replace "^$toy-(.*)-setup$", '$1'
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    & $iscc /Q "/DToy=$toy" "/DName=$name" "/DAppVersion=$version" "/DStage=$repo\installer\stage\$toy" `
        "/DIcon=$repo\$toy\$toy.ico" /DForceArch=x64 "/O$work" "/F$toy-x64-test" "$repo\win-packaging\toy.iss"
    Check 'x64 test installer builds' ($LASTEXITCODE -eq 0)
    Check 'x64 install exits 0' ((Invoke-Setup "$work\$toy-x64-test.exe") -eq 0)
    $files = @(Get-ChildItem "$dir\*" -Include *.exe, *.dll | Where-Object { $_.Name -notlike 'unins*' })
    Check 'every installed program file is x64' ($files.Count -ge 2 -and
        -not ($files | Where-Object { (Machine $_.FullName) -ne 'x64' }))
    Check 'x64 build runs' (Wait-Until { [bool](App $toy) })
    Start-Sleep 8
    Check '  and keeps running' ([bool](App $toy))

    Stop-App $toy
    Check 'reinstall exits 0' ((Invoke-Setup $setup.FullName) -eq 0)
    $null = Wait-Until { [bool](App $toy) }
    Stop-App $toy
    Check "installed program is $native again" ((Machine $exe) -eq $native)
}

if (-not (Test-Path $iscc)) {
    throw "Inno Setup 6 not found at $iscc"
}
foreach ($toy in $Toys) {
    Test-Toy $toy
}
if ($script:failures) {
    "$($script:failures) failed"
    exit 1
}
'all passed'
