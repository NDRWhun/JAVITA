@echo off
rem Double-click me. Builds the tool if needed, then bakes the texture cache.
setlocal
title Vita texture pre-compressor
cd /d "%~dp0"

if exist "texbake.exe" goto haveexe
echo.
echo   Building the tool, this happens once...
call build.bat
if not exist "texbake.exe" goto done

:haveexe
if not "%~1"=="" goto dropped

echo.
echo   Press Enter and I will look for the game myself,
echo   or drag the folder that holds assets0.pk3 into this window first.
echo.
set "BASE="
set /p BASE=   folder:
if not defined BASE goto auto
set "BASE=%BASE:"=%"
texbake.exe "%BASE%"
goto done

:auto
texbake.exe
goto done

:dropped
texbake.exe "%~1"

:done
echo.
pause
