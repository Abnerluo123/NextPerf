"""NextPerf 2D/3D 真值对照验证（使用 tests/sim/sim.exe 对照 DX11 / DX12 遥测口径）。

用法： python tests/verify_ground_truth.py
退出码 0 = DX11 & DX12 遥测与模拟游戏真值全部吻合。
"""
import atexit
import json
import os
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SIM_EXE = os.path.join(HERE, "sim", "sim.exe")

sys.path.insert(0, HERE)
from verify_inject import (
    HOOK_DLL,
    NP_EXE,
    NP_MAGIC,
    find_main_window_for_pid,
    inject,
    kill_all,
    read_telemetry,
)


def cleanup():
    kill_all()
    for name in ("sim.exe", "NextPerfBench.exe"):
        subprocess.run(["taskkill", "/F", "/IM", name], capture_output=True)


def run_case(api_name):
    print("\n=== 真值对照：%s ===" % api_name.upper())
    fails = []
    if not os.path.exists(SIM_EXE):
        return ["缺少 %s（请先运行 tests\\sim\\build_sim.bat）" % SIM_EXE]
    if not os.path.exists(NP_EXE) or not os.path.exists(HOOK_DLL):
        return ["缺少 dist\\NextPerf.exe 或 dist\\NextPerfHook.dll"]

    cleanup()
    time.sleep(0.5)

    np_proc = subprocess.Popen([NP_EXE], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    np_hwnd = find_main_window_for_pid(np_proc.pid, timeout=12.0)
    if not np_hwnd:
        cleanup()
        return ["找不到 NextPerf 主窗口"]
    time.sleep(1.5)

    sim_cmd = [
        SIM_EXE,
        "--api=%s" % api_name,
        "--seconds=10",
        "--fps-cap=60",
        "--vsync=off",
        "--hold-ms=2",
        "--gpu-load-ms=1.0",
        "--warmup=45",
        "--no-stdin",
    ]
    sim_proc = subprocess.Popen(
        sim_cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )

    time.sleep(1.8)
    if sim_proc.poll() is not None:
        out, err = sim_proc.communicate()
        cleanup()
        return ["sim.exe(%s) 提前退出 rc=%d: %s %s" % (api_name, sim_proc.returncode, out, err)]

    try:
        inject(sim_proc.pid, HOOK_DLL)
    except Exception as e:
        sim_proc.kill()
        cleanup()
        return ["注入 sim.exe(%s) 失败: %s" % (api_name, e)]

    samples = []
    t0 = time.time()
    while sim_proc.poll() is None and (time.time() - t0) < 16.0:
        t = read_telemetry(sim_proc.pid)
        if t and t.magic == NP_MAGIC and t.frameTotal >= 90:
            samples.append(t)
        time.sleep(0.25)

    out, err = sim_proc.communicate(timeout=15)
    summary = None
    for line in out.splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            obj = json.loads(line)
            if obj.get("type") == "summary":
                summary = obj
        except Exception:
            pass

    if not summary:
        cleanup()
        return ["未收到 sim.exe(%s) 的 summary JSON (stderr=%s)" % (api_name, err.strip())]

    if not samples:
        cleanup()
        return ["注入后未采集到稳态遥测样本 (%s)" % api_name]

    # 取后半段稳态样本的中位数，消除注入初期与结尾采样抖动
    steady = samples[len(samples) // 2 :]
    last = samples[-1]
    np_fps = statistics.median(s.fpsAvg for s in steady)
    np_ft = statistics.median(s.frameMsAvg for s in steady)
    np_low1 = statistics.median(s.fpsLow1 for s in steady)
    np_wait = statistics.median(s.cpuWaitAvg for s in steady)
    np_busy = statistics.median(s.cpuBusyAvg for s in steady)
    np_gpu = statistics.median(s.gpuFrameMs for s in steady)
    np_disp = max(s.dispatches for s in steady)

    gt_fps = float(summary["fps_avg"])
    gt_ft = float(summary["frame_ms_avg"])
    gt_low1 = float(summary["fps_1pct_low"])
    gt_wait = float(summary["present_ms_avg"])
    gt_gpu = float(summary["gpu_frame_ms_avg"])
    gt_gpu_ok = bool(summary["gpu_time_available"])

    print(
        "  真值(sim):   fps=%.2f  ft=%.2fms  low1=%.2f  presentWait=%.2fms  gpu=%.2fms (avail=%s)"
        % (gt_fps, gt_ft, gt_low1, gt_wait, gt_gpu, gt_gpu_ok)
    )
    print(
        "  观测(hook):  fps=%.2f  ft=%.2fms  low1=%.2f  busy=%.2fms  wait=%.2fms  gpu=%.2fms  dispatches=%d  api=%d flags=%02X"
        % (np_fps, np_ft, np_low1, np_busy, np_wait, np_gpu, np_disp, last.gfxApi, last.hookFlags)
    )

    if last.attached != 1:
        fails.append("attached != 1 (%d)" % last.attached)
    if (last.hookFlags & 0x1) == 0:
        fails.append("NP_HOOK_PRESENT 未置位 (%02X)" % last.hookFlags)

    expected_api = 12 if api_name == "dx12" else 11
    if last.gfxApi != expected_api:
        fails.append("gfxApi 期望 %d 实际 %d" % (expected_api, last.gfxApi))
    if last.renderW != 1280 or last.renderH != 720:
        fails.append("分辨率期望 1280x720 实际 %dx%d" % (last.renderW, last.renderH))

    if abs(np_fps - gt_fps) > max(5.0, gt_fps * 0.15):
        fails.append("FPS 偏差过大: hook=%.2f vs gt=%.2f" % (np_fps, gt_fps))
    if abs(np_ft - gt_ft) > max(2.5, gt_ft * 0.15):
        fails.append("帧时间偏差过大: hook=%.2f vs gt=%.2f" % (np_ft, gt_ft))
    if gt_low1 > 0 and (np_low1 <= 0 or abs(np_low1 - gt_low1) > max(15.0, gt_low1 * 0.45)):
        fails.append("1%% Low 偏差过大: hook=%.2f vs gt=%.2f" % (np_low1, gt_low1))
    if abs(np_wait - gt_wait) > 2.5:
        fails.append("Present Wait 偏差过大: hook=%.2f vs gt=%.2f" % (np_wait, gt_wait))
    # CPU Busy + CPU Wait 应接近帧间隔（pacer 在每帧间隔自旋等待）
    if abs((np_busy + np_wait) - np_ft) > max(3.5, np_ft * 0.25):
        fails.append("Busy+Wait(%.2f) 与 帧时间(%.2f) 不吻合" % (np_busy + np_wait, np_ft))
    if gt_gpu_ok:
        if np_gpu < 0 or abs(np_gpu - gt_gpu) > max(2.5, gt_gpu * 0.8 + 1.0):
            fails.append("GPU 帧时间偏差过大: hook=%.2f vs gt=%.2f" % (np_gpu, gt_gpu))
    if api_name == "dx12" and np_disp < 1:
        fails.append("DX12 Compute Dispatch 未被统计 (dispatches=%d)" % np_disp)

    cleanup()
    return fails


def main():
    atexit.register(cleanup)
    all_fails = []
    for api in ("dx11", "dx12"):
        f = run_case(api)
        if f:
            all_fails.extend("[%s] %s" % (api, x) for x in f)
    if all_fails:
        print("\n真值对照失败：")
        for x in all_fails:
            print("  FAIL:", x)
        return 1
    print("\n真值对照全部通过 ✓")
    return 0


if __name__ == "__main__":
    sys.exit(main())
