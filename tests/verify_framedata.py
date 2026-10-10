# -*- coding: utf-8 -*-
"""用模拟游戏的**自报数据**校验钩子采集的帧数据准不准。

## 为什么这是关键一步
本项目过去所有关于「帧数据对不对」的结论，都只能靠用户肉眼看面板、口述现象
（例如「Low 帧显示 38~42 但感觉很流畅」）。那是**没有 ground truth** 的验证。

现在模拟游戏**自己知道**每一帧的真实情况（它自己就是渲染方）：
  * 帧间隔、CPU 帧耗时、GPU 帧时间（时间戳查询）、Present 阻塞时长全部自报；
  * 它是我们的「被测对象」，同时也是「测量基准」。

于是第一次可以问一个硬问题：**钩子写进共享内存的数字，和模拟器自报的
真实数字，对得上吗？**

## 口径说明（不是所有字段都能直接相等，必须说清楚）
| 模拟器字段 | 钩子字段 | 能不能直接比 |
|---|---|---|
| `dt_ms`（帧间隔） | `frameMs` / `frameMsAvg` | ✅ 可以，都是「相邻 Present 间隔」 |
| `cpu_frame_ms` | `cpuFrameMs` | ⚠️ 口径可能不同：模拟器从帧首量起；钩子量的是 Present 前后两段（busy+wait）。**看量级与趋势，不要求相等** |
| `gpu_ms` | `gpuFrameMs` | ⚠️ 模拟器用时间戳查询；钩子走 PDH。**看量级** |
| `present_ms` | `msInPresent` | ⚠️ 钩子量的是 Present 内部停留；模拟器也是。**勉强可比** |
| 模拟器的 1% Low | `fpsLow1` | ⚠️ 模拟器剔除预热帧、窗口不同；**看量级** |
| `avg_fps` | `fpsAvg` | ✅ 应该接近 |

**判据**：不追求逐位相等（口径本来就不同），而是抓**量级错误**——
例如差 10 倍、恒为 0、恒定等于帧周期（说明从帧首量起）、明显不随负载变化。
这类错误正是过去靠肉眼发现不了、却真实存在的（AI 引擎那两路就恒为 -1.0%）。

## 用法
    python tests/verify_framedata.py                # DX11
    python tests/verify_framedata.py --api=dx12
    python tests/verify_framedata.py --gpu-load=8   # 加 GPU 压力，看数字跟不跟着变
"""
import argparse
import json
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIM = os.path.join(ROOT, "tests", "sim", "sim.exe")
HOOK = os.path.join(ROOT, "dist", "NextPerfHook.dll")
sys.path.insert(0, os.path.join(ROOT, "tests"))


def pct(a, b):
    """a 相对 b 的偏差百分比（b 为 0 时返回 None）。"""
    if not b:
        return None
    return (a - b) / b * 100.0


