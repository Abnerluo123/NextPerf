# NextPerf 模拟游戏进程（tests/sim）

一个**可编程的假游戏**：DX11 / DX12 双后端，专门用来在**不依赖真人**的前提下
自动化测试 NextPerf 这个注入式性能叠加层。

它和 `tests/host_run.cpp` 的区别就是它存在的理由：

| | 旧宿主 host_run.cpp | 本模拟器 sim.exe |
|---|---|---|
| 帧率 | `Sleep(16)`，实测固定在 ~32fps | QPC 三段式锁帧，60 / 120 / 144 任意档，实测 60.00fps |
| 交换链重建 | 不支持 | `resize` 命令随时触发 ResizeBuffers |
| 窗口状态 | 只有窗口化 | windowed / borderless / fullscreen 运行中热切换 |
| 垂直同步 | 写死 `Present(1,0)` | 运行中 `vsync on\|off` |
| 帧率数据 | 无 | 每帧自报 CPU / GPU / Present 三段耗时 + 汇总统计 |
| 外部控制 | 无 | stdin 逐行命令 |
| 渲染内容 | 固定单色 Clear | 逐帧变色 + 旋转三角形（着色器路径） |

---

## 1. 构建

```bat
tests\sim\build_sim.bat
```

产物：`tests\sim\sim.exe`（GUI 子系统，双击不弹黑框；被重定向时 stdout/stdin
照常可用 —— 程序自己在 `WinMain` 里接管了标准流）。

依赖：只需要 zig（`ziglang`）。脚本按 `NP_ZIG` → `PATH` → 常见 Python
site-packages 的顺序找编译器，缓存固定写在项目内（`.zig-cache` / `.zig-global`），
构建自包含。

链接的库：`user32 gdi32 shell32 ole32 oleaut32 uuid dxgi d3d11 d3d12`。
**d3dcompiler 不静态链接** —— 运行时 `LoadLibrary("d3dcompiler_47.dll")` 取
`D3DCompile`，取不到就退化成无着色器渲染路径（功能不受影响，只是画面朴素）。

清理：`tests\sim\build_sim.bat clean`

---

## 2. 命令行参数

`--key=value` 和 `--key value` 两种写法都支持（后者对 Python 的
`subprocess(arglist)` 更顺手）。

### 流程控制

| 参数 | 默认 | 语义 |
|---|---|---|
| `--api=dx11\|dx12` | `dx11` | 图形后端。DX11 = `CreateSwapChain` + `Present(sync,0)`；DX12 = flip 模型 + `IDXGISwapChain3` + 命令队列/fence |
| `--seconds=N` | 不限时 | 跑 N 秒后正常退出（`exit_reason="time_limit"`）。与 `--frames` 互斥 |
| `--frames=N` | 不限时 | 渲染 N 帧后退出（`exit_reason="frame_limit"`） |
| `--warmup=N` | `10` | 前 N 帧不计入统计。首帧有设备创建 / 着色器编译 / 交换链首次 Present 的抖动，算进去会污染 P99 |
| `--no-stdin` | 关 | 不读 stdin 控制命令 |

不给 `--seconds` / `--frames` 就是「跑到被要求退出为止」（`quit` 命令或关窗）。
这是有意的默认值：手动观察画面时不想每次都算时间。

### 帧率与显示

| 参数 | 默认 | 语义 |
|---|---|---|
| `--fps-cap=N` | `0` | 锁帧到 N fps，`0` = 不锁。实现是 QPC 精确等待：粗睡（`Sleep`，留 2ms 余量）→ 让出时间片（`SwitchToThread`）→ 自旋（最后 0.5ms）。目标锚定在固定时间网格上，抖动不累积 |
| `--vsync=on\|off` | `on` | DX11 用 `Present(1,0)`/`Present(0,0)`；DX12 用 flip 模型的 sync interval。运行中可改 |
| `--width=W --height=H` | `1280x720` | 初始分辨率（客户区像素）。**实际交换链尺寸以建好窗口后量到的客户区为准**（见「已知坑」里的 DPI 一节） |
| `--buffers=N` | DX11 `2` / DX12 `3` | 交换链 buffer 数，2..4 |
| `--window-mode=MODE` | `windowed` | `windowed` / `borderless` / `fullscreen` |
| `--hidden` | 关 | 不 `ShowWindow`（无头 / CI）。窗口仍然存在且尺寸有效 —— flip 模型的交换链在零尺寸窗口上不能 Present |
| `--hold-ms=N` | `0` | 每帧在 CPU 侧额外占用 N 毫秒（模拟游戏逻辑开销），用自旋实现，精度到微秒 |
| `--gpu-load-ms=X` | `0` | 每帧追加约 X 毫秒的 GPU 工作量（多画几遍 / 多次整屏 Clear）。**只是近似**，用来把 GPU 顶起来让时间戳有东西可测，不是精确标定 |
| `--no-tint` | 关 | 背景色不再逐帧变化（做「静止画面」对照实验用） |

