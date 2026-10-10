# NextPerf 钩子层（Hook Layer）技术文档

> 适用范围：`src/hook/` 整个目录（注入到游戏进程里的那部分）。
> 配套文档：[`HOOK-FUNCTIONS.md`](HOOK-FUNCTIONS.md)（函数/接口速查表）、[`CATALOG.md`](CATALOG.md)（全项目文件编目）。
>
> **维护约定（硬性）**：任何一次改 `src/hook/` 下的代码，**必须**同步更新本文档与 `docs/CATALOG.md`。
> 行号、函数名、口径描述一旦和代码不一致，这份文档就会变成负资产 —— 它存在的意义就是让后来者不用重新啃 2600 行注释。

---

## 0. 一句话总览

钩子层是一个**注入到游戏进程里的 DLL**：它劫持 DXGI / D3D11 / D3D12 的 vtable，
在每次 `Present` 前后采集帧时间与引擎数据，往后台缓冲上贴一张自己画的性能面板，
再把结果通过共享内存交回主程序。
**它的一切设计都服从一条最高原则：绝不能把宿主（游戏）搞崩。**

---

## 1. 这一层是干什么的

### 1.1 职责

| 做 | 不做 |
| --- | --- |
| 挂钩 DXGI 工厂 / 交换链 vtable，接管 `Present` / `Present1` | 不采集硬件传感器（CPU 温度、功耗、显存…由主程序做） |
| 挂钩 D3D12 命令队列 / 命令列表 vtable，统计 draw/dispatch/RT/AI | 不做桌面上那个 HUD（那是 `src/app/overlay.cpp`） |
| 用 D2D 把面板画成一张 BGRA 位图，再贴到游戏后台缓冲 | 不做 ETW 外置帧计时（见 §20） |
| 通过共享内存读配置/传感器、写遥测 | 不打开 NVML / WMI / PDH（主程序统一采集，见 `np_common.h:4-10`） |
| 挂钩失败/出错时**安静降级**，并写日志说明原因 | 不阻塞、不拖慢游戏线程（拿不到锁就放弃这一帧） |

### 1.2 为什么注入，而不是外置

外置方案（ETW / 桌面叠加）拿不到「引擎在干什么」：draw call 数、光追 dispatch、
AI 超分模块是否加载、渲染分辨率与输出分辨率的缩放比 —— 这些只有站在进程内部才能观测。
代价是**宿主崩溃风险**，所以本层的代码风格到处是「宁可丢数据，也不能动游戏的命令流」
（`np_hook.cpp:294-297`、`2211-2224`）。

### 1.3 文件构成

| 文件 | 行数 | 内容 |
| --- | --- | --- |
| `src/hook/dllmain.cpp` | 26 | `DllMain`：`DLL_PROCESS_ATTACH` → `NpHookAttach`；`DLL_PROCESS_DETACH` → `NpHookDetach`（`dllmain.cpp:13-26`） |
| `src/hook/np_hook.h` | 7 | 只有两个导出：`NpHookAttach(HMODULE)` / `NpHookDetach()` |
| `src/hook/np_hook.cpp` | 2684 | 全部逻辑：vtable 下标表、SEH、IPC、时间戳、Present 主流程、绘制编排、钩子安装与卸载 |
| `src/hook/np_draw.h` / `np_draw.cpp` | 104 / 610 | 叠加层绘制：`Overlay11`（D3D11）与 `Overlay12`（D3D12）两套，外加 D3DCompile 运行时编译 |
| `src/hook/np_reflex.h` | 128 | NVAPI Reflex 读取（`NvAPI_D3D_GetLatency` / `GetSleepStatus`），header-only |
| `src/common/np_common.h` | 497 | 共享内存三块结构：`NPConfig` / `NPSensors` / `NPTelemetry`，以及各种位定义 |
| `src/common/np_stats.h` | 269 | `np::FrameStats`（环形缓冲 + 各种 Low 帧口径）、`np::Ema` |
| `src/common/np_bitmap.*` | — | 面板位图：D2D 把面板画成 BGRA DIB，供三个消费方复用（见 §13.1） |

### 1.4 三个共享内存块（进程间通信的全部）

定义在 `np_common.h:22-24`、`np_common.h:30-45`：

| 块 | 名字 | 方向 | 钩子侧访问点 |
| --- | --- | --- | --- |
| Config | `Local\NextPerf_Config_v1` | 主程序写 / 钩子读 | `OpenIpc()` `np_hook.cpp:578-594`，读取统一走 `Cfg()` `596-601` |
| Sensors | `Local\NextPerf_Sensors_v1` | 主程序写 / 钩子读 | 同上，读取统一走 `Sens()` `602-607` |
| Telemetry | `Local\NextPerf_Telemetry_v1_<pid>` | 钩子写 / 主程序读 | `CreateFileMappingW`，每次注入新进程就 `NPClearTelemetry` |

要点：

- **遥测按 PID 分块**（`np_hook.cpp:583-587`）：同时注入两个游戏时，共用一个名字会互相覆盖。
- `Cfg()` / `Sens()` 返回**值拷贝**，并且先 `NPDefaultConfig` / `NPClearSensors` 再覆盖 ——
  这样主程序还没起来 / magic 不对时，调用方永远拿到一份安全的默认值，不会读到野数据（`596-607`）。
- `NPTelemetry::version` 写的是 `sizeof(NPTelemetry)` 而不是版本号，
  读方对一下大小就能立刻发现结构体布局错位（`np_common.h:305-310`；`NPSensors` 同理，见 `441-448`）。

---

## 2. 关键全局状态一览（改代码前必读）

`np_hook.cpp:219-403` 是全部状态。挑最要紧的：

| 变量 | 行 | 含义 / 陷阱 |
| --- | --- | --- |
| `gUnloading` | 281 | 主程序退出时置位。**所有跳板函数第一件事就是判它**，置位后一律直通原函数 |
| `gInPresent` | 229 | 防重入。**尾部必须复位**，中间提前 return 会让叠加永久失效（见 §19 坑 1） |
| `gSwapVts[]` / `gSwapVtCount` | 245-252 | 被补丁过的交换链 vtable 小表（blt / flip / composition 可能是不同的类） |
| `gAbsorbThisPresent` | 315 | 本次 Present 是否被判定为「同一帧内的重复呈现」 |
| `gLastPresentQpc` | 311 | 上一次**记账**的时刻（被合并时**不推进** —— 这正是合并的机制） |
| `gPrevPresentQpc` | 335 | 上一次**呈现**的时刻（**无论是否合并都推进**）—— 原始间隔序列的唯一来源 |
| `gRawStats` / `gStats` | 324 / 350 | 原始序列（只用来求合并阈值）/ 合并后序列（面板所有数字） |
| `gD12Lock` + `thread_local gD12Locked` | 291-292 | 保护全局唯一的 `gList`；`try_lock` 失败就放弃这一帧 |
| `gD12Broken` | 297 | D3D12 侧出过不可恢复的错 → 永久停手（帧时间照常工作） |
| `gProbePoisoned` | 1790 | 探测撞过 SEH → **永久**不再探测 |
| `gFrameStarted` | 349 | 必须是 `atomic`：多线程提交 + Present 线程清零 |
| `gDraws/gDispatches/gRtDispatches/gAsBuilds` | 356 | 必须是 `atomic`：多提交线程并发 `fetch_add` |
| `gNpCountingOverlayDraws` | 911 | 必须是 `thread_local`：只抑制我们自己那一路的 draw 计数 |
| `gQueue12` | 237 | **AddRef 后持有**：引擎重建队列后裸指针会指向已销毁对象（`2166-2173`） |

---

## 3. 注入入口与初始化（attach 流程）

### 3.1 功能说明

`LoadLibrary` 之后在**独立 worker 线程**里完成共享内存打开、工厂钩子安装、
vtable 探测与周期性维护；`DllMain` 本身几乎不做事。

### 3.2 涉及文件与函数

- `dllmain.cpp:13-26` `DllMain` —— `DisableThreadLibraryCalls` 后调 `NpHookAttach`
- `np_hook.cpp:2657-2661` `NpHookAttach` —— 存 `gSelf`、初始化 `gCs`、`CreateThread(Worker)`
- `np_hook.cpp:2556-2655` `Worker` —— 真正的 attach 流程 + 常驻循环
- `np_hook.cpp:578-594` `OpenIpc`
- `np_hook.cpp:612-630` `DetectAi`
- `np_hook.cpp:1928-1970` `InstallDxgi`

### 3.3 实现逻辑

1. **`DllMain` 里绝不做图形初始化**：只 `DisableThreadLibraryCalls` + 起线程（`dllmain.cpp:13-26`）。
   `DllMain` 里受 loader lock 约束，任何加载/等待都可能死锁。
2. **worker 线程**（`2556` 起）：
   - `QueryPerformanceFrequency` 填 `gQpcFreq`（所有时间换算的分母）；
   - `OpenIpc()`；失败就**直接返回**（没有共享内存就没有意义，`2562`）；
   - `DetectAi()`（`612`）：按已加载模块识别 DLSS/XeSS/FSR/DirectML/ONNX 位；
   - `InstallDxgi()`：先只挂**工厂**钩子，不碰任何设备（注释 `1695-1704` 说明了为什么）；
   - 建 `Local\NextPerf_Injected_<pid>` 互斥量并**一直持有**到进程结束，
     主程序靠它判断「这个进程已经注入过」，避免重复注入（`2567-2573`）；
   - 判断宿主图形状态，算出探测时刻与次数上限（`2583-2589`）；
   - 置 `gReady = true`，进入 200ms 心跳循环（`2599-2653`）。
3. **心跳循环**每轮做四件事：
   - 读 `gCfg->quit` → `SelfUnloadNow()`（主程序退出）；
   - 读 `gCfg->detachPid` → `SelfUnloadNow()`（主程序要求卸载，见 §17）；
   - `DetectAi()` 重跑（游戏可能刚加载 DLSS）；
   - 若 `PresentHooked()` 为假则按策略探测；无论挂没挂上都刷新 `hookFlags` 与最小遥测。
4. **图形初始化必须发生在渲染线程上**：`EnsureRt()`（`1124-1135`）由 `PresentCommon` 调用，
   注释写明「单线程 D2D 工厂不能跨线程使用，多线程渲染的游戏会因此闪退。Worker 线程不做任何图形初始化」
   （`1121-1123`）。里面必须把 `npg::GfxInit()`（D3DCompile）和 `npb::GfxInit()`（D2D）**都**调一遍，
   少了后者就是「注入成功、数据也有，但游戏里看不到面板」（`1128-1131`）。

### 3.4 关键状态

`gSelf`、`gReady`、`gWorker`、`gInjectedMutex`、`gHostApi`、`gQpcFreq`、`gRtReady`。

---

## 4. vtable 补丁原语 `Patch()`

- **说明**：把 `vt[idx]` 换成 `hook`，顺便把旧值回读给 `orig`。
- **位置**：`np_hook.cpp:418-428`。
- **逻辑**：`VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE)` → 写入 → 恢复原保护属性。
  `orig` 可以为 `nullptr`（还原时不需要回读旧值）。
- **注意**：任何一处失败都返回 `false`，调用方**不得**假定补丁成功；
  所有安装点都检查返回值，`RestoreAllHooks` 里也一律重新 `Patch` 回原值而不是自己写内存。

### 4.1 vtable 下标表 `namespace Vt`

`np_hook.cpp:25-70`。**这些数字是全文档最容易出人命的地方**：

- `SwapChain::Present = 8`、`GetBuffer = 9`、`GetFullscreenState = 11`、`GetDesc = 12`、
  `GetHwnd = 20`、`Present1 = 22`、`GetCurrentBackBufferIndex = 36`
- `Factory::CreateSwapChain = 10`；`Factory2::CreateSwapChainForHwnd = 15`、`CreateSwapChainForComposition = 24`
- `CommandQueue::ExecuteCommandLists = 10`、`Signal = 14`、`GetTimestampFrequency = 16`、`QueueGetDesc = 19`
- `CommandList::Close = 9`、`Reset = 10`、`DrawInstanced = 12`、`DrawIndexedInstanced = 13`、
  `Dispatch = 14`、`RSSetViewports = 21`、`EndQuery = 53`、`ResolveQueryData = 54`、
  `BuildRaytracingAccelerationStructure = 72`、`DispatchRays = 76`

源码注释（`26-32`）明确要求：**别凭记忆数**，用 `python tests/vt_check.py` 从 mingw 头文件重新算。
历史上错过两次：`CreateSwapChainForHwnd` 写成 13（其实是 `IsCurrent`）、
`CreateSwapChainForComposition` 写成 22（其实是 `RegisterOcclusionStatusEvent`）—— 改错的后果是游戏直接崩。

---

## 5. 异常保护（VEH + setjmp）

### 5.1 功能说明

钩子跑在别人的进程里，探测路径**本来就会触发异常**。这一套机制的作用是：
把「我们自己的探测/插桩」炸出来的致命异常兜住，转成一次优雅放弃，而不是带走游戏。

### 5.2 涉及函数

- `np_hook.cpp:141-143` `thread_local std::jmp_buf gJmp` / `gJmpArmed`、`std::atomic<bool> gSehReady`
- `np_hook.cpp:152-168` `IsFatalCode(DWORD)`
- `np_hook.cpp:170-194` `NpSeh` —— VEH 回调
- `np_hook.cpp:196` `gSehHandle`
- `np_hook.cpp:209-217` `SehReady()` —— 惰性注册
- `np_hook.cpp:2404-2415` `RemoveVectoredExceptionHandler`（在 `RestoreAllHooks` 里）

