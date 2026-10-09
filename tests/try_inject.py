"""对任意正在运行的进程做一次真实注入尝试，并把结果摊开说清楚。

用法：
    python tests/try_inject.py steamwebhelper.exe
    python tests/try_inject.py --pid 1234

它做的就是主程序「注入到前台进程」按钮做的同一件事（CreateRemoteThread +
LoadLibraryW），只是把每一步的错误码都打出来 —— 主程序的错误提示会被
状态栏刷新覆盖，所以排查得靠这个。
"""
import ctypes
import ctypes.wintypes as wt
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from diag import enum_pids          # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOOK_DLL = os.path.join(ROOT, "dist", "NextPerfHook.dll")
LOG = os.path.join(os.environ.get("TEMP", "."), "NextPerfHook.log")

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.GetModuleHandleW.restype = wt.HMODULE
k32.GetModuleHandleW.argtypes = [wt.LPCWSTR]
k32.GetProcAddress.restype = ctypes.c_void_p
k32.GetProcAddress.argtypes = [wt.HMODULE, wt.LPCSTR]
k32.VirtualAllocEx.restype = ctypes.c_void_p
k32.VirtualAllocEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_size_t, wt.DWORD, wt.DWORD]
k32.WriteProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                   ctypes.POINTER(ctypes.c_size_t)]
k32.CreateRemoteThread.restype = wt.HANDLE
k32.CreateRemoteThread.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
                                   ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD)]
k32.OpenMutexW.restype = wt.HANDLE
k32.OpenMutexW.argtypes = [wt.DWORD, wt.BOOL, wt.LPCWSTR]

NEEDED = 0x0002 | 0x0008 | 0x0020 | 0x0010 | 0x1000   # CREATE_THREAD|VM_OP|VM_WRITE|VM_READ|QUERY_LIMITED
ALL_ACCESS = 0x1F0FFF

ERRS = {
    5: "ERROR_ACCESS_DENIED —— 权限不够（游戏以管理员运行？反作弊保护？）",
    87: "ERROR_INVALID_PARAMETER",
    299: "ERROR_PARTIAL_COPY —— 目标可能是 32 位进程（本 DLL 是 64 位）",
    126: "ERROR_MOD_NOT_FOUND —— 目标进程里找不到 DLL 或其依赖",
    127: "ERROR_PROC_NOT_FOUND —— 找不到 LoadLibraryW 导出",
}


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    if sys.argv[1] == "--pid":
        pid = int(sys.argv[2])
        name = "(指定 pid)"
        for p, n in enum_pids():
            if p == pid:
                name = n
    else:
        want = sys.argv[1].lower()
        hits = [(p, n) for p, n in enum_pids() if n.lower() == want]
        if not hits:
            print("找不到进程 %s。当前匹配到的：" % sys.argv[1])
            for p, n in enum_pids():
                if want.split(".")[0][:6] in n.lower():
                    print("  pid=%-6d %s" % (p, n))
            return 1
        pid, name = hits[0]

    print("目标：pid=%d %s" % (pid, name))
    print("DLL ：%s（%d 字节）" % (HOOK_DLL, os.path.getsize(HOOK_DLL)))
    log_before = os.path.getsize(LOG) if os.path.exists(LOG) else 0
    print("日志：%s（注入前 %d 字节）" % (LOG, log_before))
    print("-" * 60)

    h = k32.OpenProcess(NEEDED, False, pid)
    err = ctypes.get_last_error()
    print("OpenProcess(0x%X)  -> %s   err=%d %s" %
          (NEEDED, "OK" if h else "FAIL", err, ERRS.get(err, "")))
    if not h:
        h = k32.OpenProcess(ALL_ACCESS, False, pid)
        err2 = ctypes.get_last_error()
        print("OpenProcess(ALL)    -> %s   err=%d %s" %
              ("OK" if h else "FAIL", err2, ERRS.get(err2, "")))
    if not h:
        print("\n结论：连进程都打不开，注入无从谈起。")
        print("      主程序里这种情况的提示是「无法打开目标进程（可能需要管理员权限）」，")
        print("      但它会在 120ms 内被状态栏刷新覆盖 —— 这就是你看不到原因的原因。")
        return 1

    path = HOOK_DLL.encode("utf-16-le") + b"\x00\x00"
    mem = k32.VirtualAllocEx(h, None, len(path), 0x3000, 0x40)
    print("VirtualAllocEx      -> %s   err=%d" % ("OK" if mem else "FAIL", ctypes.get_last_error()))
    if not mem:
        return 1
    w = ctypes.c_size_t(0)
    ok = k32.WriteProcessMemory(h, mem, path, len(path), ctypes.byref(w))
    print("WriteProcessMemory  -> %s (%d/%d)  err=%d" %
          ("OK" if ok else "FAIL", w.value, len(path), ctypes.get_last_error()))
    if not ok:
        return 1

    ll = k32.GetProcAddress(k32.GetModuleHandleW("kernel32.dll"), b"LoadLibraryW")
    th = k32.CreateRemoteThread(h, None, 0, ctypes.c_void_p(ll), mem, 0, None)
    err = ctypes.get_last_error()
    print("CreateRemoteThread  -> %s   err=%d %s" %
          ("OK" if th else "FAIL", err, ERRS.get(err, "")))
    if not th:
        return 1
    k32.WaitForSingleObject(th, 10000)
    code = wt.DWORD(0)
    k32.GetExitCodeThread(th, ctypes.byref(code))
    print("LoadLibraryW 返回值 -> 0x%X  %s" % (code.value,
          "（非 0 = DLL 已载入）" if code.value else "（0 = 载入失败！）"))
    k32.CloseHandle(th)
    k32.CloseHandle(h)

    time.sleep(3)
    m = k32.OpenMutexW(0x00100000, False, "Local\\NextPerf_Injected_%d" % pid)
    print("注入标记互斥量      -> %s" % ("存在（钩子起来了）" if m else "不存在"))
    if m:
        k32.CloseHandle(m)

    h = k32.OpenFileMappingW = None
    print("-" * 60)
    print("新增日志：")
    if os.path.exists(LOG):
        data = open(LOG, encoding="utf-8", errors="ignore").read()
        if len(data) > log_before:
            print(data[log_before:].strip())
        elif log_before and len(data) == log_before:
            print("  （没有新增 —— DLL 载入后什么都没写出来）")
        else:
            print(data.strip()[-2000:])
    else:
        print("  （日志文件不存在）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