### 输出

| 参数 | 默认 | 语义 |
|---|---|---|
| `--json` | 关 | 每帧往 **stdout** 打一行 JSON。诊断信息一律走 **stderr**，所以 stdout 是纯 JSON，`json.loads(line)` 不会被噪声打断 |
| `--json-every=N` | `1` | 每 N 帧打一行（会自动打开 `--json`） |
| `--list-outputs` | — | 列出 DXGI 适配器 / 输出后退出 |
| `--dump-argv` | — | 把解析到的原始 argv 打到 stderr（排查「脚本传参被改写」用） |
| `--help` | — | 帮助 |

**退出时永远输出一份汇总 JSON**（不管有没有 `--json`）—— 这是自报数据的核心。

---

## 3. 运行时控制（stdin 逐行命令）

外部脚本通过子进程的 stdin 发命令，命令在**渲染线程的帧间隙**执行
（DXGI 和窗口都不是线程安全的，跨线程改设置会偶发崩溃）。

| 命令 | 语义 |
|---|---|
| `resize W H` | 调 `ResizeBuffers`（先释放 RTV / backbuffer 引用，再重建） |
| `window windowed\|borderless\|fullscreen` | 切窗口状态并自动把交换链重设到新的客户区尺寸。切回 `windowed` 会还原成之前记住的窗口尺寸 |
| `vsync on\|off` | 开关垂直同步，下一帧生效 |
| `fpscap N` | 改锁帧值（`0` = 不锁） |
| `holdms N` | 改每帧 CPU 侧开销 |
| `gpuload X` | 改每帧 GPU 侧负载（毫秒） |
| `stats` | 立刻输出一份「从开始到现在」的汇总 JSON（`exit_reason="on_demand_stats"`） |
| `status` | 输出一行当前状态 JSON（分辨率 / 窗口状态 / vsync / 锁帧） |
| `mark NAME` | 在输出里插一条标记（`{"type":"mark",...}`），便于脚本对齐时间轴 |
| `help` | 打印命令列表 |
| `quit` | 正常退出并输出汇总 JSON |

命令大小写不敏感；行尾 `\r\n` 和裸 `\n` 都认；空行忽略。
未知命令 / 参数错误会打到 stderr（`命令解析失败：...`）但**不会**让进程退出。

参考驱动脚本：[`sim_drive.py`](sim_drive.py) 演示了 Python 怎么驱动（也是集成自测）：

```
python tests/sim/sim_drive.py              # 两个后端全跑
python tests/sim/sim_drive.py --api dx12   # 只测 DX12
```

---

## 4. 自报数据字段

### 4.1 每帧一行（`--json`，`{"type":"frame",...}`）

| 字段 | 类型 | 含义 |
|---|---|---|
| `frame` | int | 帧序号，从 0 开始 |
| `api` | str | `dx11` / `dx12` |
| `t_ms` | float | 本帧开始时刻（相对进程启动基准，QPC 换算，ms） |
| `frame_delta_ms` | float | 与上一帧开始的间隔。**帧率就是它的倒数**；第一帧为 0 |
| `cpu_frame_ms` | float | CPU 帧时间：帧开始 → 命令提交 + Present 返回 |
| `cpu_render_ms` | float | 其中录制/提交命令列表那一段。单线程模拟里等于 `cpu_frame_ms` |
| `gpu_frame_ms` | float \| **null** | GPU 帧时间（时间戳测量）：本帧命令列表首尾时间戳之差，**不含 Present 阻塞**。`null` = 这一帧还没有可用的时间戳样本（异步回读，前几帧必然如此）|
| `gpu_valid` | bool | `gpu_frame_ms` 是否有效。**`gpu_valid=false` 时下游必须忽略 gpu_frame_ms，不能当 0** |
| `present_ms` | float | Present 阻塞时长：进 Present 到返回 |
| `vsync` | bool | 本帧的垂直同步设置 |
| `fps_cap` | int | 本帧的锁帧值（0 = 不锁） |
| `width` / `height` | int | 本帧的交换链分辨率 |
| `window_mode` | str | `windowed` / `borderless` / `fullscreen` |
| `hidden` | bool | 是否无头模式 |
| `swapchain_generation` | int | 交换链代数，每次重建 +1。叠加层可据此判断「换交换链对象了」 |
| `resize_event` | bool | **本帧**是否紧跟一次重建 |
| `resize_events_since_last` | int | 自上一行 JSON 输出以来累计重建次数。`--json-every=N(>1)` 时用它避免漏事件 |
| `present_failed` | bool | 本帧 Present 是否失败 |
| `present_hr` | int | Present 的 HRESULT（`0`=S_OK，`0x087A0001`=DXGI_STATUS_OCCLUDED 等）|

