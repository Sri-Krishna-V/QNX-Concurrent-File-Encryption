@echo off
rem Start the QNX encryption dashboard: double-click this file, or run it from a command prompt.
title QNX encryption dashboard - backend
cd /d "%~dp0"

rem "python" can be the Microsoft Store stub; prefer the py launcher, then a real python.
set "PYCMD="
where py >nul 2>nul && set "PYCMD=py -3"
if not defined PYCMD (
    python -c "import sys" >nul 2>nul && set "PYCMD=python"
)
if not defined PYCMD (
    echo No working Python 3 found. Install it from python.org, or turn off the
    echo "python.exe" App execution alias in Settings ^> Apps ^> Advanced app settings.
    pause
    exit /b 1
)

start "" "http://localhost:8080"
%PYCMD% -u backend\server.py
pause
