@echo off
setlocal EnableExtensions
cd /d "%~dp0"
title Locus Classroom Demo

:MENU
cls
echo ============================================================
echo              LOCUS CLASSROOM MESH DEMO
echo ============================================================
echo.
echo  1. Host a shared demo for classmates on this Wi-Fi/LAN
echo  2. Join a demo someone else is hosting
echo  3. Exit
echo.
set /p "CHOICE=Choose 1, 2, or 3: "
if "%CHOICE%"=="1" goto HOST
if "%CHOICE%"=="2" goto JOIN
if "%CHOICE%"=="3" exit /b 0
echo Invalid choice.
pause
goto MENU

:HOST
echo.
where py >nul 2>nul
if not errorlevel 1 (
  py -3 server.py
  goto END
)
where python >nul 2>nul
if not errorlevel 1 (
  python server.py
  goto END
)
echo Python 3 was not found on this computer.
echo Install Python 3 from https://www.python.org/downloads/windows/
echo During installation, enable the option to add Python to PATH.
pause
goto MENU

:JOIN
set "HOSTURL="
echo.
echo Ask the host to copy one complete LAN URL from their black server window.
echo Example: http://192.168.1.23:8765/
set /p "HOSTURL=Paste the complete URL here: "
if "%HOSTURL%"=="" goto MENU
start "Locus Shared Demo" "%HOSTURL%"
goto MENU

:END
echo.
echo Locus demo server has stopped.
pause
endlocal
