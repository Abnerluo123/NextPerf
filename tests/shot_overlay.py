"""连拍：注入测试宿主后连续截 N 张叠加画面，用来看**颜色是否闪烁**与排版。

用法： python tests/shot_overlay.py [张数] [间隔秒]
输出： tests/_ov1.png / _ov2.png ...（该目录下 *.png 已被 .gitignore 排除）

为什么要连拍：颜色闪烁是**逐帧**现象，单张截图看不出来 —— 必须连拍几张再对比。
"""
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from verify_inject import inject, kill_all          # noqa: E402

u32 = ctypes.WinDLL("user32")
# 必须让本进程 DPI 感知：否则在 225% 缩放下 GetWindowRect 返回虚拟化坐标，
# 与 ImageGrab 的物理像素坐标系对不上，裁出来的区域会整体偏移。
try:
    u32.SetProcessDPIAware()
except Exception:
    pass
u32.FindWindowW.restype = wt.HWND
u32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]
u32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 3
    gap = float(sys.argv[2]) if len(sys.argv) > 2 else 1.2
    d3d11 = "--d3d11" in sys.argv

    kill_all()
    subprocess.Popen([os.path.join(ROOT, "dist", "NextPerf.exe")],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(3)
    host_args = [os.path.join(ROOT, "tests", "host_run.exe"), "--seconds", "60"]
    if d3d11:
        host_args.append("--d3d11")
    host = subprocess.Popen(host_args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(3)

    h = u32.FindWindowW(None, "NpHost")
    pid = wt.DWORD()
    u32.GetWindowThreadProcessId(h, ctypes.byref(pid))
    hpid = pid.value
    print("宿主 pid = %d" % hpid)
    inject(hpid, os.path.join(ROOT, "dist", "NextPerfHook.dll"))

    # 等钩子接管 + 让曲线攒够样本
    time.sleep(8)

    from PIL import ImageGrab
    u32.ShowWindow(h, 9)          # SW_RESTORE
    u32.BringWindowToTop(h)
    u32.SetForegroundWindow(h)
    time.sleep(1.5)
    r = wt.RECT()
    u32.GetWindowRect(h, ctypes.byref(r))
    print("宿主窗口矩形 (%d,%d)-(%d,%d) %dx%d"
          % (r.left, r.top, r.right, r.bottom, r.right - r.left, r.bottom - r.top))

    shots = []
    for i in range(n):
        full = ImageGrab.grab(all_screens=True)
        # DPI 已修正，直接按宿主窗口矩形裁
        r2 = wt.RECT()
        u32.GetWindowRect(h, ctypes.byref(r2))
        crop = full.crop((r2.left, r2.top, r2.right, r2.bottom))
        out = os.path.join(HERE, "_ov%d.png" % (i + 1))
        crop.save(out)
        shots.append(out)
        print("  已保存 %s（%dx%d）" % (out, crop.width, crop.height))
        if i < n - 1:
            time.sleep(gap)

    kill_all()
    print("连拍完成：%s" % ", ".join(os.path.basename(s) for s in shots))
    return 0


if __name__ == "__main__":
    sys.exit(main())
