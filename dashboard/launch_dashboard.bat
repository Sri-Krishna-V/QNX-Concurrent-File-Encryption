@echo off
title QNX Concurrent Encryption Dashboard
echo Starting QNX Concurrent Encryption Dashboard...
start "" "http://localhost:8080"
python -m http.server 8080 --directory "%~dp0"
pause
