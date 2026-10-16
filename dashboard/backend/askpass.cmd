@echo off
rem ssh runs this (SSH_ASKPASS) when it needs the Pi password; the backend sets ENC_PI_PASSWORD.
echo %ENC_PI_PASSWORD%