### 5.3 实现逻辑

1. **每线程一份 `jmp_buf`**（`137-142`）：Present/命令列表钩子在游戏渲染线程上，
   探测在 worker 线程上。共用一份的话，一边出错会 `longjmp` 到另一边的栈上 —— 直接闪退。
2. **必须筛异常码**（`146-168`）：`AddVectoredExceptionHandler` 拿到的是**所有**异常，
   包括 `0x40010006 DBG_PRINTEXCEPTION_C`（`OutputDebugString` 就会发，D3D 初始化时很常见）
   和 C++ 异常 `0xE06D7363`。原来不筛就把整个探测 `longjmp` 掉了；吞掉 C++ 异常还会破坏宿主逻辑。
   白名单：`ACCESS_VIOLATION`、`IN_PAGE_ERROR`、`ILLEGAL_INSTRUCTION`、`PRIV_INSTRUCTION`、
   `INT_DIVIDE_BY_ZERO`、`INT_OVERFLOW`、`STACK_OVERFLOW`、`ARRAY_BOUNDS_EXCEEDED`、
   `FLT_DIVIDE_BY_ZERO`、`FLT_INVALID_OPERATION`。
3. **`NpSeh` 里报出出错地址属于哪个模块 + 偏移**（`175-191`）：
   没有这一条，在别人进程里出异常只能瞎猜。实测靠它定位到 `nvwgf2umx.dll+0x330124`（NVIDIA 用户态驱动）。
4. **`SehReady()` 惰性注册，句柄必须存下来**（`198-217`）：注册在进程级链表上，
   注销见 §17 —— 这是「重复注入 100% 闪退」的根因。
5. **`longjmp` 之后不要再碰 setjmp 之后声明的对象**（`1812-1813` 注释）：值不确定，
   所以探测路径宁可泄漏这一次的 COM 引用，也不去 `Release`。

### 5.4 使用点

`PresentCommon`（`1161-1167`）、`ProbeSwapChainVtable`（`1808-1822`）、
`NpDispatchRays`（`2250-2275`）、`NpDispatch`（`2301-2312`、`2330-2339`）。

---

## 6. 诊断日志 `Log()`

- **位置**：`np_hook.cpp:520-573`。输出到 `%TEMP%\NextPerfHook.log`。
- **文件是轮转的，而且方向反直觉**（`529-535`）：超过 512KB 就 `SetFilePointer(FILE_BEGIN)` 覆盖重写 ——
  写满之后**最新的行在文件开头，结尾反而是旧内容**。排查时请 Ctrl+F 搜关键字或看文件开头，
  **不要只看末尾**（源码注释记录了「因为只看末尾而误判日志里没有这一行」）。
- **前缀是墙钟时间 + 进程名 + pid + tid**（`538-561`）：原来只有「开机后毫秒数」，
  排查时对不上用户说的时间点；而且这个文件是**所有被注入进程共用**的，只有 pid 不知道是哪个游戏。
  这两条都是实际排查中被卡住过的地方。
- 进程名只取一次（`static char sExe[64]`，`547-557`）。
- 单行上限 1024 字节（`537`），超长截断。
- **编码**：`Log()` 本身不做任何转码，写的是格式化后的原始字节；进程名走 `WideCharToMultiByte(CP_UTF8)`
  （`554`）。日志整体编码因此取决于**源文件的编译编码**（格式串里的中文），
  `docs/LOGGING.md` 记录为 **GBK** —— 这一条与代码里的 `CP_UTF8` 并存，
  **未确认**实测下中文是否始终可读；改动 `Log()` 的格式化方式前请先看 `docs/LOGGING.md` 第 2 节的格式约定。

---

## 7. 图形 API 识别

| 函数 | 行 | 作用 |
| --- | --- | --- |
| `SelfImports(const char*)` | 449-478 | 读**自己的 PE 导入表**，判断某个模块是不是我们 DLL 静态带进来的 |
| `HostGraphicsUp()` | 481-489 | 宿主（游戏）自己是否已经把图形栈拉起来 |
| `HostApiGuess()` | 497-509 | 宿主用的是哪套 API |
| `HostHasVulkan()` | 512-514 | 宿主自己是否加载了 `vulkan-1.dll` |
| `GuessApiFromModules()` | 2506-2513 | 挂不上 Present 时的兜底猜测（含 Vulkan/OpenGL） |
| `DetectAi()` | 612-630 | 识别 DLSS/XeSS/FSR/DirectML/ORT 模块 |

两条踩过的教训（都写在注释里，必须保留）：

1. **不能直接用 `GetModuleHandleW(L"d3d12.dll") != nullptr`**（`442-448`）：
   本 DLL 为了拿接口与 `D3D11CreateDevice` / `D3D12CreateDevice`，本身就静态链接了 dxgi/d3d11/d3d12，
   那三个模块从 `LoadLibrary` 那一刻起就「已加载」—— 判断永远为真，
   于是所有「让宿主的工厂钩子先跑」的保护统统失效。
2. **不能拿 `vulkan-1.dll` 当「这是 Vulkan 游戏」的证据**（`491-496`）：
   实测 D3D12 与 Vulkan 游戏**都**加载了它。上一版把 vulkan 排在最前，
   于是 D3D12 游戏被判成 Vulkan → 探测被永久禁用 → 中途注入再也挂不上 Present。
   判据按可靠性排序：`d3d12 > d3d11 > d3d10 > d3d9 > vulkan > opengl`。

---

## 8. 交换链 vtable 探测 `ProbeSwapChainVtable()`

### 8.1 功能说明

注入时游戏往往**已经在渲染**，`CreateSwapChain*` 永远不会再被调用 ——
这时自己造一条最小交换链把 vtable 拿到手，打完补丁立刻销毁（`1785-1791`）。

### 8.2 涉及函数与关键变量

- `np_hook.cpp:1792-1911` `ProbeSwapChainVtable`
- `np_hook.cpp:1790` `static bool gProbePoisoned`
- `np_hook.cpp:175-194` `PatchSwapChainVtable`（探测成功后由它真正打补丁）
- 调用点：`Worker` `2628-2640`

### 8.3 实现逻辑与分支

1. `PresentHooked()` 已成立 → 直接返回（工厂钩子已经抓住游戏自己的交换链）。
2. **`gProbePoisoned` 置位就永久停手**（`1794-1806`）。理由（实测证据，必须照抄）：
   ```
   SEH: code=0xc0000005 at nvwgf2umx.dll+0x330124   <- NVIDIA 用户态驱动
   SEH: code=0xc0000005 at ?+00007FFCBB59A040        <- 非模块内存（同为驱动分配区）
   ```
   两次模块内偏移**完全相同** ⇒ 驱动里一个确定的代码位置。
   在一个已经跑起来（很可能已独占全屏）的游戏里创建交换链会打到这里。
   重试上限 3 次 = **三次把游戏打崩的机会**，绝不能留。
   （「先启动 NextPerf」不会触发：工厂钩子已经抓住交换链，`PresentHooked()` 成立，探测根本不跑。）
3. 没有 `dxgi.dll` → 返回（`1807`）；没有 SEH → 返回（`1808`）。
4. `setjmp` 保护整段（`1814-1822`）；撞 SEH 就 `gProbePoisoned = true` 并放弃。
5. `D3D11CreateDevice` 先试 HARDWARE，失败退 **WARP**（虚拟机上也有软件光栅化，`1828-1833`）。
6. **8x8 隐藏窗口**（`1835-1868`）：
   - 窗口类**必须注册到宿主 exe 模块**（`wc.hInstance = hostExe`，`1860-1866`），
     不能用 `gSelf` —— 窗口类是**按进程**注册的，绑本 DLL 的话 DLL 一卸载这个类就成了指向
     **已卸载模块**的野类，而且会**永久留在游戏进程里**（自卸载里没有也不该有 `UnregisterClass`）。
     于是「上次注入过、这次再注入」时 `CreateWindowExW` 就会碰到它。
     宿主 exe 永远不会卸载，用它的模块句柄就没这个问题，顺带也消掉了类名残留在用户进程里的全局副作用。
   - 窗口**必须即时创建**：下面的工厂创建被 `hwnd` 门控着，
     曾经改成延迟创建 → `hwnd` 为 null → `f2` 永远为 null → 探测什么都不做，
     实测表现是 `probe attempt 1..3` 全部 `no vtable obtained`（`1851-1854`）。
7. `CreateDXGIFactory1` → `QI IDXGIFactory2`，然后**两种 swap effect 各探一遍**：
   `DXGI_SWAP_EFFECT_DISCARD`（BufferCount=1）与 `DXGI_SWAP_EFFECT_FLIP_DISCARD`（BufferCount=2），
   因为 blt 和 flip 可能是**不同的类**（vtable 不同，`1876-1897`）。
8. 每次创建成功就 `PatchSwapChainVtable(sc)`，然后立刻 `sc->Release()`。
   补丁作用在**进程共享的 vtable** 上，所以游戏那条**已经存在的**交换链马上就被接管了（`1791`）。
9. 收尾 `DestroyWindow` / `Release`，再判一次 `PresentHooked()`。

### 8.4 关键状态机

```
PresentHooked() ──true────> 永不探测
      │false
      ▼
gProbePoisoned? ──true────> 永久放弃
      │false
      ▼
  探测一次 ──SEH──> gProbePoisoned = true（永久）
      │成功
      ▼
PatchSwapChainVtable → gSwapVtCount>0 → PresentHooked()=true → 后续探测永不进入
```

---

## 9. DXGI 工厂钩子 `InstallDxgi()`

### 9.1 功能说明

挂钩 `IDXGIFactory::CreateSwapChain`、`IDXGIFactory2::CreateSwapChainForHwnd`、
`CreateSwapChainForComposition`，在游戏**自己创建交换链**的那一刻补上 Present 补丁。
这是兼容性最好的路径（`1695-1704`：DX12 游戏「注入即闪退」的教训换来的设计）。

### 9.2 涉及函数

- `np_hook.cpp:1928-1970` `InstallDxgi`
- `np_hook.cpp:2129-2135` `NpCreateSwapChain`
- `np_hook.cpp:2137-2146` `NpCreateSwapChainHwnd`
- `np_hook.cpp:2148-2155` `NpCreateSwapChainComp`
- `np_hook.cpp:1917-1926` `PointsIntoOurDll`（自递归防线）

### 9.3 实现逻辑

1. `CreateDXGIFactory1` 拿一个工厂实例 —— **只为读 vtable，不创建设备**（`1929-1933`）。
2. `QI IDXGIFactory` → 读 `gFactoryVt` → `Patch(CreateSwapChain)` → `Release` 实例（`1934-1944`）。
3. `QI IDXGIFactory2` → `gFactory2Vt` → 挂 `CreateSwapChainForHwnd`（下标 15）与
   `CreateSwapChainForComposition`（下标 24）（`1946-1965`）。
4. **自递归防线**（`1952-1958`）：若读回来的「原函数」落在**我们自己 DLL 的 PE 映像范围**内
   （`PointsIntoOurDll` 用 PE 头算范围，不依赖 psapi），说明上一次卸载没还干净，
   这个槽位其实是上一份 DLL 的钩子 —— 拿它当原函数用就会调用自己 → 无限递归 → 栈溢出。
   **宁可这个钩子不装**。
5. 三个跳板的共同结构：先 `Log` 一行（便于确认工厂路径到底有没有被走到），
   调用原函数，**成功且 `!gUnloading` 才** `PatchSwapChainVtable(*sc)`。

**为什么三个工厂钩子里都必须先判 `gUnloading`**（`2124-2128`）：
自卸载进行时 vtable 正在被还原，如果这期间游戏正好又建了一条交换链，
我们就会往它（进程共享的）vtable 里重新写进 `NpPresent` —— 而本 DLL 马上就要 `FreeLibrary`，
那个槽位就成了指向已卸载内存的野指针，宿主下一次 Present 必定崩。

---

## 10. 交换链 vtable 钩子（Present / Present1）

### 10.1 功能说明

把 `Present` / `Present1` 换成本进程的 `NpPresent` / `NpPresent1`，
并把「这条 vtable 的原函数」记进 `gSwapVts` 小表。

### 10.2 涉及函数与关键变量

- `np_hook.cpp:1745-1783` `PatchSwapChainVtable`
- `np_hook.cpp:254-259` `SwapVtFor` / `PresentHooked`
- `np_hook.cpp:1708-1716` `PlausibleCodePtr`
- `np_hook.cpp:1726-1742` `PtrOwner`（诊断）
- `np_hook.cpp:2114-2121` `NpPresent` / `NpPresent1`
- 关键变量：`gSwapVts[NP_MAX_SWAPVT=4]`、`gSwapVtCount`、`gFactoryVt`/`gOrigCreateSC` 等

### 10.3 实现逻辑

1. `SwapVtFor(vt)` 命中 → 这张表补过了，返回 `true`（`1749`）；表满（`>=4`）→ `false`（`1750`）。
2. **健全性检查只要求「是可执行代码 + 槽位互不相同」**（`1706-1716`、`1752-1764`）。
   注释解释了为什么放宽：Present 有可能已经被别的叠加层（Steam / Discord / RTSS / MSI Afterburner）
   换成它们的跳板 —— 那也能正常串接，所以不能因为「不在 dxgi 里」就拒掉。
   `PlausibleCodePtr` 检查 `MEM_COMMIT` + `PAGE_EXECUTE*`，并排除「等于同表里别的槽位」。
3. 打 `Present`（下标 8）；`Present1`（下标 22）**不一定存在**，失败也无所谓（`1772-1777`）。
4. 记进 `gSwapVts[gSwapVtCount++]`，置 `gTel->hookFlags |= NP_HOOK_PRESENT`。

