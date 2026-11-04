@echo off
rem One-time setup: creates engine\.venv, installs dependencies, downloads the face model.
cd /d "%~dp0"
where py >nul 2>nul && (set "PY=py -3") || (set "PY=python")
echo Creating virtual environment...
%PY% -m venv .venv || goto :fail
".venv\Scripts\python.exe" -m pip install --upgrade pip || goto :fail
".venv\Scripts\python.exe" -m pip install -r requirements.txt || goto :fail
echo Downloading face and eye models...
".venv\Scripts\python.exe" -c "import analyzer; analyzer.ensure_model(); analyzer.ensure_eye_model()" || goto :fail
echo.
echo SayCheese engine is set up. Run start.bat to launch it.
exit /b 0

:fail
echo.
echo Setup failed. Make sure Python 3.10+ is installed (python.org) and try again.
pause
exit /b 1
