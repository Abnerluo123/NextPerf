# -*- coding: utf-8 -*-
"""量出每张连拍图里面板的左边缘 —— 可以用来判断面板宽度是否随数值抖动。

面板在宿主窗口内是**右对齐**的，所以它宽度一变，左边缘就会左右移动。
所以「左边缘是否稳定」就是「宽度是否稳定」的判据。
"""
import io
import os
import sys

from PIL import Image
import numpy as np

HERE = r"C:\Users\Abner\Documents\deepseek-harness\default-workspace\NextPerf\tests"

def panel_left(path):
    """面板在宿主窗口内右对齐。宿主是蓝色底（B 明显大于 R），面板是近黑。
    找出「蓝底 → 近黑」的分界列，多行取中位数。"""
    im = Image.open(path).convert("RGB")
    a = np.array(im).astype(np.int16)
    h, w, _ = a.shape
    R, G, B = a[:, :, 0], a[:, :, 1], a[:, :, 2]
    host_blue = (B - R) > 25            # 宿主的蓝色背景
    panel_dark = (R < 55) & (G < 55) & (B < 55)

    lefts = []
    for y in range(int(h * 0.12), int(h * 0.35), 3):
        row_dark = panel_dark[y]
        row_blue = host_blue[y]
        # 从右往左找：先要看到面板的暗像素，再往左直到蓝色背景结束
        xs = np.nonzero(row_dark)[0]
        if len(xs) < 50:
            continue
        rightmost_dark = xs.max()
        # 从 rightmost_dark 往左走，找最后一段连续暗区的起点
        x = rightmost_dark
        while x > 0 and row_dark[x]:
            x -= 1
        # 再往左，可能有文字把它切断；继续往左找蓝底与暗区的真正交界
        while x > 0:
            if row_blue[x] and not row_dark[x]:
                # 蓝底；再往左若干像素若仍是蓝底，就认定这里是面板左界
                seg = row_blue[max(0, x - 30):x]
                if seg.mean() > 0.8:
                    break
            x -= 1
        lefts.append(x + 1)
    if not lefts:
        return None, None
    lefts.sort()
    l = lefts[len(lefts) // 2]
    return int(l), int(w) - 60          # 面板右缘大致贴窗口右边（留边框余量）


def main():
    files = sorted(f for f in os.listdir(HERE)
                   if f.startswith("_ov") and f.endswith(".png"))
    if not files:
        print("没有连拍图（先跑 tests/shot_overlay.py）")
        return 1
    print("  %-12s %-10s %-10s %s" % ("文件", "左边缘", "右边缘", "宽度"))
    lefts = []
    for f in files:
        l, r = panel_left(os.path.join(HERE, f))
        if l is None:
            print("  %-12s 未定位到面板" % f)
            continue
        lefts.append(l)
        print("  %-12s %-10d %-10d %d" % (f, l, r, r - l))
    if len(lefts) >= 2:
        spread = max(lefts) - min(lefts)
        print()
        print("  左边缘极差 = %d px  ->  %s"
              % (spread, "稳定 ✅（宽度不再随数值变化）" if spread <= 2
                 else "仍在抖动 ❌（宽度随数值变化）"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