### 10.4 为什么是一个「小表」而不是单个全局

`dxgi` 里**同一个交换链类的 vtable 是全进程共享**的：拿到一个实例改它的 vtable，
就等于改了这个进程里所有同类交换链 —— 这正是「游戏已经在跑、工厂钩子不会再被调用」时
唯一能把 Present 挂上去的办法（`239-244`）。
但 blt / flip / composition **可能是不同的类（vtable 不同）**，原函数也就不是同一个，
所以按 vtable 指针分表存；`PresentCommon` 里也是**按这条交换链的 vtable 反查**原函数的（`1139-1144`）。

### 10.5 检查方法

`gTel->hookFlags` 的 `NP_HOOK_PRESENT/QUEUE/CMDLIST/TIMESTAMP/OVERLAY` 位
（`np_common.h:389-401`）是主程序判断「挂到哪一步」的唯一依据；
同时每个安装点都会 `Log` 一行 `swapchain vtable patched: vt=%p present=%s present1=%p (now %d)`。

---

## 11. `ResizeBuffers`：**当前未挂钩**（明确记录为缺口）

- 全仓搜索确认：`src/hook/` 下 `ResizeBuffers` **只出现在注释里**
  （`np_draw.cpp:195,197,244,506,509,531`、`np_draw.h:87`、`np_hook.cpp:1614`），
  **没有任何一处对它做 vtable 补丁**。
- 也就是说：交换链重建（全屏转换 / `ResizeBuffers` / 分辨率切换）我们**收不到通知**，
  只能靠副作用间接感知：
  - D3D12 侧：`Overlay12::Record` 里发现「后台缓冲对象变了、8 个 RTV 槽用满」→ 换堆 +
    `settle_ = 30` 静默 30 帧（`np_draw.cpp:506-549`）；
  - D3D11 侧：每帧按 `GetCurrentBackBufferIndex()` 取当前后台缓冲 + 每帧新建 RTV（`np_hook.cpp:1528-1555`）；
  - 全局统计：`gOv12.HeapSwaps()` 记录换堆次数（`np_hook.cpp:1623-1625`）。
- 背景（为什么这些副作用是必要的）：**应用调用 `ResizeBuffers` / 切换独占全屏之前，
  必须释放所有对后台缓冲的引用**。我们长期握着引用就会让那次调用失败
  （`DXGI_ERROR_INVALID_CALL`），而不少游戏不检查返回值 —— 直接闪退。
  用户实测完全吻合：**不绘制叠加层不闪退，一绘制就闪退**（`np_draw.cpp:191-201`）。
- 若要新增 `ResizeBuffers` 钩子（下标属于 `IDXGISwapChain` 的另一格，**未确认**具体值，
  必须按 §4.1 的方式用 `tests/vt_check.py` 核对后再动），需要同步考虑：
  在回调里主动释放 D3D11 RTV / D3D12 描述符槽，并让 D3D12 侧进入 settle 状态。

---

## 12. D3D12 命令队列 / 命令列表钩子

### 12.1 功能说明

挂钩 `ExecuteCommandLists`（借它拿游戏队列、限定时间戳只插到我们选定的队列）、
`DispatchRays` / `BuildRaytracingAccelerationStructure`（光追统计）、
`Dispatch`（AI/后处理的 Tensor 区间）、`DrawInstanced` / `DrawIndexedInstanced`（draw call 计数）、
`RSSetViewports`（渲染分辨率）。

### 12.2 涉及函数

- `np_hook.cpp:1975-2106` `EnsureD3D12Hooks`（惰性安装，首次 D3D12 Present 时）
- `np_hook.cpp:2157-2226` `NpECL`
- `np_hook.cpp:2237-2283` `NpDispatchRays`
- `np_hook.cpp:2285-2290` `NpBuildAS`
- `np_hook.cpp:2292-2341` `NpDispatch`
- `np_hook.cpp:2343-2347` `NpDrawInst` / `2349-2353` `NpDrawIdx`
- `np_hook.cpp:2355-2372` `NpSetViewports`
- `np_hook.cpp:2232-2235` `CanTimestamp`

### 12.3 ⚠ D3D12 的函数实现其实在 `D3D12Core.dll` 里

`np_hook.cpp:430-440` 保留了一个**已无人使用**的 `InModule()`，它存在的唯一目的是记住这个教训：

> 曾经用它做「槽位必须落在 `dxgi.dll` / `d3d12.dll` 里」的健全性检查，
> 结果在真实 Windows 上**全数失败** —— D3D12 的实现其实在 **`D3D12Core.dll`** 里，
> `d3d12.dll` 只是个薄壳。判定依据错一个模块名，**光追和 DLSS 的钩子就一个都装不上**。

同类教训还有一层（`1718-1725`）：那套「必须落在 d3d12.dll 里」的假设只在干净的自建宿主里成立，
真实游戏里模块布局完全不同（别的叠加层、厂商模块、D3D11 与 D3D12 走不同实现），
结果命令列表钩子一个都没装 → **光追和 DLSS 数据全丢**，表面现象是「未检测到 DXR」「AI 已启用 · 估算中」。
所以现在只检查「是可执行代码、槽位互不相同」，并把**实际归属模块**（`PtrOwner`）打进日志。

### 12.4 `EnsureD3D12Hooks` 的安装逻辑

1. **失败必须能重试，但绝不能被并发重入**（`1976-1989`）：
   `attempts` / `lastTryMs` / `installing` 三个都是 `std::atomic`。
   原来是普通静态量：两个 Present 线程可以同时通过检查，各自建一套查询堆/fence/叠加资源 ——
   先建的句柄被覆盖，既泄漏又状态错乱（`gTsFreq` 也会跟着错）。
   策略是「谁先抢到谁装，其他人立刻返回 `false`」：装好后 `gOrigECL` 非空，
   下一帧所有线程都走最前面的快速路径。`InstallGuard` 保证任何返回路径都放开闸门。
2. 上限：**5 次尝试 + 2 秒冷却**（`2001-2007`）。
3. 建一条**临时 DIRECT 队列** → 读 `gQueueVt` → `Patch(ExecuteCommandLists)`（`2009-2019`）。
4. `GetTimestampFrequency` → `gTsFreq`（失败退 1000000）。
5. `InitTs12(dev, q)`：查询堆（`TS_SLOTS=64`）+ 4 个 readback 缓冲 + fence +
   `NP_ALLOC_RING=32` 个分配器 + 一条命令列表；然后**立刻 `Close()`**（`635-669`）。
6. `gOv12.Init(dev)`，成功置 `NP_HOOK_OVERLAY`。
7. **释放临时队列**（`2024`）—— 我们只借用它的 vtable 指针。
   注意：`gQueue12` 是后来在 `NpECL` 里 AddRef 锁定的**游戏队列**，与这条临时队列不是一回事。
8. 若 `Cfg().deepEngineHook`：再借一条**临时命令列表**，`QI ID3D12GraphicsCommandList4`，
   做健全性检查后挂 `DispatchRays` / `BuildRaytracingAccelerationStructure` / `Dispatch` /
   `DrawInstanced` / `DrawIndexedInstanced` / `RSSetViewports`（`2027-2104`）。
   任何一步失败都只写日志（`-> NO RT/Tensor data`），不影响帧时间路径。

### 12.5 `NpECL` 的关键逻辑

- `gUnloading` / `!q` → 直通（`2158`）。
- **只认 DIRECT 队列**（`2160-2177`）：复制/计算队列拿去做叠加和 GPU 时间戳都是错的，
  而且游戏常常先提交复制队列，先到先得会把 `gQueue12` 记错。
- **锁定队列时必须 `AddRef`**（`2166-2173`）：引擎在切换全屏/重建交换链/设备丢失恢复时
  会**销毁并重建命令队列**（有时连设备一起换）。只存裸指针，之后往已销毁的队列上
  `ExecuteCommandLists`/`Signal`：轻则围栏永不推进（`BeginList` 从此一直 allocator busy、
  叠加永久停画），重则在驱动里访问违例把游戏带崩。
- **只有 `q == gQueue12` 才插桩**（`2179-2189`）：原来只判 `q` 非空，
  于是游戏往复制队列提交时，我们会把**自己的 DIRECT 命令列表**丢给复制队列执行 ——
  非法调用，设备 removed，游戏弹 `DXGI_ERROR_INVALID_CALL` 退出。
  自建测试宿主只有一条队列所以测不出来，真实游戏大量用复制队列，症状是「数据出来一秒后闪退」。
- **「本帧已开始」用 `compare_exchange` 抢**（`2191-2201`）：
  `EndList()` 内部会再调一次 `q->ExecuteCommandLists`，也就是再进一次本函数，
  所以标记必须在 `EndList` **之前**抢到手，再加一道 `thread_local bool busy` 本线程重入锁；
  用「先读后写」会让两个线程同时通过检查、同时 `Reset` 同一条命令列表。
- **模拟阶段结束只认渲染线程**（`2202-2209`）：后台流式线程随时都在提交；
  还必须排除 `gInPresent`（我们自己的叠加和时间戳命令列表也是从渲染线程提交的），
  否则算出来的模拟阶段会变成整个帧周期。
- **逐批 GPU 时间戳夹取已被删除**（`2211-2224`）：曾经给每一次 `ExecuteCommandLists`
  前后各插一条时间戳（2 次 `BeginList`）。真实游戏一帧提交十几到几十批（实测
  "more than 16 batches per frame"），一帧要 32+ 个分配器 —— 环瞬间被掏空，然后
  `BeginList: allocator starved` 刷屏、`overlay NOT drawn`，**叠加永久停画**。
  代价远大于收益；GPU 帧时间现在有更好的来源：PDH 按进程读 `\GPU Engine(pid_*)\Running Time`。

### 12.6 `CanTimestamp`

`np_hook.cpp:2232-2235`。`ID3D12GraphicsCommandList` 的 vtable 是**打包（bundle）和直连列表共用**的，
而 bundle 上 `EndQuery` 是**非法操作** —— 真实游戏会用 bundle，一插就把设备搞成 removed。
复制列表同理不接受时间戳。所以只有 `GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT` 才允许插桩。

### 12.7 时间戳槽位布局

`np_hook.cpp:72-85`：

```
TS_SLOTS       64
TS_BATCH_BASE  0    TS_BATCH_PAIRS 16   // 槽 0..31，最多夹 16 批（当前已不使用，见 §12.5）
TS_RT_BASE     32   TS_RT_PAIRS    8    // 槽 32..47
TS_AI_START    48   TS_AI_END      49
```

`HarvestTimestamps`（`743-785`）在 fence 确认后 `Map` readback：
- 批次区间求和得 `gpuMs`，**单批超过 50ms 判为配对出错（比如跨帧）直接丢弃并记日志**（`762-764`）；
- RT 区间求和；AI 区间（`TS_AI_END > TS_AI_START` 才算）；
- 只有 `0 < gpuMs < 1000` 才写 `gTel->gpuFrameMs`；
- **RT/Tensor 的百分比不在这里算**，统一在 `UpdateTelemetryCommon` 里按**帧周期**当分母（`780-782`，理由见 §15.3）。

### 12.8 `NpSetViewports` 与渲染分辨率

`np_hook.cpp:2355-2372`。**只在视口长宽比和输出一致（`rel <= 1.05`）时才认**，
并且取满足条件的最大面积。原来取「见过的最大的视口」，
结果被 shadow atlas / 后处理用的方目标骗到：用户 16:10 的屏幕上显示成
`3072×3072 → 3840×2400（80%）`，完全是错的。

---

## 13. D3D12 资源与并发（`BeginList` / `EndList` 体系）

### 13.1 功能说明

我们有一条**全局唯一**的命令列表（`gList`）用来录制 GPU 时间戳 resolve 与叠加绘制。
真实游戏是多线程的，所以这条列表必须被严格串行化，而且**任何情况下都不能阻塞游戏线程**。

### 13.2 涉及函数

- `np_hook.cpp:671-718` `BeginList`
- `np_hook.cpp:720-741` `EndList`
- `np_hook.cpp:787-802` `SubmitPendingResolve`
- `np_hook.cpp:377-384` `struct Pending gPending[4]`
- `np_hook.cpp:361-375` 查询堆 / readback / fence / 分配器环

### 13.3 实现逻辑

1. `gD12Locked`（thread_local）已置 → 同线程重入，直接放弃（`672`）。
2. `gD12Broken` → 直接放弃（`673`）。
3. **`gD12Lock.try_lock()`，不是 `lock`**（`674-681`）：抢不到就**放弃这一帧**，
   绝不阻塞游戏线程。少一帧叠加/时间戳无所谓，卡住或搞崩游戏才是大事。
   前 5 次会记一条日志，避免刷屏。
4. **分配器环形缓冲 `NP_ALLOC_RING = 32`**（`366-374`）：一帧里每批 GPU 时间戳要 2 次
   （起止各一次），再加 Present 里的 resolve 和叠加录制；批次多的时候十几个很正常，
   只给 3 或 8 会让 GPU 稍微落后就全部 busy，然后 `BeginList` 一路返回 `nullptr`，叠加**永久停画**。
5. **绝不在这里等 GPU**（`685-701`）：原来会 `WaitForSingleObject` 最多 50ms ——
   那是在游戏 Present 的调用栈里睡觉，直接变成游戏的卡顿，还会污染我们自己的帧时间统计。
   拿不到就返回 `nullptr`。连续拿不到会记日志；**连续失败超过 600 次**则
   `gD12Broken = true` 永久停手（一直失败会让叠加永久停画，而围栏不推进往往意味着
   我们手里的队列/围栏已经失效，继续硬撑只会把游戏拖死）。
