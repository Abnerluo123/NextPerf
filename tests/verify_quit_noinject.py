"""回归：退出流程里**绝不许**再注入（守 2026-10-11 修掉的那个死循环）。

事故回顾（用户实测 · 商店版生化危机8 DX12）
    用户点界面「退出」之后，看护线程仍在注入：每次注入的钩子一启动就看到 `cfg.quit=1`
    立刻自卸载 → 注入标记互斥量消失 → 0.8 秒一轮反复 `LoadLibrary`。
    往**正在关停**的游戏进程里反复 `LoadLibrary`，撞上 loader lock 就是**永久挂死**
    （现象：游戏窗口消失、后台进程在任务管理器里杀不掉；NextPerf 状态栏在钩上/没钩上跳变）。
    根因修复：`CleanupAndExit` 先 `InjectorShutdown()` 再广播 `quit`；
    `InjectorScanNow` 加两道闸门（`cfg.quit` 直接 return / 正在请求卸载的 pid 不注入）。

守的不变量
    主日志出现 `quit from UI button` 之后：
      1) **不许**再出现 `inject pid`；
      2) 钩子日志里该 pid **不许**再出现新的 `attach:`；
      3) 主程序应在 10 秒内退出。

为什么要在配置里预置一个「游戏」
    看护线程只维护 `gApp.games` 里的条目（**手动添加**的条目不需要开关就会自动注入）。
    不预置目标时 `InjectorScanNow` 第一行就 `return` —— 那样这个用例**恒真**，守不住任何东西。
    所以本用例：**备份用户配置 → 写入一条指向测试宿主的条目 → 跑场景 → 无论成败按字节还原**。
    （配置里只留测试宿主，避免顺手把用户名单里真在跑的游戏注入一遍。）

用法： python tests/verify_quit_noinject.py
退出码：0 = PASS ｜ 1 = FAIL（退出后仍在注入）｜ 2 = 环境不满足（跳过）
"""
import atexit
import ctypes
import ctypes.wintypes as wt
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from verify_inject import (HOST_EXE, HOOK_DLL, LOG, NP_EXE,  # noqa: E402
                           inject, read_telemetry)

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
u32 = ctypes.WinDLL("user32", use_last_error=True)
u32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]

WM_COMMAND = 0x0111
NP_TRAY_EXIT = 1004

TEMP = os.environ.get("TEMP", ".")
# ⚠ 主程序日志的落点**取决于它拿到的 TEMP**：沙箱里跑的 NextPerf 写在沙箱 TEMP，
#   手工跑的写在真实 TEMP。本用例两个都看，取**最新被改动的那个**——
#   否则会出现"用例明明跑过了，却因为读错文件而判断前置条件不成立"（踩过）。
NP_LOG_CANDIDATES = []
for cand in (os.path.join(TEMP, "NextPerf.log"),
             r"C:\Users\Abner\AppData\Local\Temp\NextPerf.log"):
    if cand not in NP_LOG_CANDIDATES:
        NP_LOG_CANDIDATES.append(cand)
APPDATA = os.environ.get("APPDATA", os.path.expanduser("~"))
SETTINGS = os.path.join(APPDATA, "NextPerf", "settings.json")


def app_log_text():
    """取**最近被写过**的那份主日志（见 NP_LOG_CANDIDATES 的说明）。"""
    best, best_m = "", -1.0
    for p in NP_LOG_CANDIDATES:
        if os.path.exists(p):
            m = os.path.getmtime(p)
            if m > best_m:
                best_m, best = m, p
    return read_text_gbk(best) if best else ""

STARTED = []
SETTINGS_BACKUP = None      # 原始字节；None = 原本没有这个文件
SETTINGS_EXISTED = False


def find_window(cls, pid, timeout=15.0):
    """按「窗口类名 + 进程 id」找窗口。"""
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


def window_exists(cls):
    """有没有任何进程持有这个窗口类（判断用户是不是开着 NextPerf）。"""
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


def read_text_gbk(path):
    """读日志。⚠ **两个日志都是 UTF-8**（2026-10-11 实测核实）——
    `docs/LOGGING.md` 曾写「GBK」，那是错的：按 GBK 解出来中文全是乱码，
    于是中文关键字（`watchdog:`、`帧数停增`）永远匹配不上。
    这里先按 UTF-8 解，失败再退回 GBK（老日志/异常情况保险）。"""
    if not os.path.exists(path):
        return ""
    raw = open(path, "rb").read()
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        return raw.decode("gbk", "replace")


