"""NextPerf 现场诊断：不猜，直接读现场。

  1. NextPerfHook.dll 的导入表 —— 少一个依赖就会 LoadLibrary 失败（游戏里静默）
  2. 主程序共享内存里的配置 —— 确认跑的是哪个构建（新版有 vtableProbe 字段）
  3. 扫描所有进程的注入标记互斥量 —— 到底哪些进程真的载入了钩子
  4. 有标记的进程再读遥测块 —— 判断挂上了没有
  5. 游戏 exe 是否存在
"""
import ctypes
import ctypes.wintypes as wt
import os
import struct
import sys

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenFileMappingW.restype = wt.HANDLE
k32.OpenFileMappingW.argtypes = [wt.DWORD, wt.BOOL, wt.LPCWSTR]
k32.MapViewOfFile.restype = ctypes.c_void_p
k32.MapViewOfFile.argtypes = [wt.HANDLE, wt.DWORD, wt.DWORD, wt.DWORD, ctypes.c_size_t]
k32.OpenMutexW.restype = wt.HANDLE
k32.OpenMutexW.argtypes = [wt.DWORD, wt.BOOL, wt.LPCWSTR]
k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
k32.CreateToolhelp32Snapshot.argtypes = [wt.DWORD, wt.DWORD]

NP_MAGIC = 0x4E505231

# NPConfig 的字段偏移（和 src/common/np_common.h 对齐）
CFG_FIELDS = [
    ("magic", "I"), ("version", "I"), ("size", "I"), ("counters", "Q"),
    ("bgColor", "I"), ("textColor", "I"), ("accentColor", "I"), ("warnColor", "I"),
    ("scale", "f"), ("opacity", "f"), ("bgOpacity", "f"), ("textOpacity", "f"),
    ("offsetX", "i"), ("offsetY", "i"), ("fontHeight", "I"),
    ("graphHeight", "I"), ("overlayMode", "I"), ("fpsCap", "I"), ("pollMs", "I"),
    ("updateHz", "I"), ("deepEngineHook", "I"), ("vtableProbe", "I"), ("quit", "I"),
    ("simulate", "I"),
]


def enum_pids():
    TH32CS_SNAPPROCESS = 0x2

    class PROCESSENTRY32W(ctypes.Structure):
        _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
                    ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)), ("th32ModuleID", wt.DWORD),
                    ("cntThreads", wt.DWORD), ("th32ParentProcessID", wt.DWORD),
                    ("pcPriClassBase", ctypes.c_long), ("dwFlags", wt.DWORD),
                    ("szExeFile", wt.WCHAR * 260)]

    out = []
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if not snap or snap == -1:
        return out
    pe = PROCESSENTRY32W()
    pe.dwSize = ctypes.sizeof(pe)
    if k32.Process32FirstW(snap, ctypes.byref(pe)):
        while True:
            out.append((pe.th32ProcessID, pe.szExeFile))
            if not k32.Process32NextW(snap, ctypes.byref(pe)):
                break
    k32.CloseHandle(snap)
    return out


