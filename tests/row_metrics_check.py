# -*- coding: utf-8 -*-
"""从叠加截图里量出每个文字行的位置、高度与行距，用于判断行高是否均匀。"""
import io
import os
import glob
import sys

from PIL import Image
import numpy as np

HERE = r"C:\Users\Abner\Documents\deepseek-harness\default-workspace\NextPerf\tests"


def main():
    files = sorted(glob.glob(os.path.join(HERE, "_ov*.png")))
    if not files:
        print("没有截图，先跑 tests/shot_overlay.py")
        return 1
    im = Image.open(files[0]).convert("L")
    a = np.array(im)
    h, w = a.shape
    # 只看面板内部（右半），并排除图表区里的竖网格线：按整行亮像素数判断
    x0, x1 = int(w * 0.48), int(w * 0.99)
    sub = a[:, x0:x1]
    bright = (sub > 120).sum(axis=1)
    rows = bright > 3

    bands = []
    y = 0
    while y < h:
        if rows[y]:
            s = y
            while y + 1 < h and rows[y + 1]:
                y += 1
            e = y
            if e - s >= 4:          # 太矮的当噪声（曲线、边框）
                bands.append((s, e))
        y += 1

    print("  共 %d 个文字行（已滤掉曲线/边框）" % len(bands))
    print("  %-8s %-8s %-8s %s" % ("行首y", "行尾y", "字高", "与上一行的间距"))
    prev = None
    gaps = []
    for (s, e) in bands:
        gap = "" if prev is None else str(s - prev)
        if prev is not None:
            gaps.append(s - prev)
        prev = e
        print("  %-8d %-8d %-8d %s" % (s, e, e - s + 1, gap))
    if gaps:
        print()
        print("  行距分布: min=%d max=%d  不同取值=%s"
              % (min(gaps), max(gaps), sorted(set(gaps))))
        if max(gaps) - min(gaps) <= 3:
            print("  => 行距均匀 ✅")
        else:
            print("  => 行距不均匀 ❌（差 %d px）" % (max(gaps) - min(gaps)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
