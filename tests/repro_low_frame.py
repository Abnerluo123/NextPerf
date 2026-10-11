"""③「以显示帧为准」的取数与复现工具（**不是回归用例**，不放进 run_all.py）。

为什么需要它
    Low 帧那两行的口径问题（`1%` 与 `0.1%` 恒等、窗口边界漂移）只是表面；
    根子上是：**我们记的"帧"是 Present 调用间隔，而不是显示器上真正显示的那一帧**。
    《漫威蜘蛛侠》实机数据：真呈现间隔呈 4 帧周期 10.7/16.3/16.6/23.0ms（和 = 66.6 = 4×16.65），
    于是按 Present 间隔统计得出 P50 = 10.1ms、1% Low ≈ 42，而显示器与驱动面板是 60fps / Low≈59。

    我先试过"把间隔按垂直消隐周期量化成整数倍"，**结论是不成立**（见 `np_hook.cpp` 里那段注释）：
    某帧 Present 之后游戏多睡 6ms 就真的错过了下一个垂直消隐，量化会把这次掉帧抹平。
    所以按本项目规矩**先加诊断**：把"帧识别"需要的量打出来，用真游戏数据决定判据。

本工具做什么
    1) 起 NextPerf + 测试宿主（`host_run.exe --jitter-present`：呈现时刻在帧内抖动，
       复现"显示 60fps 但 Present 间隔乱跳"的形态 —— 这是代码注释里要求的前提①）；
    2) 注入钩子；
    3) 把下面这些**诊断行**打出来（它们就是设计 ③ 的依据）：
        * `display event probe:` —— `GetFrameStatistics` / `GetLastPresentCount` 是否可用
          （前者在 flip 模型上通常不支持；能用的话它就是权威的显示事件来源）
        * `present diag 5s:` —— 含 `vblank=` 与 `aligned=`（间隔落在整数个垂直消隐 ±0.25 内的比例）
        * `frame dist:` / `low compare:` / `SELFCHECK fpsLowLeAvg` —— 当前口径下的帧时间与 Low

用法： python tests/repro_low_frame.py [--seconds 20]
退出码：0 = 取到数据（并给出判读）｜ 2 = 环境不满足
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

from verify_inject import HOST_EXE, HOOK_DLL, LOG, NP_EXE, inject, read_telemetry  # noqa: E402

u32 = ctypes.WinDLL("user32", use_last_error=True)
STARTED = []


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


def hook_text():
    if not os.path.exists(LOG):
        return ""
    raw = open(LOG, "rb").read()
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        return raw.decode("gbk", "replace")


def find_window(cls, pid, timeout=15.0):
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


def main():
    atexit.register(cleanup)
    seconds = 20
    if "--seconds" in sys.argv:
        seconds = int(sys.argv[sys.argv.index("--seconds") + 1])

    if os.path.exists(LOG):
        os.remove(LOG)

    print("=== 1. 起主程序 ===")
    np = subprocess.Popen([NP_EXE], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    STARTED.append(np)
    if not find_window("NextPerfMainCls", np.pid, 15):
        print("环境不满足：NextPerf 主窗口没出现")
        return 2
    time.sleep(1.5)

    print("=== 2. 起宿主（呈现时刻抖动）并注入 ===")
    host = subprocess.Popen([HOST_EXE, "--jitter-present", "--seconds", str(seconds + 15)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    STARTED.append(host)
    hpid = host.pid
    if not find_window("NpHostCls", hpid, 15):
        print("环境不满足：宿主窗口没出现")
        return 2
    time.sleep(2.5)
    inject(hpid, HOOK_DLL)
    print("  宿主 pid = %d" % hpid)

    running, last_ft = False, 0
    t0 = time.time()
    while time.time() - t0 < 20:
        t = read_telemetry(hpid)
        if t and getattr(t, "attached", 0) and t.frameTotal > last_ft + 5:
            running = True
            break
        if t:
            last_ft = max(last_ft, getattr(t, "frameTotal", 0))
        time.sleep(0.5)
    if not running:
        print("环境不满足：注入后没读到增长中的帧数")
        return 2

    print("=== 3. 收 %d 秒诊断 ===" % seconds)
    time.sleep(seconds)

    mine = [l for l in hook_text().splitlines() if ("pid=%d " % hpid) in l]
    probes = [l for l in mine if "display event probe" in l]
    diags = [l for l in mine if "present diag 5s" in l]
    dists = [l for l in mine if "frame dist" in l]
    lows = [l for l in mine if "SELFCHECK fpsLowLeAvg" in l]

    for l in probes:
        print("  [探测] " + l.strip())
    for l in diags[-2:]:
        print("  [间隔] " + l.strip())
    for l in dists[-1:]:
        print("  [分布] " + l.strip())
    for l in lows[-1:]:
        print("  [Low ] " + l.strip())

    if not probes and not diags:
        print("环境不满足：没收到诊断行（钩子没接管？）")
        for l in mine[-6:]:
            print("    " + l)
        return 2

    print()
    print("判读：")
    print("  * `aligned=` 高（接近 100%）→ 呈现与垂直消隐锁相，可以按 vblank 判定显示帧；")
    print("    低 → 呈现时刻在帧内漂移，只能靠真正的显示事件（GetFrameStatistics / ETW）。")
    print("  * `GetFrameStatistics=可用` → 直接拿 PresentCount/SyncRefreshCount 当帧边界，最干净。")
    print("  * 把这两行（以及真游戏的同类日志）发我，就能定 ③ 的判据。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
