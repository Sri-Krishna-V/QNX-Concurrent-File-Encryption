@echo off
title QNX Concurrent Encryption - Live Control Center
echo Starting QNX Live Control Center Backend...
start "" "http://localhost:8080"
python "%~dp0backend_server.py"
pause
