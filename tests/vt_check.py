"""从 mingw 头文件里算出 COM vtable 下标，并和 np_hook.cpp 里的 Vt:: 枚举对比。

COM 接口的 vtable 槽位顺序 == 虚函数声明顺序（含继承链上的基类方法）。
把头文件里的声明按顺序解析出来就是权威下标 —— 比凭记忆数一遍可靠得多，
错一个就是劫持到别的成员函数，宿主直接崩。

用法： python tests/vt_check.py
"""
import os
import re
import sys

ZIG_LIB_CANDIDATES = [
    os.path.expandvars(r"%USERPROFILE%\.workbuddy\binaries\python\envs\default\Lib\site-packages\ziglang\lib"),
    os.path.expandvars(r"%LOCALAPPDATA%\Programs\Python\Python*\Lib\site-packages\ziglang\lib"),
]

# np_hook.cpp 里 Vt:: 命名空间下的常量：接口 -> {成员: 代码里的值}
EXPECTED = {
    "IDXGISwapChain": {
        "Present": 8, "GetBuffer": 9, "GetFullscreenState": 11, "GetDesc": 12,
    },
    "IDXGISwapChain1": {"GetHwnd": 20, "Present1": 22},
    "IDXGISwapChain3": {"GetCurrentBackBufferIndex": 36},
    "IDXGIFactory": {"CreateSwapChain": 10},
    "IDXGIFactory2": {"CreateSwapChainForHwnd": 15, "CreateSwapChainForComposition": 24},
    "ID3D12CommandQueue": {
        "ExecuteCommandLists": 10, "Signal": 14,
        "GetTimestampFrequency": 16, "GetDesc": 19,
    },
    "ID3D12GraphicsCommandList": {
        "Close": 9, "Reset": 10, "DrawInstanced": 12, "DrawIndexedInstanced": 13,
        "Dispatch": 14, "RSSetViewports": 21, "EndQuery": 53, "ResolveQueryData": 54,
    },
    "ID3D12GraphicsCommandList4": {
        "BuildRaytracingAccelerationStructure": 72, "DispatchRays": 76,
    },
}

HEADERS = ["dxgi.h", "dxgi1_2.h", "dxgi1_3.h", "dxgi1_4.h", "dxgi1_5.h", "dxgi1_6.h", "d3d12.h"]
# MIDL_INTERFACE("...") 下一行是 `Name : public Base`，再下一行是 `{`
# 也有 `struct Name : public Base {` 的写法
DECL_RE = re.compile(r"^\s*(?:struct\s+|class\s+)?(\w+)\s*:\s*public\s+([\w:]+)\s*$", re.M)
DECL_INLINE_RE = re.compile(r"^\s*(?:struct\s+|class\s+)?(\w+)\s*:\s*public\s+([\w:]+)\s*\{", re.M)
VIRTUAL_RE = re.compile(r"^\s*virtual\s+[^;{]*?\b(\w+)\s*\(", re.M)


def find_include_dir():
    import glob
    for base in ZIG_LIB_CANDIDATES:
        for b in glob.glob(base):
            p = os.path.join(b, "libc", "include", "any-windows-any")
            if os.path.isdir(p):
                return p
    sys.exit("找不到 mingw 头文件目录（ziglang 装在哪？）")


def load_texts(incdir):
    out = {}
    for h in HEADERS:
        p = os.path.join(incdir, h)
        if os.path.exists(p):
            with open(p, encoding="utf-8", errors="ignore") as f:
                out[h] = f.read()
    return out


def split_block(text, start):
    """从 start 处的 '{' 开始，返回 (方法体文本, 结束位置)。"""
    assert text[start] == "{", text[start:start + 20]
    depth = 0
    i = start
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start + 1:i], i + 1
        i += 1
    return text[start + 1:], len(text)


def find_decl(texts, name):
    """返回 (基类名, [方法名...])，找不到返回 (None, None)。"""
    pat = re.compile(r"^\s*(?:struct\s+|class\s+)?%s\s*:\s*public\s+([\w:]+)\s*(\{)?\s*$"
                     % re.escape(name), re.M)
    for text in texts.values():
        m = pat.search(text)
        if not m:
            continue
        base = m.group(1).split("::")[-1]
        brace = text.find("{", m.end() - 1)
        if brace < 0 or brace - m.end() > 8:
            continue
        body, _ = split_block(text, brace)
        return base, VIRTUAL_RE.findall(body)
    return None, None


def vtable_of(name, texts, cache):
    """整张 vtable：{方法名: 下标}"""
    if name in cache:
        return cache[name]
    base, methods = find_decl(texts, name)
    if methods is None:
        cache[name] = {}
        return cache[name]
    table = dict(vtable_of(base, texts, cache)) if base else {}
    start = len(table)
    for k, m in enumerate(methods):
        table[m] = start + k
    cache[name] = table
    return table


def main():
    incdir = find_include_dir()
    print("头文件目录：%s\n" % incdir)
    texts = load_texts(incdir)
    cache = {"IUnknown": {"QueryInterface": 0, "AddRef": 1, "Release": 2}}
    bad = 0
    for iface, members in EXPECTED.items():
        table = vtable_of(iface, texts, cache)
        inv = {v: k for k, v in table.items()}
        print("%s（%d 个槽位）" % (iface, len(table)))
        for member, want in members.items():
            got = table.get(member)
            if got == want:
                print("  %-42s 代码=%-4s 头文件=%-4s OK" % (member, want, got))
            else:
                bad += 1
                print("  %-42s 代码=%-4s 头文件=%-4s <<<< 不一致" % (member, want, got))
                print("      -> 下标 %d 实际是 %s" % (want, inv.get(want, "越界/不存在")))
        print()
    if bad:
        print("有 %d 处下标需要修正" % bad)
        return 1
    print("全部一致 ✓")
    return 0


if __name__ == "__main__":
    sys.exit(main())
