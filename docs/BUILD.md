# 编译说明

## 方式一：双击 `build.bat`（推荐）

```
build.bat
```

脚本会：

1. **找 zig 编译器**，按顺序：
   `NP_ZIG` 环境变量 → PATH 里的 `zig.exe` → 常见 Python 环境的
   `Lib\site-packages\ziglang\zig.exe` → 都找不到就 `pip install ziglang` 再找一遍；
2. 编译 `dist\NextPerf.exe` 和 `dist\NextPerfHook.dll`；
3. 复制一份 `NextPerfHook64.dll`，清掉 `.pdb` / `.lib`。

> 缓存固定写在项目内的 `.zig-global\` / `.zig-cache\`（可用 `ZIG_GLOBAL_CACHE_DIR` /
> `ZIG_LOCAL_CACHE_DIR` 覆盖）。zig 默认写 `%LOCALAPPDATA%\zig`，在受限环境（沙箱、
> 只读用户目录）会直接以 `AccessDenied` 失败。这两个目录可以随时删掉回收空间
> （约 100 MB），删了下次重新构建即可。

> 脚本文件本身是 **GBK + CRLF** 编码的。用 UTF-8/LF 保存会让 `cmd.exe`
> 在中文和 `for /f` 语句上解析出错（踩过），改脚本时注意保持。

## 方式二：手动编译

### 编译器

项目用 **zig** 自带的 clang 作为交叉编译器（`zig c++ -target x86_64-windows-gnu`）。
它自带 mingw-w64 的头文件和导入库，**不需要装 Visual Studio，也不需要装 MSYS2**。

```bash
pip install ziglang
```

装完后编译器是 `<venv>/Scripts/python-zig.exe`。

### 命令行

```bash
ZIG="<venv>/Scripts/python-zig.exe"
CXX="-target x86_64-windows-gnu -O2 -std=c++20 -DUNICODE -D_UNICODE -fno-exceptions -I src"

# 主程序
$ZIG c++ $CXX -o dist/NextPerf.exe \
    src/app/main.cpp src/app/ui.cpp src/app/overlay.cpp src/app/injector.cpp src/app/settings.cpp \
    src/common/np_panel.cpp src/common/np_build.cpp src/common/np_bitmap.cpp \
    src/sensors/np_vendor.cpp src/sensors/np_sensors.cpp \
    -Wl,--subsystem,windows \
    -luser32 -lgdi32 -lshell32 -ladvapi32 -lole32 -loleaut32 -luuid -lcomctl32 -lcomdlg32 \
    -lpdh -lpsapi -ldxgi -ld3d11 -ld2d1 -ldwrite -lshlwapi -lwinmm -lmsimg32

# 钩子 DLL（-shared，不要 subsystem 参数）
$ZIG c++ $CXX -shared -o dist/NextPerfHook.dll \
    src/hook/dllmain.cpp src/hook/np_hook.cpp src/hook/np_draw.cpp \
    src/common/np_panel.cpp src/common/np_build.cpp src/common/np_bitmap.cpp \
    -luser32 -lgdi32 -lshell32 -ladvapi32 -lole32 -loleaut32 -luuid \
    -ldxgi -ld3d11 -ld3d12 -ld2d1 -ldwrite -lshlwapi
```

### 如果用 MSVC

理论上可以（代码是标准 C++20 + Win32），但要注意：

* `src/sensors/np_vendor.cpp` 和 `src/app/overlay.cpp` 用了 `setjmp/longjmp` 加
  `AddVectoredExceptionHandler` 来模拟 SEH —— MSVC 下直接换成 `__try/__except` 更合适；
* `dwrite.h` 的 `IDWriteTextFormat` 没有 `SetFontSize`，代码里是按"换字号就重建 TextFormat"写的，两边通用；
* `winuser.h` 的 `#define DrawText DrawTextW` 会把 `ID2D1RenderTarget::DrawText` 改名，
  本项目统一以 `UNICODE` 编译，所以直接写 `DrawText` 即可（**不要** `#undef DrawText`）；
* 需要链接 `pdh.lib wbemuuid.lib dxgi.lib d3d11.lib d3d12.lib d2d1.lib dwrite.lib`。

### 如果用 MinGW-w64（g++）

同样可以，注意加 `-municode` 或者把 `WinMain` 换成 `wWinMain + -municode`。
本项目用的是 `WinMain(HINSTANCE, HINSTANCE, LPSTR, int)`，mingw 下不需要额外开关。

---

## 依赖

**零第三方库**。只用操作系统自带的东西：

| 用途 | 依赖 |
| --- | --- |
| UI / 窗口 / GDI | user32, gdi32, comdlg32, comctl32, shell32 |
| 叠加绘制 | d2d1, dwrite（Windows 7+ 自带） |
| 图形接口 | dxgi, d3d11, d3d12（运行时不链接 d3dcompiler_47，动态 LoadLibrary） |
| 传感器 | pdh, psapi, advapi32（注册表） |
| WMI | ole32, oleaut32, uuid |
| 厂商 SDK | nvml.dll / nvapi64.dll / atiadlxx.dll —— **全部运行时动态加载**，编译期不需要 |

---

## 验证

### 自动验证（推荐）

