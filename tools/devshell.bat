@echo off
rem Opens a PowerShell 7 window with the wwe2k14-recomp build environment loaded (see tools\env.ps1).
rem -ExecutionPolicy Bypass applies to this window only; no system setting is changed.
pwsh -NoExit -ExecutionPolicy Bypass -Command ". '%~dp0env.ps1'; Set-Location '%~dp0..'"
