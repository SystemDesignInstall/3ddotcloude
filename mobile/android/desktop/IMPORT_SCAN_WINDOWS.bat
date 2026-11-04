@echo off
chcp 65001 >nul
setlocal
if "%~1"=="" (
  echo Перетащите ZIP со сканом на этот файл.
  pause
  exit /b 1
)
py -3 "%~dp0import_scan.py" "%~1" "%~dpn1_imported"
if errorlevel 1 (
  echo Ошибка. Требуется Python 3.10 или новее. Исходный ZIP не изменён.
) else (
  echo Готово. Облако cloud.ply лежит в папке рядом с ZIP.
)
pause
