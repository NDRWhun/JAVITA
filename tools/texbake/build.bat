@echo off
rem Builds texbake.exe with the Visual Studio C++ tools.
setlocal
set "HERE=%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=C:\Program Files\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto novs

"%VSWHERE%" -latest -property installationPath > "%TEMP%\texbake_vs.txt" 2>nul
set "VSPATH="
set /p VSPATH=<"%TEMP%\texbake_vs.txt"
del "%TEMP%\texbake_vs.txt" >nul 2>&1
if not defined VSPATH goto novs
if not exist "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" goto novs

rem vcvars prints a harmless vswhere complaint of its own, so both streams go quiet
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 goto novs

if not exist "%HERE%obj" mkdir "%HERE%obj"

cl /nologo /O2 /EHsc /MT /std:c++17 /W3 /wd4996 ^
   "%HERE%texbake.cpp" "%HERE%..\..\src\code\rd-common\tr_dxt.cpp" ^
   /Fe:"%HERE%texbake.exe" /Fo:"%HERE%obj\\" ^
   /link ole32.lib windowscodecs.lib
if errorlevel 1 goto failed

echo.
echo   built %HERE%texbake.exe
exit /b 0

:novs
echo.
echo   Visual Studio with the C++ tools was not found.
echo   Install "Desktop development with C++" from the Visual Studio Installer.
exit /b 1

:failed
echo.
echo   build failed
exit /b 1
