@echo off
rem Builds dt-port natively on Windows with Visual Studio's clang (clang-cl + lld-link), CMake and Ninja.
rem
rem   src\tools\build-windows.cmd [game-dir]
rem
rem Works from any directory (it finds the repository from its own location). The game directory, holding
rem the extracted title folders (and optionally dt.elf, a decompressed RPX), is found as the launcher
rem (dt-port.cmd) finds it: the argument, DT_GAME_DIR, the first line of build\game-dir.txt, or game\. On an error the window waits for a key.
rem Needs Visual Studio 2022 (or its Build Tools) with "Desktop development with C++" and
rem "C++ Clang tools for Windows". Output: build\recomp\dt_recomp.exe, generated\, build\game\dt_port.exe.
setlocal
cd /d "%~dp0..\.."
set "GAME_DIR=%~1"
if "%GAME_DIR%"=="" set "GAME_DIR=%DT_GAME_DIR%"
if "%GAME_DIR%"=="" if exist build\game-dir.txt set /p GAME_DIR=<build\game-dir.txt
if "%GAME_DIR%"=="" set "GAME_DIR=game"
rem The program to recompile: dt.elf if there is one, else the update's (or base title's) RPX as it ships.
set "RPX=%GAME_DIR%\dt.elf"
if not exist "%RPX%" for /d %%d in ("%GAME_DIR%\00050000*" "%GAME_DIR%\0005000e*") do if exist "%%d\code\DevilsThirdGame-WiiU-Shipping.rpx" set "RPX=%%d\code\DevilsThirdGame-WiiU-Shipping.rpx"
if not exist "%RPX%" (
    echo error: no game in %GAME_DIR%; pass the game directory or put it in build\game-dir.txt
    goto fail
)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo error: vswhere.exe not found; install Visual Studio 2022 or its Build Tools
    goto fail
)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath`) do set "VS_PATH=%%i"
if "%VS_PATH%"=="" (
    echo error: no Visual Studio installation with the "C++ Clang tools for Windows" component
    goto fail
)
call "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat" >nul || goto fail

echo == recompiler
rem (a build folder from before the recompiler needed C (zlib) has no C compiler set: configure it afresh)
set "FRESH="
if exist build\recomp\CMakeCache.txt findstr /b /c:"CMAKE_C_COMPILER:" build\recomp\CMakeCache.txt >nul || set "FRESH=--fresh"
cmake %FRESH% -S src\recomp -B build\recomp -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl || goto fail
cmake --build build\recomp || goto fail

echo == generating code from %RPX%
build\recomp\dt_recomp.exe "%RPX%" generated || goto fail

echo == runtime
cmake -S src\runtime -B build\game -G Ninja -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_LINKER=lld-link || goto fail
cmake --build build\game -j 12 || goto fail

echo == done: build\game\dt_port.exe --game-dir %GAME_DIR%
exit /b 0

:fail
echo == build failed
pause
exit /b 1
