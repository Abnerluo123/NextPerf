"""验证「进程刚起来就注入」这条路 —— 走的是 DXGI 工厂钩子。

和 verify_inject.py 正好互补：
  * verify_inject.py  游戏已经在跑（交换链早就建好）-> 靠自己造临时交换链探测 vtable
  * 本脚本            在游戏初始化图形之前注入      -> 靠 CreateSwapChain* 工厂钩子

隔离手法：宿主 --delay 3（3 秒后才建 D3D），而钩子在「没发现 d3d11/d3d12 已加载」时
把探测推迟到注入后 6 秒 —— 所以这 3 秒内只有工厂钩子有机会挂上 Present。
顺带验证 CreateSwapChainForHwnd 的下标（历史上写成 13，那其实是 IsCurrent，正确的是 15）。

用法： python tests/verify_early.py
"""
import ctypes
import ctypes.wintypes as wt
import atexit
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.abspath(os.path.dirname(__file__)))
NP_EXE = os.path.join(ROOT, "dist", "NextPerf.exe")
HOOK_DLL = os.path.join(ROOT, "dist", "NextPerfHook.dll")
HOST_EXE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "host_run.exe")
LOG = os.path.join(os.environ.get("TEMP", "."), "NextPerfHook.log")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verify_inject import (inject, read_telemetry, kill_all,   # noqa: E402
                           find_main_window_for_pid, NP_MAGIC)

u32 = ctypes.WinDLL("user32", use_last_error=True)
u32.FindWindowW.restype = wt.HWND
u32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]
u32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
u32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]


def main():
    fails = []
    atexit.register(kill_all)
    for name in ("NextPerf.exe", "host_run.exe"):
        subprocess.run(["taskkill", "/F", "/IM", name], capture_output=True)
    if os.path.exists(LOG):
        os.remove(LOG)

    print("=== 1. 启动主程序 ===")
    np_proc = subprocess.Popen([NP_EXE], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(3)

    print("=== 2. 启动宿主：先开窗口，3 秒后才建 D3D ===")
    host = subprocess.Popen([HOST_EXE, "--delay", "3", "--seconds", "45"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    hwnd = None
    for _ in range(60):
        hwnd = u32.FindWindowW(None, "NpHost")
        if hwnd:
            break
        time.sleep(0.1)
    if not hwnd:
        print("!! 找不到宿主窗口")
        return 2
    pid = wt.DWORD()
    u32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    hpid = pid.value
    print("宿主 pid = %d（此时还没建交换链）" % hpid)

    print("=== 3. 立刻注入（抢在图形初始化之前） ===")
    inject(hpid, HOOK_DLL)

    print("=== 4. 等宿主建交换链并跑起来 ===")
    for _ in range(30):
        t = read_telemetry(hpid)
        if t and t.magic == NP_MAGIC and t.attached and t.frameTotal > 5:
            break
        time.sleep(0.5)
    time.sleep(2)
    t = read_telemetry(hpid)
    if t and t.magic == NP_MAGIC:
        print("  attached=%d gfxApi=%d frameTotal=%d fps=%.1f hookFlags=%02X" %
              (t.attached, t.gfxApi, t.frameTotal, t.fps, t.hookFlags))
        if not t.attached:
            fails.append("工厂钩子这条路没有接管 Present（attached=0）")
        if not (t.hookFlags & 0x1):
            fails.append("NP_HOOK_PRESENT 未置位（hookFlags=%02X）" % t.hookFlags)
        if t.frameTotal < 5:
            fails.append("frameTotal=%d，几乎没出帧" % t.frameTotal)
    else:
        fails.append("拿不到遥测块")

    print("=== 5. 钩子日志 ===")
    text = ""
    if os.path.exists(LOG):
        text = open(LOG, encoding="utf-8", errors="ignore").read()
        print(text.strip())
    if "CreateSwapChainForHwnd called" not in text:
        fails.append("日志里没有 CreateSwapChainForHwnd called —— 工厂钩子没挂对位置")
    if "swapchain vtable patched" not in text:
        fails.append("工厂钩子没有把 Present 补上")
    if "ProbeSwapChainVtable" in text:
        fails.append("宿主 3 秒就建好交换链了，不该再走探测路径")

    print("=== 6. 收尾 ===")
    np_hwnd = find_main_window_for_pid(np_proc.pid)
    if np_hwnd:
        u32.PostMessageW(np_hwnd, 0x0111, 1004, 0)
    time.sleep(5)
    print("  主程序已退出：%s   宿主仍存活：%s" % (np_proc.poll() is not None, host.poll() is None))
    if host.poll() is not None:
        fails.append("宿主崩了 —— 钩子把游戏带崩了")
    else:
        host.kill()
    if np_proc.poll() is None:
        np_proc.kill()

    print()
    if fails:
        print("结果：FAIL")
        for f in fails:
            print("  - %s" % f)
        return 1
    print("结果：PASS —— 图形初始化前注入，工厂钩子正确接管 Present")
    return 0


if __name__ == "__main__":
    sys.exit(main())
