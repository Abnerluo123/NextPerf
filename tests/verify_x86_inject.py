# -*- coding: utf-8 -*-
"""验证 32 位注入：最直接的证据是钩子有没有写出自己的日志。

钩子 DLL 载入后会往 %TEMP%\\NextPerfHook.log 写启动信息。
日志不出现 = DLL 没载入（或载入即崩）。
"""
import ctypes
import ctypes.wintypes as wt
import glob
import io
import os
import subprocess
import sys
import time

sys.path.insert(0, 'tests')
from verify_inject import inject, kill_all

ROOT = os.path.abspath('.')
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
u32 = ctypes.WinDLL('user32', use_last_error=True)
psapi = ctypes.WinDLL('psapi', use_last_error=True)

k32.OpenProcess.restype = wt.HANDLE
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.IsWow64Process.argtypes = [wt.HANDLE, ctypes.POINTER(wt.BOOL)]
psapi.EnumProcessModules.argtypes = [wt.HANDLE, ctypes.POINTER(wt.HMODULE), wt.DWORD,
                                     ctypes.POINTER(wt.DWORD)]
psapi.GetModuleBaseNameW.argtypes = [wt.HANDLE, wt.HMODULE, wt.LPWSTR, wt.DWORD]
u32.FindWindowW.restype = wt.HWND
u32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]

TARGET = sys.argv[1] if len(sys.argv) > 1 else 'tests\\host_run32.exe'
DLL = sys.argv[2] if len(sys.argv) > 2 else 'dist\\NextPerfHook32.dll'

# 清掉旧日志
for c in glob.glob(os.path.join(os.environ['TEMP'], 'NextPerfHook.log')):
    try:
        os.remove(c)
    except OSError:
        pass

kill_all()
subprocess.Popen([r'dist\NextPerf.exe'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(3)
subprocess.Popen([os.path.abspath(TARGET), '--seconds', '30'],
                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(3)

hw = u32.FindWindowW(None, 'NpHost')
pid = wt.DWORD()
u32.GetWindowThreadProcessId(hw, ctypes.byref(pid))
tgt = pid.value

hp = k32.OpenProcess(0x1000, False, tgt)
wow = wt.BOOL()
k32.IsWow64Process(hp, ctypes.byref(wow))
print('  target pid=%d  IsWow64=%d' % (tgt, wow.value))

inject(tgt, os.path.abspath(DLL))
time.sleep(12)

# 目标进程里有没有我们的模块？
hp2 = k32.OpenProcess(0x0410, False, tgt)
found = []
if hp2:
    mods = (wt.HMODULE * 1024)()
    need = wt.DWORD()
    if psapi.EnumProcessModules(hp2, mods, ctypes.sizeof(mods), ctypes.byref(need)):
        for i in range(need.value // ctypes.sizeof(wt.HMODULE)):
            buf = ctypes.create_unicode_buffer(260)
            if psapi.GetModuleBaseNameW(hp2, mods[i], buf, 260):
                if 'NextPerfHook' in buf.value:
                    found.append(buf.value)
    else:
        print('  EnumProcessModules err=%d' % ctypes.get_last_error())
else:
    print('  OpenProcess(查询) 失败 err=%d' % ctypes.get_last_error())

if found:
    print('  ✅ 目标进程已加载:', ', '.join(found))
else:
    print('  ❌ 目标进程里没有 NextPerfHook* 模块')

logs = glob.glob(os.path.join(os.environ['TEMP'], 'NextPerfHook.log'))
if logs:
    print('  ✅ 钩子日志已生成:', logs[0])
    lines = io.open(logs[0], encoding='utf-8', errors='ignore').read().splitlines()
    for l in [x.split('] ', 1)[-1] for x in lines][:6]:
        print('      ', l[:120])
else:
    print('  ❌ 钩子日志没生成（DLL 未载入或载入即崩）')

kill_all()
