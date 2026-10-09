"""结构体镜像一致性检查：C++ 的共享内存结构与 Python 测试脚本里的 ctypes 镜像。

为什么需要这个：
  曾经往 `NPTelemetry` **中间**插了一个字段，而 `tests/verify_inject.py` 里的
  ctypes 镜像没跟着改。两边**字段总数相同、总大小也相同**（只是排列不同），
  所以 `version == sizeof(...)` 那道大小自检**抓不到** —— 结果读到的全是错位
  字节：钩子明明工作正常，测试却报「未挂上」。查了很久。

  宽度的坑同理：Windows（LLP64）上 `ctypes.c_ulong` 就是 32 位，和 `c_uint32`
  等价；`c_ulonglong` 等价于 `c_uint64`。所以这里按**字节宽度**比对，而不是按
  ctypes 类名比对 —— 按类名比会误报一堆。

用法： python tests/struct_check.py     退出码 0 = 一致
"""
import ctypes
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# C++ 基础类型 -> 字节宽度
CWIDTH = {
    "float": 4,
    "double": 8,
    "char": 1,
    "uint8_t": 1, "int8_t": 1,
    "uint16_t": 2, "int16_t": 2,
    "uint32_t": 4, "int32_t": 4, "unsigned": 4, "int": 4,
    "uint64_t": 8, "int64_t": 8,
}

# ctypes 类型 -> 字节宽度
PYWIDTH = {
    "c_float": 4, "c_double": 8, "c_char": 1, "c_byte": 1, "c_ubyte": 1,
    "c_short": 2, "c_ushort": 2,
    "c_long": 4, "c_ulong": 4, "c_int": 4, "c_uint": 4,     # Windows LLP64
    "c_longlong": 8, "c_ulonglong": 8, "c_int64": 8, "c_uint64": 8,
    "c_int32": 4, "c_uint32": 4, "c_int16": 2, "c_uint16": 2,
}


def cpp_fields(header, struct_name):
    """从 C++ 头文件里按**声明顺序**取出结构体字段：(名字, 字节宽度)。"""
    src = open(header, encoding="utf-8", errors="ignore").read()
    m = re.search(r"struct\s+%s\s*\{(.*?)\n\};" % re.escape(struct_name), src, re.S)
    if not m:
        raise SystemExit("在 %s 里找不到 struct %s" % (header, struct_name))
    out = []
    for raw in m.group(1).splitlines():
        line = raw.split("//")[0].strip()
        if not line or line.startswith("#"):
            continue
        d = re.match(r"^([A-Za-z_][\w:]*)\s+(.+);$", line)
        if not d:
            continue
        base = d.group(1)
        w = CWIDTH.get(base)
        if w is None:
            raise SystemExit("不认识的 C++ 类型 %r（字段声明：%s）" % (base, line))
        for part in d.group(2).split(","):
            part = part.strip()
            if not part:
                continue
            arr = re.search(r"\[(.+?)\]", part)
            name = re.sub(r"\[.*$", "", part).strip()
            width = w
            if arr:
                dim = arr.group(1)
                if dim.endswith("u"):
                    dim = dim[:-1]
                # 容量宏也支持
                mm = re.match(r"^([A-Za-z_]\w*)$", dim)
                if mm:
                    src2 = src
                    dm = re.search(r"#define\s+%s\s+(\d+)" % re.escape(dim), src2)
                    if dm:
                        dim = dm.group(1)
                width = w * int(dim)
            out.append((name, width))
    return out


def py_fields(module_path, class_name):
    """从 Python 里取出 ctypes 镜像字段：(名字, 字节宽度)。"""
    sys.path.insert(0, os.path.dirname(module_path))
    mod_name = os.path.splitext(os.path.basename(module_path))[0]
    mod = __import__(mod_name)
    cls = getattr(mod, class_name)
    out = []
    for name, ctype in cls._fields_:
        # ⚠ 必须用 issubclass：`ctypes.c_char * 128` 是一个**类**，不是实例，
        #   isinstance(x, ctypes.Array) 会返回 False，于是数组统统被当成标量。
        is_arr = isinstance(ctype, type) and issubclass(ctype, ctypes.Array)
        if is_arr:
            base = ctype._type_
            n = ctype._length_
            w = PYWIDTH.get(getattr(base, "__name__", ""), -1) * n
        else:
            w = PYWIDTH.get(getattr(ctype, "__name__", ""), -1)
        out.append((name, w))
    return out, ctypes.sizeof(cls)


def check(struct_name, py_class):
    cpp = cpp_fields(os.path.join(ROOT, "src", "common", "np_common.h"), struct_name)
    py, pysize = py_fields(os.path.join(HERE, "verify_inject.py"), py_class)

    ok = True
    if len(cpp) != len(py):
        print("  ❌ 字段数量不同：C++ %d 个，Python %d 个" % (len(cpp), len(py)))
        ok = False
    for i, ((cn, cw), (pn, pw)) in enumerate(zip(cpp, py)):
        if cn != pn:
            print("  ❌ 第 %d 个字段名字不同：C++=%s，Python=%s" % (i + 1, cn, pn))
            ok = False
        elif cw != pw:
            print("  ❌ 第 %d 个字段 %s 宽度不同：C++=%d 字节，Python=%d 字节"
                  % (i + 1, cn, cw, pw))
            ok = False
    cppsize = sum(w for _, w in cpp)
    if ok and cppsize != pysize:
        print("  ❌ 总大小不同：C++ 字段合计 %d，Python sizeof %d" % (cppsize, pysize))
        ok = False
    if ok:
        print("  ✅ %s：%d 个字段，顺序/名字/宽度/大小全部一致（%d 字节）"
              % (struct_name, len(cpp), pysize))
    return ok


def main():
    print("### 结构体镜像一致性（C++ vs Python ctypes）")
    allok = True
    for cname, pyname in (("NPTelemetry", "NPTelemetry"),):
        if not check(cname, pyname):
            allok = False
    print("结果：%s" % ("PASS —— 镜像一致" if allok else "FAIL —— 镜像不一致，测试会读到错位字节"))
    return 0 if allok else 1


if __name__ == "__main__":
    sys.exit(main())
