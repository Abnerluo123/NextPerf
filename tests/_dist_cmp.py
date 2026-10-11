# -*- coding: utf-8 -*-
"""比较「钩子的帧时间分布」与「模拟器的帧时间分布」。

## 为什么只比平均值不够
verify_framedata.py 比的是平均值，结果 0.4% 偏差（很准）。但**平均值对不代表分布对**：
如果钩子把某些帧记成两倍、另一些记成一半，平均值照样能对上。
所以要逐项比 **P50 / P99 / P99.9 / Max** —— 两边都正好提供这些字段：
  * 模拟器汇总：frame_ms_p50 / p99 / p999 / max
  * 钩子遥测：  frameMs / p99Ms / p999Ms（另有 frameMsAvg）

## 判读
* 四个分位都接近 -> 钩子的帧时间分布是对的，用户看到的抖动是真抖动；
* 钩子的 P99/P99.9 明显偏大 -> 钩子在**加料**（例如把一次 Present 记成多帧、
  或把重建/停顿帧算进来）；
* 钩子的 P50 明显偏小 -> 有子帧间隔漏进来（帧内多次 Present 没被合并干净）。
"""
import json
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIM = os.path.join(ROOT, "tests", "sim", "sim.exe")
HOOK = os.path.join(ROOT, "dist", "NextPerfHook.dll")
sys.path.insert(0, os.path.join(ROOT, "tests"))

SECONDS = 20


def main():
    out = os.path.join(os.environ.get("TEMP", "."), "dc_out.txt")
    err = os.path.join(os.environ.get("TEMP", "."), "dc_err.txt")
    # vsync 关 + 锁 60：制造「稳定 60fps」的场景，正是用户报问题的那种工况
    p = subprocess.Popen([SIM, "--api=dx11", "--seconds=%d" % SECONDS,
                          "--vsync=off", "--fps-cap=60", "--json", "--json-every=1000"],
                         stdin=subprocess.DEVNULL, stdout=open(out, "w"),
                         stderr=open(err, "w"), text=True)
    time.sleep(3.0)
    import verify_inject as vi
    vi.inject(p.pid, HOOK)
    time.sleep(SECONDS - 8)          # 采够样本（百分位需要足够多的帧）
    tel = None
    try:
        tel = vi.read_telemetry(p.pid)
    except Exception as e:
        print("  读遥测失败：%s" % e)
    try:
        p.wait(timeout=25)
    except Exception:
        p.kill()

    summ = None
    try:
        with open(out, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.strip()
                if line.startswith("{") and '"summary"' in line:
                    summ = json.loads(line)
    except Exception:
        pass

    print("=" * 70)
    print("  帧时间分布对照（vsync off + 锁 60，%d 秒）" % SECONDS)
    print("=" * 70)
    print("  %-14s %14s %14s" % ("分位", "模拟器(真值)", "钩子"))
    print("  " + "-" * 46)

    def row(name, a, b):
        sa = "—" if a is None else "%.3f ms" % a
        sb = "—" if b is None else "%.3f ms" % b
        note = ""
        if a and b:
            d = (b - a) / a * 100.0
            if abs(d) > 25:
                note = "   <== 差 %.0f%%，值得查" % d
            else:
                note = "   (%.0f%%)" % d
        print("  %-14s %14s %14s%s" % (name, sa, sb, note))

    if summ and tel:
        g = lambda n: getattr(tel, n, None)
        row("P50", summ.get("frame_ms_p50"), g("frameMs"))
        row("P99", summ.get("frame_ms_p99"), g("p99Ms"))
        row("P99.9", summ.get("frame_ms_p999"), g("p999Ms"))
        row("平均", summ.get("frame_ms_avg"), g("frameMsAvg"))
        print()
        print("  模拟器：平均帧率 %.2f  总帧 %s  计入 %s  重建 %s 次"
              % (summ.get("fps_avg", 0), summ.get("total_frames"),
                 summ.get("counted_frames"), summ.get("resize_events")))
        print("  钩子：  frameTotal=%s  fpsAvg=%.2f  gfxApi=%s  hookFlags=0x%02X"
              % (g("frameTotal"), g("fpsAvg") or 0, g("gfxApi"), g("hookFlags") or 0))
    else:
        print("  数据不全：summary=%s  telemetry=%s" % (bool(summ), bool(tel)))
    print("=" * 70)
    return 0


if __name__ == "__main__":
    sys.exit(main())
