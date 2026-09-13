@echo off
rem Installs every toy in this package for the current user. See README.txt.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -Action InstallAll
if /i not "%~1"=="/quiet" pause