### 4.2 汇总 JSON（`{"type":"summary",...}`）

退出时输出一次；`stats` 命令输出的是同一结构，`exit_reason` 为
`"on_demand_stats"`。

**基本**

| 字段 | 含义 |
|---|---|
| `api` | 后端 |
| `exit_reason` | `time_limit` / `frame_limit` / `command_quit` / `window_closed` / `on_demand_stats` |
| `total_frames` | 总帧数（含 warmup） |
| `counted_frames` | 计入统计的帧数（不含 warmup，也是分位数样本数 N） |
| `warmup_frames` | 跳过的帧数 |
| `elapsed_ms` | 计入统计的时间跨度（帧间隔之和） |
| `swapchain_generation` | 退出时的交换链代数 |
| `resize_events` | 整个运行期间的重建次数 |

**帧率 / 帧时间**

| 字段 | 含义 |
|---|---|
| `fps_avg` | 总帧数 / 总时间（整体平均帧率） |
| `frame_ms_avg` / `_min` / `_max` / `_stddev` | 帧间隔的均值 / 最小 / 最大 / 标准差 |
| `frame_ms_p50` / `p95` / `p99` / `p999` | 帧间隔分位数（最近秩定义：`idx = ceil(p/100*N)-1`，夹到 `[0,N-1]`） |
| `fps_1pct_low` | **1% Low**：帧时间升序排后，最差 1% 那段（`ceil(0.01*N)` 帧）的**算术平均帧时间**换算成 FPS（`1000/平均帧时间`）。等价于「对帧时间取 P99 再取倒数」，与 PresentMon / CapFrameX 习惯口径一致 |
| `fps_01pct_low` | **0.1% Low**：同上，取最差 0.1%（`ceil(0.001*N)` 帧），即 `1000/P99.9(帧时间)` |
| `low1_frame_count` / `low01_frame_count` | 参与上面两个统计的帧数（帧少时至少 1） |
| `fps_best_1s` / `fps_worst_1s` | 1 秒滑动窗口里最好 / 最差的帧率。用来发现「平均 60 但偶尔掉到 20」这类平均帧率看不出来的问题 |

**分段耗时**

| 字段 | 含义 |
|---|---|
| `cpu_frame_ms_avg` | 平均 CPU 帧时间 |
| `present_ms_avg` | 平均 Present 阻塞时长 |
| `gpu_frame_ms_avg` | 平均 GPU 帧时间（只对有样本的帧求平均） |
| `gpu_valid_frames` | 有 GPU 时间戳样本的帧数 |
| `gpu_time_available` | 整个运行期间 GPU 时间戳是否真的可用 |
| `gpu_timestamp_freq_hz` | GPU 时间戳计数频率 |
| `gpu_timestamp_freq_source` | 频率来源：DX11 = `timestamp-disjoint`（`D3D11_QUERY_DATA_TIMESTAMP_DISJOINT::Frequency`）；DX12 = `clock-calibration`（`ID3D12CommandQueue::GetClockCalibration` 两次采样求差）；`none` = 不可用 |

**结束时的设置快照**：`final_width` `final_height` `final_window_mode`
`final_vsync` `final_fps_cap` `hidden` —— 脚本可据此确认测试期间状态改没改过。

**`backend_note`**：后端能力的如实声明。DX12 会带时间戳回读的成败计数
（`ts-read(attempt=… ok=… bad=… mapFail=… fenceWait=…)`），
半吊子状态（可用但只覆盖一部分帧）一眼能看出来。

### 4.3 标记与状态行

```json
{"type":"mark","frame":123,"name":"phase2"}
{"type":"status","frame":58,"api":"dx11","width":1280,"height":720,
 "window_mode":"windowed","vsync":false,"fps_cap":0,"fps_recent":57.03,"hidden":false}
```

`fps_recent` = 最近约 1 秒的实测帧率（真测值，不是估计）。

---

## 5. 用法示例

