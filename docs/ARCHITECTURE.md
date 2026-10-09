# NextPerf 架构说明

## 1. 进程模型

```
┌────────────────────────────────┐        共享内存          ┌──────────────────────────────┐
│  NextPerf.exe（主程序）         │                          │  NextPerfHook.dll（游戏内）    │
│                                │                          │                              │
│  SensorHub ──► NPSensors ──────┼──► [NP_SHM_SENSORS] ────►│  读传感器                     │
│  NPConfig  ─────────────────────┼──► [NP_SHM_CONFIG]  ────►│  决定画什么、怎么画            │
│  面板 ◄── NPTelemetry ◄────────┼──◄ [遥测_<pid>，每个进程一份]┤  写帧时间/帧延迟/RT/Tensor    │
│  桌面叠加窗口 / UI / 注入器      │                          │  Present 钩子 + 游戏内叠加     │
└────────────────────────────────┘                          └──────────────────────────────┘
```

三块共享内存就是全部的进程间通信。**传感器只在主程序里采集一次**，钩子只读不采，
这样即使同时注入到多个进程，也不会出现 NVML / WMI 被重复打开、互相打架的问题。

| 名字 | 大小 | 写方 | 读方 | 内容 |
| --- | --- | --- | --- | --- |
| `Local\NextPerf_Config_v1` | `NPConfig` | 主程序 | 钩子 | 勾选了哪些计数器、颜色、缩放、位置、刷新率 |
| `Local\NextPerf_Sensors_v1` | `NPSensors` | 主程序 | 钩子 | CPU/GPU/显存/内存/NVAPI 域/硬件 RT·Tensor |
| `Local\NextPerf_Telemetry_v1_<pid>` | `NPTelemetry` | 该进程的钩子 | 主程序 | 帧时间环形缓冲、帧延迟、RT/Tensor、分辨率、AI 模块 |

> 遥测**必须按 PID 分开**（`NPTelemetryShmName()`）。共用一个名字的话，
> 同时注入两个游戏时两个钩子会往同一块内存里互相覆盖，主程序读到的是一锅粥。
> 主程序侧按自己注入过的 PID 分别打开（`AppState::telSlots`），
> 每次发布取 `tickMs` 最新的那一份当「当前游戏」。

