"""NextPerf 注入链路验证（真实场景：先建交换链，再注入）。

复现的就是用户报的问题：游戏已经在跑、交换链早就建好了，这时候注入。
旧实现只挂 DXGI 工厂的 CreateSwapChain*，那条路永远不会再被调用，
Present 挂不上 -> 主程序读不到任何游戏数据。

用法： python tests/verify_inject.py [--d3d11]
"""
import ctypes
import ctypes.wintypes as wt
import atexit
import os
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.abspath(os.path.dirname(__file__)))
DIST = os.path.join(ROOT, "dist")
NP_EXE = os.path.join(DIST, "NextPerf.exe")
HOOK_DLL = os.path.join(DIST, "NextPerfHook.dll")
HOST_EXE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "host_run.exe")
LOG = os.path.join(os.environ.get("TEMP", "."), "NextPerfHook.log")

NP_MAGIC = 0x4E505231
NP_FRAME_CAP = 4096
NP_GRAPH_CAP = 512
NP_NAME_LEN = 128

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
u32 = ctypes.WinDLL("user32", use_last_error=True)

k32.OpenProcess.restype = wt.HANDLE
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.CloseHandle.argtypes = [wt.HANDLE]
k32.GetModuleHandleW.restype = wt.HMODULE
k32.GetProcAddress.restype = ctypes.c_void_p
k32.GetProcAddress.argtypes = [wt.HMODULE, wt.LPCSTR]
k32.VirtualAllocEx.restype = wt.LPVOID
k32.VirtualAllocEx.argtypes = [wt.HANDLE, wt.LPVOID, ctypes.c_size_t, wt.DWORD, wt.DWORD]
k32.WriteProcessMemory.argtypes = [wt.HANDLE, wt.LPVOID, wt.LPCVOID, ctypes.c_size_t,
                                   ctypes.POINTER(ctypes.c_size_t)]
k32.CreateRemoteThread.restype = wt.HANDLE
k32.CreateRemoteThread.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
                                   ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD)]
k32.OpenFileMappingW.restype = wt.HANDLE
k32.OpenFileMappingW.argtypes = [wt.DWORD, wt.BOOL, wt.LPCWSTR]
k32.MapViewOfFile.restype = ctypes.c_void_p
k32.MapViewOfFile.argtypes = [wt.HANDLE, wt.DWORD, wt.DWORD, wt.DWORD, ctypes.c_size_t]

u32.FindWindowW.restype = wt.HWND
u32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]
u32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
u32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]


class NPTelemetry(ctypes.Structure):
    """必须和 src/common/np_common.h 的 NPTelemetry 逐字段对齐。"""
    _fields_ = [
        ("magic", ctypes.c_uint32), ("version", ctypes.c_uint32),
        ("pid", ctypes.c_uint32), ("attached", ctypes.c_uint32),
        ("tickMs", ctypes.c_uint64),
        ("gfxApi", ctypes.c_uint32), ("presentMode", ctypes.c_uint32),
        ("renderW", ctypes.c_uint32), ("renderH", ctypes.c_uint32),
        ("windowW", ctypes.c_uint32), ("windowH", ctypes.c_uint32),
        ("processName", ctypes.c_char * NP_NAME_LEN),
        ("frameTotal", ctypes.c_uint32), ("frameWrite", ctypes.c_uint32),
        ("frames", ctypes.c_float * NP_FRAME_CAP),
        ("cpuFrames", ctypes.c_float * NP_FRAME_CAP),
        ("gpuFrames", ctypes.c_float * NP_FRAME_CAP),
        ("fps", ctypes.c_float), ("fpsAvg", ctypes.c_float),
        ("fpsLow1", ctypes.c_float), ("fpsLow01", ctypes.c_float),
        ("frameMs", ctypes.c_float), ("frameMsAvg", ctypes.c_float),
        ("cpuFrameMs", ctypes.c_float), ("cpuFrameMsAvg", ctypes.c_float),
        ("simMs", ctypes.c_float), ("submitMs", ctypes.c_float),
        ("gpuFrameMs", ctypes.c_float),
        ("msInPresent", ctypes.c_float),
        ("p99Ms", ctypes.c_float), ("p999Ms", ctypes.c_float),
        ("drawCalls", ctypes.c_uint32), ("dispatches", ctypes.c_uint32),
        ("rtDispatches", ctypes.c_uint32), ("asBuilds", ctypes.c_uint32),
        ("rtGpuMs", ctypes.c_float), ("aiGpuMs", ctypes.c_float),
        ("rtLoad", ctypes.c_float), ("tensorLoad", ctypes.c_float),
        ("aiModules", ctypes.c_uint32), ("rtMeasured", ctypes.c_uint32),
        ("tensorMeasured", ctypes.c_uint32),
        ("graphWrite", ctypes.c_uint32), ("graphCount", ctypes.c_uint32),
        ("graphFrame", ctypes.c_float * NP_GRAPH_CAP),
        ("graphCpu", ctypes.c_float * NP_GRAPH_CAP),
        ("graphGpu", ctypes.c_float * NP_GRAPH_CAP),
        ("hookFlags", ctypes.c_uint32),
        ("lastError", ctypes.c_char * NP_NAME_LEN),
    ]


