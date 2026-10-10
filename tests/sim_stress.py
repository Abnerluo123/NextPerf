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
    """复用 verify_inject.py 里**已经实测可用**的注入实现。

    自己手写的那版 OpenProcess 会被拒（err=5 ACCESS_DENIED）—— 多半是
    ctypes 的 argtypes/handle 声明细节问题。verify_inject.py 的 inject()
    是反复实测过的，直接导入，不要在同一个坑里踩第二次。
    """
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import verify_inject as vi
    vi.inject(pid, dll)
    return 1   # 成功（它会自己断言/抛异常，这里只表示没抛）


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
    ap.add_argument("--seconds", type=int, default=90,
                    help="模拟器时限；必须大于 12 步场景的耗时，否则会把「到时自动退出」误判成崩溃")
    args = ap.parse_args()

    if not os.path.exists(SIM):
        print("找不到模拟器：%s（先跑 tests/sim/build_sim.bat）" % SIM)
        return 2

    # 钩子日志先备份，避免与旧内容混淆
    if os.path.exists(HOOK_LOG) and not args.no_inject:
        os.replace(HOOK_LOG, HOOK_LOG + ".bak")

    print("启动模拟器 api=%s，计划跑 %d 秒 ..." % (args.api, args.seconds))
    # ⚠ 一定要抓 stderr：模拟器的诊断信息（SimLog）全走 stderr，
    #   包括「ResizeBuffers 失败 -> 退回重建整条交换链」这类关键线索。
    #   上一版把它丢进 DEVNULL，结果崩溃时完全没有诊断信息可看。
    err_path = os.path.join(os.environ.get("TEMP", "."), "sim_stderr.txt")
    err_f = open(err_path, "w", encoding="utf-8", errors="replace")
    p = subprocess.Popen([SIM, "--api=" + args.api, "--seconds=%d" % args.seconds,
                          "--json", "--json-every=30"],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=err_f, text=True, bufsize=1)
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
    # 模拟器自己的诊断（SimLog 走 stderr）—— 崩溃时全靠它定位
    try:
        err_f.flush()
        err_f.close()
        with open(err_path, "r", encoding="utf-8", errors="replace") as f:
            err_lines = [l.rstrip() for l in f if l.strip()]
        print("  模拟器 stderr 共 %d 行，下面是最关键的部分：" % len(err_lines))
        key = [l for l in err_lines if any(k in l for k in
               ("ResizeBuffers", "INVALID_CALL", "重建", "崩溃", "异常", "错误",
                "失败", "错误码", "退出", "generation", "window", "borderless"))]
        for l in (key[-10:] if key else err_lines[-10:]):
            print("     %s" % l[:135])
    except Exception as e:
        print("  （读 stderr 失败：%s）" % e)
    print("=" * 68)
    return 0 if alive else 1


if __name__ == "__main__":
    sys.exit(main())
