@echo off
rem Removes every toy in this package for the current user. See README.txt.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -Action UninstallAll
if /i not "%~1"=="/quiet" pause
