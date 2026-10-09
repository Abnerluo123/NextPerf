"""截取 NextPerf 主窗口，用来肉眼对照 UI 改动。

用法： python tests/shot_ui.py [输出文件名]
说明：只截图、不改代码；输出默认写到 tests/_ui.png（该目录下 *.png 已被 .gitignore 排除）。
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
from verify_inject import kill_all, find_main_window_for_pid   # noqa: E402

u32 = ctypes.WinDLL("user32")


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "_ui.png")
    kill_all()
    proc = subprocess.Popen([os.path.join(ROOT, "dist", "NextPerf.exe")],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(5)

    hwnd = find_main_window_for_pid(proc.pid)
    if not hwnd:
        print("找不到主窗口")
        kill_all()
        return 1
    print("主窗口 hwnd=%s" % hwnd)

    u32.SetForegroundWindow(hwnd)
    time.sleep(1.0)

    r = wt.RECT()
    u32.GetWindowRect(hwnd, ctypes.byref(r))
    print("窗口矩形 (%d,%d)-(%d,%d)  %dx%d"
          % (r.left, r.top, r.right, r.bottom, r.right - r.left, r.bottom - r.top))

    # ⚠ 不能按 GetWindowRect 的 bbox 去抓：高 DPI（本项目实测 225%）下
    #   GetWindowRect 返回的坐标系与 ImageGrab 的抓屏坐标系不一致，
    #   抓出来的图会整体偏移（实测窗口只占画面右下角一小块）。
    #   改成抓**整个虚拟桌面**再自己看 —— 稳，且不依赖 DPI 换算。
    from PIL import ImageGrab
    img = ImageGrab.grab(all_screens=True)
    img.save(out)
    print("已保存 %s（整屏 %dx%d；NextPerf 窗口在其中 (%d,%d)-(%d,%d)）"
          % (out, img.width, img.height, r.left, r.top, r.right, r.bottom))

    kill_all()
    return 0


if __name__ == "__main__":
    sys.exit(main())
