@echo off
rem ===================================================================
rem  Build the NextPerf simulated game process  ->  tests\sim\sim.exe
rem
rem  This batch file is deliberately ASCII-only. Windows cmd.exe parses a
rem  .bat file using the *current console code page* (GBK on this machine);
rem  Chinese comments stored as UTF-8 bytes can decode into stray command
rem  separators and break the script. The Chinese documentation lives in
rem  tests\sim\README.md and inside the C++ sources instead (both UTF-8).
rem
rem  Conventions kept identical to ..\..\build.bat and ..\build_hosts.bat:
rem    * compiler is always zig (ziglang); lookup order:
rem      NP_ZIG -> PATH -> common Python site-packages locations
rem    * caches stay inside the project (.zig-cache / .zig-global) so the
rem      build is self-contained and needs no writable %LOCALAPPDATA%
rem    * GUI subsystem, so double-clicking shows no black console box.
rem      sim_main.cpp takes over the standard streams in wWinMain, so
rem      stdout/stdin still work when a Python script redirects them.
rem
rem  usage:
rem    tests\sim\build_sim.bat          build sim.exe
rem    tests\sim\build_sim.bat clean    remove build products
rem ===================================================================
setlocal enabledelayedexpansion

set HERE=%~dp0
set ROOT=%~dp0..\..
set ZIG=

if /I "%~1"=="clean" (
    del /Q "%HERE%sim.exe" "%HERE%*.pdb" "%HERE%*.lib" "%HERE%*.obj" >nul 2>&1
    echo cleaned build products
    goto :eof
)

rem ---------- 1. locate the zig compiler ----------
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

if not defined ZIG (
    echo [ERROR] zig compiler not found. Run: pip install ziglang
    echo         or point NP_ZIG at the full path of zig.exe
    exit /b 1
)

echo [1/2] compiler: %ZIG%
"%ZIG%" version
if errorlevel 1 exit /b 1

set ZIG_GLOBAL_CACHE_DIR=%ROOT%\.zig-global
set ZIG_LOCAL_CACHE_DIR=%ROOT%\.zig-cache

rem Both backends are linked in; --api is a runtime switch.
rem   -ldxgi   CreateDXGIFactory1 / CreateDXGIFactory2
rem   -ld3d11  D3D11CreateDeviceAndSwapChain
rem   -ld3d12  D3D12CreateDevice
rem   -luuid   a few GUID symbols
rem NOTE: d3dcompiler is NOT linked. sim_dx11.cpp loads d3dcompiler_47.dll at
rem runtime with LoadLibrary/GetProcAddress, so a system without that dll only
rem loses the shader render path instead of failing to start.
rem NOTE: zig 0.16 wants a COMMA here: -Wl,--subsystem,windows. The colon form
rem (-Wl,--subsystem:windows) is rejected with "unsupported linker arg".
set CXX=-target x86_64-windows-gnu -O2 -std=c++20 -DUNICODE -D_UNICODE -fno-exceptions -Wno-nullability-completeness
set LIBS=-luser32 -lgdi32 -lshell32 -lole32 -loleaut32 -luuid -ldxgi -ld3d11 -ld3d12 -Wl,--subsystem,windows
set SRC=%HERE%sim_main.cpp %HERE%sim_common.cpp %HERE%sim_dx11.cpp %HERE%sim_dx12.cpp

echo [2/2] compiling tests\sim\sim.exe ...
"%ZIG%" c++ %CXX% -o "%HERE%sim.exe" %SRC% %LIBS%
if errorlevel 1 (
    echo.
    echo BUILD FAILED - see the errors above.
    exit /b 1
)

del /Q "%HERE%*.pdb" "%HERE%*.lib" "%HERE%*.obj" >nul 2>&1
echo.
echo OK: %HERE%sim.exe
echo.
echo try:
echo   "%HERE%sim.exe" --help
echo   "%HERE%sim.exe" --api=dx11 --seconds=3 --json
echo   "%HERE%sim.exe" --api=dx12 --seconds=3 --json
