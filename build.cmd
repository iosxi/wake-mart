@echo off
rem Build wake-mart.exe with MSVC (Visual Studio Build Tools 2022)
rem
rem %ProgramFiles(x86)% contains ")" so it must not appear inside a ( ... ) block.
rem vcvars64.bat itself prints "'vswhere.exe' is not recognized ..." here; it is harmless.
setlocal
cd /d "%~dp0"
if defined VCINSTALLDIR goto build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto novs
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

:build
if not exist build mkdir build
rc /nologo /fo build\wakemart.res wakemart.rc || exit /b 1
cl /nologo /utf-8 /std:c17 /O2 /W4 /MT /DUNICODE /D_UNICODE /Fobuild\ /Fdbuild\ wakemart.c build\wakemart.res /link /SUBSYSTEM:WINDOWS /OUT:wake-mart.exe || exit /b 1
echo built wake-mart.exe
exit /b 0

:novs
echo MSVC not found
exit /b 1
