#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
NextPerf 模拟游戏进程 —— 参考驱动脚本（同时也是集成自测）。

这个脚本演示「Python 怎么驱动 sim.exe」这件事该怎么做，并且把几件最容易
踩坑的地方都处理掉了：

  1. **必须用子进程的管道而不是 shell 重定向**：cmd.exe 在命令行里同时出现
     重定向和 '=' 时会改写参数（实测 `--api=dx11` 会变成 `--api dx11`）。
     Python 的 subprocess(arglist) 没有这个问题。
  2. **stdin 写完命令后要 flush**：否则命令会卡在缓冲区里，看起来像「命令
     没生效」。行尾用 '\\n' 即可（sim 的解析器同时认 CRLF 和裸 LF）。
  3. **stdout 要边读边解析**：sim 每帧输出一行 JSON，如果等进程结束再读，
     管道缓冲区满了会把子进程卡死。—— 这里让 sim 在测试里用 --json-every
     降低输出量，同时把读放在独立线程里。
  4. **退出要优雅**：往 stdin 写 quit，sim 会输出汇总 JSON 后正常退出。
     只有超时兜底才 Kill。

用法：
    python tests/sim/sim_drive.py                 # 跑默认的集成自测
    python tests/sim/sim_drive.py --api dx12      # 只测 DX12
    python tests/sim/sim_drive.py --exe <path>    # 指定 sim.exe
