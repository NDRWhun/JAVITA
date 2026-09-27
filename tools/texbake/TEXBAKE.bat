@echo off
rem Double-click me. Builds the tool if needed, then runs it; a folder dropped on me is used as the game folder.
setlocal
title Vita texture pre-compressor
cd /d "%~dp0"

if exist "texbake.exe" goto haveexe
echo.
echo   Building the tool, this happens once...
call build.bat
if not exist "texbake.exe" goto done

:haveexe
if "%~1"=="" (
	texbake.exe
) else (
	texbake.exe "%~1"
)

:done
echo.
pause