6. `Reset` 分配器 / `Reset` 命令列表失败 → 立刻 `gD12Broken = true`（`703-716`）。
7. `EndList`：`Close` → `ExecuteCommandLists(1, {gList})` → `++gFenceVal` → `Signal(gFence)` →
   记 `gAllocFence[gAllocCur]` → 若叠加有自己的 fence 也一起 `Signal` 并把值写进
   `fenceValuePtr()` → 解锁。**没拿锁就不要瞎解锁**（`721`）。
8. `SubmitPendingResolve`：找空的 `Pending` 槽（4 个），`ResolveQueryData(0, TS_SLOTS)` 到对应
   readback，`EndList` 后才把 `busy/fenceVal/rtCount/batchCount/aiUsed` 记上。

### 13.4 D3D11 时间戳

`InitTs11`（`807-817`）建 3 组（disjoint + a + b）；
`Ts11Tick`（`819-848`）用 `seq % 3` 做三缓冲轮转：结束上一组、开始当前组、尝试读最旧一组
（`GetData` 返回 `S_OK` 才算就绪，且必须 `!dj.Disjoint`、`dj.Frequency` 非 0、`b > a`、
`0 < ms < 1000`）。

---

## 14. Present 主流程 `PresentCommon`

**这是整个钩子层的心脏**，`np_hook.cpp:1137-1693`。两个跳板 `NpPresent`/`NpPresent1`
（`2114-2121`）只是转发。读懂这一节，就理解了钩子层 80% 的行为。

### 14.1 入口防护（顺序不能改）

```
1141-1144  按这条交换链的 vtable 反查原函数（op / op1），组装 CallOriginal lambda
1157       if (gUnloading) 直通        ← 必须在 gTel 解引用之前！
1158       if (!gTel || !sc) 直通
1159       if (gInPresent) 直通        ← 防重入
1160-1167  gInPresent = true；SehReady / setjmp 保护；失败一律复位后直通
1168       EnsureRt()                  ← D2D/DirectWrite 只能在渲染线程初始化
```

`gUnloading` 为什么必须放在最前（`1152-1156`）：`SelfUnloadNow()` 会先置 `gUnloading`、Sleep 排空，
然后 `UnmapViewOfFile(gTel)` 并把 `gTel` 置空；如果渲染线程正好晚一步进来，
`!gTel` 检查可能通过、而下一行 `*gTel` 指向的视图已经被解除映射 —— 直接访问违例。

### 14.2 首次接入（设备识别，`1174-1200`）

`GetDevice` 会 AddRef，所以设备/上下文是安全的（真正缺 AddRef 的是命令队列，见 §12.5）。
先试 `ID3D11Device`，再试 `ID3D12Device`；都失败就按已加载模块猜 API。
随后写 `gTel->processName`（取 exe 文件名）。

### 14.3 分辨率与全屏模式（`1202-1227`）

`GetDesc` 填 `windowW/windowH`；`GetFullscreenState` 为真 → `NP_PM_EXCLUSIVE`；
否则 `QI IDXGISwapChain1` → `GetHwnd` → 检查 `WS_POPUP && !WS_EX_TOPMOST && WS_VISIBLE`
→ `NP_PM_BORDERLESS`。

### 14.4 帧时间采集与「帧内重复 Present 合并」（`1229-1292`）★

这是本轮最关键的一段。

1. **只统计真正呈现的调用**（`1234`）：`realPresent = (flags & DXGI_PRESENT_TEST) == 0`。
   `DXGI_PRESENT_TEST` 只是问一句「能不能呈现」，不产生新帧；算进去会让帧数虚高、
   帧时间忽上忽下 —— 正是「游戏稳稳 60、我们却在 60~70 之间波动」的一个来源。
2. **原始间隔**：`fmRaw = now - gPrevPresentQpc`，然后 `gPrevPresentQpc = now` **无条件推进**。
   `fmRaw` 落在 `(0.02, 1000)` 才喂给 `gRawStats`（`1256-1262`）。
3. **基准 = 长窗口均值 `gRawStats.meanMs(240)`**（`1264-1272`）：
   - **不能用 `fpsAvg`**：那是从**合并后**序列算出来的 →
     合并 → 间隔被抬高 → `fpsAvg` 变小 → 阈值变大 → 合并更多 = **正反馈**。
     上一版就是这么翻车的（用户实测「帧时间」本身变得忽高忽低）。
   - **也不能用中位数**：双峰分布（8.4/24.9 各占一半）上中位数会随样本奇偶振荡，
     阈值在 9.99/5.04 之间跳 —— 数值模拟实测到过，表现就是「该合并的时而不合并」。
     均值在双峰上恒定 16.65，所以基准稳定（`np_stats.h:168-187`）。
   - 阈值 = `meanMs * 0.6`，**夹在 [2.0, 30.0]**：下限别把 120fps 的帧吞了，
     上限真卡顿时别乱合并（`1269-1271`）。
4. **判定用 `fmRaw`，记账用 `fm`**（`1273-1278`）：`fm = now - gLastPresentQpc`
   是「距上一次**记账**」的间隔，被合并时它自然覆盖整个帧。
   `fmRaw < mergeThresh` → `gAbsorbThisPresent = true`，`++gMergedCount`。
5. **只有未被合并的间隔才进统计**（`1282-1291`）：写 `gTel->frameMs`、`gStats.push`、
   `++frameTotal`、写 `frames[]/cpuFrames[]/gpuFrames[]` 环形缓冲。
6. **超过 1 秒的间隔不是「一帧」**（`1279-1282`）：是切出去/加载/挂起留下的空档。
   塞进统计会把 1% Low / 0.1% Low 直接拖到个位数 —— 用户看到「稳定 60fps 却显示 1% Low = 10」
   就是这么来的。

**为什么要合并（实测证据，`1238-1255`）**：真实 Present 之间的**最小**间隔只有 2.17ms，
而帧周期是 16.7ms —— 说明游戏在**一帧内调用了多次真实 Present**。
若每次都记账，就会把一个 16.7ms 的帧劈成「~5ms 的假快帧 + ~25ms 的假慢帧」，
于是 24% 的帧落进 20~30ms 区间、却在 30ms 处被硬生生切断（`over30≈0%`）。
真抖动会有长尾（掉垂直同步 33.3ms、加载几百 ms），不会这样齐刷刷截断。

验证过的三种情形：

| 输入形态 | 均值基准 | 阈值 | 结果 |
| --- | --- | --- | --- |
| 8.4 / 24.9 交替 | 16.65 | 9.96 | 8.4 被合并 → 得 33.3 ✓ |
| RE8 干净 16.6 | 16.6 | 9.96 | 16.6 > 9.96 → 不动 ✓ |
| 60fps 轻微抖动 14~20 | ~16.6 | 9.96 | 全部不动 ✓ |

> 数值模拟抓到过的反例（`327-334`）：用合并口径的间隔去喂原始序列，
> 会让「形态 A 只吸收了 80 次而非约 200 次，合并后均值 20.81 而非 33.3」。
> 这就是 `gPrevPresentQpc` 必须与 `gLastPresentQpc` 分开的原因。

7. **`gLastPresentQpc` 只在真实且未被合并时推进**（`1403-1405`），
   否则下一帧的间隔会从这个中间时刻算起，又会得到一段偏短的假帧时间。

### 14.5 CPU 两段（sim / submit，`1293-1332`）

```
模拟阶段 = 上一帧 Present 返回 → 本帧渲染线程第一次提交   （gSimEndQpc - gPresentRetQpc）
渲染提交 = 第一次提交 → 调 Present                        （now - gSimEndQpc）
```

- 为什么不量「第一次提交 → Present」当 CPU 帧时间（`1295-1300`）：这游戏有后台流式线程在不停提交，
  「本帧第一次提交」可能发生在**上一帧 Present 的阻塞当中**，量出来恒等于帧周期 16.66ms ——
  那是帧周期，不是 CPU 干的活。
- 用户明确要的是**模拟阶段**而不是两段之和：渲染提交本质是排队（录制命令列表 + 等提交），
  算进 CPU 帧会让这个数被帧率绑架（锁 60 就恒等于 16.66）（`1301-1310`）。
- 计算条件：`gRenderTid == GetCurrentThreadId() && gPresentRetQpc && gSimEndQpc > gPresentRetQpc`。
- 首帧兜底：`if (!gPresentRetQpc) gTel->cpuFrameMs = gTel->frameMs;`（`1340`）。

### 14.6 尾部收尾（**不可跳过**）

```
1631      gJmpArmed = false
1635-1639 tPresent0 → CallOriginal() → tPresent1；算 gLastInPresentMs / gTel->msInPresent
1652-1681 CPU 帧时间 = CPUBusy + CPUWait（同一帧的两个半边）
1684-1686 gRenderTid / gPresentRetQpc = tPresent1 / gSimEndQpc = 0
1690-1691 gFrameStarted = false；gInPresent = false
1692      return hr
```

**`gInPresent = false` 是这一整套的命门**：任何从中间提前 return 的路径都会让
`gInPresent` 永远为 `true`，之后每帧都在开头「已在 Present 中」分支里直通原函数，
叠加与 GPU 时间戳**永久失效**、帧时间也不再更新（`1490-1495`、`1508-1513` 两处注释，
历史上踩过一次）。所以「不想画」只能**置标志跳过**，不能 return。

---

## 15. 遥测发布 `UpdateTelemetryCommon`

### 15.1 功能说明

把本帧的原始数据加工成面板与主程序要的字段（平滑、窗口计数、百分位、RT/Tensor 占比、
图表环形缓冲），并**清零本帧的计数钩子累加器**。

### 15.2 位置与调用点

`np_hook.cpp:913-1117`；唯一调用点 `np_hook.cpp:1495`：
`if (!paused) UpdateTelemetryCommon(cfg, now, realPresent);`

### 15.3 关键逻辑

1. **`fps` 用时间窗口计数，不用逐帧换算**（`925-947`）：
   底层数据是对的（实测帧周期 avg=16.66ms = 精确 60fps），但逐帧间隔本身在 9~24ms 抖动。
   500ms 窗口计数 → 锁 60 时窗口内就是 30 帧/0.5 秒 = 恰好 60.0，不会 60↔100 乱跳。
   `frameMsAvg` 走 `np::Ema(0.10f)` 指数平滑。
   - ⚠ **窗口计数必须只数「真正呈现」的调用**（`939-942`）：这游戏每帧调两次 Present
     （一次 `DXGI_PRESENT_TEST` 探测 + 一次真的），不过滤的话窗口计数正好是真实帧率的两倍
     —— 用户实测显示 120（实际 60）。
2. **RT / Tensor 占比的分母是帧周期 `frameMs`，不是 `gpuFrameMs`**（`949-960`）：
   `gpuFrameMs` 那段跨度实际上约等于帧周期，是个**上界**，拿它当分母会把光追占比严重低估
   （用户实测：开了光追却看着像没开）。用帧周期当分母，含义是「光追 pass 吃掉了这一帧多少时间预算」，
   既准确又稳定，也是玩家真正关心的口径；绝对值（ms）同时给出来。
3. **`fpsAvg = gStats.avgFps(600)`**（`961`）。
4. **诊断块**：`gStats.count() >= 120` 且距上次超过 5 秒时打三组日志（`968-1049`）：
   - `frame dist`：采样率（必须用**累计帧数 `frameTotal`** 算，`count()` 满容量后不再增长）、
     「最近 1 秒」vs「整个缓冲区」的 p50/p90/p99 与 `over20/over30` 比例；
   - `recent ft`：最近 40 帧的**连续**帧时间序列 —— 一眼区分「真抖动」
     （`17 17 17 25 17 17 25`）与「帧内多次 Present 的伪影」（`17 17 17 3 3 28 28 17`）；
   - `low compare`：`merged1/merged01` vs `raw1/raw01`，用来判断帧内合并是否生效/误合并。
5. **GPU 帧时间在源头统一回填**（`1090-1103`）：优先用系统 PDH 按进程算出来的
   `Sens().gpuBusyMs`。为什么必须在**源头**回填：`t.gpuFrameMs` 是多个地方共同的数据源
   （`graphGpu`、`gpuFrames`、面板兜底），钩子自己那套逐批时间戳已经拆掉（§12.5），
   拆掉之后这个字段再没人写、**恒为 0** —— 于是所有以它为源的曲线都是**一条零线**
   （用户实测反馈："gpu 帧这个曲线全部为 0"）。
6. **图表环形缓冲** `graphFrame/graphCpu/graphGpu`（`1105-1111`）。
7. **收尾清零** `gDraws/gDispatches/gAsBuilds`（`1114-1116`）—— 必须在帧末清，
   否则下一帧的「本帧 draw 数」会累积。

### 15.4 同文件里另外两个发布函数（**注意区分**）

| 函数 | 行 | 何时调用 | 作用 |
| --- | --- | --- | --- |
| `UpdateTelemetryCommon` | 913 | 每次真实 Present（未暂停） | 完整遥测：帧统计、CPU 拆分、图表、计数器清零 |
| `PublishMinimalTelemetry` | 2517 | worker 循环里，**仅当 `!PresentHooked()`** | 心跳：`tickMs`、`gfxApi`、`processName`、`lastError`（把「已注入但没数据」变成一句看得懂的话） |

`PublishMinimalTelemetry` 的 `lastError` 文案按 API 分支（`2532-2553`）：
Vulkan / OpenGL 渲染 →「钩不进它的呈现链（已跳过 D3D 探测，不会影响游戏）」；
D3D11/D3D12 且 `gD12Broken` →「D3D12 侧检测到非法调用，已自动停手保护游戏」；
D3D 已加载但 Present 未挂上 →「看 NextPerfHook.log」；其余 →「游戏可能还没开始渲染」。

