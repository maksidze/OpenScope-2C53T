@echo off
setlocal EnableExtensions DisableDelayedExpansion

rem OpenScope Windows build wrapper.
rem It deliberately invokes GNU Make from MSYS2 instead of an unrelated
rem Embarcadero make.exe that may appear first in PATH.

cd /d "%~dp0"

set "GNU_MAKE=C:\msys64\usr\bin\make.exe"
set "MSYS_BIN=C:\msys64\usr\bin"

if not exist "%GNU_MAKE%" (
    echo ERROR: GNU Make was not found at:
    echo   %GNU_MAKE%
    echo Install MSYS2 or edit GNU_MAKE and MSYS_BIN in this script.
    exit /b 2
)

set "ARM_GCC_EXE="
if exist "C:\Program Files (x86)\Arm\GNU Toolchain mingw-w64-i686-arm-none-eabi\bin\arm-none-eabi-gcc.exe" (
    set "ARM_GCC_EXE=C:\Program Files (x86)\Arm\GNU Toolchain mingw-w64-i686-arm-none-eabi\bin\arm-none-eabi-gcc.exe"
)

if not defined ARM_GCC_EXE (
    for /f "delims=" %%I in ('where arm-none-eabi-gcc.exe 2^>nul') do if not defined ARM_GCC_EXE set "ARM_GCC_EXE=%%I"
)

if not defined ARM_GCC_EXE (
    echo ERROR: arm-none-eabi-gcc.exe was not found.
    echo Install Arm GNU Toolchain or add its bin directory to PATH.
    exit /b 3
)

for %%I in ("%ARM_GCC_EXE%") do set "ARM_GCC_BIN=%%~dpI"

if not exist "lvgl\lvgl.h" (
    echo ERROR: firmware\lvgl is missing.
    echo Clone the pinned LVGL release first:
    echo   git clone --branch v9.5.0 --depth 1 https://github.com/lvgl/lvgl.git lvgl
    exit /b 4
)

if not exist "FreeRTOS\include\FreeRTOS.h" (
    echo ERROR: firmware\FreeRTOS is missing. Initialize the project submodules.
    exit /b 5
)

if not exist "at32f403a_lib\libraries\cmsis\cm4\device_support\at32f403a_407.h" (
    echo ERROR: firmware\at32f403a_lib is missing. Initialize the project dependencies.
    exit /b 6
)

rem The Makefile uses its POSIX branch when OS is unset. MSYS2 supplies sh,
rem find and the other commands required by that branch.
set "OS="
set "PATH=%MSYS_BIN%;%ARM_GCC_BIN%;%PATH%"

set "ACTION=%~1"
if not defined ACTION set "ACTION=build"

if /i "%ACTION%"=="build" goto build
if /i "%ACTION%"=="rebuild" goto rebuild
if /i "%ACTION%"=="clean" goto clean
if /i "%ACTION%"=="help" goto usage
if /i "%ACTION%"=="--help" goto usage
if /i "%ACTION%"=="-h" goto usage

echo ERROR: Unknown action "%ACTION%".
goto usage_error

:rebuild
echo [1/2] Cleaning OpenScope build...
"%GNU_MAKE%" clean "FEATURES=FFT ESP_LINK_LVGL ESP_LINK_BRIDGE"
if errorlevel 1 exit /b %errorlevel%
echo [2/2] Building OpenScope with LVGL...
goto run_build

:build
echo Building OpenScope with LVGL...

:run_build
"%GNU_MAKE%" guest "FEATURES=FFT ESP_LINK_LVGL ESP_LINK_BRIDGE"
if errorlevel 1 exit /b %errorlevel%
python "%~dp0tests\check_display_stack_budget.py"
if errorlevel 1 exit /b %errorlevel%
echo.
echo BUILD OK: %CD%\build\firmware.bin
exit /b 0

:clean
"%GNU_MAKE%" clean "FEATURES=FFT ESP_LINK_LVGL ESP_LINK_BRIDGE"
exit /b %errorlevel%

:usage
echo Usage: build_lvgl.bat [build^|rebuild^|clean]
echo.
echo   build    LVGL guest build ^(default; Makefile performs a clean build^)
echo   rebuild  Clean, then build
echo   clean    Remove build outputs
exit /b 0

:usage_error
echo Usage: build_lvgl.bat [build^|rebuild^|clean]
exit /b 1
