"""回归：**纯 DX12 游戏 + 进程里有 vulkan-1.dll** 时，钩子必须仍然认出 DX12 并挂上 Present。

事故回顾（2026-10-11 用户实测 · 《漫威蜘蛛侠》）
    用户报：「以前读出来是 DX12，现在这个版本显示 Vulkan，而且完全没数据」。
    钩子日志把整条链记得死死的：
        attach: hostGraphicsUp=1 hostApi=20 probe at +800ms       ← 注入时游戏已经有交换链了，API 被猜成 Vulkan
        probe disabled: host api=20 (non-D3D), refusing to create a D3D device here
        RestoreAllHooks: swapVt=0 …                               ← 交换链一次都没挂钩
    根因（`np_hook.cpp` 里两处 API 判定口径不一致 + 自我导入排除的副作用）：
      1. 我们的钩子 DLL 静态导入 `d3d12.dll` / `d3d11.dll` / `dxgi.dll`，
         而 `SelfImports()` 会把"我们自己带进来的"模块全部跳过
         → 最强的两个 D3D 信号**永远被跳过**；
      2. 剩下 `vulkan-1.dll`（我们没导入）就命中了
         → 纯 DX12 游戏被判成 Vulkan → `hostIsD3D=false` → 探测拒绝运行；
      3. 而这时**只有探测能救**：加了 10 秒自动注入延迟后属于"晚注入"，
         游戏的交换链早就建好了，工厂钩子抓不到创建过程，只能靠探测去补挂钩子。
    修法：把 `D3D12Core.dll` 放在判定最前面 —— 它是 `d3d12.dll` **真正创建 D3D12 设备时**
    才拉起的运行时，我们的导入表里没有它（实测：纯 DX12 进程里有、我们自己的进程里没有）。

本用例怎么复现那个组合
    `host_run.exe --decoy-vulkan`：**照常跑 DX12**，但额外 `LoadLibrary("vulkan-1.dll")`
    —— 就是《漫威蜘蛛侠》那个组合。断言三件事：
      1. 遥测 `attached=1` 且帧数在涨（说明 Present 真挂上了）；
      2. 钩子日志里 `attach: … hostApi=12`（不是 20）；
      3. 钩子日志里**没有** `probe disabled: … (non-D3D)`。

用法： python tests/verify_dx12_decoyvk.py
退出码：0 = PASS ｜ 1 = FAIL ｜ 2 = 环境不满足（跳过）
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

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
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
    """钩子日志是 UTF-8（不是 GBK）。"""
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

    # 这个用例需要进程里真的有 vulkan-1.dll 可加载
    if not os.path.exists(os.path.join(os.environ.get("SystemRoot", r"C:\Windows"),
                                       "System32", "vulkan-1.dll")):
        print("环境不满足：系统里没有 vulkan-1.dll，无法复现该组合。")
        return 2

    if os.path.exists(LOG):
        os.remove(LOG)

    print("=== 1. 起主程序 ===")
    np = subprocess.Popen([NP_EXE], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    STARTED.append(np)
    if not find_window("NextPerfMainCls", np.pid, 15):
        print("环境不满足：NextPerf 主窗口没出现")
        return 2
    time.sleep(1.5)

    print("=== 2. 起宿主（DX12 + 诱饵 vulkan-1.dll）===")
    host = subprocess.Popen([HOST_EXE, "--decoy-vulkan", "--seconds", "40"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    STARTED.append(host)
    hpid = host.pid
    if not find_window("NpHostCls", hpid, 15):
        print("环境不满足：宿主窗口没出现")
        return 2
    time.sleep(2.5)

    print("=== 3. 注入（晚注入：宿主的交换链已经建好了）===")
    inject(hpid, HOOK_DLL)

    running = False
    last_ft = 0
    t0 = time.time()
    while time.time() - t0 < 25:
        t = read_telemetry(hpid)
        if t:
            if getattr(t, "attached", 0) and t.frameTotal > last_ft + 5:
                running = True
                break
            last_ft = max(last_ft, getattr(t, "frameTotal", 0))
        time.sleep(0.5)

    text = hook_text()
    mine = [l for l in text.splitlines() if ("pid=%d " % hpid) in l]
    attach = next((l for l in mine if "attach: hostGraphicsUp" in l), "")
    disabled = [l for l in mine if "probe disabled" in l]
    attempts = [l for l in mine if "probe attempt" in l]

    t = read_telemetry(hpid)
    print("  遥测：attached=%s frameTotal=%s gfxApi=%s"
          % (getattr(t, "attached", "?"), getattr(t, "frameTotal", "?"),
             getattr(t, "gfxApi", "?")))
    print("  钩子日志：%s" % (attach.strip() or "(没有 attach 行)"))
    for l in attempts[:3]:
        print("    " + l.strip())
    for l in disabled:
        print("    ⚠ " + l.strip())

    fails = []
    if not running:
        fails.append("DX12 宿主没有被挂上（relive：判成 Vulkan → 探测被禁 → 没数据）")
    if "hostApi=20" in attach:
        fails.append("API 仍然被判成 Vulkan（hostApi=20），应当是 12")
    if disabled:
        fails.append("出现了 `probe disabled … (non-D3D)` —— 探测又被错误的 API 猜测挡住了")
    if getattr(t, "gfxApi", 0) not in (12,):
        fails.append("遥测里的 gfxApi=%s，应当是 12（DX12）" % getattr(t, "gfxApi", "?"))

    print()
    if fails:
        print("结果：FAIL")
        for f in fails:
            print("  - %s" % f)
        return 1
    print("结果：PASS —— 进程里有 vulkan-1.dll 也不影响认出 DX12 并挂上 Present")
    return 0


if __name__ == "__main__":
    sys.exit(main())
