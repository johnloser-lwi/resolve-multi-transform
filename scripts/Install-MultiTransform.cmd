@echo off
REM Double-click this to build and install the Multi Transform OFX plugin into
REM C:\Program Files\Common Files\OFX\Plugins.
REM
REM It builds first (as you), then prompts for administrator rights for the copy.
REM
REM -Force is deliberately NOT passed: it would close DaVinci Resolve for you.
REM If Resolve has a previous build locked, the script says so and you can close
REM it yourself -- better than having an installer shut down your editor.

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -Build
