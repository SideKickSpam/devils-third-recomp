@echo off
rem Double-click launcher for Windows: runs build\game\dt_port.exe normally (window, sound, no call log).
rem
rem It finds the repository from its own location (src\tools\..\..). The game directory (holding dt.elf and
rem the extracted title folders) is, in order: DT_GAME_DIR, the first line of build\game-dir.txt, or game\.
rem For a game kept in WSL, put \\wsl.localhost\Ubuntu\<path>\game in build\game-dir.txt.
rem Extra arguments are passed on, e.g. --validate.
rem Saves go to the game directory's save\ folder. The log is written to build\dt_port.log.
setlocal
set "ROOT=%~dp0..\.."
set "EXE=%ROOT%\build\game\dt_port.exe"
if "%DT_GAME_DIR%"=="" if exist "%ROOT%\build\game-dir.txt" set /p DT_GAME_DIR=<"%ROOT%\build\game-dir.txt"
if "%DT_GAME_DIR%"=="" set "DT_GAME_DIR=%ROOT%\game"
if not exist "%EXE%" (
    echo dt_port.exe not found at %EXE%; build it first with src\tools\build-windows.cmd
    pause
    exit /b 1
)
if not exist "%DT_GAME_DIR%\dt.elf" (
    echo No dt.elf in %DT_GAME_DIR%; put the game directory in build\game-dir.txt
    pause
    exit /b 1
)
cd /d "%ROOT%"
"%EXE%" --game-dir "%DT_GAME_DIR%" --no-import-log --log build\dt_port.log %*
if errorlevel 1 (
    echo dt_port exited with code %errorlevel%; see build\dt_port.log
    pause
)