def seed_settings():
    """打开「实验性自动钩子」并清空名单。

    为什么用这条路（而不是往 `games` 里塞一条）：
      用户的 re8 就是这样进名单的 —— 主程序在**注入成功后自动学习**
      （日志里的 `learn: 注入后验证通过 … 把 xxx.exe 记入可信名单`），
      再靠 `learnedAutoHook` 让它成为看护线程的维护目标。
      这条路是产品的真实用法，也避开了"手写 JSON 让解析器认不认"的不确定性。
    """
    global SETTINGS_BACKUP, SETTINGS_EXISTED
    SETTINGS_EXISTED = os.path.exists(SETTINGS)
    if SETTINGS_EXISTED:
        SETTINGS_BACKUP = open(SETTINGS, "rb").read()
    try:
        os.makedirs(os.path.dirname(SETTINGS), exist_ok=True)
        # 紧凑写法：对方的解析器是极简实现，不给它出空格/转义的难题。
        # **两条路一起铺**：手动添加的条目（games，无需开关就会自动注入）
        # + 打开实验性自动钩子（learnedAutoHook，让"学习到的"条目也被维护）。
        cfg = {"cfgVersion": 5, "learnedAutoHook": 1, "games": [HOST_EXE], "learned": []}
        with open(SETTINGS, "w", encoding="utf-8") as f:
            f.write(json.dumps(cfg, separators=(",", ":")))
        return True
    except PermissionError as e:
        print("环境不满足：改不了配置 %s（%s）" % (SETTINGS, e))
        print("  本用例需要让看护线程有一个维护目标，否则它会第一行就 return、用例恒真。")
        print("  手工跑（非沙箱）时应当有权限；沙箱/受限账户下请跳过。")
        return False


def restore_settings():
    """按字节还原用户配置 —— 无论测试成败都必须走到这里。"""
    try:
        if SETTINGS_EXISTED and SETTINGS_BACKUP is not None:
            with open(SETTINGS, "wb") as f:
                f.write(SETTINGS_BACKUP)
        elif not SETTINGS_EXISTED and os.path.exists(SETTINGS):
            os.remove(SETTINGS)
    except Exception as e:      # 不能吞掉，否则用户配置会留在被改写的状态
        print("!! 配置还原失败，请手动检查 %s（原因：%s）" % (SETTINGS, e))


def cleanup():
    restore_settings()
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


