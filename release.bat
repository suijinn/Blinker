@echo off
rem Release helper. All logic lives in release.ps1 (no version literal here).
rem   release.bat 0.1.2
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0release.ps1" %*
pause
