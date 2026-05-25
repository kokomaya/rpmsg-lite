@echo off
REM RPMsg-Lite Simulator Build Script (Windows with MinGW GCC)
REM Usage: build.bat

set SCRIPT_DIR=%~dp0
set LIB_DIR=%SCRIPT_DIR%..\lib
set BUILD_DIR=%SCRIPT_DIR%build_sim
set GCC=D:\03_Tools\msys64\mingw64\bin\gcc.exe

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

echo Building RPMsg-Lite Simulator...

%GCC% -Wall -Wno-unused-parameter -O2 -std=c11 -pipe ^
    -DRL_USE_STATIC_API=1 -DSIM_BUILD=1 -D_CRT_SECURE_NO_WARNINGS -DMG_ENABLE_WINSOCK=1 -DMG_ENABLE_LOG=0 ^
    -I"%LIB_DIR%\include" ^
    -I"%SCRIPT_DIR%backend" ^
    -I"%SCRIPT_DIR%third_party\mongoose" ^
    "%LIB_DIR%\rpmsg_lite\rpmsg_lite.c" ^
    "%LIB_DIR%\rpmsg_lite\rpmsg_ns.c" ^
    "%LIB_DIR%\rpmsg_lite\rpmsg_queue.c" ^
    "%LIB_DIR%\virtio\virtqueue.c" ^
    "%LIB_DIR%\common\llist.c" ^
    "%SCRIPT_DIR%backend\main.c" ^
    "%SCRIPT_DIR%backend\sim_env.c" ^
    "%SCRIPT_DIR%backend\sim_platform.c" ^
    "%SCRIPT_DIR%backend\sim_core.c" ^
    "%SCRIPT_DIR%backend\sim_events.c" ^
    "%SCRIPT_DIR%third_party\mongoose\mongoose.c" ^
    -lws2_32 ^
    -o "%BUILD_DIR%\rpmsg_sim.exe" 2> "%BUILD_DIR%\gcc.err"
if exist "%BUILD_DIR%\gcc.err" type "%BUILD_DIR%\gcc.err"

if %ERRORLEVEL% NEQ 0 (
    echo BUILD FAILED
    exit /b 1
)

echo Build successful: %BUILD_DIR%\rpmsg_sim.exe

REM Copy web files
xcopy /E /I /Y "%SCRIPT_DIR%web" "%BUILD_DIR%\web" >nul

echo Web files copied.
echo.
echo Run: cd %BUILD_DIR% ^&^& rpmsg_sim.exe
echo Then open: http://localhost:8080
