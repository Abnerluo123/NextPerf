@echo off
rem ===================================================================
rem  构建测试宿主（人工验证用，不属于交付产物）
rem    tests\host_run.exe  启动即建交换链，模拟「已经在跑的游戏」
rem ===================================================================
setlocal

set ROOT=%~dp0..
set ZIG=
if defined NP_ZIG if exist "%NP_ZIG%" set ZIG=%NP_ZIG%
if not defined ZIG for /f "delims=" %%i in ('where zig.exe 2^>nul') do if not defined ZIG set ZIG=%%i
for /d %%a in (
    "%USERPROFILE%\.workbuddy\binaries\python\envs\*"
    "%LOCALAPPDATA%\Programs\Python\Python*"
    "%APPDATA%\Python\Python*"
) do (
    if not defined ZIG if exist "%%~a\Lib\site-packages\ziglang\zig.exe" set ZIG=%%~a\Lib\site-packages\ziglang\zig.exe
)
if not defined ZIG (
    echo [错误] 找不到 zig 编译器，请先 pip install ziglang 或设置 NP_ZIG
    exit /b 1
)

set ZIG_GLOBAL_CACHE_DIR=%ROOT%\.zig-global
set ZIG_LOCAL_CACHE_DIR=%ROOT%\.zig-cache

set CXX=-target x86_64-windows-gnu -O2 -std=c++20 -DUNICODE -D_UNICODE -fno-exceptions -Wno-nullability-completeness
set LIBS=-luser32 -lgdi32 -ldxgi -ld3d11 -ld3d12 -Wl,--subsystem,windows

echo 编译 tests\host_run.exe ...
"%ZIG%" c++ %CXX% -o "%~dp0host_run.exe" "%~dp0host_run.cpp" %LIBS%
if errorlevel 1 (
    echo 构建失败。
    exit /b 1
)
del /Q "%~dp0*.pdb" "%~dp0*.lib" >nul 2>&1
echo 完成： %~dp0host_run.exe