def dump_imports(dll):
    """极简 PE 导入表解析（不依赖 pefile）。"""
    data = open(dll, "rb").read()
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[e_lfanew:e_lfanew + 4] == b"PE\0\0", "不是 PE 文件"
    coff = e_lfanew + 4
    nsec, = struct.unpack_from("<H", data, coff + 2)
    opt_size, = struct.unpack_from("<H", data, coff + 16)
    opt = coff + 20
    magic, = struct.unpack_from("<H", data, opt)
    pe32plus = (magic == 0x20B)
    dd_off = opt + (112 if pe32plus else 96)
    imp_rva, imp_size = struct.unpack_from("<II", data, dd_off + 8)   # dir[1] = import
    sec_off = opt + opt_size
    sections = []
    for i in range(nsec):
        s = sec_off + i * 40
        va, vsize = struct.unpack_from("<II", data, s + 12)[0], struct.unpack_from("<I", data, s + 8)[0]
        raw, rawsize = struct.unpack_from("<II", data, s + 20)
        sections.append((va, vsize, raw, rawsize))

    def rva2off(rva):
        for va, vsize, raw, rawsize in sections:
            if va <= rva < va + max(vsize, rawsize):
                return raw + (rva - va)
        return None

    names = []
    off = rva2off(imp_rva)
    while off:
        entry = struct.unpack_from("<IIIII", data, off)
        if entry[0] == 0 and entry[1] == 0 and entry[2] == 0 and entry[3] == 0 and entry[4] == 0:
            break
        name_off = rva2off(entry[3])
        if name_off is None:
            break
        end = data.index(b"\0", name_off)
        names.append(data[name_off:end].decode("ascii", "ignore"))
        off += 20
    return names


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    dll = os.path.join(root, "dist", "NextPerfHook.dll")

    print("=" * 70)
    print("1) NextPerfHook.dll 导入表 —— 任何一项在目标进程里找不到，")
    print("   LoadLibraryW 就会失败，而游戏那边是完全静默的")
    print("=" * 70)
    if os.path.exists(dll):
        imps = dump_imports(dll)
        print("  %s" % dll)
        for n in sorted(set(imps)):
            print("    - %s" % n)
        risky = [n for n in imps if n.lower().startswith(("libgcc", "libstdc++", "libwinpthread"))]
        print("  → 可能缺失的 mingw 运行时依赖：%s" % (risky if risky else "无（已静态链接）"))
    else:
        print("  找不到 %s" % dll)

    print()
    print("=" * 70)
    print("2) 主程序共享内存里的配置（判断跑的是哪个构建）")
    print("=" * 70)
    h = k32.OpenFileMappingW(0x0004, False, "Local\\NextPerf_Config_v1")
    if not h:
        print("  打不开 Local\\NextPerf_Config_v1 —— 主程序没在跑？")
    else:
        n = 0
        for name, fmt in CFG_FIELDS:
            sz = struct.calcsize("<" + fmt)
            # 手工按自然对齐推算偏移
            align = min(sz, 8)
            n = (n + align - 1) // align * align
            n += sz
        view = k32.MapViewOfFile(h, 0x0004, 0, 0, n)
        raw = ctypes.string_at(view, n)
        k32.UnmapViewOfFile(ctypes.c_void_p(view))
        k32.CloseHandle(h)
        vals = {}
        off = 0
        for name, fmt in CFG_FIELDS:
            sz = struct.calcsize("<" + fmt)
            align = min(sz, 8)
            off = (off + align - 1) // align * align
            vals[name] = struct.unpack_from("<" + fmt, raw, off)[0]
            off += sz
        print("  magic=0x%08X %s" % (vals["magic"], "OK" if vals["magic"] == NP_MAGIC else "!! 不对"))
        for k in ("counters", "vtableProbe", "deepEngineHook", "simulate", "quit", "overlayMode",
                  "updateHz", "pollMs"):
            print("  %-16s = %s" % (k, vals[k]))
        print("  → %s" % ("是新构建（有 vtableProbe 字段）" if "vtableProbe" in vals
                         else "?? 字段对不上"))
    print("  注：字段值还需和 config.json 对照；config.json 里没有 vtableProbe 说明")
    print("      那份文件是旧构建写的。")

    print()
    print("=" * 70)
    print("3) 哪些进程真的载入了钩子（注入标记互斥量）")
    print("=" * 70)
    found = []
    for pid, name in enum_pids():
        m = k32.OpenMutexW(0x00100000, False, "Local\\NextPerf_Injected_%d" % pid)
        if m:
            k32.CloseHandle(m)
            found.append((pid, name))
    if found:
        for pid, name in found:
            print("  pid=%-6d %s" % (pid, name))
    else:
        print("  （一个都没有 —— 钩子从没成功载入任何进程）")

    print()
    print("=" * 70)
    print("4) 这些进程的遥测状态")
    print("=" * 70)
    for pid, name in found:
        h = k32.OpenFileMappingW(0x0004, False, "Local\\NextPerf_Telemetry_v1_%d" % pid)
        if not h:
            print("  pid=%-6d %s : 没有遥测块（钩子 early-return 了）" % (pid, name))
            continue
        view = k32.MapViewOfFile(h, 0x0004, 0, 0, 64)
        raw = ctypes.string_at(view, 32)
        k32.UnmapViewOfFile(ctypes.c_void_p(view))
        k32.CloseHandle(h)
        magic, ver, tpid, attached = struct.unpack_from("<IIII", raw, 0)
        tick, = struct.unpack_from("<Q", raw, 16)
        print("  pid=%-6d %s : magic=0x%08X attached=%d tickMs=%d" % (pid, name, magic, attached, tick))

    print()
    print("=" * 70)
    print("5) 配置里的游戏 exe 是否存在")
    print("=" * 70)
    for g in []:   # 如需检查具体游戏路径，自己往这个列表里填
        print("  %s -> %s" % (g, "存在" if os.path.exists(g) else "不存在/盘符不在"))
    print("  G: 盘可访问：%s" % os.path.exists("G:\\"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