`tests\` 下有一整套端到端测试，全部用真实 D3D 宿主进程跑，覆盖注入链路的各条路径：

```bash
python tests\vt_check.py                       # COM vtable 下标 vs mingw 头文件，必须全 OK
python tests\verify_inject.py                  # 游戏已在运行（交换链早建好）-> 探测 vtable，D3D12
python tests\verify_inject.py --d3d11          # 同上，D3D11 + flip 模型后台缓冲
python tests\verify_early.py                   # 图形初始化之前注入 -> 走 DXGI 工厂钩子
python tests\verify_frontinject.py             # 「注入到前台进程」按钮（前台是自己时）
```

测试宿主先编译：`tests\build_hosts.bat`。

`vt_check.py` 值得单独说一句：它把头文件里的虚函数声明顺序解析出来，
和 `src/hook/np_hook.cpp` 里手写的 `Vt::` 枚举逐条对比。**改钩子之前先跑它** ——
下标错一个就是劫持到别的成员函数，宿主直接崩。
（历史上 `CreateSwapChainForHwnd` 被写成 13，那其实是 `IsCurrent`；正确值是 15。）

### 自检

```bash
dist\NextPerf.exe --selftest    # 传感器 + 渲染自检，结果写 **项目根的 selftest.txt**，退出码 0 = 全通过
dist\NextPerf.exe --uismoke     # 真正创建窗口跑 2 秒消息循环（验证 WM_PAINT 路径），退出码 0 = 正常
```

钩子跑在别的进程里，出问题看日志：

| 文件 | 谁写 | 什么时候有 |
| --- | --- | --- |
| `%TEMP%\NextPerf.log` | 主程序 | 总是有：启动环境、游戏列表、每次注入尝试的每一步 + Win32 错误码 |
| `%TEMP%\NextPerfHook.log` | 注入进游戏的钩子 | 只有 DLL 真的载入之后才有 |

两个文件每行都带 pid / tid（多进程共用同一个文件），超过 1MB 从头覆盖。

现场排查用这两个：

```
python tests\diag.py                        # DLL 依赖 / 主程序构建版本 / 谁真的载入了钩子 / exe 是否存在
python tests\try_inject.py <exe 名或 --pid N>  # 单步注入并打印每一步的错误码
```

`diag.py` 里最有用的一条是「谁载入了钩子」—— 它遍历所有进程查
`Local\NextPerf_Injected_<pid>` 互斥量。**一个都没有**就说明注入这一环没过，
和游戏内渲染无关；这时 `NextPerf.log` 里会有具体错误码。

自检输出示例：

```
NextPerf 自检报告
=================
[ OK ] Direct2D / DirectWrite 可用
数据源: NVML+PDH+WMI
显卡: NVIDIA GeForce RTX 5080 Laptop GPU
CPU 占用 3.7%  温度 -273.0 C
GPU 占用 1.0%  温度 38.0 C  功耗 12.8 W
显存 2.95 / 15.92 GB   内存 13.98 / 31.42 GB
NVAPI 域: GPU -1.0  FB -1.0  VID -1.0  BUS -1.0
硬件 RT/Tensor: -1.0 / -1.0 （-1 表示厂商未开放）
面板尺寸: 260 x 120，行数 17
[ OK ] 面板位图渲染出 201 个非空字节
[ OK ] 共享内存 Config / Sensors 创建成功
钩子 DLL: ...\dist\NextPerfHook.dll
[ OK ] NextPerfHook.dll 存在
=================
自检结束：全部通过
```

`温度 -273.0` 和 `NVAPI 域 -1.0` 是预期内的：
这台机器没开 HWiNFO 共享内存（读不到 CPU 温度），当前 NVIDIA 驱动的 `nvapi_QueryInterface`
拒绝分发函数指针（拿不到利用率域）。这两个都有降级路径，不影响其它指标。

---

## 常见问题

**Q: 提示找不到 python-zig.exe？**
A: `pip install ziglang` 之后，编译器就在 Python 环境的
`Lib\site-packages\ziglang\zig.exe`。`Scripts\python-zig.exe` 只是个转发 shim，
它依赖同环境的 `python.exe` —— 那个 python 被删掉或挪走之后 shim 就废了
（本机就是这样）。所以 build.bat 找的是真正的 `zig.exe`，不依赖 shim。

**Q: 报 `failed to create output directory ...AccessDenied`？**
A: zig 想写默认缓存目录 `%LOCALAPPDATA%\zig` 但没权限。build.bat 已经把缓存
指到项目内的 `.zig-cache` / `.zig-global`；手动编译时自己设
`ZIG_GLOBAL_CACHE_DIR` / `ZIG_LOCAL_CACHE_DIR`（注意它们是**环境变量**，
`zig c++ --cache-dir` 这种写法不被接受）。

**Q: 链接报 `undefined symbol: WinMain`？**
A: 用了 `wWinMain` 但没加 `-municode`。本项目用的是 `WinMain`，不需要这个开关。

**Q: 报 `no member named 'DrawText'`？**
A: 说明你在代码里 `#undef DrawText` 了。不要这么干，详见上面 MSVC 那节。

**Q: 编译很慢？**
A: zig 首次编译要构建缓存。之后会快很多。`-O1` 会比 `-O2` 明显快一些，调试期可以用。