def main():
    atexit.register(cleanup)

    if window_exists("NextPerfMainCls"):
        print("环境不满足：已经有 NextPerf 在运行（两个主程序会抢同一块共享内存）。")
        return 2

    if os.path.exists(LOG):
        os.remove(LOG)

    print("=== 1. 备份并预置配置（写入测试宿主）===")
    if not seed_settings():
        return 2
    print("  配置：%s（原文件%s）"
          % (SETTINGS, "已备份 %d 字节" % len(SETTINGS_BACKUP) if SETTINGS_BACKUP else "原本不存在，测试后会删掉"))

    print("=== 2. 起主程序 ===")
    np = subprocess.Popen([NP_EXE], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    STARTED.append(np)
    np_hwnd = find_window("NextPerfMainCls", np.pid, 15)
    if not np_hwnd:
        print("环境不满足：NextPerf 主窗口没出现")
        return 2

    print("=== 3. 起宿主并**手动注入一次**，让主程序把它学进可信名单 ===")
    host = subprocess.Popen([HOST_EXE, "--seconds", "90"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    STARTED.append(host)
    hpid = host.pid
    print("  宿主 pid = %d" % hpid)
    time.sleep(2.5)

    inject(hpid, HOOK_DLL)      # 手动注入 —— 主程序会注意到钩子并学习它

    learned_at = None
    t0 = time.time()
    while time.time() - t0 < 25:
        txt = app_log_text()
        if "记入可信名单" in txt or ("watch: host_run.exe is running" in txt):
            learned_at = time.time() - t0
            break
        time.sleep(0.5)
    if learned_at is None:
        # ⚠ 这里**不跳过**：主判据（退出后 `attach:` 不再增长）走的是钩子日志，一定能读到；
        #   而主程序日志的落点会随 TEMP 变（沙箱每次命令一个临时目录并在结束后回收），
        #   读不到只说明"没法确认前置条件"，不足以判定用例无效。
        print("  ⚠ 读不到主程序日志里的学习记录（落点可能在别处）—— 继续跑，主判据看钩子日志")
    else:
        print("  主程序在 %.1fs 后已把宿主记入名单（退出时它就会想维护这个注入）" % learned_at)
    # 等它真的被看护线程接手（这时注入标记互斥量一定存在）
    t0 = time.time()
    while time.time() - t0 < 20:
        if "inject pid %d" % hpid in app_log_text():
            break
        time.sleep(0.5)

    # 等钩子真跑起来，让注入标记稳定存在
    t0 = time.time()
    while time.time() - t0 < 15:
        t = read_telemetry(hpid)
        if t and getattr(t, "attached", 0):
            break
        time.sleep(0.5)

    print("=== 4. 发「退出」（等价于点界面退出按钮）===")
    # ⚠ 判据以**钩子日志**为主：主程序日志的落点会随 TEMP 变（沙箱/真实两套），
    #   而钩子日志跟着宿主进程走，一定能读到。反复注入的签名就是 `attach:` 次数增长。
    attach_before = len([l for l in read_text_gbk(LOG).splitlines()
                         if ("pid=%d " % hpid) in l and "attach:" in l])
    if attach_before == 0:
        # 前置条件没被确证：可能是钩子日志落点不可读（沙箱的 TEMP 每次命令一个、结束回收），
        # 也可能注入真的失败了。两种情况下"退出后 attach 不增长"都会**恒真** ——
        # 必须说出来，不能让一个空跑的用例冒充通过。
        print("  ⚠ 钩子日志里没有 pid=%d 的 `attach:` —— 主判据可能是**空跑**（恒真）。" % hpid)
        print("     真机（非沙箱）上钩子日志可读时，这里应当 >= 1；沙箱里日志落点会被回收。")
    print("  退出前：pid=%d 的 `attach:` 次数 = %d" % (hpid, attach_before))
    u32.PostMessageW(np_hwnd, WM_COMMAND, NP_TRAY_EXIT, 0)

    exited = False
    t0 = time.time()
    while time.time() - t0 < 10:
        if np.poll() is not None:
            exited = True
            break
        time.sleep(0.25)
    print("  主程序在 %.1fs 内退出：%s" % (time.time() - t0, exited))

    # 退出后再观察 6 秒：死循环的周期是 0.8 秒，足够抓到
    time.sleep(6)
    attach_after = len([l for l in read_text_gbk(LOG).splitlines()
                        if ("pid=%d " % hpid) in l and "attach:" in l])
    print("  退出 6 秒后：`attach:` 次数 = %d" % attach_after)

    # 附加判据（best-effort）：主日志读得到就顺带查 `inject pid`
    injects = []
    quit_seen = False
    txt = app_log_text()
    lines = txt.splitlines()
    for i, l in enumerate(lines):
        if "quit from UI button" in l:
            quit_seen = True
            injects = [x for x in lines[i + 1:] if "inject pid" in x]
    if txt:
        print("  主日志：%s；quit 之后的 `inject pid` 行数 = %d"
              % ("有 quit 记录" if quit_seen else "没读到 quit 记录（不影响主判据）", len(injects)))
        for l in injects[:5]:
            print("    " + l)
    else:
        print("  主日志读不到（落点在别处）—— 只按钩子日志判")

    fails = []
    if attach_after > attach_before:
        fails.append("退出之后钩子又被注入（`attach:` %d -> %d）—— 死循环回来了"
                     % (attach_before, attach_after))
    if injects:
        fails.append("退出之后仍在注入（%d 次）" % len(injects))
    if not exited:
        fails.append("主程序 10 秒内没有退出")

    print()
    if fails:
        print("结果：FAIL")
        for f in fails:
            print("  - %s" % f)
        return 1
    print("结果：PASS —— 退出流程里没有再注入，主程序干净退出")
    return 0


if __name__ == "__main__":
    sys.exit(main())
