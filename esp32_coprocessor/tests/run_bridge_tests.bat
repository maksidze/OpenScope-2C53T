@echo off
setlocal EnableExtensions DisableDelayedExpansion
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo MSVC Build Tools not found. Use run_bridge_tests.py with GCC, Clang or --zig.
    exit /b 2
)
set "BRIDGE_VS="
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "BRIDGE_VS=%%I"
if not defined BRIDGE_VS exit /b 2
call "%BRIDGE_VS%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
python "%~dp0run_bridge_tests.py" --msvc
exit /b %errorlevel%