assert ctypes.sizeof(NPTelemetry) > 0
TEL_SIZE = ctypes.sizeof(NPTelemetry)


def kill_all():
    """测试收尾：绝不留残留进程。

    早前的教训：脚本中途抛异常（比如 OpenProcess 失败）就直接退出，
    启动的 NextPerf 留在系统里 —— 它跑在受限环境里、无法注入任何游戏，
    还会锁住 dist\\NextPerf.exe 让后续构建失败。所以每个脚本都 atexit 注册这个。
    """
    for name in ("NextPerf.exe", "host_run.exe"):
        subprocess.run(["taskkill", "/F", "/IM", name], capture_output=True)


def find_main_window_for_pid(pid, timeout=12.0):
    """找**属于指定 pid** 的主窗口（按窗口类名 + 进程 id）。

    为什么要按 pid：整个套件连跑时，上一个用例的 NextPerf 可能还没死透，
    光按类名 FindWindow 会拿到它那个正在消失的窗口，点击就打空了 ——
    单独跑就过、连跑就挂，就是这么来的。
    也不能按标题找：标题里带构建时间戳。
    """
    u32.FindWindowW.restype = wt.HWND
    u32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]
    CB = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    t0 = time.time()
    while time.time() - t0 < timeout:
        found = []

        def cb(hwnd, _):
            buf = ctypes.create_unicode_buffer(64)
            u32.GetClassNameW(hwnd, buf, 64)
            if buf.value == "NextPerfMainCls":
                p = wt.DWORD()
                u32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
                if p.value == pid:
                    found.append(hwnd)
            return True

        u32.EnumWindows(CB(cb), 0)
        if found:
            return found[0]
        time.sleep(0.2)
    return None


def find_main_window():
    """按窗口类名找主窗口（不校验 pid，一般用 find_main_window_for_pid）。"""
    u32.FindWindowW.restype = wt.HWND
    u32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]
    return u32.FindWindowW("NextPerfMainCls", None)


def read_telemetry(pid):
    """按 PID 打开钩子建的遥测块；没有就返回 None。"""
    name = "Local\\NextPerf_Telemetry_v1_%d" % pid
    h = k32.OpenFileMappingW(0x0004, False, name)   # FILE_MAP_READ
    if not h:
        return None
    view = k32.MapViewOfFile(h, 0x0004, 0, 0, TEL_SIZE)
    if not view:
        k32.CloseHandle(h)
        return None
    data = ctypes.string_at(view, TEL_SIZE)
    k32.UnmapViewOfFile(ctypes.c_void_p(view))
    k32.CloseHandle(h)
    t = NPTelemetry.from_buffer_copy(data)
    # 布局自检：钩子把 sizeof(NPTelemetry) 写在 version 字段里。
    # 不校验的话，往结构体中间插字段会让这里读到错位字节，
    # 表现为「钩子明明工作正常却报未挂上」——踩过一次，很难查。
    if t.magic == NP_MAGIC and t.version != TEL_SIZE:
        raise AssertionError(
            "NPTelemetry 结构体布局不一致：钩子说 %d 字节，测试脚本算出来 %d 字节。"
            "本文件的 ctypes 结构体要和 src/common/np_common.h 同步。"
            % (t.version, TEL_SIZE))
    return t


def module_loaded(pid, dllname):
    TH32CS_SNAPMODULE = 0x00000008
    k32.CreateToolhelp32Snapshot.restype = wt.HANDLE

    class MODULEENTRY32W(ctypes.Structure):
        _fields_ = [("dwSize", wt.DWORD), ("th32ModuleID", wt.DWORD), ("th32ProcessID", wt.DWORD),
                    ("GlblcntUsage", wt.DWORD), ("ProccntUsage", wt.DWORD),
                    ("modBaseAddr", ctypes.c_void_p), ("modBaseSize", wt.DWORD),
                    ("hModule", wt.HMODULE), ("szModule", wt.WCHAR * 256),
                    ("szExePath", wt.WCHAR * 260)]

    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid)
    if not snap or snap == wt.HANDLE(-1).value:
        return None
    me = MODULEENTRY32W()
    me.dwSize = ctypes.sizeof(me)
    found = False
    if k32.Module32FirstW(snap, ctypes.byref(me)):
        while True:
            if me.szModule.lower() == dllname.lower():
                found = True
                break
            if not k32.Module32NextW(snap, ctypes.byref(me)):
                break
    k32.CloseHandle(snap)
    return found


