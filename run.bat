@echo off
rem Runs the game under the debugger harness with the stub DLLs on PATH. Kills it after 60 s.
rem Usage: run.bat "X:\path\to\game_scarlett_master.exe"   (or set GTA6_GAME_EXE)
rem Logs: logs\harness.log (events) and logs\stubs.log (Xbox API calls the game made)
set "GAME=%~1"
if "%GAME%"=="" set "GAME=%GTA6_GAME_EXE%"
if "%GAME%"=="" ( echo Pass the path to game_scarlett_master.exe & exit /b 1 )
if not exist "%~dp0logs" mkdir "%~dp0logs"
"%~dp0bin\harness.exe" "%GAME%" "%~dp0bin\stubs" "%~dp0logs" 60 run -skip
pause