def fmt(v, unit=""):
    return "—" if v is None else ("%.3f%s" % (v, unit))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--api", default="dx11")
    ap.add_argument("--seconds", type=int, default=12)
    ap.add_argument("--gpu-load", type=float, default=0.0)
    ap.add_argument("--hold-ms", type=float, default=0.0)
    ap.add_argument("--no-inject", action="store_true")
    args = ap.parse_args()

    out_path = os.path.join(os.environ.get("TEMP", "."), "fd_out.txt")
    err_path = os.path.join(os.environ.get("TEMP", "."), "fd_err.txt")
    cmd = [SIM, "--api=" + args.api, "--seconds=%d" % args.seconds, "--json",
           "--json-every=1", "--warmup=10"]
    if args.gpu_load:
        cmd.append("--gpu-load-ms=%g" % args.gpu_load)
    if args.hold_ms:
        cmd.append("--hold-ms=%g" % args.hold_ms)

    print("启动模拟器：%s" % " ".join(cmd[1:]))
    # stdout 落文件（PIPE 不读会死锁 —— 见 docs/SIMULATOR.md 的坑 2）
    p = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=open(out_path, "w"),
                         stderr=open(err_path, "w"), text=True)

    time.sleep(3.0)
    tel = None
    if not args.no_inject:
        import verify_inject as vi
        vi.inject(p.pid, HOOK)
        time.sleep(8.0)          # 给钩子足够时间采集并算出平均值
        try:
            tel = vi.read_telemetry(p.pid)
        except Exception as e:
            print("  读遥测失败：%s" % e)

    try:
        p.wait(timeout=args.seconds + 20)
    except Exception:
        p.kill()

    frames = []
    summary = None
    try:
        with open(out_path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.strip()
                if not line.startswith("{"):
                    continue
                try:
                    o = json.loads(line)
                except Exception:
                    continue
                if o.get("type") == "frame":
                    frames.append(o)
                elif o.get("type") == "summary":
                    summary = o
    except Exception:
        pass

    # 只取最后 1/3 的帧（避开启动与注入期）
    tail = frames[len(frames) * 2 // 3:] if frames else []
    sim = {}
    if tail:
        def avg(k):
            vals = [f[k] for f in tail if isinstance(f.get(k), (int, float)) and f[k] >= 0]
            return sum(vals) / len(vals) if vals else None
        sim["dt"] = avg("dt_ms") or avg("frame_delta_ms")
        sim["cpu"] = avg("cpu_frame_ms")
        sim["gpu"] = avg("gpu_frame_ms")
        sim["present"] = avg("present_ms")
    if summary:
        sim["fps"] = summary.get("avg_fps")

    print("\n" + "=" * 72)
    print("  模拟器自报（ground truth，取末段帧平均）  vs  钩子采集（共享内存）")
    print("=" * 72)
    if not tel:
        print("  （没有钩子数据 —— 用 --no-inject 时这是预期的）")
        for k, v in sim.items():
            print("  模拟器 %-10s = %s" % (k, fmt(v)))
    else:
        rows = [
            ("帧间隔 ms", sim.get("dt"),
             (tel.frames[0] if False else None) or None),
        ]
        # 钩子的字段名以 verify_inject 的 ctypes 镜像为准
        g = lambda n: getattr(tel, n, None)
        pairs = [
            ("平均帧率", sim.get("fps"), g("fpsAvg"), "fps"),
            ("帧间隔 ms", sim.get("dt"),
             g("frameMsAvg") or (1000.0 / g("fpsAvg") if g("fpsAvg") else None), "ms"),
            ("CPU 帧时间 ms", sim.get("cpu"), g("cpuFrameMsAvg") or g("cpuFrameMs"), "ms"),
            ("GPU 帧时间 ms", sim.get("gpu"), g("gpuFrameMs"), "ms"),
            ("Present 阻塞 ms", sim.get("present"), g("msInPresent"), "ms"),
            ("1% Low fps", (summary or {}).get("low1_fps"), g("fpsLow1"), "fps"),
            ("0.1% Low fps", (summary or {}).get("low01_fps"), g("fpsLow01"), "fps"),
        ]
        print("  %-18s %14s %14s %10s" % ("指标", "模拟器(真值)", "钩子", "偏差"))
        print("  " + "-" * 62)
        for name, a, b, unit in pairs:
            d = pct(b, a) if (a is not None and b is not None) else None
            flag = ""
            if d is not None and abs(d) > 50:
                flag = "  <== 量级差得多，值得查"
            print("  %-18s %14s %14s %9s%s"
                  % (name, fmt(a, unit), fmt(b, unit),
                     "—" if d is None else "%.1f%%" % d, flag))
        print()
        print("  钩子附加自检字段：")
        print("    hookFlags=0x%02X（bit0=Present 已挂）  gfxApi=%s  frameTotal=%s"
              % (g("hookFlags") or 0, g("gfxApi"), g("frameTotal")))
    print("=" * 72)
    print("  判读：不要求逐位相等（口径本来就不同），抓的是**量级错误** ——")
    print("  差 10 倍 / 恒为 0 / 恒定等于帧周期（说明从帧首量起）/ 不随负载变化。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
