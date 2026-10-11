"""回归：Present 看门狗**真的会报**（守 2026-10-11 加的诊断）。

为什么要测「诊断本身」
    看门狗是为了回答一个问题：游戏卡住时，是**游戏不再调 Present**，还是**卡在我们的
    Present 路径里**？两种结论对应完全不同的改法。它已经在真机上给出过正确结论
    （生化危机8 关窗后报「游戏侧停住了，不是卡在我们的 Present 路径里」），
    但**"它会报"这件事从来没有被自动化验证过** —— 一个永远不触发的诊断等于没有诊断。

本用例怎么造出「不 Present 但进程还活着」
    1) 起 NextPerf + 宿主，注入钩子，确认真的有帧在增长；
    2) 从钩子日志里找出**渲染线程**的 tid（打 `frame dist:` / `overlay 5s:` 的那个线程）；
    3) 只 `SuspendThread` 它 —— 钩子的 worker 线程（200ms 心跳）**不受影响**，仍然在跑；
    4) 看门狗阈值是「5 秒没有 Present 调用」，所以等 8 秒后应能在钩子日志里看到
       `watchdog: 已有 X.Xs 没有 Present 调用`；
    5) `ResumeThread` 之后应看到 `watchdog: Present 已恢复`。

    注意：**绝不能**挂起整个进程（那样 worker 也停了，看门狗当然不会报）。
    这也是这个用例最容易写错的地方。

用法： python tests/verify_watchdog.py
退出码：0 = PASS ｜ 1 = FAIL（看门狗没报）｜ 2 = 环境不满足（跳过）
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

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
u32 = ctypes.WinDLL("user32", use_last_error=True)
k32.OpenThread.restype = wt.HANDLE
k32.OpenThread.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.SuspendThread.argtypes = [wt.HANDLE]
k32.ResumeThread.argtypes = [wt.HANDLE]
k32.CloseHandle.argtypes = [wt.HANDLE]

THREAD_SUSPEND_RESUME = 0x0002
STARTED = []
SUSPENDED = []          # (tid, handle)，收尾时一定要 Resume


def cleanup():
    for tid, h in SUSPENDED:
        try:
            k32.ResumeThread(h)
            k32.CloseHandle(h)
        except Exception:
            pass
    del SUSPENDED[:]
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
    """钩子日志是 **UTF-8**（2026-10-11 实测核实；`docs/LOGGING.md` 曾写 GBK，是错的）。
    本用例要匹配**中文**关键字（`watchdog: 已有…`），编码搞错就会一直匹配不上。"""
    if not os.path.exists(LOG):
        return ""
    raw = open(LOG, "rb").read()
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        return raw.decode("gbk", "replace")


def pid_lines(pid):
    key = "pid=%d " % pid
    return [l for l in hook_text().splitlines() if key in l]


def render_tid(pid):
    """渲染线程 = 打 `frame dist:` / `overlay 5s:` / `present diag` 的那个 tid。

    这些日志都在 Present 的调用栈里打，所以就是渲染线程。worker 线程只打
    `worker: enter` / `watchdog:` —— 挂错线程这个用例就废了。
    """
    votes = {}
    for l in pid_lines(pid):
        if not any(k in l for k in ("frame dist:", "overlay 5s:", "present diag", "cpu split avg")):
            continue
        m = re.search(r"tid=(\d+)", l)
        if m:
            votes[int(m.group(1))] = votes.get(int(m.group(1)), 0) + 1
    if not votes:
        return None
    return max(votes.items(), key=lambda kv: kv[1])[0]


def wait_for(needle, pid, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        for l in pid_lines(pid):
            if needle in l:
                return l
        time.sleep(0.5)
    return None


def main():
    atexit.register(cleanup)

    if os.path.exists(LOG):
        os.remove(LOG)

    print("=== 1. 起主程序 + 宿主 ===")
    np = subprocess.Popen([NP_EXE], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    STARTED.append(np)
    time.sleep(1.5)
    host = subprocess.Popen([HOST_EXE, "--seconds", "90"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    STARTED.append(host)
    hpid = host.pid
    time.sleep(2.5)

    print("=== 2. 注入并等真的有帧 ===")
    inject(hpid, HOOK_DLL)
    last_ft, running = 0, False
    t0 = time.time()
    while time.time() - t0 < 25:
        t = read_telemetry(hpid)
        if t:
            if getattr(t, "attached", 0) and t.frameTotal > last_ft + 5:
                running = True
                break
            last_ft = max(last_ft, t.frameTotal)
        time.sleep(0.5)
    if not running:
        print("环境不满足：注入后没读到增长中的帧数（钩子没接管）")
        return 2
    print("  帧数在涨，钩子已接管")

    print("=== 3. 找出渲染线程并只挂起它 ===")
    tid = None
    t0 = time.time()
    while time.time() - t0 < 10 and tid is None:
        tid = render_tid(hpid)
        if tid is None:
            time.sleep(0.5)
    if tid is None:
        print("环境不满足：日志里找不到渲染线程的 tid")
        return 2
    h = k32.OpenThread(THREAD_SUSPEND_RESUME, False, tid)
    if not h:
        print("环境不满足：OpenThread(tid=%d) 失败（err=%d）" % (tid, ctypes.get_last_error()))
        return 2
    print("  渲染线程 tid=%d，SuspendThread（**不挂 worker**）" % tid)
    k32.SuspendThread(h)
    SUSPENDED.append((tid, h))

    print("=== 4. 看门狗阈值是 5 秒，等 9 秒 ===")
    line = wait_for("watchdog: 已有", hpid, 9.0)
    if line:
        print("  ✓ 报到了：" + line)
    else:
        print("  ✗ 没报到。pid=%d 的日志尾部：" % hpid)
        for l in pid_lines(hpid)[-8:]:
            print("    " + l)

    print("=== 5. 恢复渲染线程，应看到「Present 已恢复」===")
    k32.ResumeThread(h)
    k32.CloseHandle(h)
    SUSPENDED.clear()
    back = wait_for("watchdog: Present 已恢复", hpid, 8.0)
    if back:
        print("  ✓ " + back)
    else:
        print("  ✗ 没看到恢复行")

    fails = []
    if not line:
        fails.append("挂起渲染线程 9 秒，看门狗**没有**报「没有 Present 调用」")
    if not back:
        fails.append("恢复渲染线程后，看门狗**没有**报「Present 已恢复」")

    print()
    if fails:
        print("结果：FAIL")
        for f in fails:
            print("  - %s" % f)
        return 1
    print("结果：PASS —— 看门狗能报出「游戏侧停住了」，恢复后也能报回来")
    return 0


if __name__ == "__main__":
    sys.exit(main())
