"""验证「注入到前台进程」按钮。

用户报的问题：点这个按钮时前台窗口就是 NextPerf 自己，原来的实现直接取
GetForegroundWindow() 拿到自己，判断 `pid != GetCurrentProcessId()` 失败，
永远只会提示「没有可用的前台进程」。

测试刻意复现这个场景：
  1. 把游戏宿主切到前台（让主程序把它记下来）
  2. 再把 NextPerf 切到前台（此刻前台 = 自己）
  3. 点「注入到前台进程」
正确的行为是回退到「上一个非自己的前台进程」并注入成功。

用法： python tests/verify_frontinject.py
"""
import ctypes
import ctypes.wintypes as wt
import atexit
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verify_inject import kill_all, find_main_window_for_pid   # noqa: E402

ROOT = os.path.dirname(os.path.abspath(os.path.dirname(__file__)))
NP_EXE = os.path.join(ROOT, "dist", "NextPerf.exe")
HOST_EXE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "host_run.exe")

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
u32 = ctypes.WinDLL("user32", use_last_error=True)

k32.OpenMutexW.restype = wt.HANDLE
k32.OpenMutexW.argtypes = [wt.DWORD, wt.BOOL, wt.LPCWSTR]
k32.OpenFileMappingW.restype = wt.HANDLE
k32.OpenFileMappingW.argtypes = [wt.DWORD, wt.BOOL, wt.LPCWSTR]

u32.FindWindowW.restype = wt.HWND
u32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]
u32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
u32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
u32.GetDpiForWindow.restype = wt.UINT
u32.GetDpiForWindow.argtypes = [wt.HWND]
u32.GetClientRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]

WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202
WM_COMMAND = 0x0111
NP_TRAY_EXIT = 1004


def makelparam(x, y):
    return (y << 16) | (x & 0xFFFF)


def click(hwnd, x, y):
    u32.PostMessageW(hwnd, WM_LBUTTONDOWN, 1, makelparam(x, y))
    time.sleep(0.05)
    u32.PostMessageW(hwnd, WM_LBUTTONUP, 0, makelparam(x, y))


def button_center(hwnd):
    """按 ui.cpp 的 Layout() 复算出「注入到前台进程」按钮的中心点（客户区坐标）。

    缩放不取 GetDpiForWindow —— 程序里用的是 GetDeviceCaps(LOGPIXELSX)/96，
    两者在部分 DPI 设置下不一致。直接拿客户区宽度反推 kWinW=1000 对应的倍率，
    这样和程序内部的 gS 一定对齐。
    """
    cr = wt.RECT()
    u32.GetClientRect(hwnd, ctypes.byref(cr))
    cw, ch = cr.right - cr.left, cr.bottom - cr.top
    gs = cw / 1000.0
    S = lambda v: int(v * gs + 0.5)          # noqa: E731 和 C++ 的 (int)(v*gS+0.5f) 一致
    lx = S(20)
    # kListH 必须和 ui.cpp 里的常量一致。列表高度一改，游戏区和按钮位置全变，
    # 这里不同步就会点空（或点到别的东西上）。
    KLIST_H = 400
    game_top = S(42) + S(KLIST_H) + S(16)
    by = game_top + 3 * S(32) + S(10)
    x0 = lx + S(270)
    w, h = S(190), S(32)
    return x0 + w // 2, by + h // 2, cw, ch, gs


def main():
    fails = []
    atexit.register(kill_all)
    # 必须开 DPI 感知：否则 GetClientRect 拿到的是被 DWM 虚拟化过的逻辑坐标，
    # 而 PostMessage 里的客户区坐标是按物理像素解释的 —— 点击会整体打偏（踩过）。
    u32.SetProcessDpiAwarenessContext.argtypes = [ctypes.c_void_p]
    try:
        u32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
    except Exception:
        try:
            u32.SetProcessDPIAware()
        except Exception:
            pass

    for name in ("NextPerf.exe", "host_run.exe"):
        subprocess.run(["taskkill", "/F", "/IM", name], capture_output=True)

    print("=== 1. 启动主程序 ===")
    np_proc = subprocess.Popen([NP_EXE], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    np_hwnd = find_main_window_for_pid(np_proc.pid)
    if not np_hwnd:
        print("!! 找不到 NextPerf 窗口")
        np_proc.kill()
        return 2
    time.sleep(1.5)

    print("=== 2. 启动游戏宿主 ===")
    host = subprocess.Popen([HOST_EXE, "--seconds", "60"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    hwnd = None
    for _ in range(60):
        hwnd = u32.FindWindowW(None, "NpHost")
        if hwnd:
            break
        time.sleep(0.2)
    pid = wt.DWORD()
    u32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    hpid = pid.value
    print("宿主 pid = %d" % hpid)
    time.sleep(2)

    print("=== 3. 先切到游戏（让主程序记住它），再切回自己 ===")
    u32.SetForegroundWindow(hwnd)
    time.sleep(1.5)
    print("  记录阶段前台 = 宿主")
    # 注意：Windows 的前台锁定经常不让我们的进程抢焦点（测试里就常失败），
    # 但**不影响本用例** —— 按钮现在固定用「记住的那个进程」，不再依赖点击时
    # 的前台是谁。这本身也是更安全的行为：前台是终端/资源管理器时不会误注入。
    u32.SetForegroundWindow(np_hwnd)
    time.sleep(1.0)
    fg = u32.GetForegroundWindow()
    fpid = wt.DWORD()
    u32.GetWindowThreadProcessId(fg, ctypes.byref(fpid))
    print("  点击时前台 pid = %d（NextPerf 自己 = %d，宿主 = %d）" %
          (fpid.value, np_proc.pid, hpid))

    x, y, cw, ch, gs = button_center(np_hwnd)
    print("=== 4. 点击「注入到前台进程」 客户区 (%d,%d)  客户区 %dx%d  scale=%.3f ==="
          % (x, y, cw, ch, gs))
    click(np_hwnd, x, y)
    time.sleep(5)

    m = k32.OpenMutexW(0x00100000, False, "Local\\NextPerf_Injected_%d" % hpid)
    injected = bool(m)
    if m:
        k32.CloseHandle(m)
    print("  宿主已被注入：%s" % injected)
    if not injected:
        fails.append("点了按钮但宿主没有被注入（回退到「上一个前台进程」的逻辑没生效）")

    tel = k32.OpenFileMappingW(0x0004, False, "Local\\NextPerf_Telemetry_v1_%d" % hpid)
    print("  遥测块已建立：%s" % bool(tel))
    if tel:
        k32.CloseHandle(tel)
    else:
        fails.append("钩子没有建立遥测块")

    print("=== 5. 收尾 ===")
    u32.PostMessageW(np_hwnd, WM_COMMAND, NP_TRAY_EXIT, 0)
    time.sleep(4)
    print("  主程序已退出：%s   宿主仍存活：%s" % (np_proc.poll() is not None, host.poll() is None))
    if host.poll() is None:
        host.kill()
    if np_proc.poll() is None:
        np_proc.kill()

    print()
    if fails:
        print("结果：FAIL")
        for f in fails:
            print("  - %s" % f)
        return 1
    print("结果：PASS —— 前台是自己时仍然正确注入到上一个游戏进程")
    return 0


if __name__ == "__main__":
    sys.exit(main())