---

## 16. Low 帧（Intel PresentMon 权威口径）

### 16.1 功能说明

`fpsLow1` / `fpsLow01` **只使用 Intel PresentMon 的权威口径**（用户要求，不再提供可调开关）。

### 16.2 涉及代码

- 计算：`np_hook.cpp:1050-1088`（`lowWin` 的换算在 `1056-1058`）
- 实现：`np_stats.h:215-234` `lowPercentileFps(pct, window)` → `percentileMs`
- 对照日志：`np_hook.cpp:1028-1047`

### 16.3 口径三要素（源码里逐条注明了出处）

| 要素 | 取值 | 出处（源码引用） |
| --- | --- | --- |
| 窗口 | **1000 ms 滑动时间窗** | `SampleClient/CliOptions.h:49`（`--window-size` 默认 1000）；实现 `PresentMonMiddleware/DynamicQuery.cpp:221`：`oldest = newest - windowSize` |
| 百分位 | 线性插值 `index = (n-1)*p` | `Core/source/pmon/StatisticsTracker.cpp:35` —— 与我们的 `percentileMs` 一致 |
| 倒数指标排名反转 | 对 **FPS** 取 **P1**，等价于 `1000 / P99(帧时间)` | `PresentMonMiddleware/DynamicStat.cpp:323` —— 正是 `lowPercentileFps(99)` 做的事 |

窗口换算（`1051-1058`）：按当前平均 FPS 把 1 秒换算成帧数，并夹在 **[30, 480]**。
原来用 1200 帧 = 60fps 下 20 秒，大了 20 倍 —— 窗口越长，历史里的偶发 spike
越容易被算进「最差 1%」，数值被压低。

附带好处（`1084`）：1 秒窗口意味着**卡顿约 1 秒后就被遗忘**，不会「钉住」很久。

### 16.4 为什么不是别的口径（历史演进，全部保留在注释里）

- **按时间累加（integral）会撞「刀刃效应」**（`962-967`）：33.3ms 的 spike 消耗预算的速度
  是正常帧两倍，6 帧正好用满 1% 预算 → 恰好停在 spike 上 → 显示 30。
  而驱动面板显示的是「按帧数」口径的 59。实测「驱动 59 / 本程序 30」就是这个差异。
- **窗口平均口径（口径 4）** 曾经是默认（`1059-1072`）：用户实机日志（RE8 锁 60）显示
  严格（最差 1% 单帧）= 53.8，与驱动 59 差 5；窗口平均 = 57.1，明显更贴近驱动。
  原因是游戏内 overlay 与驱动面板显示的本来就是窗口平均后的 FPS。
  现在**面板固定用 PresentMon 口径**，`np::FrameStats::lowWindowed` 仍保留在 `np_stats.h:138-166` 供对照，
  但钩子层不再调用它 —— 若将来要恢复「Low 帧（严格）」开关，那才是入口。
- `p99Ms` / `p999Ms` 同时给出（`1087-1088`），走**全缓冲区**（默认窗口 = `kCap`）。

---

## 17. CPU Busy / CPU Wait 拆分

### 17.1 功能说明

把 CPU 帧时间拆成「自己干活」与「卡在 Present 里等显示器」两个半边，
口径照搬 Intel PresentMon。

### 17.2 涉及代码

- 计算点：`np_hook.cpp:1641-1681`（**在 `CallOriginal()` 返回之后**）
- 前置：`np_hook.cpp:1633-1639`（量 `tPresent0` / `tPresent1` / `gLastInPresentMs`）
- 为什么不在 Present 之前算：`np_hook.cpp:1333-1340`（长注释）
- 公式出处：`np_hook.cpp:1312-1328`

### 17.3 公式

按 `IntelPresentMon/MetricsCalculatorCpuGpu.cpp:155` + 单元测试 `MetricsCore.cpp` 的用例：

```
CPUBusy  = 本帧 Present 开始 − 上一帧 Present 返回     (tPresent0 − gPresentRetQpc)
CPUWait  = 本帧 Present 内部停留                       (tPresent1 − tPresent0)
CPUTime  = CPUBusy + CPUWait = 帧周期                  (tPresent1 − gPresentRetQpc)  ← 直接取这个
```

原文用例：`presentStartTime=1'100'000`、上一帧返回 `=1'000'000`
→ `msCPUBusy = 100'000 ticks = 10ms`；`timeInPresent = 200'000` → `msCPUWait = 20ms`；
`msCPUTime = 30ms = 帧周期`。

**为什么要放在 Present 返回之后**（`1333-1339`）：Busy 需要「本帧 Present 开始」，
Wait 需要「本帧 Present 返回」—— 在 Present 调用之前 `tPresent1` 还不存在。
之前在这里求和，Wait 用的是**上一帧**的值（而且赋值顺序还写在求和之后），
结果面板上 `CPU 帧时间 ≠ Busy + Wait`（实测 0.37 vs 0.24+5.95=6.19）。

**三项由同一次减法导出**，所以不存在「用了上一帧的值」这种错配：

```
Busy  = tPresent0 − 上帧返回
Wait  = tPresent1 − tPresent0
和    = tPresent1 − 上帧返回   <-- cpuFrameMs 直接取这个
```

其他要点：`busyMs` 下限夹 0；`totalMs` 必须落在 `(0, 1000)`（超过 1 秒的不是一帧，
是切出去/加载留下的空档）；只有 `realPresent` 才参与；
三个 EMA（alpha=0.10）分别平滑 `cpuBusyAvg/cpuWaitAvg/cpuFrameMsAvg`；
每 10 秒打一条 `cpu split avg` 日志用于核对「帧时间 = Busy + Wait」。

**原来的三条分支都不对**（`1322-1328`）：用「本帧开始 − 上帧**开始**」= 帧周期，没减掉 Present 内的等待；
兜底直接 `cpuFrameMs = frameMs` —— 那正是 PresentMon issue #222
「CPUBusy always shows up as equal to FrameTime」的现象。
sim/submit 仍单独记录供对照，但不再拿 `simMs` 当 CPU 帧时间（那只是「模拟阶段」，
不是 CPU 一帧的总工作量）。

字段定义见 `np_common.h:351-361`；UI 对应 `NP_C_CPU_BUSY` / `NP_C_CPU_WAIT`
（`np_common.h:93-96`，**故意不加进 `NP_ALL_COUNTERS`**，默认不显示）。

---

## 18. 叠加层绘制

### 18.1 三层结构

```
PresentCommon (np_hook.cpp:1497-1584)   决定画不画 / 取哪张后台缓冲 / 算位置
   └─ RenderPanel (np_hook.cpp:866-892) 决定尺寸 → npb::PanelBitmap::Render 画成 BGRA DIB
        └─ DrawCb (np_hook.cpp:858-861) → np::PanelRenderer::Render（真正的 D2D 绘制）
   └─ Overlay11::Draw (np_draw.cpp:155-252)   D3D11：Map/UpdateSubresource 贴图 + Draw(3)
   └─ Overlay12::Record (np_draw.cpp:400-608) D3D12：上传堆 + CopyTextureRegion + 全屏三角形
```

面板**不直接在设备上下文里用 D2D 互操作**，而是先画成 BGRA 位图再贴（`np_draw.h:1-8`）：
同一套位图能给 D3D11、D3D12、桌面分层窗口（`UpdateLayeredWindow`）三种目标复用，
代码量小、行为一致，也不会和游戏自己的状态打架。

### 18.2 是否绘制（`np_hook.cpp:1486-1524`）

```
paused      = cfg.pauseHook != 0            // 主程序点了「停止监视」
settling    = gOv12.settlePending()         // 交换链刚重建，静默期
wantOverlay = !paused && cfg.overlayMode != 2 && (gDev11 || gDev12)
              && !(gDev12 && !d12ok) && !settling
```

- `overlayMode == 2` 是「强制桌面叠加」：游戏内面板不画。
- **暂停时既不更新遥测也不画面板**（`1486-1495`、`1502-1506`）：
  原来停止监视只关掉桌面 HUD，游戏内 HUD 照旧在画、数据照旧在读，用户反馈「点了退出监视，
  画面里却还挂着一个面板在更新，很怪」。
- 刷新率限制：`cfg.updateHz`（默认 20），未到间隔就复用上一张位图（`1516-1523`）。
- **绝不在中间 return**：后台缓冲不属于我们接入的设备时只置 `stateOk = false`
  （`1508-1514`），而不是 `return CallOriginal()`。

### 18.3 取后台缓冲（`1524-1582`）

- **必须问 `GetCurrentBackBufferIndex()`**，不能固定 `GetBuffer(0)`（`1528-1532`）：
  flip 模型（`FLIP_DISCARD` / `FLIP_SEQUENTIAL`）下 `GetBuffer(0)` 不一定是当前正在显示的那张，
  否则面板只会画进某一张、交替闪烁甚至完全看不见。只有老式 blt 模型（`DISCARD`）才固定是 0。
- **光栅倍率以 1080p 为基准随分辨率放大**：`npb::SetRasterScale(clamp(Height/1080, 1.0, 2.5))`，
  面板在各分辨率下占屏比例一致；变化超过 0.05 才重画（`1548-1549`、`1570-1571`）。
- 位置：`px = 后台缓冲宽 - 面板宽 - offsetX * scale`（右上角对齐，`1550`、`1572`）。

### 18.4 D3D11 叠加 `Overlay11`（`np_draw.cpp:98-252`）

- `Init`：编译 `vs_5_0`/`ps_5_0`（一条覆盖全屏的三角形，`vid==0/1/2` → `(-1,-1)/(3,-1)/(-1,3)`），
  建采样器、**预乘 alpha** 混合状态、以及一个 `CullMode = NONE` 的光栅化状态。
- **必须显式建关掉背面剔除的光栅化状态并绑定**（`130-139`）：
  D3D11 默认是 `CullMode = BACK`，而全屏三角形正好是背面 —— 不绑这个的话 `Draw` 会「成功」返回，
  但屏幕上什么都没有（D3D12 那边 PSO 里本来就写了 `CULL_NONE`，所以没事）。
- 面板纹理：`B8G8R8A8_UNORM` + `D3D11_USAGE_DYNAMIC` + `CPU_ACCESS_WRITE`，
  尺寸变化才重建；**`texW_/texH_` 必须在纹理和 SRV 都建好之后才写**（`172-181`）：
  原来先写尺寸再建 SRV，一旦 SRV 失败就永远不再重建，于是永久带着 NULL SRV 去
  `PSSetShaderResources`，面板再也画不出来。
- **RTV 每帧新建、本帧结束就放掉**（`191-203`、`243-250`）：原来缓存 RTV 并对后台缓冲 `AddRef`，
  这是对 DXGI 规则的**严重违反**（见 §11 背景）。
- 位置用**视口**实现：`RSSetViewports({x, y, w, h})` 把全屏三角形拉伸到面板矩形，
  画完还原为整屏视口（`226-242`）。

### 18.5 D3D12 叠加 `Overlay12`（`np_draw.cpp:255-608`）

- `Init`：根签名（1 个 SRV 描述符表 + 1 个静态采样器）、SRV 堆（1 个，`SHADER_VISIBLE`）、
  RTV 堆（**8 个描述符**）、fence。PSO **不在 Init 建**，因为依赖后台缓冲格式，见 `EnsurePso`。
- **`Retire` / `OnFrameCompleted`：fence 延迟回收**（`376-398`）：
  D3D12 不会因为我们提交过命令列表就给资源加引用 —— 立刻 `Release` 一个
  「已录制进命令列表、但 GPU 还没执行完」的资源，就是释放正在使用的显存。
  队列上限 16 项，满了立刻放（宁可立刻放也别无限涨）；上传缓冲那条路径在满时
  **只清已完成项再重试，绝不直接 Release**（`456-472`）。
- **RTV 描述符「换堆」而不是覆盖槽位**（`497-549`）★：
  - 每种后台缓冲占**各自**一个槽（`rtvSlots_[8]`）。原来只有一个槽、每帧按当前缓冲重写 ——
    上一帧的命令列表可能还在执行，而 **GPU 是执行时（不是录制时）才去读描述符堆的**，
    等于让它画到别的缓冲上，轻则画面错乱、重则设备 removed。
  - 8 个槽用满 ⇒ 交换链被重建过多次（全屏独占转换 / `ResizeBuffers` 都会换掉一整批后台缓冲对象）。
    旧槽里的描述符仍然引用着**已经作废的旧后台缓冲** —— 这既让游戏的 `ResizeBuffers` 失败
    （就是那个闪退），也让我们自己画到旧缓冲上（用户实测：全屏后换场景开始**闪烁**，
    闪一会就**看不见**了，正是槽位用尽后本函数返回 `false` 停止绘制）。
  - **为什么必须换堆而不是覆盖旧槽位**：读描述符堆的是**游戏自己的命令列表**
    （我们只是往那个列表里录了 `OMSetRenderTargets`），所以「有没有在途命令列表」
    我们**根本无法判断** —— `pending_` 只跟踪我们自己的上传缓冲。
    之前用 `pendingN_ == 0` 做判据是**不够的**，会覆盖正在被读的描述符，表现就是偶发闪退 + 闪烁。
    换堆绕开了整个问题：旧堆原封不动留给还在执行的那几帧用，
    由 `Retire` → `OnFrameCompleted` 在 fence 确认后才 `Release`。
  - 换堆后 `settle_ = 30`：**静默 30 帧**。实测证据：切换窗口/无边框/全屏时会在
    `nvwgf2umx.dll+0x330124`（NVIDIA 用户态驱动，同一偏移反复出现 = 驱动里一个确定的崩溃点）
    触发 `0xC0000005` 把游戏带走，而崩溃只发生在交换链重建那段窗口期。
    我们改不了驱动，但可以**避开那个时刻**。