命名用 `Local\` 前缀（会话命名空间），普通权限即可创建，不会被 UAC 完整性级别拦住。

---

## 2. 模块

### 2.1 `src/common` —— 主程序与钩子共用

| 文件 | 职责 |
| --- | --- |
| `np_common.h` | 三块共享内存的结构体定义、计数器位枚举、图形 API / AI 模块 / 数据源枚举 |
| `np_stats.h` | 帧时间环形缓冲（4096 帧 ≈ 60 秒 @60FPS）、1% / 0.1% Low、百分位、EMA、滑动最大值 |
| `np_json.h/.cpp` | 递归下降的极简 JSON，只服务配置持久化 |
| `np_panel.h/.cpp` | Direct2D + DirectWrite 绘制：圆角面板、标签 / 数值两栏、比例条、折线图 |
| `np_bitmap.h/.cpp` | 把面板渲染进一张 **BGRA DIB**（自上而下、预乘 alpha），供三种显示路径复用 |
| `np_build.h/.cpp` | 把 `NPSensors + NPTelemetry + NPConfig` 整理成一行行的 `PanelRow` |

`np_bitmap` 是关键设计：桌面叠加用 `UpdateLayeredWindow` 直接吃这张位图，
D3D11 用 `UpdateSubresource`，D3D12 用上传堆 + `CopyTextureRegion`。
**一份绘制代码，三条通路，排版绝对一致。**

### 2.2 `src/sensors` —— 传感器中枢

`SensorHub::Init()` 一次性探测所有数据源，`Poll()` 每轮填一个 `NPSensors`：

```
NVML ─┐
NVAPI ├─► NVIDIA 分支（ nvml_ / nvapi_ / adl_ 三个上下文对象）
ADL  ─┘
HWiNFO 共享内存 ──► 温度 / 功耗 / 风扇 / 热点
PDH ─────────────► 通用 GPU 引擎占用、显存占用、CPU 性能百分比
WMI ─────────────► ACPI 热区（CPU 温度兜底）、LibreHardwareMonitor
GetSystemTimes ──► CPU 占用率（差分）
GlobalMemoryStatusEx ──► 内存
DXGI ────────────► 显卡型号 / 厂商 / 显存总量（跨厂商通用）
```

**所有厂商库都是 `LoadLibrary` + `GetProcAddress` 动态加载**，
没有 NVIDIA / AMD 驱动也不会报错——只是对应数据源显示"未检测到"。

NVAPI 的函数不是直接导出的，要通过 `nvapi_QueryInterface(interfaceId)` 换函数指针。
用到的 interfaceId 是 nvapi.h 里公开且长期稳定的那几个（Initialize / EnumPhysicalGPUs /
GetFullName / GetThermalSettings / GetDynamicPstatesInfoEx / GetUsages / GetTachReading）。

### 2.3 `src/hook` —— 注入到游戏进程的部分

`DllMain` 里只做一件事：开一个工作线程 `Worker`，剩下的活都在线程里干，
避免违反 Windows 的 loader lock 规则。

`Worker` 的顺序：

1. `QueryPerformanceFrequency` + `OpenIpc()` —— 打开 Config / Sensors 共享内存，
   按**自己的 PID** 建遥测块（`Local\NextPerf_Telemetry_v1_<pid>`）
2. `DetectAi()` —— 扫进程里加载的 AI 模块（DLSS / FSR / XeSS / DirectML…）
3. `InstallDxgi()` —— 只挂 DXGI 工厂的 `CreateSwapChain*`（**不碰设备**）
4. 建一个命名互斥量 `Local\NextPerf_Injected_<pid>` —— 主程序靠它判断是否已注入
5. 循环（200ms 一跳）：轮询 `quit` 标志、重扫 AI 模块、按需触发交换链探测

图形资源全部**惰性**创建，而且必须在游戏的渲染线程上：
D2D / DirectWrite 工厂、面板渲染器、查询堆、叠加资源都在第一次 `Present` 里
（`EnsureRt()` / `EnsureD3D12Hooks()`）才初始化 —— 单线程 D2D 工厂跨线程用会崩，
多线程渲染的游戏尤其明显。

#### Present 是怎么挂上的（两条路，缺一不可）

| 场景 | 走哪条路 |
| --- | --- |
| 进程刚起来就注入（图形还没初始化） | 工厂钩子：游戏自己调 `CreateSwapChain*` 时补 `Present` 补丁 |
| **游戏已经在跑了**（最常见） | 自己造一条 8×8 的隐藏交换链，拿到 vtable 直接补 |

第二条路是必须的：交换链在游戏启动时就建好了，工厂钩子**永远不会再被调用**。
但同一个交换链类的 vtable 在 `dxgi.dll` 里是**全进程共享**的，
所以只要拿到任意一个同类实例，改它的 vtable 就等于改掉游戏那条已经存在的交换链。
探测时机由 `HostGraphicsUp()` 决定（读自己的 PE 导入表，排除我们带进来的
dxgi/d3d11/d3d12）：宿主已经加载图形栈 → 注入后 800ms 就探；还没加载 →
先让工厂钩子干，6 秒后再兜底。配置项 `vtableProbe` 可以整体关掉。

原始函数**按 vtable 分别保存**（`gSwapVts[]` 小表）：blt / flip 可能是不同的类，
原始 `Present` 就不是同一个函数，全局只存一个会串线。
补丁前会校验槽位确实在 `dxgi.dll` 里且互不相同（`GetBuffer` / `GetDesc` 没人去钩，
用来确认这确实是一张交换链表）。

#### vtable 下标

**不要凭记忆改这些数字** —— 用 `python tests/vt_check.py` 从 mingw 头文件里
把真实下标算出来，它会和 `src/hook/np_hook.cpp` 里的 `Vt::` 枚举逐条对比：

```
IDXGISwapChain       : Present = 8,  GetBuffer = 9,  GetFullscreenState = 11,  GetDesc = 12
IDXGISwapChain1      : GetHwnd = 20,  Present1 = 22
IDXGISwapChain3      : GetCurrentBackBufferIndex = 36
IDXGIFactory         : CreateSwapChain = 10
IDXGIFactory2        : CreateSwapChainForHwnd = 15,  CreateSwapChainForComposition = 24
ID3D12CommandQueue   : ExecuteCommandLists = 10,  Signal = 14,
                       GetTimestampFrequency = 16,  GetDesc = 19
ID3D12GraphicsCommandList4 :
    DrawInstanced = 12,  DrawIndexedInstanced = 13,  Dispatch = 14,
    RSSetViewports = 21,  EndQuery = 53,  ResolveQueryData = 54,
    BuildRaytracingAccelerationStructure = 72,  DispatchRays = 76
```

> 这里错过两次：`CreateSwapChainForHwnd` 写成 13（真实是 `IsCurrent`），
> `CreateSwapChainForComposition` 写成 22（真实是 `RegisterOcclusionStatusEvent`）。
> 症状是工厂钩子挂在无关方法上——真正的 `CreateSwapChainForHwnd` 从没被拦截，
> 而且游戏每次调 `IsCurrent()` 都会进我们的钩子。所以这个检查脚本要常跑。

安装命令列表级钩子前有三重校验：驱动确实 `QueryInterface` 出 `ID3D12GraphicsCommandList4`、
目标槽位指针落在 `d3d12.dll` 的模块范围内、且几个槽位互不相同。任一不满足就整组跳过。

#### 帧计时

```
                 GPU 时间线（同一个命令队列，天然有序）
   ──[TS_START]──[ 游戏本帧的所有命令 ]──[TS_END]──[ResolveQueryData]──►
        ▲                                    ▲
   第一次 ExecuteCommandLists 前           Present 之前
   （用我们自己的命令列表插进去）       （同一个列表里紧接着做 resolve）
