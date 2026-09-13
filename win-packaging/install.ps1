# Installs or removes the win-toys package for the current user: the Windows
# counterpart of the ace-toys Debian package.
#
#   %LOCALAPPDATA%\Programs\win-toys\     every toy's .exe, and the MSYS2
#                                         runtime DLLs they share
#   Start menu\Programs\Ace\<Name>.lnk    one shortcut per toy, in the Ace
#                                         folder, as Linux has an Ace menu
#
# Per-user, so no administrator rights are needed.
#
#   -Action InstallAll / UninstallAll   every toy listed in toys.txt beside
#                                       this script (the package; install.cmd)
#   -Action Install / Uninstall         one toy from a build tree
#                                       (win-packaging/install.mk)
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("Install", "Uninstall", "InstallAll", "UninstallAll")]
    [string]$Action,
    [string]$Toy = "",
    [string]$Name = "",
    [string]$Exe = "",
    [string]$Dlls = ""
)
$ErrorActionPreference = "Stop"

$Package = "win-toys"
$MenuFolder = "Ace"
$ToyList = "toys.txt"      # one "toy|Start menu name" line per toy

$root = Join-Path ([Environment]::GetFolderPath("LocalApplicationData")) "Programs\$Package"
$menu = Join-Path ([Environment]::GetFolderPath("Programs")) $MenuFolder

function Install-Toy([string]$toy, [string]$name, [string]$exe, [string[]]$dlls) {
    $target = Join-Path $root "$toy.exe"
    $running = Get-Process -Name $toy -ErrorAction SilentlyContinue |
               Where-Object { $_.Path -eq $target }
    if ($running) {
        throw "$name is running; quit it, then install again"
    }

    New-Item -ItemType Directory -Force -Path $root | Out-Null
    New-Item -ItemType Directory -Force -Path $menu | Out-Null
    Copy-Item -Force -Path $exe -Destination $target
    foreach ($dll in $dlls) {
        Copy-Item -Force -Path $dll -Destination $root
    }

    $shortcut = Join-Path $menu "$name.lnk"
    $shell = New-Object -ComObject WScript.Shell
    $link = $shell.CreateShortcut($shortcut)
    $link.TargetPath = $target
    $link.WorkingDirectory = $root
    $link.IconLocation = "$target,0"
    $link.Save()

    "installed ${name}: $target"
    "start menu:    $shortcut"
}

function Uninstall-Toy([string]$toy, [string]$name) {
    $shortcut = Join-Path $menu "$name.lnk"
    $target = Join-Path $root "$toy.exe"
    Remove-Item -Force -ErrorAction SilentlyContinue -Path $shortcut, $target

    # The DLLs and both folders are shared; they go with the last toy.
    if (-not (Get-ChildItem -ErrorAction SilentlyContinue -Path $root -Filter *.exe)) {
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue -Path $root
    }
    if (-not (Get-ChildItem -ErrorAction SilentlyContinue -Path $menu)) {
        Remove-Item -Force -ErrorAction SilentlyContinue -Path $menu
    }
    "removed ${name}"
}

function Read-ToyList {
    Get-Content (Join-Path $PSScriptRoot $ToyList) | Where-Object { $_ } | ForEach-Object {
        $fields = $_.Split("|")
        [pscustomobject]@{ Toy = $fields[0]; Name = $fields[1] }
    }
}

function Assert-Toy {
    if (-not $Toy -or -not $Name) {
        throw "-Toy and -Name are required for -Action $Action"
    }
}

switch ($Action) {
    "Install" {
        Assert-Toy
        if (-not $Exe) {
            throw "-Exe is required for -Action Install"
        }
        Install-Toy $Toy $Name $Exe @($Dlls.Split(";") | Where-Object { $_ })
    }
    "Uninstall" {
        Assert-Toy
        Uninstall-Toy $Toy $Name
    }
    "InstallAll" {
        $packageDlls = @(Get-ChildItem -Path $PSScriptRoot -Filter *.dll | ForEach-Object { $_.FullName })
        foreach ($entry in Read-ToyList) {
            Install-Toy $entry.Toy $entry.Name (Join-Path $PSScriptRoot "$($entry.Toy).exe") $packageDlls
        }
    }
    "UninstallAll" {
        foreach ($entry in Read-ToyList) {
            Uninstall-Toy $entry.Toy $entry.Name
        }
    }
}
