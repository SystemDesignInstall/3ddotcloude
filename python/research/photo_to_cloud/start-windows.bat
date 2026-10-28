@echo off
setlocal
cd /d "%~dp0"

if not exist ".venv\Scripts\python.exe" (
  py -3.13 -m venv .venv
  if errorlevel 1 py -3.12 -m venv .venv
  if errorlevel 1 py -3.11 -m venv .venv
  if errorlevel 1 goto python_error
)

".venv\Scripts\python.exe" -m pip install -r requirements.txt
if errorlevel 1 goto install_error

".venv\Scripts\python.exe" app.py
if errorlevel 1 goto app_error
exit /b 0

:python_error
echo Python 3.11, 3.12 or 3.13 was not found. Install 64-bit Python and its launcher.
pause
exit /b 1

:install_error
echo Could not install dependencies. Check the internet connection and try again.
pause
exit /b 1

:app_error
echo Could not start the application. Review the error above.
pause
exit /b 1
