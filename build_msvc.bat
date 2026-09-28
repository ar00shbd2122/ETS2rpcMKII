@echo off
rem ============================================================
rem  ETS2rpcMKII - one-shot MSVC build (no CMake required)
rem  Produces: build\ets2rpcmkii.dll
rem  Requires: Visual Studio Build Tools with the "Desktop C++"
rem            workload (uses vcvarsall.bat + cl.exe directly).
rem ============================================================
setlocal enabledelayedexpansion

rem -- locate vcvarsall.bat (edit VSVARS if yours lives elsewhere) --
set "VSVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VSVARS%" set "VSVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VSVARS%" set "VSVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VSVARS%" (
    echo [!] vcvarsall.bat not found - install VS Build Tools "Desktop C++".
    echo     Tried several default paths; edit VSVARS in this script.
    exit /b 1
)

call "%VSVARS%" x64 >nul
if errorlevel 1 (
    echo [!] Failed to set up the x64 MSVC environment.
    exit /b 1
)

if not exist build mkdir build

echo [1/3] Compiling...
cl /nologo /std:c++17 /O2 /EHsc /MT /utf-8 ^
   /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX ^
   /Iinclude\scs /Isrc ^
   /c src\plugin.cpp src\discord_ipc.cpp src\config.cpp tests\failsafe_test.cpp
if errorlevel 1 (
    echo [!] Compile failed.
    exit /b 1
)

echo [2/3] Linking DLL...
link /nologo /DLL /DEF:src\exports.def /OUT:build\ets2rpcmkii.dll ^
     plugin.obj discord_ipc.obj config.obj user32.lib kernel32.lib
if errorlevel 1 (
    echo [!] Link failed.
    exit /b 1
)

echo [3/3] Building and running the ini failsafe test...
link /nologo /OUT:build\failsafe_test.exe ^
     failsafe_test.obj config.obj user32.lib kernel32.lib
if errorlevel 1 (
    echo [!] Test link failed.
    exit /b 1
)
build\failsafe_test.exe
if errorlevel 1 (
    echo [!] Ini failsafe test FAILED - fix before shipping.
    exit /b 1
)

del plugin.obj discord_ipc.obj config.obj failsafe_test.obj >nul 2>&1
del build\failsafe_test.exe >nul 2>&1

rem import lib + exp are linker byproducts, not needed to run the plugin
del build\ets2rpcmkii.lib build\ets2rpcmkii.exp >nul 2>&1

rem ship the ini + presets next to the DLL so build\ is a ready bundle
copy /y templates\ets2rpcmkii.ini build\ets2rpcmkii.ini >nul
if not exist build\templates mkdir build\templates
copy /y templates\presets\*.ini build\templates\ >nul

echo.
echo [OK] Build finished. Everything you need is in build\:
echo      build\ets2rpcmkii.dll
echo      build\ets2rpcmkii.ini
echo      build\templates\   (style presets)
echo      ini failsafe test: PASSED
echo.
echo      Copy all of that to ETS2\bin\win_x64\plugins\
endlocal
