# -*- coding: utf-8 -*-
"""用模拟游戏复现「改分辨率 / 开关垂直同步 / 切窗口状态」导致的闪退。

## 为什么写这个
用户报了三个闪退：游戏内改分辨率、开关垂直同步、切窗口/全屏。
它们**全都走交换链重建**（ResizeBuffers），而子代理全仓 grep 证实：
**我们根本没有挂钩 ResizeBuffers**。
推测的因果链：
    不挂钩 ResizeBuffers -> D3D12 的 RTV 描述符引用没在重建前释放
    -> ResizeBuffers 失败（DXGI_ERROR_INVALID_CALL）
    -> 很多游戏不检查返回值 -> 直接闪退
这个脚本就是来验证它：往模拟游戏里注入钩子，然后发重建类命令，看崩不崩。

## 为什么可信
模拟游戏**自己知道**交换链重建了几次、Present 失败几次、代数是多少
（逐帧 JSON 里的 resize_event / swapchain_generation / present_failed），
所以「是不是我们的钩子把它搞崩的」有客观对照，不靠猜。

## 用法
    python tests/sim_stress.py                 # 默认跑一轮完整场景
    python tests/sim_stress.py --api=dx12
    python tests/sim_stress.py --no-inject     # 对照组：不注入，看它自己稳不稳
"""
import argparse
import ctypes
import glob
import json
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIM = os.path.join(ROOT, "tests", "sim", "sim.exe")
HOOK = os.path.join(ROOT, "dist", "NextPerfHook.dll")
HOOK_LOG = os.path.join(os.environ.get("TEMP", "."), "NextPerfHook.log")

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = ctypes.c_void_p
k32.VirtualAllocEx.restype = ctypes.c_void_p
k32.WriteProcessMemory.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                                   ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.GetProcAddress.restype = ctypes.c_void_p
k32.GetProcAddress.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
k32.CreateRemoteThread.restype = ctypes.c_void_p


def inject(pid, dll):
    """标准 CreateRemoteThread + LoadLibraryW 注入（64 位）。"""
    PROCESS_ALL = 0x1F0FFF
    h = k32.OpenProcess(PROCESS_ALL, False, pid)
    if not h:
        raise OSError("OpenProcess 失败 err=%d" % ctypes.get_last_error())
    buf = dll.encode("utf-16-le") + b"\x00\x00"
    MEM_COMMIT_RESERVE, PAGE_RW = 0x3000, 0x04
    remote = k32.VirtualAllocEx(h, None, len(buf), MEM_COMMIT_RESERVE, PAGE_RW)
    if not remote:
        raise OSError("VirtualAllocEx 失败")
    written = ctypes.c_size_t(0)
    if not k32.WriteProcessMemory(h, remote, buf, len(buf), ctypes.byref(written)):
        raise OSError("WriteProcessMemory 失败")
    k32.GetModuleHandleW.restype = ctypes.c_void_p
    k32.GetModuleHandleW.argtypes = [ctypes.c_wchar_p]
    k32.LoadLibraryW.restype = ctypes.c_void_p
    k32.LoadLibraryW.argtypes = [ctypes.c_wchar_p]
    local_k32 = k32.GetModuleHandleW("kernel32.dll")
    load_lib = k32.GetProcAddress(local_k32, b"LoadLibraryW")
    th = k32.CreateRemoteThread(h, None, 0, ctypes.c_void_p(load_lib), remote, 0, None)
    if not th:
        raise OSError("CreateRemoteThread 失败 err=%d" % ctypes.get_last_error())
    k32.WaitForSingleObject(th, 15000)
    code = ctypes.c_ulong(0)
    k32.GetExitCodeThread(th, ctypes.byref(code))
    k32.CloseHandle(th)
    k32.CloseHandle(h)
    return code.value


