@echo off
rem Gets the newest Strikers Online from GitHub and installs it over this folder.
rem Keeps strikers.ini, your controls and sound settings. Saves are not in this folder.
rem (Everything runs from the one line below, so this file can be replaced while it runs.)
cd /d "%~dp0" & powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0updater.ps1" %* & exit /b