"""

import argparse
import json
import os
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_EXE = os.path.join(HERE, "sim.exe")


class SimRun:
    """起一个 sim.exe，收集它自报的 JSON 行，并在退出时拿到汇总。"""

    def __init__(self, exe, args, timeout=30.0):
        self.exe = exe
        self.args = list(args)
        self.timeout = timeout
        self.frames = []          # 每帧一行 JSON（type == "frame"）
        self.marks = []           # mark 事件
        self.summaries = []       # type == "summary"（含退出时那份）
        self.other = []           # status 等
        self.stderr_lines = []
        self.returncode = None

    def __enter__(self):
        self.proc = subprocess.Popen(
            [self.exe] + self.args,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=1,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        self._t_out = threading.Thread(target=self._read_stdout, daemon=True)
        self._t_err = threading.Thread(target=self._read_stderr, daemon=True)
        self._t_out.start()
        self._t_err.start()
        return self

    def _read_stdout(self):
        for line in self.proc.stdout:
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                self.other.append(line)
                continue
            t = obj.get("type")
            if t == "frame":
                self.frames.append(obj)
            elif t == "summary":
                self.summaries.append(obj)
            elif t == "mark":
                self.marks.append(obj)
            else:
                self.other.append(obj)

    def _read_stderr(self):
        for line in self.proc.stderr:
            self.stderr_lines.append(line.rstrip("\n"))

    def send(self, cmd, settle=0.0):
        """写一条控制命令。settle > 0 时等一小会儿让它生效。"""
        if self.proc.poll() is not None:
            raise RuntimeError("sim.exe 已经退出了，无法再发命令")
        self.proc.stdin.write(cmd + "\n")
        self.proc.stdin.flush()
        if settle:
            time.sleep(settle)

    def wait(self):
        """等进程退出（必要时超时兜底 Kill），返回汇总 JSON。"""
        try:
            self.returncode = self.proc.wait(timeout=self.timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=5)
            raise RuntimeError("sim.exe 超时未退出（已 Kill）")
        finally:
            try:
                self.proc.stdin.close()
            except Exception:
                pass
            self._t_out.join(timeout=5)
            self._t_err.join(timeout=5)
        return self.summaries[-1] if self.summaries else None

    def __exit__(self, *exc):
        if self.proc.poll() is None:
            try:
                self.send("quit")
                self.proc.wait(timeout=8)
            except Exception:
                self.proc.kill()
        return False


def check(cond, msg):
    tag = "OK  " if cond else "FAIL"
    print("  [%s] %s" % (tag, msg))
    return bool(cond)


def run_case_dx(exe, api):
    """一个后端的基础用例：3 秒 + 每帧 JSON，检查自报字段是否齐全/自洽。"""
    print("== 用例 1：--api=%s --seconds=3 --json ==" % api)
    ok = True
    with SimRun(exe, ["--api=%s" % api, "--seconds=3", "--json", "--warmup=10"]) as r:
        summary = r.wait()

    ok &= check(r.returncode == 0, "退出码 0（实际 %s）" % r.returncode)
    ok &= check(summary is not None, "拿到了汇总 JSON")
    if not summary:
        return False

    ok &= check(len(r.frames) >= 30, "收到了 %d 行帧 JSON" % len(r.frames))
    ok &= check(summary["total_frames"] >= 30,
                "总帧数 %d" % summary["total_frames"])
    ok &= check(summary["exit_reason"] == "time_limit",
                "退出原因 = %s" % summary["exit_reason"])
    ok &= check(abs(summary["fps_avg"] - 60.0) < 12.0,
                "平均帧率 %.2f（vsync on，应当在 60 附近）" % summary["fps_avg"])

    # 分位数必须单调
    ok &= check(summary["frame_ms_p50"] <= summary["frame_ms_p99"] <= summary["frame_ms_p999"],
                "P50(%.3f) <= P99(%.3f) <= P99.9(%.3f)"
                % (summary["frame_ms_p50"], summary["frame_ms_p99"], summary["frame_ms_p999"]))
    ok &= check(summary["fps_1pct_low"] <= summary["fps_avg"] + 0.01,
                "1%% Low(%.2f) <= 平均帧率(%.2f)"
                % (summary["fps_1pct_low"], summary["fps_avg"]))
    ok &= check(summary["fps_01pct_low"] <= summary["fps_1pct_low"] + 0.01,
                "0.1%% Low(%.2f) <= 1%% Low(%.2f)"
                % (summary["fps_01pct_low"], summary["fps_1pct_low"]))

    # 字段齐全性
    need = ["frame_delta_ms", "cpu_frame_ms", "gpu_frame_ms", "gpu_valid", "present_ms",
            "vsync", "fps_cap", "width", "height", "window_mode", "swapchain_generation",
            "resize_event", "present_failed"]
    f0 = r.frames[len(r.frames) // 2]
    missing = [k for k in need if k not in f0]
    ok &= check(not missing, "帧 JSON 字段齐全（缺 %s）" % missing)
    ok &= check(f0["width"] == 1280 and f0["height"] == 720,
                "分辨率自报 %dx%d" % (f0["width"], f0["height"]))

    # GPU 时间戳：要么有值且 > 0，要么明确标 gpu_valid=false —— 不能拿 0 冒充
    bad = [x for x in r.frames if x["gpu_valid"] and not (x["gpu_frame_ms"] > 0)]
    ok &= check(not bad, "gpu_valid=true 的帧全都有 > 0 的 gpu_frame_ms（异常 %d 帧）" % len(bad))
    ok &= check(summary["gpu_time_available"] in (True, False),
                "GPU 时间戳可用性如实上报：%s（频率来源 %s，%d/%d 帧有样本）"
                % (summary["gpu_time_available"], summary["gpu_timestamp_freq_source"],
                   summary["gpu_valid_frames"], summary["counted_frames"]))
    return ok


def run_case_control(exe, api):
    """运行时控制：改分辨率 / 切窗口状态 / 开关 vsync / 改锁帧 / 请求退出。"""
    print("== 用例 2：运行时控制通道（%s）==" % api)
    ok = True
    with SimRun(exe, ["--api=%s" % api, "--json-every=4", "--warmup=5"]) as r:
        time.sleep(1.0)
        r.send("status", settle=0.5)
        r.send("resize 1024 768", settle=0.8)
        r.send("vsync off", settle=0.4)
        r.send("fpscap 30", settle=1.2)
        r.send("resize 1600 900", settle=0.8)
        r.send("window borderless", settle=1.2)
        r.send("window windowed", settle=1.0)
        r.send("vsync on", settle=0.4)
        r.send("fpscap 0", settle=0.6)
        r.send("mark control_done", settle=0.4)
        r.send("stats", settle=0.6)
        # 注意：sim 默认是「跑到被要求退出为止」，所以脚本必须显式发 quit，
        # 否则它会一直跑（这正是可控模拟进程该有的行为）。
        r.send("quit")
        summary = r.wait()

    ok &= check(r.returncode == 0, "退出码 0（实际 %s）" % r.returncode)
    ok &= check(summary is not None and summary["exit_reason"] == "command_quit",
                "退出原因 = %s" % (summary or {}).get("exit_reason"))
    if not summary:
        return False

    ok &= check(summary["resize_events"] >= 2,
                "自报交换链重建 %d 次（>=2）" % summary["resize_events"])
    ok &= check(summary["swapchain_generation"] >= 3,
                "交换链代数 %d（>=3）" % summary["swapchain_generation"])

    # 分辨率真的变了：找一条 1600x900 的帧
    sizes = sorted({(f["width"], f["height"]) for f in r.frames})
    ok &= check((1600, 900) in sizes, "帧数据里出现过 1600x900（实测分辨率集合 %s）" % sizes)

    # resize 事件在帧数据里可见。
    # 注意：--json-every=N（N>1）会把发生重建的那一帧采样掉，所以同时看两个
    # 信号 —— 本帧布尔量，以及「自上一行以来重建过几次」的累计差值。
    ok &= check(any(f["resize_event"] or f.get("resize_events_since_last", 0) > 0
                    for f in r.frames),
                "resize 事件在帧数据里可见（resize_event / resize_events_since_last）")
    # vsync 状态变化被如实记录
    vs = sorted({f["vsync"] for f in r.frames})
    ok &= check(vs == [False, True], "帧数据里 vsync 两种状态都出现过（%s）" % vs)
    # 锁帧值变化被如实记录
    caps = sorted({f["fps_cap"] for f in r.frames})
    ok &= check(30 in caps, "帧数据里出现过 fps_cap=30（%s）" % caps)
    # 窗口状态变化
    modes = sorted({f["window_mode"] for f in r.frames})
    ok &= check("borderless" in modes, "帧数据里出现过 borderless（%s）" % modes)

    # status 命令有回应
    st = [o for o in r.other if isinstance(o, dict) and o.get("type") == "status"]
    ok &= check(len(st) >= 1, "status 命令返回了 %d 行状态 JSON" % len(st))
    # stats 命令有回应（on_demand_stats）
    od = [s for s in r.summaries if s.get("exit_reason") == "on_demand_stats"]
    ok &= check(len(od) >= 1, "stats 命令返回了 %d 份即时汇总" % len(od))
    # mark 事件
    ok &= check(any(m.get("name") == "control_done" for m in r.marks),
                "mark 事件被回显（%d 条）" % len(r.marks))
    return ok


def run_case_fpscap(exe, api):
    """锁帧精度：144fps 上限在 vsync off 下应当显著快于 60。"""
    print("== 用例 3：锁帧精度（%s，vsync off，cap=144 / cap=60）==" % api)
    ok = True
    results = {}
    for cap in (60, 144):
        with SimRun(exe, ["--api=%s" % api, "--seconds=3", "--vsync=off",
                          "--fps-cap=%d" % cap, "--warmup=10", "--json-every=1000"]) as r:
            s = r.wait()
        if not s:
            ok &= check(False, "cap=%d 没拿到汇总" % cap)
            continue
        results[cap] = s["fps_avg"]
        print("      cap=%3d -> 实测 %.2f fps   P50 %.3f ms   CPU %.3f ms"
              % (cap, s["fps_avg"], s["frame_ms_p50"], s["cpu_frame_ms_avg"]))

    if 60 in results:
        ok &= check(abs(results[60] - 60.0) < 4.0,
                    "cap=60 实测 %.2f fps（目标 60，允许 ±4）" % results[60])
    if 60 in results and 144 in results:
        # 144 档能不能真跑上去取决于显示器刷新率（60Hz 上会被 DWM 顶住），
        # 所以这里只检查「没有比 60 档更慢」，不硬性要求达到 144。
        ok &= check(results[144] >= results[60] - 3.0,
                    "cap=144 (%.2f) 不比 cap=60 (%.2f) 慢" % (results[144], results[60]))
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--api", default="both", choices=["dx11", "dx12", "both"])
    args = ap.parse_args()

    if not os.path.isfile(args.exe):
        print("找不到 sim.exe：%s（先跑 tests/sim/build_sim.bat）" % args.exe)
        return 2

    apis = ["dx11", "dx12"] if args.api == "both" else [args.api]
    all_ok = True
    for api in apis:
        try:
            all_ok &= run_case_dx(args.exe, api)
            all_ok &= run_case_control(args.exe, api)
            all_ok &= run_case_fpscap(args.exe, api)
        except Exception as e:  # noqa: BLE001 - 自测脚本，任何异常都算失败
            print("  用例异常：%r" % (e,))
            all_ok = False

    print()
    print("总体：%s" % ("全部通过" if all_ok else "有失败项"))
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
