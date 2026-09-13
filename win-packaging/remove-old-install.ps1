# Removes a toy installed before the installers, by the win-toys zip or
# `make install`: %LOCALAPPDATA%\Programs\win-toys\<Toy>.exe and
# Start menu\Programs\Ace\<Name>.lnk. Does nothing when there is no such copy.
param(
    [Parameter(Mandatory = $true)][string]$Toy,
    [Parameter(Mandatory = $true)][string]$Name
)
$ErrorActionPreference = "Stop"

$root = Join-Path ([Environment]::GetFolderPath("LocalApplicationData")) "Programs\win-toys"
$exe = Join-Path $root "$Toy.exe"
if (-not (Test-Path $exe)) {
    return
}

Get-Process -Name $Toy -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -eq $exe } |
    Stop-Process -Force
Start-Sleep -Seconds 1

$menu = Join-Path ([Environment]::GetFolderPath("Programs")) "Ace"
Remove-Item -Force -ErrorAction SilentlyContinue -Path (Join-Path $menu "$Name.lnk"), $exe

# The DLLs and the folder were shared; they go with the last toy.
if (-not (Get-ChildItem -Path $root -Filter *.exe)) {
    Remove-Item -Recurse -Force -Path $root
}
if (-not (Get-ChildItem -ErrorAction SilentlyContinue -Path $menu)) {
    Remove-Item -Force -ErrorAction SilentlyContinue -Path $menu
}
"removed old ${Name}: $exe"