def read_hook_log(pid):
    """读钩子日志（注意：新行在文件开头）。"""
    if not os.path.exists(HOOK_LOG):
        return []
    with open(HOOK_LOG, "rb") as f:
        txt = f.read().decode("gbk", "replace")
    return [l for l in txt.splitlines() if ("pid=%d " % pid) in l]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--api", default="dx11")
    ap.add_argument("--no-inject", action="store_true", help="对照组：不注入钩子")
    ap.add_argument("--seconds", type=int, default=25)
    args = ap.parse_args()

    if not os.path.exists(SIM):
        print("找不到模拟器：%s（先跑 tests/sim/build_sim.bat）" % SIM)
        return 2

    # 钩子日志先备份，避免与旧内容混淆
    if os.path.exists(HOOK_LOG) and not args.no_inject:
        os.replace(HOOK_LOG, HOOK_LOG + ".bak")

    print("启动模拟器 api=%s，计划跑 %d 秒 ..." % (args.api, args.seconds))
    p = subprocess.Popen([SIM, "--api=" + args.api, "--seconds=%d" % args.seconds,
                          "--json", "--json-every=30"],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, text=True, bufsize=1)
    time.sleep(3.0)
    if p.poll() is not None:
        print("模拟器提前退出（rc=%s）—— 环境问题，不是钩子的问题" % p.returncode)
        return 2

    if not args.no_inject:
        print("注入钩子到 pid=%d ..." % p.pid)
        rc = inject(p.pid, HOOK)
        print("  远端 LoadLibraryW 返回 = 0x%X（非 0 即成功）" % rc)
        time.sleep(4.0)   # 等钩子完成探测与挂钩
    else:
        print("对照组：不注入")

    # 场景：故意做那些会触发交换链重建的操作
    steps = [
        ("resize 1600 900", "改分辨率 1"),
        ("resize 1280 720", "改分辨率 2（回退）"),
        ("vsync off", "关垂直同步"),
        ("vsync on", "开垂直同步"),
        ("window borderless", "切无边框"),
        ("window windowed", "切回窗口"),
        ("fpscap 60", "锁帧 60"),
        ("fpscap 0", "解除锁帧"),
        ("window borderless", "再切无边框"),
        ("resize 1920 1080", "改分辨率 3"),
        ("vsync off", "再关垂直同步"),
        ("vsync on", "再开垂直同步"),
    ]

    died_at = None
    for i, (cmd, desc) in enumerate(steps):
        if p.poll() is not None:
            died_at = "（在 '%s' **之前**就已退出）" % desc
            break
        print("  [%2d/%d] %-22s %s" % (i + 1, len(steps), cmd, desc))
        try:
            p.stdin.write(cmd + "\n")
            p.stdin.flush()
        except Exception as e:
            died_at = "（写 '%s' 时管道断开：%s）" % (cmd, e)
            break
        time.sleep(1.6)
        if p.poll() is not None:
            died_at = "（刚发完 '%s' 就退出）" % cmd
            break

    time.sleep(1.5)
    alive = p.poll() is None
    rc = p.returncode
    try:
        p.stdin.write("quit\n")
        p.stdin.flush()
    except Exception:
        pass
    try:
        p.wait(timeout=8)
    except Exception:
        p.kill()
    rc = p.returncode

    # 收结果
    try:
        out = p.stdout.read() or ""
    except Exception:
        out = ""
    frames = []
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("{"):
            try:
                frames.append(json.loads(line))
            except Exception:
                pass

    print("\n" + "=" * 68)
    print("  结果：模拟器 %s（退出码 %s）" % ("存活" if alive else "已退出", rc))
    if died_at:
        print("  ★ 崩溃/退出时机：%s" % died_at)
    if frames:
        last = frames[-1]
        print("  最后采样帧：frame=%s gen=%s resize_event=%s present_failed=%s"
              % (last.get("frame"), last.get("swapchain_generation"),
                 last.get("resize_event"), last.get("present_failed")))
        print("  采样帧数：%d" % len(frames))
    if not args.no_inject:
        lines = read_hook_log(p.pid)
        seh = [l for l in lines if "SEH" in l]
        print("  钩子日志行数（本 pid）：%d" % len(lines))
        print("  ★ SEH（崩溃）条数：%d" % len(seh))
        for l in seh[:3]:
            print("     %s" % l.strip()[:130])
        for key in ("SELFCHECK", "overlay 5s", "detachPid"):
            hits = [l for l in lines if key in l]
            if hits:
                print("  %s：%d 条，例如 %s" % (key, len(hits), hits[0].strip()[:110]))
    print("=" * 68)
    return 0 if alive else 1


if __name__ == "__main__":
    sys.exit(main())