```bat
rem 冒烟：DX11 跑 3 秒，每帧一行 JSON
tests\sim\sim.exe --api=dx11 --seconds=3 --json

rem DX12 同样
tests\sim\sim.exe --api=dx12 --seconds=3 --json

rem 精确 60fps 锁帧、无 vsync（验证锁帧实现）
tests\sim\sim.exe --api=dx11 --seconds=5 --vsync=off --fps-cap=60 --json-every=1000

rem 无头 CI：跑 300 帧就退
tests\sim\sim.exe --api=dx12 --frames=300 --hidden --json-every=10

rem 从无边框全屏开始（铺满显示器）
tests\sim\sim.exe --api=dx11 --window-mode=borderless --seconds=5

rem 把 CPU 顶成瓶颈（每帧 12ms 逻辑开销）
tests\sim\sim.exe --api=dx11 --hold-ms=12 --seconds=5

rem 看这台机器有哪些适配器
tests\sim\sim.exe --list-outputs
```

Python 驱动（节选，见 `sim_drive.py` 完整实现）：

```python
import subprocess, threading, json

p = subprocess.Popen([r"tests\sim\sim.exe", "--api=dx11", "--json-every=4"],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=subprocess.PIPE, text=True, encoding="utf-8")

def read():
    for line in p.stdout:
        obj = json.loads(line)
        if obj["type"] == "frame":
            ...      # 逐帧真值

threading.Thread(target=read, daemon=True).start()

p.stdin.write("resize 1024 768\n"); p.stdin.flush()
p.stdin.write("vsync off\n");       p.stdin.flush()
p.stdin.write("fpscap 30\n");       p.stdin.flush()
p.stdin.write("window borderless\n"); p.stdin.flush()
p.stdin.write("stats\n");           p.stdin.flush()
p.stdin.write("quit\n");            p.stdin.flush()   # 汇总 JSON 随后输出
p.wait(timeout=20)
```

---

## 6. 已知坑 / 未实现 / 未验证

这一节是**如实记录**，不是「大概没问题」。

### 已实现并实测通过

* DX11 / DX12 双后端的帧循环、Present、交换链重建（`ResizeBuffers`）、
  分辨率热切换、窗口状态热切换、vsync 热切换、锁帧热切换
* 锁帧精度：`--fps-cap=60 --vsync=off` 实测 **59.4~60.0 fps**，
  `--frames=120` 场景实测 **60.00 fps / P50 16.665ms**（这台机器 60Hz 显示器）
* GPU 时间戳：DX11 `D3D11_QUERY_TIMESTAMP_DISJOINT` + `TIMESTAMP`
  （167/167 帧有样本）；DX12 `D3D12_QUERY_HEAP_TYPE_TIMESTAMP` +
  `ResolveQueryData` + `GetClockCalibration`（167/167 帧有样本，
  回读计数 `bad=0`）
* stdin 控制通道全部 11 条命令
* `--json` 每帧行 + 退出汇总 + `stats` 即时汇总 + `mark` 标记

### 未验证 / 做不到

* **DXGI 独占全屏（`fullscreen`）在本机做不到**。`IDXGISwapChain::SetFullscreenState(TRUE)`
  一致返回 `E_FAIL (0x80004005)`，无论从窗口化还是从无边框切、无论分辨率是否等于
  桌面分辨率（都试过）。推测与本机显卡/驱动配置有关（RTX 5080 Laptop + 混合输出）。
  程序的行为是：**回退为 borderless 并把自报数据也改成 `borderless`** ——
  绝不「切失败还自报 fullscreen」。所以 `--window-mode=fullscreen` 在这台机器上
  等价于 `borderless`；在别的机器上有可能真的能进独占全屏（代码路径是完整的）。
* **DX12 后端不做独占全屏**（`WantsExclusiveFullscreen()` 返回 false）：
  现代 DX12 游戏普遍用 borderless 模拟全屏，这里就按那条路走。
* **DX12 没有真正的几何渲染**。`--gpu-load-ms` 和画面内容都靠
  `ClearRenderTargetView`（整屏 + 矩形）。原因：DX12 的 PSO 需要**序列化好的
  DXIL**，而本项目只有 zig 自带的 MinGW 头，没有能产出 DXIL 的 dxc/d3dcompiler
  路径（zig 的 `-fno-exceptions` C++ 工具链里也没有 HLSL 编译器）。
  被测环节（Present / 交换链重建 / 资源状态转换 / 时间戳 / fence 同步）
  一个都没少。
* **DX11 的着色器路径依赖 `d3dcompiler_47.dll`**（Win10+ 系统自带，运行时
  动态加载）。取不到就退化成「整屏 Clear + 逐帧变色」，仍然每帧不同、
  仍然走完整的 Present 路径，但看不到旋转三角形。
