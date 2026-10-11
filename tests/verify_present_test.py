"""回归：游戏每帧先来一次 `DXGI_PRESENT_TEST` 再真呈现时，`Busy + Wait` 必须仍等于**整个帧周期**。

事故回顾（2026-10-11 用户实测 ·《漫威蜘蛛侠》）
    用户报「CPU 帧时间 0.几 ms 太异常，之前是 2-3ms」。钩子日志：
        present diag 5s: calls=600 real=300 test=300      ← 每帧调两次 Present（一次探测 + 一次真的）
        cpu split avg: busy=0.35 wait=7.88 sum=8.23       ← 而帧周期是 16.7ms（**正好一半**）
    根因：`PresentCommon` 收尾处那三行
        gRenderTid = GetCurrentThreadId();
        gPresentRetQpc = tPresent1;     ← ★ 漏在 realPresent 判断之外
        gSimEndQpc = 0;
    `gPresentRetQpc` 是"上一帧返回时刻"，也就是 Busy 与模拟阶段的**基准**。被**探测调用**的返回
    覆盖之后，基准落到帧中间 → Busy + Wait 只剩半个帧周期 → 面板（显示 `cpuBusyAvg`）掉到 0.3ms。
    对照：帧记账那边的 `gLastPresentQpc` 本来就有 `realPresent` 判断，所以这是**判断标准不一致**。

本用例怎么复现
    `host_run.exe --probe-present`：**每次真 Present 之前先调一次 `Present(0, DXGI_PRESENT_TEST)`**。
    宿主自己的帧周期约 32ms（Present 阻塞约 16ms + Sleep 16ms），所以：
      * 修复前：sum ≈ wait ≈ 16ms ≈ **半个**帧周期 → 比值 ~0.5 → FAIL
      * 修复后：sum ≈ 32ms ≈ 整个帧周期 → 比值 ~1.0 → PASS

断言用的都是日志里的**原始量**（不引入新的口径）：
    * `cpu split avg: busy=… wait=… sum=…`（10 秒一条）
    * `present diag 5s: … avg=…`（真实呈现的平均间隔 = 帧周期）

用法： python tests/verify_present_test.py
退出码：0 = PASS ｜ 1 = FAIL ｜ 2 = 环境不满足（跳过）
"""
import atexit
import ctypes
import ctypes.wintypes as wt
import os
import re
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

    if os.path.exists(LOG):
        os.remove(LOG)

    print("=== 1. 起主程序 ===")
    np = subprocess.Popen([NP_EXE], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    STARTED.append(np)
    if not find_window("NextPerfMainCls", np.pid, 15):
        print("环境不满足：NextPerf 主窗口没出现")
        return 2
    time.sleep(1.5)

    print("=== 2. 起宿主（DX12，每帧先探测一次 Present）===")
    host = subprocess.Popen([HOST_EXE, "--probe-present", "--seconds", "45"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    STARTED.append(host)
    hpid = host.pid
    if not find_window("NpHostCls", hpid, 15):
        print("环境不满足：宿主窗口没出现")
        return 2
    time.sleep(2.5)

    print("=== 3. 注入并等帧在流动 ===")
    inject(hpid, HOOK_DLL)
    running, last_ft = False, 0
    t0 = time.time()
    while time.time() - t0 < 25:
        t = read_telemetry(hpid)
        if t:
            if getattr(t, "attached", 0) and t.frameTotal > last_ft + 5:
                running = True
                break
            last_ft = max(last_ft, getattr(t, "frameTotal", 0))
        time.sleep(0.5)
    if not running:
        print("环境不满足：注入后没读到增长中的帧数")
        return 2

    print("=== 4. 等两条诊断日志（cpu split 是 10 秒一条）===")
    split_line, diag_line = "", ""
    t0 = time.time()
    while time.time() - t0 < 35:
        mine = [l for l in hook_text().splitlines() if ("pid=%d " % hpid) in l]
        for l in reversed(mine):
            if "cpu split avg:" in l and not split_line:
                split_line = l
            if "present diag 5s:" in l and not diag_line:
                diag_line = l
            if split_line and diag_line:
                break
        if split_line and diag_line:
            break
        time.sleep(1)

    if not split_line or not diag_line:
        print("环境不满足：没等到诊断行（split=%s diag=%s）" % (bool(split_line), bool(diag_line)))
        for l in [x for x in hook_text().splitlines() if ("pid=%d " % hpid) in x][-6:]:
            print("    " + l)
        return 2

    print("  " + split_line.strip())
    print("  " + diag_line.strip())

    m = re.search(r"busy=([\d.]+) wait=([\d.]+) sum=([\d.]+)", split_line)
    d = re.search(r"real=(\d+) test=(\d+)", diag_line)
    a = re.search(r"avg=([\d.]+)", diag_line)
    if not (m and d and a):
        print("环境不满足：诊断行格式没解析出来")
        return 2
    busy, wait, summ, period = float(m.group(1)), float(m.group(2)), float(m.group(3)), float(a.group(1))
    real, test = int(d.group(1)), int(d.group(2))

    print("  busy=%.2f wait=%.2f sum=%.2f | 帧周期 avg=%.2f | real=%d test=%d"
          % (busy, wait, summ, period, real, test))
    ratio = summ / period if period > 0 else 0.0

    fails = []
    if test <= 0:
        fails.append("宿主没有产生 `DXGI_PRESENT_TEST` 调用（test=0），本用例没复现出那个模式")
    if ratio < 0.75:
        fails.append("Busy+Wait(%.2fms) 明显小于帧周期(%.2fms)，比值 %.2f —— "
                     "基准又被探测调用切成半个帧了" % (summ, period, ratio))
    if busy <= 0.0:
        fails.append("Busy 为 0，取不到有效值")

    print()
    if fails:
        print("结果：FAIL")
        for f in fails:
            print("  - %s" % f)
        return 1
    print("结果：PASS —— 每帧带一次 PRESENT_TEST 也不影响：Busy + Wait ≈ 帧周期（比值 %.2f）" % ratio)
    return 0


if __name__ == "__main__":
    sys.exit(main())
