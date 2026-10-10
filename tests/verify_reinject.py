# -*- coding: utf-8 -*-
"""复现用户的场景：注入 -> 优雅退出主程序 -> 重新注入同一个游戏进程。

这是「重复注入 100% 闪退」的判定性测试。

预期（修复后）：
  两次注入都成功，第二次不再出现
      SEH: code=0xc0000005 at ?+...      （地址不属于任何已加载模块 = 旧 DLL 的野指针）
  且日志里能看到两次
      RestoreAllHooks: ... seh=1        （说明 VEH 处理器被注销了）
"""
import ctypes
import ctypes.wintypes as wt
import io
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
# ⚠ 日志目录要用**本进程自己的 TEMP**：
#   测试脚本（及其启动的宿主）可能在沙箱里跑，那时 TEMP 是被重定向的目录，
#   钩子 DLL 也是往**被注入进程**的 TEMP 写。之前写死成
#   %LOCALAPPDATA%\Temp 读的是"外面"那份 —— 那里是用户真实游戏写的日志，
#   结果把用户的会话数据当成了自己的测试结果（踩过一次）。
LOGDIR = os.environ.get('TEMP') or os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Temp')
HOOKLOG = os.path.join(LOGDIR, 'NextPerfHook.log')

u32 = ctypes.WinDLL('user32', use_last_error=True)
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
u32.FindWindowW.restype = wt.HWND
u32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]
u32.EnumWindows.argtypes = [ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM), wt.LPARAM]
u32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
u32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]


def read_log_tail(path, n=400):
    """读整个日志（钩子日志写满后会**从文件开头覆盖**，所以"最后 N 行"是最旧的内容）。"""
    try:
        with io.open(path, encoding='utf-8', errors='ignore') as f:
            return f.read().splitlines()
    except OSError:
        return []


def find_main_window(pid):
    found = []

    def cb(hwnd, _):
        p = wt.DWORD()
        u32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid and u32.IsWindowVisible(hwnd):
            found.append(hwnd)
            return False
        return True

    u32.EnumWindows(ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)(cb), 0)
    return found[0] if found else None


def alive(pid):
    h = k32.OpenProcess(0x1000, False, pid)
    if not h:
        return False
    k32.CloseHandle(h)
    return True


def start_app():
    p = subprocess.Popen([os.path.join(ROOT, 'dist', 'NextPerf.exe')],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(4)
    return p


def inject(pid):
    r = subprocess.run([os.path.join(ROOT, 'dist', 'NextPerf.exe'), '--inject', str(pid)],
                       capture_output=True)
    time.sleep(6)
    return r.returncode


def main():
    # 清日志
    for p in (HOOKLOG,):
        try:
            os.remove(p)
        except OSError:
            pass

    host = subprocess.Popen([os.path.join(ROOT, 'tests', 'host_run.exe'), '--seconds', '90'],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(4)
    print('  宿主 pid =', host.pid)

    # ---------- 第一轮 ----------
    app1 = start_app()
    hw1 = find_main_window(app1.pid)
    print('  第一轮：主程序 pid=%d 窗口=%s' % (app1.pid, hw1))
    inject(host.pid)
    log1 = read_log_tail(HOOKLOG, 200)
    ok1 = any('RestoreAllHooks' in l for l in log1)
    print('  第一轮注入完成')

    # ---------- 优雅退出主程序（触发钩子自卸载）----------
    if hw1:
        u32.PostMessageW(hw1, 0x0111, 1004, 0)   # WM_COMMAND / NP_TRAY_EXIT
    for _ in range(40):
        if not alive(app1.pid):
            break
        time.sleep(0.5)
    print('  主程序已退出：%s   宿主仍存活：%s' % (not alive(app1.pid), alive(host.pid)))
    time.sleep(3)

    # 记录卸载日志
    unload = [l for l in read_log_tail(HOOKLOG, 300) if 'RestoreAllHooks' in l]
    print('  卸载时的还原日志：')
    for l in unload[-2:]:
        print('     ', l.split('] ', 1)[-1][:110])

    # ---------- 第二轮：重新注入同一个宿主 ----------
    app2 = start_app()
    print('  第二轮：主程序 pid=%d' % app2.pid)
    inject(host.pid)
    time.sleep(6)

    # ---------- 判定 ----------
    # ⚠ 只统计**宿主进程**写的行：用户可能还开着游戏，而那个进程里挂着旧版钩子，
    #   会往同一个日志文件里写它自己的（旧地址的）SEH，必须排除掉。
    tail = [l for l in read_log_tail(HOOKLOG, 800) if ('pid=%d ' % host.pid) in l]
    seh = [l for l in tail if 'SEH: code=' in l and '?+' in l]
    hooked = any('present diag' in l or 'swapchain vtable patched' in l for l in tail)
    print()
    print('  第二轮之后（只看宿主 pid=%d）：' % host.pid)
    print('    日志行数 = %d' % len(tail))
    print('    野指针 SEH（?+...）= %d 条   %s' % (len(seh), '✅ 没有' if not seh else '❌ 仍有'))
    print('    钩子是否挂上 = %s' % ('✅ 是' if hooked else '❌ 否'))
    for l in seh[-3:]:
        print('      ', l.split('] ', 1)[-1][:110])

    host.kill()
    for a in (app2,):
        try:
            a.kill()
        except Exception:
            pass
    return 0 if (not seh and hooked) else 1


if __name__ == '__main__':
    sys.exit(main())
