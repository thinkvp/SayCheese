@echo off
cd /d "%~dp0"
if not exist ".venv\Scripts\python.exe" (
  call "%~dp0setup.bat" || exit /b 1
)
title SayCheese engine
".venv\Scripts\python.exe" server.py %*
if errorlevel 1 pause
