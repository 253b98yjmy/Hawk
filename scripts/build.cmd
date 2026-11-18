@echo off
rem Usage: scripts\build.cmd [debug^|release^|fuzz]
rem Configures, builds and (except for fuzz) tests the given preset. Default: debug.
setlocal
set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=debug"

if not defined VCToolsInstallDir (
    call :vcvars || exit /b 1
)

cmake --preset %PRESET% || exit /b 1
cmake --build --preset %PRESET% || exit /b 1
if /i not "%PRESET%"=="fuzz" (
    ctest --preset %PRESET% || exit /b 1
)
exit /b 0

:vcvars
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo vswhere.exe not found. Install Visual Studio Build Tools with the C++ workload.
    exit /b 1
)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo MSVC x64 tools not found.
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
exit /b %errorlevel%