- 上传：行距必须按 `D3D12_TEXTURE_DATA_PITCH_ALIGNMENT`(256) 对齐（`435-455`）。
- 绘制：`OMSetRenderTargets` + 视口 + **裁剪矩形**把全屏三角形限制到面板区域，
  前后各一次屏障（`bbBefore` ↔ `RENDER_TARGET`），面板纹理末尾转回 `COPY_DEST` 供下一帧上传（`566-604`）。
- `Release` 里**必须重置 `psoFormat_ = UNKNOWN`**（`368-373`）：不重置的话，
  若本对象在同一进程里被重新 `Init`，`EnsurePso` 会因为 `psoFormat_` 恰好相等而认为旧 PSO 还能用
  （其实已经 `Release` 了）。同时这里也补上了原来漏掉的 `vsBlob_`/`psBlob_`。
- **`Overlay12::Record` 必须由调用方用 `BeginList`/`EndList` 包起来**（`np_hook.cpp:1573-1578`），
  屏障与绘制录进的是**我们自己的**命令列表，再 `EndList(gQueue12)` 提到游戏队列。

### 18.6 抑制自己的 draw 计数

`gNpCountingOverlayDraws`（定义 `np_hook.cpp:911`，声明使用 `np_draw.cpp:15`）：
- 画自己的全屏三角形时置 `true`（`np_draw.cpp:237-239`、`593-595`），
  `NpDrawInst`/`NpDrawIdx`（`np_hook.cpp:2343-2353`）直接转发、不计数。
- **必须是 `thread_local`**（`908-910`）：叠加是自己人画的，只应该抑制**我们自己这一路**的计数。
  原来是普通全局 bool —— 叠加在 Present 线程绘制时，游戏在别的提交线程恰好也 draw 一笔，
  那笔就会被误抑制。
- 定义必须放在匿名命名空间**之外**（`900-907`）：`np_draw.cpp` 用的是 `extern "C"` 声明，
  匿名命名空间里的名字没有外部链接，链接会直接失败。

### 18.7 叠加诊断

| 日志 | 行 | 用途 |
| --- | --- | --- |
| `panel logical size WxH ...` | 875-886 | 面板逻辑尺寸一变就记一条。「HUD 宽度随数值一直变化」靠它客观判定；理想情况整局只出现一次 |
| `overlay NOT drawn: want=... panel=... bmp=...` | 1586-1597 | 每秒最多一条。「注入成功、数据也对，但游戏里看不到面板」这类问题 |
| `overlay drawn: WxH scale=` | 1598-1604 | 只记一次 |
| `overlay 5s: drawn / skipped / heapSwaps` | 1606-1629 | **区分闪烁的两种成因**：A) 我们没画上去（`drawnOk=false`）→ 画面交替有/无；B) 每帧都画了但画到的不是最终呈现的那张缓冲 → 面板内容在几张缓冲之间跳。`heapSwaps` 暴涨说明游戏在频繁重建交换链 |
| `D3D12 device lost: hr=...` / `GetDeviceRemovedReason hr=...` | 1460-1470 | 每 128 帧查一次；健康的设备返回 `S_OK`，拿到别的失败码说明设备指针本身有问题，也要记下来 |

---

## 19. NvAPI Reflex / 低延迟状态读取

### 19.1 功能说明

游戏若启用 Reflex，会**自己**打 sim/submit/present 的标记，我们直接读它 ——
这才是权威的 CPU 帧时间拆分（`np_reflex.h:1-14`）。

### 19.2 涉及代码

- `src/hook/np_reflex.h` 全文（header-only）
- 使用点：`np_hook.cpp:1342-1402`
- 结构体：`NVFrameReport`（`np_reflex.h:24-42`，**与 nvapi.h 逐字段对齐，不能改顺序/宽度**）、
  `NVLatencyParams`（`44-48`）、`NVGetSleepStatusParams`（`53-58`）
- 接口 ID：`NvAPI_D3D_GetLatency = 0x1A587F9C`（来自 Intel PresentMon 的 `nvapi_interface_table.h`）、
  `NvAPI_Initialize = 0x0150E828`、`NvAPI_D3D_GetSleepStatus = 0xAEF96CA1`
- 版本宏：`NP_NVAPI_VERSION(s,v) = sizeof(s) | (v << 16)`（`np_reflex.h:50-51`）

### 19.3 实现逻辑

1. `ReflexReader::Init()`（`63-81`）：`LoadLibraryW("nvapi64.dll")`，失败退 `nvapi32.dll`；
   `GetProcAddress("nvapi_QueryInterface")`；先 `NvAPI_Initialize`（**返回非 0 视为失败**，
   0 = `NVAPI_OK`）；再取 `getLatency_` 与 `getSleepStatus_`。幂等，可任意时刻调用。
2. `Poll(dev, out)`（`87-107`）：填 `version` → 调 `getLatency_` → 从 **64 帧**里挑
   **最后一条有效的**（`frameID` 最大且 `simStartTime`/`simEndTime` 非 0）；
   明显不合理的（结束早于开始）也不要。
3. `SleepStatus(dev, &on)`（`111-118`）：**驱动直证**低延迟是否开启。
   注释说明了为什么比推断可靠：「用 CPU Wait 推断是『等待被移出 Present』的旁证，这个是驱动直说」。
4. 钩子侧（`1342-1402`）：
   - **设备只取一次**（`1348-1361`）：先试 `GetDevice(ID3D12Device)` 再试 `ID3D11Device`，
     **AddRef 后长期持有，不释放**；`gReflexTried` 保证只试一次；并把 `init/ok` 打进日志。
   - **低延迟状态 1 秒查一次**（`1362-1380`）：置 `NP_HOOK_REFLEX_KNOWN`；
     开启才置 `NP_HOOK_REFLEX`；每 10 秒打一条 `reflex sleep status: lowLatency=%d (authoritative)`。
   - **每帧 Poll**（`1381-1401`）：`sim`/`sub` 都夹在 `(0, 1000)` 才写 `simMs`/`submitMs`；
     读到就置 `NP_HOOK_REFLEX`；每 10 秒打一条 `reflex frame: id=... sim=... submit=... gpuActiveUs=...`。
   - 只在 `realPresent` 时做。

### 19.4 回退语义（`np_common.h:395-400`）

- `NP_HOOK_REFLEX`：游戏自己在上报 Reflex 延迟标记（说明低延迟技术已启用）。
  **只有 `NP_HOOK_REFLEX_KNOWN` 也置位时，这一位才是权威结论**；
  否则那一位只是缺省值，调用方要回退到自己的判据（如 CPU Wait < 1.6ms）。
- ⚠ 只有游戏**自己在用 Reflex** 时才会有数据；没有就返回 `false`，调用方必须保持回退路径
  （`np_reflex.h:14`）。
- sim/submit 的**启发式**结果（§14.5）与 Reflex 的**权威**结果写的是同两个字段
  （`gTel->simMs` / `gTel->submitMs`）—— 读到时覆盖，这是有意的。

---

## 20. ETW：**不在钩子层**

- ETW 帧计时在 `src/etw/np_etw.h` / `np_etw.cpp`，由**主程序**（`src/app/main.cpp`）驱动，
  走的是「订阅内核 DxgKrnl Present 事件」，**不注入游戏**（`np_etw.h:1-9`）。
- 钩子层**不包含任何 ETW 代码**：`src/hook/` 下 grep `ETW` 无匹配。
- 两者在 `main.cpp` 里的分工（供理解边界，不在本文档职责内）：
  ETW 只用来替换 **Low 帧**读数与帧计数兜底（`main.cpp:121-191`），
  其余帧统计仍来自钩子的遥测块。
- 因此：**给钩子层加 ETW 不是自然的扩展方向**。若要动 ETW，去改 `src/etw/` 与 `docs/CATALOG.md` 里对应的条目。

---

## 21. 干净卸载

### 21.1 功能说明

把 vtable 补丁全部还原、注销 VEH、释放所有资源、从宿主进程里卸载自己。
**只有两条路径，而且必须都走 `RestoreAllHooks()`。**

### 21.2 涉及函数

| 函数 | 行 | 触发条件 |
| --- | --- | --- |
| `RestoreAllHooks()` | 2397-2453 | 被下面两条路径调用（**幂等**） |
| `SelfUnloadNow()` | 2462-2501 | `gCfg->quit` 或 `gCfg->detachPid == 自己的 pid` |
| `NpHookDetach()` | 2663-2684 | `DllMain` 的 `DLL_PROCESS_DETACH` |
| `Worker` 里的 `quit` 判定 | 2602 | 主程序退出 |
| `Worker` 里的 `detachPid` 判定 | 2603-2611 | 主程序要求卸载 |
| 三个工厂跳板里的 `!gUnloading` | 2133 / 2144 / 2153 | 防止卸载窗口期重新写回 vtable |

### 21.3 `RestoreAllHooks()` 做的事（顺序有意义）

1. **先记一条日志**（`2398-2402`）：这样从日志就能确认**两条卸载路径**
   （`SelfUnloadNow` / `DLL_PROCESS_DETACH`）到底有没有真的还原。
   之前没有这条日志，所以无法判断还原是否发生。
   日志里带 `swapVt / factory / factory2 / queue / cmdlist / seh` 六个计数。
2. **注销 VEH 处理器**（`2404-2415`）——**这是「重复注入 100% 闪退」的真正根因**：
   注册了却从不注销，DLL 卸载后进程的 VEH 链表里就留下一个指向**已卸载内存**的处理器；
   下次注入再注册一个，链表变成 `[野指针, 新指针]`；而探测路径**本来就会触发异常**
   （SEH 机制正是为此存在），于是 Windows 先调用那个野指针 → 跳进已卸载内存 → `0xC0000005`。
   实测：注入过一次的进程再注入必崩，全新进程首次注入正常 —— 完全吻合。
   同时也把 `gSehReady` 复位，允许后续重新注册。
3. 交换链：**遍历整张小表**逐条还原 `Present` / `Present1`（`2416-2422`），然后 `gSwapVtCount = 0`。
4. 工厂 1/2 的 `CreateSwapChain` / `CreateSwapChainForHwnd` / `CreateSwapChainForComposition`（`2424-2433`）。
5. 队列 `ExecuteCommandLists`（`2434-2435`）。
6. 命令列表六项（`2436-2448`）。
7. 把所有 vtable 指针全局置 `nullptr`（`2449-2452`）。

### 21.4 `SelfUnloadNow()` 的顺序（**不能乱**）

```
gUnloading = true         // 1. 所有跳板转为直通
Sleep(150)                //    给在途的 Present 调用时间退出
RestoreAllHooks()         // 2. 还原 vtable + 注销 VEH
Sleep(250)                // 3. 在途调用排空窗口
释放图形与采集资源          // 4. gOv11/gOv12/gBmp/gPanel/GfxShutdown；D3D11 查询；readback；gList；
                          //    分配器环；查询堆；fence；gCtx11；gDev11；gQueue12；gDev12
释放互斥量与共享内存        // 5. gInjectedMutex；UnmapViewOfFile(gTel/gSens/gCfg)；CloseHandle(三个 map)
FreeLibraryAndExitThread(gSelf, 0)   // 6. 从宿主进程卸载自己并结束本线程
```

源码注释（`2382-2385`）：顺序不能乱 —— 先让跳板直通（`gUnloading`），再还原 vtable，
**短暂等待在途调用排空后才允许 `FreeLibrary`**。

### 21.5 `detachPid` 机制（主程序主动要求卸载）

- 字段定义：`np_common.h:193-199`。
- 判定：`np_hook.cpp:2603-2611`，逐字条件
  `gCfg && gCfg->magic == NP_MAGIC && gCfg->detachPid != 0 && gCfg->detachPid == GetCurrentProcessId()`
  → `Log("detachPid matched -> SelfUnloadNow (asked by host)")` → `SelfUnloadNow()`。
- **为什么用共享内存字段 + 轮询，而不是 `CreateRemoteThread` 调 `NpHookDetach`**（`np_common.h:196-198`）：
  后者需要解析远端导出地址（ASLR 下要自己算偏移），而且跨位数（32 位游戏）还得另做一套。
  钩子本来每帧就在读 `NPConfig`，用这个字段既简单又天然支持跨位数。
- **为什么判定放在 worker 循环而不是 Present 里**（`2603-2606`）：
  `SelfUnloadNow` 的定义在 `Worker` 下面，在它之前用需要前置声明，
  而跨作用域的前置声明会变成 `ambiguous`。worker 每 200ms 轮询一次，
  这个延迟对「卸载钩子」完全够用。
- **触发后必须自己 `SelfUnloadNow()`**：因为 `detachPid` 只是「请求」，
  真正的还原动作只在 `RestoreAllHooks` 里。

### 21.6 `NpHookDetach()` 为什么也必须还原（`2665-2675`）

这里原来是**没有**还原的 —— 还原只写在 `SelfUnloadNow()` 里，而 `DllMain` 的
`DLL_PROCESS_DETACH` 分支完全不碰补丁。于是只要卸载没走「优雅自卸载」那条路，
补丁就留在原地指向即将被卸载的代码；下次注入时 `InstallDxgi` 读到的「原函数」
其实是上一次的钩子 → 钩子调用自己 → **无限递归 → 栈溢出**
（实测 `0xC0000005`，崩溃地址正好是 DLL 基址 + `0xA040`）。
`DLL_PROCESS_DETACH` 是**唯一**保证「在代码被卸载前执行」的位置，所以必须在这里还。
`RestoreAllHooks` 是幂等的：`SelfUnloadNow` 已经还过一遍也没关系。