def inject(pid, dll):
    # 只要注入真正需要的那几项。PROCESS_ALL_ACCESS 里含 SET_INFORMATION /
    # SUSPEND_RESUME，在受限环境（沙箱）下会被拒绝，而这几项是放行的。
    NEEDED = 0x0002 | 0x0008 | 0x0020 | 0x0010 | 0x1000   # CREATE_THREAD|VM_OP|VM_WRITE|VM_READ|QUERY_LIMITED
    hp = k32.OpenProcess(NEEDED, False, pid)
    if not hp:
        raise OSError("OpenProcess failed: %d" % ctypes.get_last_error())
    path = dll.encode("utf-16-le") + b"\x00\x00"
    mem = k32.VirtualAllocEx(hp, None, len(path), 0x3000, 0x40)
    written = ctypes.c_size_t(0)
    k32.WriteProcessMemory(hp, mem, path, len(path), ctypes.byref(written))
    h32 = k32.GetModuleHandleW("kernel32.dll")
    ll = k32.GetProcAddress(h32, b"LoadLibraryW")
    th = k32.CreateRemoteThread(hp, None, 0, ctypes.c_void_p(ll), mem, 0, None)
    if not th:
        raise OSError("CreateRemoteThread failed: %d" % ctypes.get_last_error())
    k32.WaitForSingleObject(th, 8000)
    k32.CloseHandle(th)
    k32.CloseHandle(hp)


def close_window(pid):
    """关掉属于 pid 的顶层窗口，让宿主尽快退出。"""
    CB = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

    def cb(hwnd, _):
        p = wt.DWORD()
        u32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid:
            u32.PostMessageW(hwnd, 0x0010, 0, 0)   # WM_CLOSE
        return True

    u32.EnumWindows(CB(cb), 0)


