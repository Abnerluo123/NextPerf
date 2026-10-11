"""复现「钩子开着时关掉游戏窗口，进程却不退出」（2026-10-11 用户实测 · 生化危机8 DX12）。

用法：
    python tests/_exitprobe.py [--d3d11] [--wait 15]

流程：
    1) 起 NextPerf —— **必须**：钩子要靠它创建的 Config/Sensors 共享内存才工作
    2) 起测试宿主 host_run.exe，注入钩子，等它**真的有帧**（attached 且 frameTotal 在涨）
    3) 给宿主窗口发 WM_CLOSE（等价于用户点那个叉）
    4) 看宿主进程是否在 --wait 秒内退出

退出码：0 = 宿主正常退出（未复现）｜ 1 = 宿主仍活着（**复现**）｜ 2 = 环境不满足。

复现时会把钩子日志里属于该 pid 的行打出来。判读只看三行：
    worker: enter           -> 钩子工作线程起来了
    DETACH: enter           -> 宿主**开始卸载 DLL**（说明我们没挡住它走到这一步）
    DETACH: worker wait ->  -> exited / TIMEOUT(500ms)
**没有** `DETACH: enter` 就说明宿主卡在卸载 DLL 之前，与钩子的卸载路径无关。

⚠ 本脚本**只结束自己启动的进程**（不像套件里的 kill_all 那样按映像名全杀），
   以免误杀用户正开着的 NextPerf。
"""
import atexit
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from verify_inject import (HOST_EXE, HOOK_DLL, LOG, NP_EXE,  # noqa: E402
                           close_window, inject, read_telemetry)

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
u32 = ctypes.WinDLL("user32", use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.GetExitCodeProcess.argtypes = [wt.HANDLE, ctypes.POINTER(wt.DWORD)]
k32.CloseHandle.argtypes = [wt.HANDLE]

STILL_ACTIVE = 259
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
STARTED = []


def alive(pid):
    h = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not h:
        return False
    code = wt.DWORD()
    ok = k32.GetExitCodeProcess(h, ctypes.byref(code))
    k32.CloseHandle(h)
    return bool(ok) and code.value == STILL_ACTIVE


def cleanup():
    for p in STARTED:
        try:
            if p.poll() is None:
                p.terminate()
                try:
                    p.wait(3)
                except Exception:
                    p.kill()
        except Exception:
            pass


def window_exists(cls):
    """有没有任何进程持有这个窗口类（用来判断"用户是不是开着 NextPerf"）。"""
    CB = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    found = []

    def cb(hwnd, _):
        buf = ctypes.create_unicode_buffer(64)
        u32.GetClassNameW(hwnd, buf, 64)
        if buf.value == cls:
            found.append(hwnd)
        return True

    u32.EnumWindows(CB(cb), 0)
    return bool(found)


def find_window(cls, pid, timeout=15.0):
    """按「窗口类名 + 进程 id」找窗口（只按类名会拿到正在消失的上一个实例）。"""
    CB = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    t0 = time.time()
    while time.time() - t0 < timeout:
        found = []

        def cb(hwnd, _):
            buf = ctypes.create_unicode_buffer(64)
            u32.GetClassNameW(hwnd, buf, 64)
            if buf.value == cls:
                p = wt.DWORD()
                u32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
                if p.value == pid:
                    found.append(hwnd)
            return True

        u32.EnumWindows(CB(cb), 0)
        if found:
            return found[0]
        time.sleep(0.2)
    return None


def hook_lines(pid):
    """钩子日志是 **UTF-8**（2026-10-11 实测核实；本文件曾写 GBK，是错的），
    而且**所有被注入进程共用**：只挑这个 pid 的行。"""
    if not os.path.exists(LOG):
        return []
    raw = open(LOG, "rb").read()
    try:
        txt = raw.decode("utf-8")
    except UnicodeDecodeError:
        txt = raw.decode("gbk", "replace")
    key = "pid=%d" % pid
    return [l for l in txt.splitlines() if key in l]


def main():
    d3d11 = "--d3d11" in sys.argv
    wait = 15.0
    if "--wait" in sys.argv:
        wait = float(sys.argv[sys.argv.index("--wait") + 1])
    atexit.register(cleanup)

    # 独占检查：两个主程序会抢同一块共享内存，用户开着 NextPerf 时不要跑。
    if window_exists("NextPerfMainCls"):
        print("环境不满足：已经有 NextPerf 在运行。请先关掉它再跑本探针。")
        return 2

    if os.path.exists(LOG):
        os.remove(LOG)

    print("=== 1. 起主程序 ===")
    np = subprocess.Popen([NP_EXE], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    STARTED.append(np)
    if not find_window("NextPerfMainCls", np.pid, 15):
        print("环境不满足：NextPerf 主窗口没出现")
        return 2

    print("=== 2. 起宿主并注入 ===")
    host = subprocess.Popen([HOST_EXE] + (["--d3d11"] if d3d11 else []),
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    STARTED.append(host)
    hpid = host.pid
    if not find_window("NpHostCls", hpid, 15):
        print("环境不满足：宿主窗口没出现")
        return 2
    inject(hpid, HOOK_DLL)

    print("=== 3. 等它真的有帧 ===")
    last_ft, t0, running = 0, time.time(), False
    while time.time() - t0 < 20:
        t = read_telemetry(hpid)
        if t:
            if t.attached and t.frameTotal > last_ft + 5:
                running = True
                break
            last_ft = max(last_ft, t.frameTotal)
        time.sleep(0.5)
    t = read_telemetry(hpid)
    print("  遥测：attached=%s frameTotal=%s"
          % (getattr(t, "attached", "?"), getattr(t, "frameTotal", "?")))
    if not running:
        print("环境不满足：注入后没读到增长中的帧数（钩子没接管）")
        return 2

    print("=== 4. 发 WM_CLOSE（= 点叉）===")
    close_window(hpid)
    t1 = time.time()
    while time.time() - t1 < wait:
        if not alive(hpid):
            print("宿主在 %.1fs 内退出 -> **未复现**" % (time.time() - t1))
            for l in hook_lines(hpid)[-10:]:
                print("   " + l)
            return 0
        time.sleep(0.5)

    print("宿主关窗 %.0f 秒后**仍然活着 -> 复现了**" % wait)
    lines = hook_lines(hpid)
    print("--- 钩子日志里 pid=%d 的行（%d 条）---" % (hpid, len(lines)))
    for l in lines:
        print("   " + l)
    joined = "\n".join(lines)
    print("--- 判读 ---")
    if "DETACH: enter" not in joined:
        print("  没有 `DETACH: enter` -> 宿主卡在**卸载 DLL 之前**，与钩子的卸载路径无关")
    elif "DETACH: worker wait" not in joined:
        print("  有 `DETACH: enter` 没有 worker wait -> **卡在 RestoreAllHooks 里**")
    else:
        print("  DETACH 走完了 -> 看 worker wait 是 exited 还是 TIMEOUT")
    return 1


if __name__ == "__main__":
    sys.exit(main())