然后是 `gReady = false`（停掉 worker 循环）、等 worker 线程结束
（**自卸载路径下 detach 发生在 worker 线程自己身上，不能等自己**，`2676-2682`）、
`DeleteCriticalSection(&gCs)`。

### 21.7 卸载后仍然残留的东西（明确记录）

| 残留物 | 为什么留着 | 影响 |
| --- | --- | --- |
| 窗口类 `NextPerfProbeWnd` | 注册在**宿主 exe 模块**上（`1860-1866`），exe 永不卸载，无法也不该注销 | 只是一个类名，用宿主模块句柄，不会变成野类 |
| `nvapi64.dll`（`ReflexReader::dll_`） | 只在实现文件里 `LoadLibrary`，没有对应的 `FreeLibrary`（`np_reflex.h:65-69`） | 进程退出时自然回收；**未确认**是否有意为之 |
| `gReflexDev`（D3D 设备） | 注释明确写「AddRef 后长期持有，**不释放**」（`np_hook.cpp:341`） | `SelfUnloadNow` 的释放清单里确实没有它 |
| `gCs` | `InitializeCriticalSection` / `DeleteCriticalSection` 成对，但**全文件没有任何 `EnterCriticalSection`** | 当前是死代码（`gCs` 只在 `228/2659/2683` 出现） |
| `gCpuStartQpc` | `NpECL` 里写（`2199`），**没有任何读点** | 死变量，原为「本帧 CPU 起点」 |

> 若将来要把这些都清干净，注意 `gReflexDev` 一旦 `Release` 就必须同时让
> `gReflexTried`/`ReflexReader` 状态可重置，否则「重新 Init」路径会拿到悬空设备指针。

---

## 22. 重要的坑与教训（逐条索引）

这一节是本文档**最值钱**的部分。每一条都来自源码注释里的实证记录。

### 22.1 进程与模块

| # | 坑 | 位置 |
| --- | --- | --- |
| 1 | **VEH 注册了不注销 → 重复注入 100% 闪退**。链表里留下指向已卸载内存的处理器；崩溃地址 = 旧 DLL 基址 + 固定偏移（按地址反查会得到「不属于任何模块」） | `198-208`、`2404-2415` |
| 2 | **VEH 拿到的是所有异常**，不筛就把探测整个 `longjmp` 掉（`DBG_PRINTEXCEPTION_C` 在 D3D 初始化时很常见）；C++ 异常也不能吞 | `146-168` |
| 3 | **`jmp_buf` 必须每线程一份**，否则一边出错会 `longjmp` 到另一边的栈上 | `136-142` |
| 4 | **D3D12 的实现在 `D3D12Core.dll`**，不在 `d3d12.dll`。按模块名做健全性检查 → 光追/DLSS 钩子一个都装不上 | `430-440`、`1718-1725` |
| 5 | **本 DLL 静态导入 dxgi/d3d11/d3d12**，所以 `GetModuleHandleW(L"d3d12.dll") != nullptr` 永远为真 → 所有「让宿主工厂钩子先跑」的保护失效。必须读自己的 PE 导入表 | `442-448`、`449-478` |
| 6 | **不能拿 `vulkan-1.dll` 当「这是 Vulkan 游戏」的证据**：D3D12 游戏也加载它，上一版因此把 D3D12 游戏判成 Vulkan → 探测永久禁用 | `491-496` |
| 7 | **上一次卸载没还干净 → 「原函数」其实是上一次的钩子 → 调用自己 → 无限递归 → 栈溢出**（实测 `0xC0000005`，地址 = DLL 基址 + `0xA040`） | `1913-1926`、`1952-1958`、`2665-2675` |
| 8 | 探测用的窗口类**必须注册在宿主 exe 模块**上；绑 `gSelf` 会留下指向已卸载内存的野类，并且**永久留在游戏进程里** | `1843-1859` |
| 9 | **绝不在 Vulkan 进程里创建 D3D 设备**（实测某款 Vulkan 游戏：注入成功，一秒后闪退且无任何报错）。探测次数因此要分级限制 | `299-302`、`2579-2633` |

### 22.2 Present 主流程

| # | 坑 | 位置 |
| --- | --- | --- |
| 10 | ★★ **绝不能在 `PresentCommon` 中间提前 `return`**：尾部还有一整套收尾（`gInPresent` / `gFrameStarted` / `gLastPresentQpc` / `gLastInPresentMs`），跳过会让 `gInPresent` 永远为 `true`，之后每帧都直通原函数，**叠加与 GPU 时间戳永久失效**。这个坑踩过**两次**（暂停分支、后台缓冲设备不匹配分支） | `1490-1495`、`1508-1514` |
| 11 | **`gUnloading` 必须放在 `gTel` 解引用之前**，否则自卸载解除映射后渲染线程晚一步进来就是访问违例 | `1152-1157` |
| 12 | **CPU 帧时间必须在 Present 返回之后算**：Busy 要用「本帧 Present 开始」、Wait 要用「本帧 Present 返回」，在 Present 之前求和会用到**上一帧**的 Wait（实测面板 `CPU 帧时间 ≠ Busy + Wait`：0.37 vs 0.24+5.95=6.19） | `1333-1340`、`1641-1651` |
| 13 | **`DXGI_PRESENT_TEST` 不是帧**：不过滤会让帧数虚高、帧时间忽上忽下，窗口计数正好是真实帧率两倍（用户实测显示 120，实际 60） | `1229-1234`、`939-942` |
| 14 | **超过 1 秒的间隔不是「一帧」**：塞进统计会把 1% Low / 0.1% Low 拖到个位数（「稳定 60fps 却显示 1% Low = 10」） | `1279-1282`、`1657-1658` |
| 15 | **模拟阶段结束只认渲染线程、且要排除 `gInPresent`**：后台流式线程随时都在提交；我们自己的叠加/时间戳命令列表也是从渲染线程提交的 | `2202-2209` |
| 16 | `gFrameStarted` 必须**在 `EndList` 之前**用 `compare_exchange` 抢（`EndList` 会再进一次 `NpECL`），否则两个线程会同时 `Reset` 同一条命令列表 | `2191-2201` |
| 17 | **`GetBuffer(0)` 在 flip 模型下不一定是当前显示的那张**，必须问 `GetCurrentBackBufferIndex()`，否则面板交替闪烁甚至看不见 | `1528-1532` |

### 22.3 帧内重复 Present 的合并（本轮核心修复）

| # | 坑 | 位置 |
| --- | --- | --- |
| 18 | ★★ **合并基准绝不能用 `fpsAvg`**：那是从**合并后**序列算出来的 → 合并抬高间隔 → `fpsAvg` 变小 → 阈值变大 → 合并更多 = **正反馈**（上一版就是这么翻车的，用户实测「帧时间」本身忽高忽低） | `1246-1249` |
| 19 | ★★ **基准也不能用中位数**：双峰分布（8.4/24.9 各半）上中位数随样本奇偶振荡（16.65 / 8.4 / 24.9），阈值在 9.99/5.04 之间跳 → 「该合并的时而不合并」= 用户上次看到的帧时间忽高忽低。必须用**长窗口均值**（240 样本 ≈ 4 秒） | `1264-1266`、`np_stats.h:168-177` |
| 20 | ★★ **原始间隔必须单独记 `gPrevPresentQpc`**：`now - gLastPresentQpc` 是**合并口径**的间隔，拿它喂原始序列等于让合并污染基准 → 中位数被抬高 → 阈值漂移 → 正反馈。数值模拟抓到过：形态 A 只吸收了 80 次而非约 200 次，合并后均值 20.81 而非 33.3 | `317-335`、`1256-1262` |
| 21 | **纯中位数基准对「基准污染」极度敏感**：一旦没有独立原始序列，中位数会自己爬升（每次合并把对喂进去就被自己抬高），呈螺旋上升；均值有自限性 | `317-335` |

### 22.4 绘制与 D3D 资源

| # | 坑 | 位置 |
| --- | --- | --- |
| 22 | ★★ **绝不能长期引用后台缓冲**（`AddRef` / 缓存 RTV）：DXGI 要求 `ResizeBuffers` / 切独占全屏前**所有**后台缓冲引用必须释放，否则那次调用失败（`DXGI_ERROR_INVALID_CALL`），不少游戏不检查返回值直接闪退。用户实测：**不绘制叠加层不闪退，一绘制就闪退** | `np_draw.cpp:191-201`、`243-250` |
| 23 | ★★ **描述符堆必须「换堆」而不是覆盖槽位**：GPU 是**执行时**才读描述符堆，而读它的是**游戏自己的命令列表** —— 「有没有在途命令列表」我们根本无法判断（`pending_` 只跟踪我们自己的上传缓冲）。用 `pendingN_ == 0` 做判据不够，会覆盖正在被读的描述符 = 偶发闪退 + 闪烁 | `np_draw.cpp:497-549` |
| 24 | **RTV 槽位用尽 ⇒ 交换链被重建过多次**：旧槽引用着**已作废的旧后台缓冲**，既让游戏 `ResizeBuffers` 失败（闪退），也让我们画到旧缓冲上（全屏后换场景闪烁，闪一会就看不见了） | `np_draw.cpp:506-517`、`518-531` |
| 25 | **交换链重建窗口期必须静默**：实测 `nvwgf2umx.dll+0x330124`（NVIDIA 用户态驱动里一个确定位置）会 `0xC0000005` 把游戏带走，崩溃只在重建那段窗口期发生 → `settle_ = 30` | `np_draw.cpp:542-548`、`1499-1501` |
| 26 | **D3D12 资源绝不能立刻 `Release`**：D3D12 不会因为我们提交过命令列表就给资源加引用 → 必须 fence 延迟回收（`Retire` / `OnFrameCompleted`） | `np_draw.cpp:376-385` |
| 27 | **D3D11 默认 `CullMode = BACK`**，而全屏三角形正好是背面 → `Draw` 会「成功」返回但屏幕上什么都没有。必须显式建 `CULL_NONE` 光栅化状态并绑定 | `np_draw.cpp:130-139` |
| 28 | **`texW_/texH_` 必须在纹理和 SRV 都建好之后才写**：先写尺寸再建 SRV，一旦 SRV 失败就永远不再重建，永久带着 NULL SRV 去 `PSSetShaderResources` | `np_draw.cpp:172-181` |
| 29 | **`Overlay12::Release` 必须重置 `psoFormat_`**，否则同一进程内重新 `Init` 时 `EnsurePso` 会以为旧 PSO 还能用（其实已 `Release`） | `np_draw.cpp:368-373` |
| 30 | **少了 `npb::GfxInit()` 就是「注入成功、数据也有，但游戏里看不到面板」** | `1128-1131` |
| 31 | **面板纹理必须等 D2D 与 D3DCompile 都就绪**，而 D2D/DirectWrite 只能在**渲染线程**初始化（单线程 D2D 工厂不能跨线程用，多线程渲染的游戏会因此闪退） | `1121-1123` |
| 32 | **渲染分辨率不能取「见过的最大的视口」**：会被 shadow atlas / 后处理的方目标骗到（16:10 屏幕显示 3072×3072 → 3840×2400）。必须要求长宽比与输出一致（`rel <= 1.05`） | `2355-2372` |

### 22.5 并发与统计口径

| # | 坑 | 位置 |
| --- | --- | --- |
| 33 | **`FrameStats` 的临时缓冲必须 `thread_local`**：原来是 `static float tmp[kCap]`（16KB 全线程共享），多线程 Present 并发进入会算出垃圾值，而且完全没有报错、看起来「只是数不对」 | `np_stats.h:45-55`、`78-80` |
| 34 | **计数变量必须是 `atomic`**：命令列表钩子被多个提交线程同时调用，而 Present 线程同时在读并清零；普通变量的读-改-写既丢计数也是 UB | `352-357` |
| 35 | **`gNpCountingOverlayDraws` 必须 `thread_local`**：普通全局 bool 会让「别的提交线程恰好也 draw 一笔」被误抑制 | `908-911` |
| 36 | **`EnsureD3D12Hooks` 必须防并发重入**：两个线程同时通过检查会各建一套查询堆/fence/叠加资源，先建的句柄被覆盖，泄漏 + 状态错乱（`gTsFreq` 也跟着错） | `1976-1999` |
| 37 | **槽位分配必须用 `fetch_add` 的返回值**，不能「先 load 判断、再 fetch_add」：两个提交线程会读到同一个 `n`，同时往同一个时间戳槽里写 | `2253-2257` |
| 38 | **`setjmp` 返回值当条件用会「提交两次」**：`if (setjmp(...)==0) {...; gOrigX(); ...; return;} gOrigX();` —— 探针出异常时条件为假，掉到兜底再转发一次，同一次光追被提交两遍（画面错乱 + 设备 removed）。必须单独记 `faulted` 并**只转发一次** | `2237-2283`、`2296-2311` |
| 39 | **RT/Tensor 占比的分母是帧周期不是 `gpuFrameMs`**：后者是个**上界**，会让光追占比严重低估（用户实测：开了光追却看着像没开） | `949-960` |
| 40 | **`gpuFrameMs` 拆掉逐批时间戳后恒为 0**，所有以它为源的曲线变成一条零线（用户实测「gpu 帧这个曲线全部为 0」）→ 必须在**源头**用 PDH 的 `Sens().gpuBusyMs` 回填 | `1090-1103`、`1444-1453` |
| 41 | **逐批 GPU 时间戳夹取的代价远大于收益**：真实游戏一帧十几到几十批（实测 "more than 16 batches per frame"），一帧要 32+ 分配器 → 环被掏空 → `allocator starved` 刷屏 + 叠加永久停画 | `2211-2224`、`366-369` |
| 42 | **Low 帧口径**：按帧数（P99） vs 按时间累加 → 后者有「刀刃效应」（33.3ms spike 6 帧正好用满 1% 预算 → 恰好停在 spike 上 → 30）。驱动面板用前者（实测驱动 59 / 本程序 30） | `962-967`、`1028-1047` |
| 43 | **窗口大小**：1200 帧 = 60fps 下 20 秒，比 PresentMon 默认的 1000ms 大 20 倍 → 历史里的偶发 spike 更容易被算进「最差 1%」 | `1051-1055` |
| 44 | **采样率必须用累计帧数 `frameTotal` 算**，`count()` 满容量后不再增长 | `976-982` |
| 45 | **「本帧第一次提交」不能当 CPU 起点**：后台流式线程的提交可能发生在**上一帧 Present 的阻塞当中**，量出来恒等于帧周期 | `1295-1300` |