def main():
    d3d11 = "--d3d11" in sys.argv
    fails = []
    atexit.register(kill_all)

    # 不开 DPI 感知的话，GetWindowRect 给的是被 DWM 虚拟化过的逻辑坐标，
    # 而 ImageGrab 抓的是物理像素 —— 截出来的图会整体错位（踩过）。
    try:
        u32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
    except Exception:
        try:
            u32.SetProcessDPIAware()
        except Exception:
            pass

    for name in ("NextPerf.exe", "host_run.exe"):
        subprocess.run(["taskkill", "/F", "/IM", name], capture_output=True)
    if os.path.exists(LOG):
        os.remove(LOG)

    print("=== 1. 启动主程序 ===")
    np_proc = subprocess.Popen([NP_EXE], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(3)

    print("=== 2. 启动宿主（立刻建交换链并持续 Present） ===")
    host = subprocess.Popen([HOST_EXE] + (["--d3d11"] if d3d11 else []),
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    hwnd = None
    for _ in range(60):
        hwnd = u32.FindWindowW(None, "NpHost")
        if hwnd:
            break
        time.sleep(0.2)
    if not hwnd:
        print("!! 找不到宿主窗口，终止")
        host.kill()
        np_proc.kill()
        return 2
    hpid = wt.DWORD()
    u32.GetWindowThreadProcessId(hwnd, ctypes.byref(hpid))
    hpid = hpid.value
    print("宿主 pid = %d，已经跑了 3 秒（交换链早就建好了）" % hpid)
    time.sleep(3)

    pre = read_telemetry(hpid)
    print("注入前遥测块：%s" % ("存在（意外）" if pre else "不存在（符合预期）"))

    print("=== 3. 注入钩子 ===")
    inject(hpid, HOOK_DLL)
    time.sleep(1.0)

    print("=== 4. 轮询遥测（约 12 秒） ===")
    series = []
    t0 = time.time()
    while time.time() - t0 < 12.0:
        t = read_telemetry(hpid)
        if t and t.magic == NP_MAGIC:
            series.append((time.time() - t0, t))
        time.sleep(0.5)

    if not series:
        fails.append("注入后完全拿不到遥测块")
        t = None
    else:
        t = series[-1][1]
        print("  magic=%08X attached=%d pid=%d process=%s" %
              (t.magic, t.attached, t.pid, t.processName.decode(errors="ignore")))
        print("  gfxApi=%d frameTotal=%d fps=%.1f avg=%.1f low1=%.1f frameMs=%.2f" %
              (t.gfxApi, t.frameTotal, t.fps, t.fpsAvg, t.fpsLow1, t.frameMs))
        print("  cpuFrameMs=%.2f gpuFrameMs=%.2f rtLoad=%.2f hookFlags=%02X" %
              (t.cpuFrameMs, t.gpuFrameMs, t.rtLoad, t.hookFlags))
        print("  render=%dx%d window=%dx%d presentMode=%d" %
              (t.renderW, t.renderH, t.windowW, t.windowH, t.presentMode))
        print("  采样序列 (秒, frameTotal, fps):")
        for ts, s in series[::2]:
            print("    %5.1f  %6d  %6.1f" % (ts, s.frameTotal, s.fps))

        if not t.attached:
            fails.append("attached == 0，主程序认为没有接管")
        if not (t.hookFlags & 0x1):
            fails.append("NP_HOOK_PRESENT 未置位（hookFlags=%02X）" % t.hookFlags)
        if t.renderW == 0:
            fails.append("没读到分辨率")

        # 稳态帧率：用后半段采样的 frameTotal 增量算，避开首帧初始化
        if len(series) >= 4:
            mid = series[len(series) // 2]
            dt = series[-1][0] - mid[0]
            df = series[-1][1].frameTotal - mid[1].frameTotal
            if df <= 0:
                fails.append("后半段 frameTotal 没有增长，Present 卡住了")
            else:
                rate = df / dt if dt > 0 else 0
                print("  稳态帧率：%.1f FPS（后半段 %d 帧 / %.1f 秒）" % (rate, df, dt))
                if rate < 10:
                    fails.append("稳态帧率只有 %.1f FPS，钩子把宿主拖慢了" % rate)

        if t.frameTotal < 10:
            fails.append("frameTotal=%d，Present 几乎没有被调用" % t.frameTotal)
        if not (5.0 < t.fps < 500.0):
            fails.append("fps=%.1f 不合理（宿主约 30-60FPS）" % t.fps)

    print("=== 5. 截图 ===")
    try:
        from PIL import ImageGrab
        u32.SetForegroundWindow(hwnd)
        time.sleep(0.8)
        r = wt.RECT()
        u32.GetWindowRect(hwnd, ctypes.byref(r))
        print("  窗口矩形 (%d,%d)-(%d,%d)" % (r.left, r.top, r.right, r.bottom))
        shot = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "shot_host_d3d11.png" if d3d11 else "shot_host_d3d12.png")
        img = ImageGrab.grab(bbox=(r.left, r.top, r.right, r.bottom), all_screens=True)
        img.save(shot)
        # 叠加上是白字黑底，纯色画面里应该出现明显的高对比像素；顺手统计一下
        px = img.convert("L").getdata()
        bright = sum(1 for v in px if v > 200)
        print("  已保存 %s（%dx%d，亮像素 %d）" % (shot, img.width, img.height, bright))
        if bright < 500:
            print("  [警告] 画面里几乎没有亮像素 —— 面板可能没画出来")
    except Exception as e:
        print("  截图失败（不影响结论）：%s" % e)

    print("=== 6. 退出主程序，验证钩子自卸载 ===")
    np_hwnd = find_main_window_for_pid(np_proc.pid)
    if np_hwnd:
        u32.PostMessageW(np_hwnd, 0x0111, 1004, 0)   # WM_COMMAND / NP_TRAY_EXIT
    time.sleep(6)
    np_dead = np_proc.poll() is not None
    host_alive = host.poll() is None
    print("  主程序已退出：%s   宿主仍存活：%s" % (np_dead, host_alive))
    if not np_dead:
        fails.append("主程序没有退出")
    if not host_alive:
        fails.append("宿主崩了 —— 钩子卸载把游戏带崩了")
    if host_alive:
        # 钩子自卸载时会关掉「已注入」互斥量，用它当卸载证据最直接
        k32.OpenMutexW.restype = wt.HANDLE
        k32.OpenMutexW.argtypes = [wt.DWORD, wt.BOOL, wt.LPCWSTR]
        m = k32.OpenMutexW(0x00100000, False, "Local\\NextPerf_Injected_%d" % hpid)
        print("  注入标记互斥量还在：%s" % bool(m))
        if m:
            k32.CloseHandle(m)
            fails.append("钩子没有自卸载（注入互斥量仍存在）")

    print("=== 7. 钩子日志 ===")
    if os.path.exists(LOG):
        print(open(LOG, encoding="utf-8", errors="ignore").read().strip())
    else:
        print("  (没有日志)")

    if host.poll() is None:
        close_window(hpid)
        time.sleep(1)
        if host.poll() is None:
            host.kill()
    if np_proc.poll() is None:
        np_proc.kill()

    print()
    if fails:
        print("结果：FAIL")
        for f in fails:
            print("  - %s" % f)
        return 1
    print("结果：PASS —— 注入后成功接管 Present 并读到游戏帧数据")
    return 0


if __name__ == "__main__":
    sys.exit(main())
