@echo off
rem ===================================================================
rem  NextPerf tests/simvk —— 构建 Vulkan 模拟游戏进程 simvk.exe
rem
rem  产物：tests\simvk\simvk.exe
rem
rem  为什么用「控制台子系统」（不加 -Wl,--subsystem,windows）？
rem    因为这个程序的用途就是被 Python/脚本驱动：要能从 stdin 读命令、
rem    往 stdout 逐行吐 JSON。Windows 子系统会让 stdout/stdin 没有控制台，
rem    管道读取行为也会变，所以这里必须是控制台程序。
rem
rem  编译器：zig（ziglang）。查找顺序与项目根目录 build.bat 一致：
rem    NP_ZIG 环境变量 -> PATH -> 常见 Python 环境的 site-packages -> pip 安装
rem
rem  不需要 Vulkan SDK、不需要 MSVC、不需要 Windows SDK：
rem    * Vulkan 声明全部手写在 vk_min.h 里；
rem    * windows.h / user32 / gdi32 / advapi32 / winmm 由 zig 自带的 MinGW-w64 提供；
rem    * vulkan-1.dll 是运行时 LoadLibrary 加载的，链接期完全不需要它。
rem ===================================================================
setlocal enabledelayedexpansion

set SIMDIR=%~dp0
set ROOT=%SIMDIR%..\..
set ZIG=

rem ---------- 1. 找 zig 编译器 ----------
if defined NP_ZIG if exist "%NP_ZIG%" set ZIG=%NP_ZIG%
if not defined ZIG for /f "delims=" %%i in ('where zig.exe 2^>nul') do if not defined ZIG set ZIG=%%i

for /d %%a in (
    "%USERPROFILE%\.workbuddy\binaries\python\envs\*"
    "%LOCALAPPDATA%\Programs\Python\Python*"
    "%APPDATA%\Python\Python*"
    "%ROOT%\..\tools\uv-python\*"
    "%USERPROFILE%\scoop\apps\zig\current"
) do (
    if not defined ZIG if exist "%%~a\Lib\site-packages\ziglang\zig.exe" set ZIG=%%~a\Lib\site-packages\ziglang\zig.exe
    if not defined ZIG if exist "%%~a\zig.exe" set ZIG=%%~a\zig.exe
)

rem ---------- 2. 还是没有就用 pip 装 ----------
if not defined ZIG (
    set PY=
    for /f "delims=" %%i in ('where python.exe 2^>nul') do (
        if not defined PY (
            "%%i" -c "import sys" >nul 2>&1
            if not errorlevel 1 set PY=%%i
        )
    )
    if not defined PY for /d %%a in ("%ROOT%\..\tools\uv-python\*") do (
        if not defined PY if exist "%%~a\python.exe" set PY=%%~a\python.exe
    )
    if defined PY (
        echo [1/3] 未发现 zig 编译器，正在用 pip 安装 ziglang ...
        "!PY!" -m pip install -q --disable-pip-version-check ziglang
        for /f "delims=" %%i in ('"!PY!" -c "import ziglang,os;print(os.path.join(os.path.dirname(ziglang.__file__),'zig.exe'))" 2^>nul') do set ZIG=%%i
    )
)

if not defined ZIG (
    echo [错误] 找不到 zig 编译器。请先安装：  pip install ziglang
    echo        或用 NP_ZIG 环境变量指定 zig.exe 的完整路径。
    exit /b 1
)

echo [1/3] 编译器：%ZIG%
"%ZIG%" version
if errorlevel 1 goto :fail

rem zig 默认缓存写在 %LOCALAPPDATA%\zig。受限环境（沙箱、只读用户目录）会以
rem AccessDenied 失败，所以固定用项目内的缓存目录 —— 和根目录 build.bat 一致，
rem 这样两边的编译缓存还能共用一部分。
set ZIG_GLOBAL_CACHE_DIR=%ROOT%\.zig-global
set ZIG_LOCAL_CACHE_DIR=%ROOT%\.zig-cache

rem 编译选项刻意和根目录 build.bat 保持一致（同一个编译器、同一套宏），
rem 这样 simvk 里出现的行为问题和主程序不会是"编译器不一样"造成的。
set CXX=-target x86_64-windows-gnu -O2 -std=c++20 -DUNICODE -D_UNICODE -fno-exceptions -Wno-nullability-completeness

rem 只需要这几个库：
rem   user32   —— 窗口、消息、ChangeDisplaySettingsExW、EnumDisplaySettingsExW
rem   gdi32    —— 基本 GDI（窗口类注册路径会碰到）
rem   advapi32 —— RegOpenKeyExW / RegEnumValueW（--list-layers 要读层注册表）
rem   winmm    —— timeBeginPeriod（锁帧要 1ms 定时器精度）
set LIBS=-luser32 -lgdi32 -ladvapi32 -lwinmm

echo [2/3] 编译 simvk.exe ...
"%ZIG%" c++ %CXX% -I "%SIMDIR%." -o "%SIMDIR%simvk.exe" ^
    "%SIMDIR%simvk_main.cpp" ^
    %LIBS%
if errorlevel 1 goto :fail

echo [3/3] 清理中间产物 ...
del /Q "%SIMDIR%*.pdb" "%SIMDIR%*.lib" "%SIMDIR%*.exp" >nul 2>&1

echo.
echo 构建完成： %SIMDIR%simvk.exe
echo.
echo 常用：
echo   simvk.exe --list-layers              列出系统上的 Vulkan 层（隐式/显式）
echo   simvk.exe --seconds=3 --json         跑 3 秒，每帧一行 JSON
echo   simvk.exe --seconds=5 --vsync=off --fps-cap=120
echo   echo resize 1920 1080 ^| simvk.exe --seconds=5
echo.
goto :eof

:fail
echo.
echo 构建失败，请看上面的错误信息。
exit /b 1