* **`--gpu-load-ms` 是近似值**，不是标定过的 GPU 负载。按固定批量折算，
  实际占用因驱动/分辨率而异。汇总里的 `gpu_frame_ms_avg` 才是真实测量值。
* **`fps_recent`（`status` 命令里）是「最近约 1 秒」的实测帧率**，来自一个
  240 槽的环形缓冲。它是真测值，但在锁帧值刚改完的头几帧里会混着旧节奏
  （实测：`fpscap 30` 之后 1.5 秒问一次得到 29.95，对的）。
  要精确统计请用 `stats` 或帧 JSON。
* **隐藏窗口（`--hidden`）下的 frame pacing** 没有系统性对照过：
  DWM 对不可见窗口的合成行为与可见窗口不同，`present_ms` 的含义会有差异。
  做帧率结论时建议用可见窗口。
* **1% Low / 0.1% Low 的样本量下限**没有强制要求。`--seconds=1` 这种短跑里
  「最差 1%」可能只对应 1 帧，结论不稳。做低帧率结论建议至少 10 秒。

### 开发过程中真实踩到、已修掉的坑（写下来免得别人再踩）

1. **`--json --api=dx11` 参数错位**：早期解析器对「不带取值的开关」没有白名单，
   `--json` 会把 `--api=dx11` 当成自己的值吃掉。现在开关参数显式列在
   `is_flag()` 里，且**不消费**下一个 argv。
2. **`--k=v` 形式完全不生效**：分支顺序写反了（先判断「没有值」再判断 `'='`），
   导致 `--api=dx11` 永远解析不出值。现在取值来源按
   「`--k=v` → 开关 → `--vsync [on|off]` → `--k v`」的确定顺序取。
3. **stdin 命令里带 `\n`**：`SplitWs` 只把 `' '` `'\t'` `'\r'` 当空白，
   裸 LF 行尾（Python 管道常见）让 `status` 变成 `"status\n"` → 未知命令。
   现在 `'\n'` 也算空白，并且 token 会再裁一遍。
4. **`ParseBool` 恒真**：用 `sed` 把 `_stricmp(a,b)` 批量替换成 `EqNoCase(a,b)`
   时忘了去掉 `!`（`_stricmp` 相等返回 0，`EqNoCase` 相等返回 true），
   结果 `vsync off` 被解析成 `on`。现在语义写清楚了，注释里也点了这个坑。
5. **`resize_event` 永远是 false**：交换链代数快照取在控制命令**之前**，
   于是「命令引起的重建」被算进了同一帧的 before/after 两边。现在用
   「上一帧结束时的代数」比较。
6. **DPI 缩放导致全屏尺寸错乱**：不声明 DPI 感知时，`GetSystemMetrics(SM_CXSCREEN)`
   给物理像素而 `SetWindowPos` 收逻辑像素。2560x1440 + 125% 缩放的机器上
   请求「铺满屏幕」拿到的是 1707x1067 的客户区。现在启动时声明 DPI 感知，
   并且交换链尺寸一律以**量到的真实客户区**为准。
7. **DX12 `CreateCommandList(allocator=nullptr)` 在 NVIDIA 驱动上返回
   `E_INVALIDARG`**（文档说可以传 nullptr）。现在先建一个临时分配器再建列表。
8. **DX12 时间戳只有 1/3 的帧有值**：readback buffer 的 `Map` 范围写成
   `{0, 32}`，而每槽数据在不同字节偏移上，槽 1/2 读到的永远是槽 0 那份。
   现在 Map 从本槽自己的偏移开始。
9. **`zig 0.16` 的 linker 参数是逗号**：`-Wl,--subsystem,windows` 可以，
   `-Wl,--subsystem:windows` 报 `unsupported linker arg`。
10. **`cmd.exe` 会改写命令行**：同时出现重定向和 `=` 时，`--api=dx11` 传进去
    变成 `--api dx11`。验证时别用 `cmd`，用 Python `subprocess(arglist)`
    或者 `sim_drive.py`。
11. **构建脚本必须是 ASCII**：`cmd.exe` 按当前控制台代码页解析 `.bat`，
    以 UTF-8 存的中文注释会解码出杂散的命令分隔符，脚本直接崩。
    所以 `build_sim.bat` 里只有英文注释，中文文档放在这里和 C++ 源码里。
12. **`-Wl,--subsystem,windows` 的入口是 `WinMain`**：zig 自带的 mingw CRT
    （`crtexewin.c`）只定义 `WinMain`，用 `wWinMain` 会链接失败
    （`undefined symbol: WinMain`）。程序里忽略 `lpCmdLine`，
    改用 `GetCommandLineW` + `CommandLineToArgvW`（它能正确处理带空格的路径）。