### 22.6 队列、命令列表与设备

| # | 坑 | 位置 |
| --- | --- | --- |
| 46 | ★ **`gQueue12` 必须 `AddRef`**：引擎在全屏切换/重建交换链/设备丢失恢复时会**销毁并重建命令队列**，裸指针之后 `ExecuteCommandLists`/`Signal` 轻则围栏不推进（叠加永久停画），重则驱动里访问违例 | `2166-2173` |
| 47 | ★ **只给选定的 DIRECT 队列插桩**：原来只判 `q` 非空，于是把**我们自己的 DIRECT 命令列表**丢给游戏的复制队列执行 —— 非法调用、设备 removed、游戏弹 `DXGI_ERROR_INVALID_CALL` 退出。自建宿主只有一条队列所以测不出来 | `2179-2189` |
| 48 | **只认 DIRECT 队列做时间戳/叠加**，且要先 `GetDesc` 判类型（游戏常常先提交复制队列，先到先得会记错） | `2160-2177` |
| 49 | **bundle 上 `EndQuery` 是非法操作**，而 bundle 与直连列表**共用同一个 vtable** → 必须用 `GetType()` 过滤 | `2228-2235` |
| 50 | **`Pending` 槽位有限（4 个）**，满了就放弃这一帧的 resolve，不能等 | `787-802` |
| 51 | **`EndList` 里没拿锁就不要瞎解锁**（`BeginList` 放弃过的情况） | `720-721` |
| 52 | **`BeginList` 里绝不能 `WaitForSingleObject`**：那是在游戏 Present 的调用栈里睡觉，直接变成游戏卡顿，还会污染我们自己的帧时间统计 | `685-687` |
| 53 | **分配器环不能太小**（只给 3 或 8 会让 GPU 稍微落后就全部 busy → 叠加永久停画），现在是 32 | `366-369` |
| 54 | **`gD12Broken` 是「永久停手」语义**：出过不可恢复的错（`Close`/`Reset` 失败、探测撞 SEH、连续 600 次拿不到分配器）就彻底放弃 D3D12 插桩，**帧时间路径继续工作** | `294-297`、`693-697`、`703-716`、`2276-2281`、`2322-2325` |
| 55 | **`Dispatch` 探针异常后不能补写另一半时间戳**，也必须复位 `gAiSpanOpen`，否则 `HarvestTimestamps` 会把没写过的 `TS_AI_END` 当有效结束点，报出荒唐的 Tensor 占用 | `2316-2326` |
| 56 | **设备移除检测**：GPU 侧非法操作会导致 device removed，游戏随即自杀式退出；`GetDeviceRemovedReason` 健康时返回 `S_OK`，拿到别的失败码说明设备指针本身可疑，也要记 | `1460-1470` |

### 22.7 日志与排查

| # | 坑 | 位置 |
| --- | --- | --- |
| 57 | ★ **日志文件写满后是从头覆盖的**：`SetFilePointer(FILE_BEGIN)` → **最新的行在文件开头，结尾反而是旧内容**。只看末尾会误判「日志里没有这一行」 | `529-535` |
| 58 | **日志前缀必须有墙钟时间 + 进程名 + pid**：只有「开机后毫秒数」对不上用户说的时间点；这个文件是所有被注入进程共用的，光看 pid 不知道是哪个游戏 | `538-561` |
| 59 | 日志上限从 64KB 提到 512KB：`present diag` / `frame dist` 每 5 秒各一条，64KB 只能存几分钟 | `532-534` |
| 60 | **`recent ft` 连续序列是判断「伪影 vs 真抖动」最直接的手段**：`17 17 17 25 17 17 25` = 真抖动；`17 17 17 3 3 28 28 17` = 帧内多次 Present 的伪影 | `1010-1026` |
| 61 | **`low compare`（merged vs raw）用来判断帧内合并是否生效或误合并** —— 光看一个数字分不出这两种情况 | `1028-1047` |
| 62 | **`overlay 5s: drawn/skipped` 用来区分闪烁的两种成因**（没画上 vs 画错缓冲），不分开就无法决定怎么改 | `1606-1629` |
| 63 | **`panel logical size` 只在尺寸变化时记** —— 「HUD 宽度随数值一直变化」这个问题靠它客观判定，理想情况整局只出现一次 | `872-886` |

---

## 23. 对未来的自己 / 其他迭代者的叮嘱

### 23.1 硬性要求

1. **改 `src/hook/` 下任何代码，必须同步更新本文件和 [`docs/CATALOG.md`](CATALOG.md)。**
   本文件里的行号、函数名、vtable 下标、口径描述一旦和代码不一致，就会误导后来者去改错的地方 ——
   而这里的错误代价是**把用户的游戏搞崩**，不是编译报错。
2. **改 vtable 下标前，先跑 `python tests/vt_check.py`**（`np_hook.cpp:29`）。
   **永远不要凭记忆数 `Vtbl`。** 历史上错过两次，后果是游戏直接崩。
3. **不要删除注释里的「为什么」。** `src/hook/` 的价值有一半在注释里：
   每一条长注释背后都是一次真实的崩溃或一次被用户抓到的错误读数。
   要精简就精简代码，注释里的证据（模块偏移、实测数值、用户日志）请保留。
4. **不要为了「顺手」加中间 `return`**（见 §22.2 坑 10）。
   要在 `PresentCommon` 里跳过某段，就置一个 `bool` 标志，让尾部收尾照常跑。
5. **不要在游戏线程上等待。** 任何一个 `WaitForSingleObject` / `lock` / `Sleep`
   放进 `Present` 的调用栈，都会变成玩家的卡顿（§22.6 坑 52）。
   拿不到资源就放弃这一帧。

### 23.2 新增钩子时的检查清单

- [ ] vtable 下标用 `tests/vt_check.py` 核对过，并在 `Vt::` 里加了命名注释（哪个接口的第几格）。
- [ ] 跳板函数第一件事判 `gUnloading`，第二件事判自己的前置条件，**失败一律原样转发一次**。
- [ ] 原函数指针存进对应的 `gOrig*` 全局；`RestoreAllHooks()` 里加了对应的还原，且顺序与安装相反。
- [ ] 有 `Idx3D12` 那样「可能被多个线程并发调用」的钩子 → 计数用 `std::atomic` + `fetch_add`。
- [ ] 有插桩（`EndQuery` 等）→ 用 `CanTimestamp()` 过滤列表类型，用 SEH 包住，异常时置 `gD12Broken`。
- [ ] 触及共享资源（`gList`、描述符堆、后台缓冲）→ 加锁用 `try_lock`，或改成「换新对象 + fence 回收」。
- [ ] 所有失败路径都能重试（除非语义是永久的，如 `gProbePoisoned` / `gD12Broken`），
      并明确写清「为什么这次可以永久放弃」。
- [ ] 加了 `Log()` 关键行（安装成功/失败、异常、口径变化），**日志是唯一的事后证据**。
- [ ] 更新 `NP_HOOK_*` 标志位（`np_common.h:389-401`）与 `docs/HOOK-FUNCTIONS.md` 的表格。
- [ ] 若新增/改动了 `NPTelemetry` / `NPSensors` / `NPConfig` 的**字段**：
      这是跨进程 ABI，必须走「加在末尾或占用 reserved 槽位」的方式，
      `version` 的 `sizeof` 自检要跟着一起验（`np_common.h:182-200`、`305-310`、`441-448`）。

### 23.3 已知缺口（可作为后续迭代的入口）

| 缺口 | 现状 | 备注 |
| --- | --- | --- |
| 没有 `ResizeBuffers` 钩子 | 只能靠 D3D12 换堆 / D3D11 每帧新建 RTV 间接应对 | 见 §11；补上能显著减少「全屏后闪烁」类问题 |
| `gCs` 死代码、`gCpuStartQpc` 只写不读、`drawSkips_` 只声明不计数 | 未确认是否有意保留 | 清理时注意 `gCs` 的 `DeleteCriticalSection` 也一起去掉 |
| `InModule()` 已无人使用 | 有意保留为教训备忘（`430-440`） | **不要「顺手删掉」**，注释说明它就是文档 |
| `lowWindowed` 口径不再被钩子层调用 | 保留在 `np_stats.h:138-166` 供对照 | 若恢复「Low 帧（严格）」开关，这里是入口 |
| `gReflexDev` / `nvapi64.dll` 卸载时不释放 | 见 §21.7 | 若释放，必须同时让 Reflex 状态可重置 |
| `gVpW/gVpH` 是 32 位标量竞态（注释认可「影响可忽略」） | `357` | 若要严谨可换成 `std::atomic<uint32_t>` |

---

## 附录 A：vtable 下标与补丁点对照

| 接口 | 成员 | 下标 | 补丁函数 | 原函数全局 | 还原点 |
| --- | --- | --- | --- | --- | --- |
| `IDXGISwapChain` | `Present` | 8 | `NpPresent` | `gSwapVts[i].origPresent` | `RestoreAllHooks` `2420` |
| `IDXGISwapChain1` | `Present1` | 22 | `NpPresent1` | `gSwapVts[i].origPresent1` | `2421` |
| `IDXGIFactory` | `CreateSwapChain` | 10 | `NpCreateSwapChain` | `gOrigCreateSC` | `2425` |
| `IDXGIFactory2` | `CreateSwapChainForHwnd` | 15 | `NpCreateSwapChainHwnd` | `gOrigCreateSCHwnd` | `2428` |
| `IDXGIFactory2` | `CreateSwapChainForComposition` | 24 | `NpCreateSwapChainComp` | `gOrigCreateSCComp` | `2431` |
| `ID3D12CommandQueue` | `ExecuteCommandLists` | 10 | `NpECL` | `gOrigECL` | `2435` |
| `ID3D12GraphicsCommandList4` | `DispatchRays` | 76 | `NpDispatchRays` | `gOrigDR` | `2438` |
| `ID3D12GraphicsCommandList4` | `BuildRaytracingAccelerationStructure` | 72 | `NpBuildAS` | `gOrigBAS` | `2440` |
| `ID3D12GraphicsCommandList` | `Dispatch` | 14 | `NpDispatch` | `gOrigDispatch` | `2442` |
| `ID3D12GraphicsCommandList` | `DrawInstanced` | 12 | `NpDrawInst` | `gOrigDrawInst` | `2444` |
| `ID3D12GraphicsCommandList` | `DrawIndexedInstanced` | 13 | `NpDrawIdx` | `gOrigDrawIdx` | `2446` |
| `ID3D12GraphicsCommandList` | `RSSetViewports` | 21 | `NpSetViewports` | `gOrigVP` | `2447` |

> 仅**读取**、不做补丁的下标：`SwapChain::GetBuffer(9)`、`GetFullscreenState(11)`、`GetDesc(12)`、
> `GetHwnd(20)`、`GetCurrentBackBufferIndex(36)`；`CommandQueue::Signal(14)`、
> `GetTimestampFrequency(16)`、`QueueGetDesc(19)`；`CommandList::Close(9)`、`Reset(10)`、
> `EndQuery(53)`、`ResolveQueryData(54)`。

## 附录 B：时间戳槽位与环

| 常量 | 值 | 含义 |
| --- | --- | --- |
| `TS_SLOTS` | 64 | 查询堆槽位数（`np_hook.cpp:79`） |
| `TS_BATCH_BASE` / `TS_BATCH_PAIRS` | 0 / 16 | 槽 0..31：逐批夹取 —— **当前已不使用**（§12.5） |
| `TS_RT_BASE` / `TS_RT_PAIRS` | 32 / 8 | 槽 32..47：`DispatchRays` 每对夹一次 |
| `TS_AI_START` / `TS_AI_END` | 48 / 49 | AI/后处理 compute 区间 |
| `NP_ALLOC_RING` | 32 | 命令分配器环（`np_hook.cpp:370`） |
| `gPending[4]` | 4 | readback 延迟收割槽（`np_hook.cpp:384`） |
| `gTs11[3]` | 3 | D3D11 三缓冲轮转（`np_hook.cpp:393`） |
| `NP_MAX_SWAPVT` | 4 | 交换链 vtable 小表容量（`np_hook.cpp:250`） |
| `rtvSlots_[8]` | 8 | D3D12 每后台缓冲一个 RTV 槽（`np_draw.h:85`） |
| `pending_[16]` | 16 | D3D12 fence 延迟回收队列（`np_draw.h:100`） |