```

* **帧生成时间** = 相邻两次 `Present` 的间隔
* **CPU 帧延迟** = 本帧第一次 `ExecuteCommandLists` → `Present` 的 CPU 墙钟时间
  （D3D11 没有这个锚点，退化为帧间隔）
* **GPU 帧延迟** = `TS_END - TS_START`，除以命令队列的 `GetTimestampFrequency()`

resolve 出来的结果不马上读（会卡 GPU）。写进 4 个轮转的 readback 缓冲，
记下当时的 fence 值，等 `GetCompletedValue()` 追上了再 `Map` 出来算。

D3D11 用另一套：`TIMESTAMP_DISJOINT` + 两个 `TIMESTAMP` 查询，3 组轮转，
`GetData(..., 0)` 非阻塞轮询，`Disjoint` 标志为真时丢弃该次采样。

> ⚠ `NpECL` 里有一道**重入保护**（`thread_local bool busy`）不能删：
> `EndList()` 内部会再调一次 `q->ExecuteCommandLists`，也就是再进一次 `NpECL`。
> 如果 `gFrameStarted` 不是在这之前置位，就会无限递归，最后爆栈在 `nvwgf2umx.dll`
> 里 —— 现象像「NVIDIA 驱动崩了」，实际是自己递归。爆栈之后驱动状态被带坏，
> 后续 `CreateGraphicsPipelineState` 之类全部失败，游戏内面板就再也画不出来。

#### 异常处理

钩子跑在别人的进程里，**任何一步出错都必须自己扛住**。
`PresentCommon` / `NpECL` 用「向量化异常处理 + `setjmp/longjmp`」包住整个处理流程
（clang 在 mingw 目标下不支持 MSVC 的 `__try/__except`，所以用 VEH 自己实现）；
出异常时直接 longjmp 出来、照常调用原始的 `Present`，游戏完全无感。

两个坑：

* **`jmp_buf` 必须 `thread_local`**。Present / 命令列表钩子跑在游戏的渲染线程上，
  注入探测跑在工作线程上，共用一个 buffer 的话一边出错会 longjmp 到另一边的栈上。
* **必须筛异常码**。VEH 拿到的是*所有*异常，包括 `0x40010006`
  `DBG_PRINTEXCEPTION_C` —— `OutputDebugString` 就发这个，D3D11/DXGI 初始化时很常见。
  不筛的话探测第一次就被自己中断掉。现在只认 `ACCESS_VIOLATION` 那一类真崩溃，
  C++ 异常（`0xE06D7363`）之类一律放行。

### 2.4 `src/app` —— 主程序

| 文件 | 职责 |
| --- | --- |
| `main.cpp` | `WinMain`、共享内存创建、主循环（120ms 一跳）、系统托盘、自检 / UI 冒烟 |
| `ui.cpp` | 自绘深色窗口：计数器勾选列表（可滚动）、控件（按钮 / 滑杆 / 循环选择）、实时预览 |
| `overlay.cpp` | `WS_EX_LAYERED` 分层窗口，`UpdateLayeredWindow` 输出；定位到前台窗口所在显示器的右上角 |
| `injector.cpp` | 进程看护线程（800ms 一轮）+ `CreateRemoteThread(LoadLibraryW)` 注入 |
| `settings.cpp` | `%APPDATA%\NextPerf\config.json` |

**叠加模式的自动切换逻辑**：

```cpp
bool attached = telemetry.attached && (now - telemetry.tickMs) < 2500;
bool wantDesktop = monitoring && (overlayMode == 2 || (!attached && overlayMode != 1));
```

也就是：钩子在线 → 交给游戏内叠加画；钩子不在线（比如 Vulkan 游戏）→ 退回桌面叠加。

---

## 3. 关键设计取舍

**为什么不用 ETW（PresentMon 那套）？**
ETW 能跨 API 拿到帧事件，但需要 "Performance Log Users" 权限或管理员，
而且拿不到 RT / Tensor 这种引擎内部信息。注入虽然侵入性强一点，但唯一能同时拿到
帧延迟和引擎 pass 级 GPU 时间的方案就是它。

**为什么 D3D12 叠加走"CPU 位图上传"而不是 D3D11-on-12 互操作？**
`D3D11On12CreateDevice` + 共享句柄 + 跨 API fence 同步，代码量大概是现在的三倍，
而且要处理游戏设备与我们的 D3D11 设备的资源状态同步。
面板只有 ~300×250，每帧上传 300KB，按默认 20Hz 刷新也只有 6MB/s，完全可以接受，
换来的是一条和 D3D11 / 桌面叠加完全一致的通路。

**为什么传感器只在主程序采集？**
NVML / WMI 初始化有开销，而且 NVAPI 这类库在同一进程里重复初始化容易出问题。
集中到一个进程，多开游戏也不会互相干扰。

**为什么不用 MinHook / Detours？**
这次只劫持 COM vtable（改函数指针）就够了，不需要 inline hook 和 trampoline，
少一个依赖、少一层风险。
