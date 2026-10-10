@echo off
rem ===================================================================
rem  NextPerf —— 次世代性能计数器  一键构建脚本
rem
rem  产物：
rem    dist\NextPerf.exe        主程序（UI / 传感器 / 桌面叠加 / 注入器）
rem    dist\NextPerfHook.dll    注入到游戏进程的钩子
rem    dist\NextPerfHook64.dll  同名副本，方便外部注入工具识别
rem
rem  编译器：zig（ziglang）。脚本按顺序自动查找：
rem    NP_ZIG 环境变量 -> PATH -> 常见 Python 环境的 site-packages -> pip 安装
rem ===================================================================
setlocal enabledelayedexpansion

set ROOT=%~dp0
set DIST=%ROOT%dist
set ZIG=

rem ---------- 1. 找 zig 编译器 ----------
if defined NP_ZIG if exist "%NP_ZIG%" set ZIG=%NP_ZIG%
if not defined ZIG for /f "delims=" %%i in ('where zig.exe 2^>nul') do if not defined ZIG set ZIG=%%i

for /d %%a in (
    "%USERPROFILE%\.workbuddy\binaries\python\envs\*"
    "%LOCALAPPDATA%\Programs\Python\Python*"
    "%APPDATA%\Python\Python*"
    "%ROOT%..\tools\uv-python\*"
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
    if not defined PY for /d %%a in ("%ROOT%..\tools\uv-python\*") do (
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

echo [2/3] 编译器：%ZIG%
"%ZIG%" version
if errorlevel 1 goto :fail

if not exist "%DIST%" mkdir "%DIST%"

rem zig 默认缓存写在 %LOCALAPPDATA%\zig。受限环境（沙箱、只读用户目录）会以
rem AccessDenied 失败，所以固定用项目内的缓存目录：构建自包含，删掉即可回收空间。
set ZIG_GLOBAL_CACHE_DIR=%ROOT%.zig-global
set ZIG_LOCAL_CACHE_DIR=%ROOT%.zig-cache

set CXX=-target x86_64-windows-gnu -O2 -std=c++20 -DUNICODE -D_UNICODE -fno-exceptions -Wno-nullability-completeness -I "%ROOT%src"
set LIBS_EXE=-luser32 -lgdi32 -lshell32 -ladvapi32 -lole32 -loleaut32 -luuid -lcomctl32 -lcomdlg32 -lpdh -lpsapi -ldxgi -ld3d11 -ld2d1 -ldwrite -lshlwapi -lwinmm -lmsimg32 -ltdh -Wl,--subsystem,windows
set LIBS_DLL=-luser32 -lgdi32 -lshell32 -ladvapi32 -lole32 -loleaut32 -luuid -ldxgi -ld3d11 -ld3d12 -ld2d1 -ldwrite -lshlwapi

echo [3/3] 编译 NextPerf.exe ...
set SRC_EXE="%ROOT%src\app\main.cpp" "%ROOT%src\app\ui.cpp" "%ROOT%src\app\overlay.cpp" "%ROOT%src\app\injector.cpp" "%ROOT%src\app\settings.cpp" "%ROOT%src\common\np_panel.cpp" "%ROOT%src\common\np_build.cpp" "%ROOT%src\common\np_bitmap.cpp" "%ROOT%src\etw\np_etw.cpp" "%ROOT%src\sensors\np_vendor.cpp" "%ROOT%src\sensors\np_sensors.cpp"
"%ZIG%" c++ %CXX% -o "%DIST%\NextPerf.exe" %SRC_EXE% %LIBS_EXE%
if errorlevel 1 goto :fail

echo [3/3] 编译 NextPerfHook.dll ...
set SRC_DLL="%ROOT%src\hook\dllmain.cpp" "%ROOT%src\hook\np_hook.cpp" "%ROOT%src\hook\np_draw.cpp" "%ROOT%src\common\np_panel.cpp" "%ROOT%src\common\np_build.cpp" "%ROOT%src\common\np_bitmap.cpp"
"%ZIG%" c++ %CXX% -shared -o "%DIST%\NextPerfHook.dll" %SRC_DLL% %LIBS_DLL%
if errorlevel 1 goto :fail

copy /Y "%DIST%\NextPerfHook.dll" "%DIST%\NextPerfHook64.dll" >nul

rem --- 32-bit hook for 32-bit games (PE Machine 0x014C) ---
rem Same sources; clang uses the LAST -target, so this overrides the x64 one.
echo [3/3] Building NextPerfHook32.dll (32-bit games) ...
"%ZIG%" c++ %CXX% -target x86-windows-gnu -shared -o "%DIST%\NextPerfHook32.dll" %SRC_DLL% %LIBS_DLL%
if errorlevel 1 goto :fail
del /Q "%DIST%\*.pdb" "%DIST%\*.lib" >nul 2>&1

echo.
echo 构建完成：
dir /b "%DIST%"
echo.
echo 提示：可以运行  dist\NextPerf.exe --selftest  做一次自检。
goto :eof

:fail
echo.
echo 构建失败，请看上面的错误信息。
exit /b 1
