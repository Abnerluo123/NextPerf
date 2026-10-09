"""端到端验证「GPU 帧时间」这条指标链路（PDH 按游戏进程读驱动数据）。

为什么单独写这个：
  原来的 run_all.py 只覆盖 vtable 下标和四条注入链路 —— **传感器与指标计算
  一概没覆盖**。也就是说「回归全绿」从来没验证过面板上那些数。而这个指标
  恰恰是反复出问题的那个：曾经因为
    ① PDH 通配实例在 AddCounter 那一刻冻结（游戏后启动就永远读不到）
    ② 采样基线被无条件重置（间隔永远到不了阈值）
    ③ 算出的值不跨轮询保留（界面"时有时无"）
  连续坏了三次。

本用例断言的是**用户实际能看到的结果**：注入之后，应用日志里最终会出现
一个 > 0 的 GPU 帧时间，并且诊断行里报出了命中的引擎实例数。

覆盖不到的部分（如实标注）：
  * 数值**是否准确** —— 需要真实游戏和另一套可信来源对照，自动化测不了；
  * ETW 帧计时 —— 建会话需要管理员，测试环境不保证有。

用法： python tests/verify_metrics.py
"""
import ctypes
import ctypes.wintypes as wt
import io
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from verify_inject import inject, kill_all          # noqa: E402

u32 = ctypes.WinDLL("user32")
u32.FindWindowW.restype = wt.HWND
u32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]
u32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]

APP_LOG = os.path.join(os.environ["TEMP"], "NextPerf.log")


def find_pid(title):
    h = u32.FindWindowW(None, title)
    if not h:
        return 0
    pid = wt.DWORD()
    u32.GetWindowThreadProcessId(h, ctypes.byref(pid))
    return pid.value


def read_log_tail(marker):
    """返回最后一次『数据来源』块之后的全部行。"""
    if not os.path.exists(APP_LOG):
        return []
    lines = io.open(APP_LOG, encoding="utf-8", errors="ignore").read().splitlines()
    idx = [i for i, l in enumerate(lines) if marker in l]
    return lines[idx[-1]:] if idx else lines


def main():
    kill_all()
    try:
        os.remove(APP_LOG)
    except OSError:
        pass

    app = subprocess.Popen([os.path.join(ROOT, "dist", "NextPerf.exe")],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(4)

    # 关键：**先开 NextPerf，再开游戏宿主** —— 复现用户那种启动顺序，
    # 这正是曾经让 PDH 通配实例读不到的场景。
    host = subprocess.Popen([os.path.join(ROOT, "tests", "host_run.exe"),
                             "--seconds", "40"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(4)
    hpid = find_pid("NpHost")
    if not hpid:
        print("找不到测试宿主窗口")
        kill_all()
        return 1
    print("宿主 pid = %d" % hpid)

    inject(hpid, os.path.join(ROOT, "dist", "NextPerfHook.dll"))

    # 最多等 25 秒，等日志里出现一个有效的 GPU 帧时间
    ok, hits, value, reason = False, None, None, None
    for _ in range(25):
        time.sleep(1)
        for line in read_log_tail("数据来源"):
            m = re.search(r"命中 (\d+) 个引擎实例", line)
            if m:
                hits = int(m.group(1))
            m = re.search(r"GPU 帧时间\s*:\s*(-?[\d.]+) ms", line)
            if m:
                v = float(m.group(1))
                if v > 0:
                    value = v
                    ok = True
            m = re.search(r"PDH 未出数原因\s*:\s*(.+?)\)", line)
            if m:
                reason = m.group(1)
        if ok:
            break

    kill_all()

    if hits is None:
        print("  ❌ 日志里没有『命中 N 个引擎实例』—— PDH 根本没读到该进程的引擎")
        print("     GPU 帧时间行：%s" % [l for l in read_log_tail("数据来源") if "GPU 帧时间" in l][-1:])
        return 1
    if hits <= 0:
        print("  ❌ 命中引擎实例数为 0（PDH 动态实例没抓到该 pid）")
        return 1
    if not ok:
        print("  ❌ 命中了 %d 个引擎实例，但 GPU 帧时间始终没有正数" % hits)
        if reason:
            print("     最后的原因行：%s" % reason)
        return 1

    print("  ✅ 命中 %d 个引擎实例，GPU 帧时间 = %.2f ms（先开 NextPerf、后开游戏的顺序）"
          % (hits, value))
    print("结果：PASS —— PDH 指标链路端到端可用")
    print("注意：本用例只验证『有值』，不验证『值准不准』（那需要真实游戏对照）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
