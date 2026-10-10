# NextPerf 钩子层 函数 / 接口速查表

> 配套文档：[`HOOK.md`](HOOK.md)（钩子层主文档，含每个功能的实现逻辑与坑）、[`CATALOG.md`](CATALOG.md)（全项目文件编目）。
>
> **行号基准**：本文档写作时的 `src/hook/np_hook.cpp`（2684 行）、`np_draw.cpp`（610 行）、
> `np_draw.h`（104 行）、`np_reflex.h`（128 行）、`dllmain.cpp`（26 行）。
> 只要改动过这些文件，**行号必须重新核对**（见 HOOK.md §23.1）。
>
> 列含义：
> - **位置**：`文件:起-止`（绝对行号）
> - **作用**：一句话
> - **被谁调用**：主要调用方（外部宿主调用单独标注）
> - **注意**：改这个函数时必须知道的事；`⚠` 表示踩过坑

---

## 1. 导出接口（DLL 边界）

| 函数名 | 位置 | 作用 | 被谁调用 | 注意 |
| --- | --- | --- | --- | --- |
| `DllMain` | `dllmain.cpp:13-26` | `ATTACH` → `NpHookAttach`；`DETACH` → `NpHookDetach` | Windows loader（注入器 `LoadLibrary`） | 先 `DisableThreadLibraryCalls`。**里面不做任何图形初始化**（loader lock） |
| `NpHookAttach(HMODULE)` | `np_hook.cpp:2657-2661` | 存 `gSelf`、`InitializeCriticalSection(&gCs)`、起 worker 线程 | `dllmain.cpp:17` | 立即返回；真正工作全在 worker 线程 |
| `NpHookDetach()` | `np_hook.cpp:2663-2684` | `gReady=false` → `RestoreAllHooks()` → 等 worker → `DeleteCriticalSection` | `dllmain.cpp:20`（`DLL_PROCESS_DETACH`） | ⚠ **必须还原 vtable**（`2665-2675`）；自卸载路径下不能等自己（`2676-2682`） |
| `gNpCountingOverlayDraws` | 定义 `np_hook.cpp:911`，声明 `np_draw.cpp:15` | `extern "C" thread_local bool`：叠加绘制期间抑制 draw 计数 | `NpDrawInst`/`NpDrawIdx` 读；两个 overlay 写 | ⚠ 必须 `thread_local`；定义必须在匿名命名空间**之外**（否则链接失败） |

---

## 2. 注入入口与 attach 流程

| 函数名 | 位置 | 作用 | 被谁调用 | 注意 |
| --- | --- | --- | --- | --- |
| `Worker(LPVOID)` | `np_hook.cpp:2556-2655` | attach 全流程 + 200ms 心跳循环 | `NpHookAttach` | 处理 `quit`/`detachPid`；每轮重跑 `DetectAi`；按策略探测；`!PresentHooked()` 时发最小遥测 |
| `OpenIpc()` | `np_hook.cpp:578-594` | 打开 Config/Sensors（只读）、按 pid 建 Telemetry（读写），`NPClearTelemetry` | `Worker` `2562` | 失败即 `return`（没有共享内存就没意义）。Telemetry 名按 PID 分开（`583-587`） |
| `Cfg()` | `np_hook.cpp:596-601` | 返回 `NPConfig` **值拷贝** | 全文件多处 | 先 `NPDefaultConfig` 兜底；magic 不对时用默认值 |
| `Sens()` | `np_hook.cpp:602-607` | 返回 `NPSensors` **值拷贝** | `RenderPanel` `867`、图历史 `1442`、GPU 回填 `1101` | 先 `NPClearSensors` 兜底；要求 `magic` 正确且 `valid` |
| `DetectAi()` | `np_hook.cpp:612-630` | 按已加载模块置 `gAiModules`（DLSS SR/RR/FG、XeSS、FSR、DirectML、ORT） | `Worker` `2564`、`2612` | 每轮心跳都重跑（游戏可能刚加载 DLSS） |
| `EnsureRt()` | `np_hook.cpp:1124-1135` | 渲染线程上的图形初始化：`npg::GfxInit` + `npb::GfxInit` + `gPanel.Init` | `PresentCommon` `1168` | ⚠ **只能在渲染线程调**（单线程 D2D 工厂）；⚠ 少了 `npb::GfxInit()` = 面板永远画不出来（`1128-1131`） |

---

## 3. vtable 补丁原语与健全性检查

| 函数名 | 位置 | 作用 | 被谁调用 | 注意 |
| --- | --- | --- | --- | --- |
| `Patch(void** vt, int idx, void* hook, void** orig)` | `np_hook.cpp:418-428` | `VirtualProtect` → 写槽位 → 还原保护属性；`orig` 可空 | 所有安装点 + `RestoreAllHooks` | 返回 `false` 表示没装上；**所有调用方都检查返回值** |
| `Namepace Vt::*`（下标表） | `np_hook.cpp:25-70` | 交换链/工厂/队列/命令列表的 vtable 下标 | 全文件 | ⚠⚠ **别凭记忆数**，用 `python tests/vt_check.py` 核对。历史错过两次 |
| `PlausibleCodePtr(p, a, b)` | `np_hook.cpp:1708-1716` | 判「已提交 + `PAGE_EXECUTE*` + 不等于同表另两个槽」 | `PatchSwapChainVtable`、`EnsureD3D12Hooks` | 只做「像代码」判断，**不按模块名过滤**（见 §12.3 / HOOK.md §12.3） |
| `PtrOwner(p)` | `np_hook.cpp:1726-1742` | 返回 `模块名+偏移` 字符串（诊断） | 各处日志 | 用于把**实际归属模块**写进日志；`GetModuleHandleExA(FROM_ADDRESS)` 反查 |
| `InModule(HMODULE, void*)` | `np_hook.cpp:435-440` | 判指针是否属于某模块 | **无人调用** | 有意保留为教训备忘：D3D12 实现其实在 **`D3D12Core.dll`**，按 `d3d12.dll` 判断会让光追/DLSS 钩子全装不上。**不要顺手删** |
| `PointsIntoOurDll(const void*)` | `np_hook.cpp:1917-1926` | 指针是否落在本 DLL 的 PE 映像范围（用 PE 头算，不依赖 psapi） | `InstallDxgi` `1952` | 自递归防线：上一次卸载没还干净时「原函数」其实是上一份 DLL 的钩子 |

---

## 4. 异常保护（VEH + setjmp）

| 函数名 | 位置 | 作用 | 被谁调用 | 注意 |
| --- | --- | --- | --- | --- |
| `IsFatalCode(DWORD)` | `np_hook.cpp:152-168` | 白名单：只有真正的致命异常才兜 | `NpSeh` `173` | ⚠ 必须筛：VEH 会收到 `DBG_PRINTEXCEPTION_C`（`OutputDebugString`）与 C++ 异常（`0xE06D7363`） |
| `NpSeh(PEXCEPTION_POINTERS)` | `np_hook.cpp:170-194` | VEH 回调：记录出错模块+偏移 → `longjmp(gJmp,1)` | Windows | ⚠ `thread_local gJmpArmed` 未置位时直接 `CONTINUE_SEARCH`；日志里的 `模块+偏移` 是定位驱动崩溃的唯一线索（实测 `nvwgf2umx.dll+0x330124`） |
| `SehReady()` | `np_hook.cpp:209-217` | 惰性注册 VEH，**保存句柄** | `PresentCommon` `1161`、`ProbeSwapChainVtable` `1808`、`NpDispatchRays` `2250`、`NpDispatch` `2301`/`2330` | ⚠⚠ 句柄必须存下来并在卸载前注销，否则**重复注入 100% 闪退**（HOOK.md §22.1 坑 1） |
| `gJmp` / `gJmpArmed` | `np_hook.cpp:141-142` | `thread_local std::jmp_buf` / `bool` | 各处 `setjmp`/`longjmp` | ⚠ 必须每线程一份；`longjmp` 后不要碰 setjmp 之后声明的对象 |
| `gSehHandle` | `np_hook.cpp:196` | `PVOID`，`AddVectoredExceptionHandler` 返回值 | `SehReady` 写；`RestoreAllHooks` `2411-2415` 注销 | `RemoveVectoredExceptionHandler` 后把 `gSehReady` 复位 |

---

## 5. vtable 探测与安装

| 函数名 | 位置 | 作用 | 被谁调用 | 注意 |
| --- | --- | --- | --- | --- |
| `ProbeSwapChainVtable()` | `np_hook.cpp:1792-1911` | 自造临时 D3D11 设备 + 8x8 隐藏窗口 + 交换链，拿 vtable 后立即销毁 | `Worker` `2638` | ⚠⚠ **有崩溃风险**：撞 SEH 即 `gProbePoisoned = true` 永久停手；窗口类注册在**宿主 exe** 模块；窗口必须**即时创建**（延迟创建会导致 `hwnd==null` → 探测什么都不做） |
| `gProbePoisoned` | `np_hook.cpp:1790` | 探测已污染标志 | `ProbeSwapChainVtable` `1806`/`1817` | 一旦置位**永久**不再探测（重试 = 多几次把游戏打崩的机会） |
| `PatchSwapChainVtable(IDXGISwapChain*)` | `np_hook.cpp:1745-1783` | 给一条交换链补 `Present`(8) / `Present1`(22)，记进 `gSwapVts` | 三个工厂跳板 + 探测 | 同一 vtable 只补一次；表满（`NP_MAX_SWAPVT=4`）返回 `false`；`Present1` 失败可接受 |
| `SwapVtFor(void** vt)` | `np_hook.cpp:254-258` | 按 vtable 指针查原函数条目 | `PresentCommon` `1142`、`PatchSwapChainVtable` `1749` | ⚠ blt / flip / composition 可能是**不同的类**，所以是小表不是单例 |
| `PresentHooked()` | `np_hook.cpp:259` | `gSwapVtCount > 0` | `Worker`、探测 | 是「Present 已挂上」的唯一判据 |
| `InstallDxgi()` | `np_hook.cpp:1928-1970` | 挂 `CreateSwapChain`(10) / `ForHwnd`(15) / `ForComposition`(24) | `Worker` `2565` | 只为读 vtable 创建工厂实例；**自递归防线**在 `1952-1958`（读到自己的钩子就宁可不装） |
| `EnsureD3D12Hooks(ID3D12Device*)` | `np_hook.cpp:1975-2106` | 首次 D3D12 Present 时惰性安装：队列钩子 + 查询堆/fence/分配器环 + 叠加 + （可选）命令列表钩子 | `PresentCommon` `1185` | ⚠ 失败可重试但**必须防并发重入**（`installing` 原子闸）；5 次上限 + 2 秒冷却；临时队列/列表只借 vtable，用完 `Release` |

---

## 6. 跳板函数（`extern "C"`，`np_hook.cpp:2112-2374`）

> 这些函数**签名必须与被 hook 的成员函数完全一致**（`__stdcall`、参数顺序），
> 否则栈会错位 —— 编译期也未必报错。

| 函数名 | 位置 | 作用的 vtable 槽 | 被谁调用 | 注意 |
| --- | --- | --- | --- | --- |
| `NpPresent` | `2114-2116` | `Present`(8) | 宿主进程的 DXGI | 纯转发到 `PresentCommon(..., isP1=false)` |
| `NpPresent1` | `2118-2121` | `Present1`(22) | 宿主 | 纯转发，`isP1=true` |
| `NpCreateSwapChain` | `2129-2135` | `IDXGIFactory::CreateSwapChain`(10) | 宿主 | 先 `Log`；成功且 `!gUnloading` 才补 Present |
| `NpCreateSwapChainHwnd` | `2137-2146` | `IDXGIFactory2::CreateSwapChainForHwnd`(15) | 宿主 | 同上；⚠ 若原函数缺失返回 `E_FAIL`（不会调用自己） |
| `NpCreateSwapChainComp` | `2148-2155` | `IDXGIFactory2::CreateSwapChainForComposition`(24) | 宿主 | 同上 |
| `NpECL` | `2157-2226` | `ID3D12CommandQueue::ExecuteCommandLists`(10) | 宿主提交线程 | ⚠ 只认 DIRECT 队列；锁定队列要 `AddRef`；只有 `q==gQueue12` 才插桩；`gFrameStarted` 用 CAS 抢且必须在 `EndList` 之前 |
| `NpDispatchRays` | `2237-2283` | `DispatchRays`(76) | 宿主提交线程 | ⚠ 槽位号用 `fetch_add` **返回值**；探针异常 → `gD12Broken` + **只转发一次**（历史 bug：转发两遍） |
| `NpBuildAS` | `2285-2290` | `BuildRaytracingAccelerationStructure`(72) | 宿主 | 只累加 `gAsBuilds` 后转发 |
| `NpDispatch` | `2292-2341` | `Dispatch`(14) | 宿主提交线程 | ⚠ 与 `DispatchRays` 同类坑；异常时复位 `gAiSpanOpen` 并永久停手 |
| `NpDrawInst` | `2343-2347` | `DrawInstanced`(12) | 宿主 + 我们自己的叠加 | `gNpCountingOverlayDraws` 时只转发不计数 |
| `NpDrawIdx` | `2349-2353` | `DrawIndexedInstanced`(13) | 宿主 + 叠加 | 同上 |
| `NpSetViewports` | `2355-2372` | `RSSetViewports`(21) | 宿主 | ⚠ 渲染分辨率**只认长宽比与输出一致**（`rel <= 1.05`）的最大视口 |

---

## 7. Present 主流程

| 函数名 | 位置 | 作用 | 被谁调用 | 注意 |
| --- | --- | --- | --- | --- |
| `PresentCommon(sc, sync, flags, pp, isP1)` | `np_hook.cpp:1137-1693` | 整个钩子层的心脏：防护 → 设备识别 → 分辨率/全屏 → 帧时间与帧内合并 → CPU 拆分 → Reflex → 时间戳收尾 → 遥测 → 叠加 → 调原函数 → **尾部收尾** | `NpPresent`/`NpPresent1` | ⚠⚠ **绝不在中间 `return`**（会跳过尾部收尾 → `gInPresent` 永远 `true` → 叠加永久失效）；⚠ `gUnloading` 判定必须在 `gTel` 解引用之前；⚠ 顺序：`setjmp`→`EnsureRt`→…→`CallOriginal`→CPU 拆分→复位状态 |

子步骤定位（都在 `PresentCommon` 内）与对应行号：

| 逻辑段 | 行 | 说明 |
| --- | --- | --- |
| 入口防护 | `1141-1167` | 反查原函数、`gUnloading`/`gTel`/`gInPresent` 三道闸、`SehReady`+`setjmp` |
| 首次接入/设备识别 | `1174-1200` | `GetDevice` 先 D3D11 后 D3D12，写 `processName` |
| 分辨率与全屏模式 | `1202-1227` | `GetDesc` / `GetFullscreenState` / `GetHwnd`+窗口样式 → `NP_PM_*` |
| 帧时间 + 帧内合并 | `1229-1292` | `DXGI_PRESENT_TEST` 过滤、`gRawStats`、阈值 `meanMs(240)*0.6` 夹 [2,30]、`gAbsorbThisPresent` |
| sim/submit 两段 | `1293-1332` | 只认 `gRenderTid`；渲染提交本质是排队 |
| Reflex 读取 | `1342-1402` | 设备只取一次；`SleepStatus` 1 秒一次；`Poll` 每帧 |
| `gLastPresentQpc` 推进 | `1403-1405` | **被合并时不推进**（合并机制本身） |
| `present diag` 5 秒诊断 | `1407-1438` | 调用数/real/test/sync 分布 + 帧时间极值 |
| 图表历史采样 | `1440-1458` | 每 8 帧一点；GPU 曲线**优先 PDH** `ss.gpuBusyMs` |
| 设备移除检测 | `1460-1470` | 每 128 帧一次 `GetDeviceRemovedReason` |
| GPU 时间戳收尾 | `1472-1484` | D3D12：`HarvestTimestamps` + `SubmitPendingResolve`；D3D11：`Ts11Tick` |
| 暂停判定 | `1486-1495` | `cfg.pauseHook`；⚠ 只跳过更新，**不 return** |
| 叠加：是否绘制 | `1497-1524` | `wantOverlay` 六条件；`updateHz` 限流；`settlePending` |
| 叠加：取缓冲并绘制 | `1524-1582` | ⚠ `GetCurrentBackBufferIndex()`；`stateOk=false` **只跳过本帧** |
| 叠加诊断日志 | `1586-1629` | `overlay NOT drawn` / `overlay drawn` / `overlay 5s` |
| 调原函数 + 计时 | `1631-1639` | `gJmpArmed=false`；`tPresent0`/`tPresent1`/`msInPresent` |
| **CPU Busy/Wait 拆分** | `1641-1681` | ⚠ 必须在 Present **返回之后**；三项同一次减法导出 |
| 尾部收尾 | `1683-1692` | `gRenderTid`/`gPresentRetQpc`/`gSimEndQpc=0`/`gFrameStarted=false`/`gInPresent=false` |

---

## 8. 帧时间统计

| 函数名 | 位置 | 作用 | 被谁调用 | 注意 |
| --- | --- | --- | --- | --- |
| `NowQpc()` | `np_hook.cpp:411-415` | `QueryPerformanceCounter` 包装 | 各处 | — |
| `QpcMs(uint64_t)` | `np_hook.cpp:416` | QPC 差值 → 毫秒 | 各处 | 分母 `gQpcFreq` 由 `Worker` `2558` 填；为 0 时返回 0 |
| `np::FrameStats::push` | `np_stats.h:24-28` | 环形缓冲压入一帧 | `gStats`/`gRawStats` | 容量 4096 |
| `np::FrameStats::meanMs(window)` | `np_stats.h:179-187` | 窗口均值（**帧内合并的基准**） | `PresentCommon` `1266` | ⚠ 不能用中位数当合并基准（双峰振荡）；调用处夹 [2,30] |
| `np::FrameStats::medianMs` | `np_stats.h:197-205` | 中位数 | **当前无人调用** | 保留；注释说明为何不能用于合并基准 |
| `np::FrameStats::avgFps(600)` | `np_stats.h:52-61` | 平均 FPS | `UpdateTelemetryCommon` `961` | ⚠ 临时缓冲必须是 `thread_local`（原 `static` 会算错） |
| `np::FrameStats::percentileMs / lowPercentileFps` | `np_stats.h:215-234` | 帧时间百分位 / 其倒数 | `UpdateTelemetryCommon` `1085-1088` | 线性插值 `index=(n-1)*p`，与 PresentMon `StatisticsTracker.cpp:35` 一致 |
| `np::FrameStats::recent / overRatio` | `np_stats.h:33-38` / `120-128` | 取最近 n 帧 / 超阈值比例 | `UpdateTelemetryCommon` 诊断块 | 采样率必须用 `frameTotal` 算，`count()` 会饱和 |
| `np::Ema(alpha)` | `np_stats.h:243-257` | 指数移动平均 | `frameMsAvg`、三个 CPU EMA | alpha 均为 0.10 |
| `gStats` / `gRawStats` | `np_hook.cpp:350` / `324` | 合并后序列 / **原始**序列 | `PresentCommon` | ⚠ 两者不可混用：`gRawStats` 只用来求合并阈值 |
| `gLastPresentQpc` / `gPrevPresentQpc` | `np_hook.cpp:311` / `335` | 上一次**记账** / 上一次**呈现** | `PresentCommon` | ⚠ 被合并时前者不推进、后者无条件推进（这是合并能成立的前提） |
| `gAbsorbThisPresent` / `gMergedCount` | `np_hook.cpp:315` / `325` | 本次是否被并入上一帧 / 累计合并数 | `PresentCommon` | `gMergedCount` 在诊断日志里清零 |

---

## 9. 遥测发布

| 函数名 | 位置 | 作用 | 被谁调用 | 注意 |
| --- | --- | --- | --- | --- |
| `UpdateTelemetryCommon(cfg, nowQpc, realPresent)` | `np_hook.cpp:913-1117` | 完整遥测：状态、计数器、FPS 窗口计数、RT/Tensor 占比、Low 帧、PDH GPU 回填、图表、清零计数器 | `PresentCommon` `1495`（未暂停时） | ⚠ `fps` 窗口只数 `realPresent`；⚠ RT/Tensor 分母用 `frameMs` 不用 `gpuFrameMs`；⚠ 末尾必须清 `gDraws/gDispatches/gAsBuilds` |
| `PublishMinimalTelemetry()` | `np_hook.cpp:2517-2554` | 心跳：`tickMs`/`gfxApi`/`processName`/`lastError` | `Worker` `2644`（仅 `!PresentHooked()`） | 把「已注入但没数据」变成一句能看懂的话；按 API 分支写文案（`2532-2553`） |
| `GuessApiFromModules()` | `np_hook.cpp:2506-2513` | 按模块猜 API（含 Vulkan/OpenGL） | `PublishMinimalTelemetry` `2521` | 与 `HostApiGuess` 不同：后者排除了本 DLL 自己导入的模块 |
| `NPClearTelemetry` / `NPClearSensors` | `np_common.h:472-497` / `441-470` | 结构体清零 + 写 `sizeof` 版本 | `OpenIpc` `591`；`Sens()` `604` | ⚠ `version` 写的是 `sizeof`，读方用它做布局自检 |

---

## 10. D3D12 时间戳与命令列表资源

| 函数名 | 位置 | 作用 | 被谁调用 | 注意 |
| --- | --- | --- | --- | --- |
| `InitTs12(dev, q)` | `np_hook.cpp:635-669` | 建查询堆（64 槽）、4 个 readback、fence、`NP_ALLOC_RING=32` 个分配器、一条命令列表并 `Close()` | `EnsureD3D12Hooks` `2021` | 任一失败返回 `false`（会走日志分支）；`gTsFreq` 拿不到退 1000000 |
| `BeginList()` | `np_hook.cpp:671-718` | 取分配器环下一个槽 → 检查 fence → `Reset` 分配器与命令列表 → 返回 `gList` | 叠加绘制 `1573`、`SubmitPendingResolve` `793` | ⚠ `try_lock`（抢不到就放弃本帧，**绝不阻塞**）；⚠ **绝不等 GPU**；⚠ 连续失败 > 600 → `gD12Broken`；任何 `Reset` 失败 → `gD12Broken` |
| `EndList(q)` | `np_hook.cpp:720-741` | `Close` → `ExecuteCommandLists` → `Signal(gFence)` → 记 `gAllocFence` → 同步叠加 fence → 解锁 | 同 `BeginList` 的两处 | ⚠ 没拿锁时直接返回（不要瞎解锁）；`Close` 失败 → `gD12Broken` |
| `HarvestTimestamps()` | `np_hook.cpp:743-785` | fence 完成后 `Map` readback，算 GPU/RT/AI 毫秒并写遥测 | `PresentCommon` `1477` | ⚠ 单批 > 50ms 判为配对出错，丢弃；`0 < gpuMs < 1000` 才写；RT/Tensor 百分比**不在这里算** |
| `SubmitPendingResolve(q, rtCount, aiUsed, batchCount)` | `np_hook.cpp:787-802` | 找一个空 `Pending` 槽，`ResolveQueryData(0, TS_SLOTS)` 到 readback | `PresentCommon` `1478` | 4 个槽都忙就**放弃这一帧**（不等待） |
| `CanTimestamp(cl)` | `np_hook.cpp:2232-2235` | 列表是否允许插时间戳（必须 `DIRECT`） | `NpDispatchRays` `2258`、`NpDispatch` `2304` | ⚠ bundle 与直连列表**共用 vtable**，bundle 上 `EndQuery` 非法 → 必须用 `GetType()` 过滤 |
| `gD12Lock` / `gD12Locked` | `np_hook.cpp:291-292` | 全局命令列表的互斥 + 线程重入标记 | `BeginList`/`EndList` | ⚠ 用 `try_lock`；`thread_local` 标记防止同线程重入 |
| `gD12Broken` | `np_hook.cpp:297` | D3D12 侧永久停手标志 | 多处读写 | 语义是**永久的**；置位后帧时间路径继续工作 |
| `gAlloc[32]` / `gAllocFence[32]` / `gAllocCur` / `gBeginListFails` | `np_hook.cpp:370-375` | 分配器环与状态 | `BeginList`/`EndList` | ⚠ 环太小（3/8）会让叠加**永久停画** |
| `gPending[4]` | `np_hook.cpp:377-384` | readback 延迟收割槽 | `HarvestTimestamps`/`SubmitPendingResolve` | `struct Pending` 定义在同处 |
| `InitTs11(dev)` / `Ts11Tick(seq)` | `np_hook.cpp:807-817` / `819-848` | D3D11 三缓冲时间戳 | `PresentCommon` `1182` / `1483` | `GetData` 返回 `S_OK` 才算就绪；要求 `!dj.Disjoint` 且 `0 < ms < 1000` |

---

## 11. 叠加绘制（`src/hook/np_draw.*`）

### 11.1 全局与编排

| 函数名 | 位置 | 作用 | 被谁调用 | 注意 |
| --- | --- | --- | --- | --- |
| `npg::GfxInit()` | `np_draw.cpp:74-81` | `npb::GfxInit()` + `LoadLibraryW("d3dcompiler_47.dll")` 取 `D3DCompile` | `EnsureRt` `1127` | 运行时编译着色器，不依赖预编译 blob |
| `npg::GfxShutdown()` | `np_draw.cpp:83-86` | `npb::GfxShutdown()` + 释放 d3dcompiler | `SelfUnloadNow` `2474` | `gCompiler`/`gD3DCompile` 一并清空 |
| `CompileShader(entry, target, out)` | `np_draw.cpp:88-95` | 编译内置全屏三角形 VS/PS | 两个 `Init` | 源码 `kShaderSrc` 在 `np_draw.cpp:61-72`；错误 blob 会被 `Release` |
| `Overlay11::Draw(...)` | `np_draw.cpp:155-252` | D3D11 路径：动态纹理上传 + 每帧新建 RTV + `Draw(3,0)` | `PresentCommon` `1552` | ⚠⚠ **RTV 每帧新建、本帧放掉**（长期引用后台缓冲会让游戏 `ResizeBuffers` 失败 → 闪退）；⚠ 必须绑 `CULL_NONE` 光栅状态；⚠ 视口定位面板矩形 |
| `Overlay12::Record(...)` | `np_draw.cpp:400-608` | D3D12 路径：上传堆 → `CopyTextureRegion` → 屏障 → 全屏三角形 | `PresentCommon` `1575`（由 `BeginList`/`EndList` 包住） | ⚠⚠ 描述符堆**换堆**而不是覆盖槽位；⚠ 换堆后 `settle_ = 30`；⚠ 上传缓冲走 fence 延迟回收 |
| `DrawCb(rt, ud)` | `np_hook.cpp:858-861` | D2D 回调：`PanelRenderer::Render` | `PanelBitmap::Render`（`np_hook.cpp:888`） | `DrawCtx` 定义在 `853-857` |
| `RenderPanel(cfg)` | `np_hook.cpp:866-892` | `BuildPanelData` → `Measure` → `PanelBitmap::Render` → 记 `gBw/gBh` | `PresentCommon` `1520`、`1549`、`1571` | 尺寸变化时记一条 `panel logical size` 诊断日志 |
| `gBw` / `gBh` | `np_hook.cpp:863` | 面板纹理**物理像素**尺寸（含光栅倍率） | 叠加路径 | `gBw>0 && gBh>0` 是「可以画」的前置条件 |

### 11.2 `Overlay11`（`np_draw.h:27-46`）

| 函数名 | 位置 | 作用 | 被谁调用 | 注意 |
| --- | --- | --- | --- | --- |
| `Overlay11::Init(dev)` | `np_draw.cpp:98-141` | 编译 VS/PS、采样器、**预乘 alpha** 混合、`CULL_NONE` 光栅状态 | `PresentCommon` `1181` | ⚠ D3D11 默认 `CullMode=BACK`，全屏三角形正好是背面 → 不建这个状态则 `Draw` 成功但屏幕无内容 |
| `Overlay11::Release()` | `np_draw.cpp:143-153` | 释放全部成员 | `SelfUnloadNow` `2470` | — |
| 成员 | `np_draw.h:36-45` | `vs_/ps_/tex_/srv_/blend_/samp_/raster_/rtv_/rtvSrc_/texW_/texH_` | — | ⚠ `texW_/texH_` 必须在纹理与 SRV 都建好之后才写（`np_draw.cpp:172-181`）；`rtvSrc_` 是历史遗留（现在不再 `AddRef` 后台缓冲） |

### 11.3 `Overlay12`（`np_draw.h:49-102`）

| 函数名 | 位置 | 作用 | 被谁调用 | 注意 |
| --- | --- | --- | --- | --- |
| `Overlay12::Init(dev)` | `np_draw.cpp:255-322` | 根签名（SRV 表 + 静态采样器）、SRV 堆（1）、RTV 堆（8）、fence | `EnsureD3D12Hooks` `2022` | PSO **不在这里建**（依赖后台缓冲格式） |
| `Overlay12::EnsurePso(dev, rtvFormat)` | `np_draw.cpp:324-356` | 按后台缓冲格式建/换 PSO | `Record` `407` | 旧 PSO 走 `Retire` 延迟回收 |
| `Overlay12::Retire(IUnknown*)` | `np_draw.cpp:379-385` | 挂到 fence 队列延迟回收（上限 16） | `EnsurePso`、`Record`、换堆 | ⚠ 立刻 `Release` 一个「已录进命令列表但 GPU 没执行完」的资源 = 释放正在使用的显存 |
| `Overlay12::OnFrameCompleted()` | `np_draw.cpp:387-398` | fence 确认后真正 `Release` | `Record` `405`/`463` | 每帧回收一次 |
| `Overlay12::Release()` | `np_draw.cpp:358-374` | 清空 pending + 释放全部 + **重置 `psoFormat_`** | `SelfUnloadNow` `2471` | ⚠ 不重置 `psoFormat_` 会让重新 `Init` 后的 `EnsurePso` 误判 |
| `Overlay12::fence()` / `fenceValuePtr()` | `np_draw.h:58-59` | 给 `EndList` 用来 `Signal` 并写值 | `EndList` `734-738` | 值必须与 `gFenceVal` 同步推进 |
| `Overlay12::HeapSwaps()` | `np_draw.h:62` | RTV 堆被换过几次（诊断） | 叠加 5s 日志 `1625` | 暴涨说明游戏在频繁重建交换链 |
| `Overlay12::settlePending()` / `tickSettle()` | `np_draw.h:65-66` | 交换链重建后的静默帧计数 | `PresentCommon` `1500-1501` | 置位点是 `np_draw.cpp:548`（`settle_ = 30`） |
| 成员 | `np_draw.h:73-101` | `root_/pso_/vsBlob_/psBlob_/psoFormat_/tex_/srvHeap_/rtvHeap_/rtvSlots_[8]/rtvSlotsN_/rtvHeapSwaps_/settle_/drawSkips_/rtvHeapSize_/texW_/texH_/fence_/fenceValue_/pending_[16]/pendingN_` | — | `drawSkips_` 只声明未使用；`rtvSlots_` 是 `struct RtvSlot { ID3D12Resource* res; int slot; }` 数组 |

---

## 12. NvAPI Reflex（`src/hook/np_reflex.h`，header-only）

| 函数 / 结构 | 位置 | 作用 | 被谁调用 | 注意 |
| --- | --- | --- | --- | --- |
| `np::NVFrameReport` | `np_reflex.h:24-42` | 逐字段对齐 `nvapi.h` 的帧报告（sim/submit/present/driver/os/gpu 各起止时刻 + `gpuActiveRenderTimeUs` / `gpuFrameTimeUs`） | `ReflexReader::Poll` 输出 | ⚠⚠ **不能改字段顺序/宽度**，否则解析全是垃圾 |
| `np::NVLatencyParams` / `NVGetSleepStatusParams` | `np_reflex.h:44-48` / `54-58` | NVAPI 入参结构（64 帧报告 / 低延迟状态） | `Poll` / `SleepStatus` | `NvBool` 是 32 位整数 |
| `NP_NVAPI_VERSION(s, v)` | `np_reflex.h:51` | `sizeof(s) | (v << 16)` | 两个入参填充 | 版本号必须与结构大小绑定 |
| `ReflexReader::Init()` | `np_reflex.h:63-81` | 载入 `nvapi64.dll`（失败退 `nvapi32.dll`）→ `nvapi_QueryInterface` → `NvAPI_Initialize` → 取两个接口 | `PresentCommon` `1357` | 幂等；`NvAPI_Initialize` 返回非 0 视为失败；**没有对应的 `FreeLibrary`**（见 HOOK.md §21.7） |
| `ReflexReader::ok()` | `np_reflex.h:83` | `getLatency_ != nullptr` | `PresentCommon` `1359`/`1381` | — |
| `ReflexReader::Poll(dev, out)` | `np_reflex.h:87-107` | 取最近一帧报告：从 64 帧里挑 `frameID` 最大且时刻非 0 的；剔除「结束早于开始」 | `PresentCommon` `1383` | 返回 `false` 表示这次没数据（游戏没用 Reflex）→ 调用方必须保持回退路径 |
| `ReflexReader::SleepStatus(dev, &on)` | `np_reflex.h:111-118` | **驱动直证**低延迟是否开启 | `PresentCommon` `1369`（1 秒一次） | 返回 `true` 才代表「驱动明确回答了」；比用 CPU Wait 推断可靠（那是旁证） |
| `ReflexReader::hasSleepStatus()` | `np_reflex.h:120` | 该接口是否可用 | `PresentCommon` `1363` | — |
| 接口 ID 常量 | `np_reflex.h:73-79` | `NvAPI_Initialize=0x0150E828`、`NvAPI_D3D_GetLatency=0x1A587F9C`、`NvAPI_D3D_GetSleepStatus=0xAEF96CA1` | `Init` | GetLatency 的 ID 来自 Intel PresentMon 的 `nvapi_interface_table.h` |
| `gReflex` / `gReflexDev` / `gReflexTried` | `np_hook.cpp:340-342` | 读取器实例 / D3D 设备（AddRef 后**不释放**）/ 只试一次标记 | `PresentCommon` | 字段写入：`simMs`/`submitMs` 被 Reflex 值与启发式值**共用**（读到就覆盖） |

---

## 13. 卸载与还原

| 函数名 | 位置 | 作用 | 被谁调用 | 注意 |
| --- | --- | --- | --- | --- |
| `RestoreAllHooks()` | `np_hook.cpp:2397-2453` | **幂等**还原：记日志 → 注销 VEH → 交换链表 → 工厂 → 队列 → 命令列表 → 指针置空 | `SelfUnloadNow` `2466`、`NpHookDetach` `2675` | ⚠⚠ 必须从**两条路径**都调（只写在自卸载里会让补丁留在原地 → 下次注入自递归栈溢出）；⚠ VEH 注销是「重复注入 100% 闪退」的根因修复 |
| `SelfUnloadNow()` | `np_hook.cpp:2462-2501` | `gUnloading=true` → `Sleep(150)` → `RestoreAllHooks` → `Sleep(250)` → 释放图形/采集资源 → 释放互斥量与共享内存 → `FreeLibraryAndExitThread` | `Worker` `2602`/`2610` | ⚠ 顺序不能乱（先直通、再还原、排空后才允许 `FreeLibrary`）；**不返回** |
| `detachPid` 判定 | `np_hook.cpp:2607-2611` | `gCfg->detachPid == GetCurrentProcessId()` → `SelfUnloadNow()` | worker 循环（200ms 轮询） | 字段定义 `np_common.h:193-199`；**为什么不用 `CreateRemoteThread`** 见 `np_common.h:196-198`（ASLR + 跨位数） |
| `quit` 判定 | `np_hook.cpp:2602` | 主程序退出 → `SelfUnloadNow()` | worker 循环 | 条件含 `magic == NP_MAGIC` |
| `gWorker` / `gInjectedMutex` | `np_hook.cpp:2379-2380` | worker 线程句柄 / 防重复注入互斥量 | `NpHookAttach` / `Worker` / `NpHookDetach` | 互斥量在 `Worker` `2567-2573` 创建并持有到进程结束；`NpHookDetach` 里**不能等自己** |

---

## 14. 日志与诊断

| 函数名 | 位置 | 作用 | 被谁调用 | 注意 |
| --- | --- | --- | --- | --- |
| `Log(fmt, ...)` | `np_hook.cpp:520-573` | 写 `%TEMP%\NextPerfHook.log`，前缀 = 墙钟 + 进程名 + pid + tid | 全文件 | ⚠⚠ 写满 512KB 后**回到文件开头覆盖** → **最新的行在开头，结尾是旧内容**，排查别只看末尾。格式/编码约定见 `docs/LOGGING.md` 第 2 节 |
| 关键诊断日志（行号） | `1133` render-thread gfx init；`1229` reflex init；`1376` reflex sleep status；`1396` reflex frame；`1430` present diag 5s；`1594` overlay NOT drawn；`1602` overlay drawn；`1623` overlay 5s；`1673` cpu split avg；`2400` RestoreAllHooks；`2561` attach；`2588` probe 计划；`2637` probe attempt | — | 这些是事后定位问题的**唯一证据**，改动相关路径时不要删 | — |

---

## 15. 共享内存接口速查（`src/common/np_common.h`）

| 符号 | 位置 | 作用 | 钩子层使用点 | 注意 |
| --- | --- | --- | --- | --- |
| `NP_SHM_CONFIG` / `NP_SHM_SENSORS` | `np_common.h:22-23` | 共享内存名字 | `OpenIpc` `579`/`581` | 只读映射（`FILE_MAP_READ`） |
| `NPTelemetryShmName(pid, out, n)` | `np_common.h:30-45` | 拼 `Local\NextPerf_Telemetry_v1_<pid>` | `OpenIpc` `585` | 自带十进制转换，不用 `swprintf` |
| `NPConfig` | `np_common.h:151-201` | 配置块 | `Cfg()` | 钩子关心的字段：`pauseHook`(188)、`overlayMode`(173)、`updateHz`(176)、`deepEngineHook`(177)、`vtableProbe`(178)、`quit`(180)、`detachPid`(199)、`offsetX/offsetY`(167-168)、`scale`(163) |
| `NPSensors` | `np_common.h:236-300` | 传感器块 | `Sens()` | 钩子只真正用 `gpuBusyMs`（GPU 曲线/GUI 回填，`289`）、`cpuUsage`/`gpuUsage`（图表，`243`/`259`） |
| `NPTelemetry` | `np_common.h:303-387` | 遥测块（钩子写、主程序读） | `UpdateTelemetryCommon`、`PublishMinimalTelemetry`、各跳板 | ⚠ `version` = `sizeof`，跨进程 ABI；新增字段只能加在末尾或用 `reserved` |
| `NP_HOOK_*` 标志 | `np_common.h:389-401` | `PRESENT/QUEUE/CMDLIST/TIMESTAMP/OVERLAY/REFLEX/REFLEX_KNOWN` | 各安装点 + `Worker` `2646-2652` | 主程序判断「挂到哪一步」的唯一依据 |
| `NP_C_CPU_BUSY` / `NP_C_CPU_WAIT` | `np_common.h:93-96` | CPU 两个半边的计数器位 | 面板 | **故意不在 `NP_ALL_COUNTERS`** 里（默认不显示） |
| `NPHistoryPush` | `np_common.h:223-233` | 游戏内图表环形缓冲压入（**12 个参数**） | `PresentCommon` `1454-1457` | 参数顺序：`fps,avg,low1,low01,ucpu,ugpu,latFrame,latCpu,latGpu,latBusy,latWait` |
| `NPCopyStr` | `np_common.h:404-411` | 安全字符串拷贝 | `SelfImports` `468`、`processName` 各处 | 保证 `'\0'` 结尾 |

---

## 16. 未使用 / 历史遗留（清理前先读这里）

| 符号 | 位置 | 状态 | 说明 |
| --- | --- | --- | --- |
| `InModule(HMODULE, void*)` | `np_hook.cpp:435-440` | **无人调用** | 有意保留为教训备忘（D3D12 在 `D3D12Core.dll`）。**不要顺手删** |
| `gCs` | `np_hook.cpp:228`（初始化 `2659`、销毁 `2683`） | 死代码 | 全文件**没有 `EnterCriticalSection`** |
| `gCpuStartQpc` | `np_hook.cpp:343` | 只写不读（写点 `2199`） | 原为「本帧 CPU 起点」 |
| `Overlay11::rtvSrc_` | `np_draw.h:44` | 历史遗留 | 曾用于缓存并 `AddRef` 后台缓冲 —— 那个做法已被判定为**严重违反 DXGI 规则**（`np_draw.cpp:191-201`） |
| `Overlay12::drawSkips_` | `np_draw.h:93` | 只声明不计数 | 原计划用于「闪烁是不是因为我们没画上去」的诊断（该诊断现由 `np_hook.cpp:1615-1629` 的 `overlay 5s` 日志承担） |
| `np::FrameStats::lowWindowed` / `lowPct` / `lowIntegral` / `medianMs` | `np_stats.h:138-166` / `104-117` / `76-96` / `197-205` | 钩子层**当前不调用** | 保留供对照；`lowWindowed` 是「Low 帧（严格）」开关恢复时的入口 |
| `TS_BATCH_BASE` / `TS_BATCH_PAIRS` 与 `HarvestTimestamps` 里的批次求和 | `np_hook.cpp:80-81`、`757-766` | **已不再产生数据** | 逐批夹取已整个拆掉（`2211-2224`），`batchCount` 现在恒由 `gBatchCount`（恒 0）传入 |
| `gVpW` / `gVpH` | `np_hook.cpp:357` | 32 位标量竞态（注释认可可忽略） | 要严谨可换 `std::atomic<uint32_t>` |
| `np_reflex.h` 的 `dll_` | `np_reflex.h:123` | 无 `FreeLibrary` | 见 HOOK.md §21.7 |

---

## 17. 相关但**不在**钩子层的接口

| 符号 | 位置 | 说明 |
| --- | --- | --- |
| `np::EtwMonitor` / `np::Etw()` | `src/etw/np_etw.h` / `np_etw.cpp` | ETW 帧计时，**外置**（不注入游戏），由主程序驱动。钩子层不含任何 ETW 代码 |
| `np::PanelRenderer` | `src/common/np_panel.h` | 真正的 D2D 面板绘制（`Render` 由 `DrawCb` `np_hook.cpp:860` 调用） |
| `npb::PanelBitmap` / `npb::RasterScale()` / `SetRasterScale()` / `GfxInit()` / `GfxShutdown()` / `Factory()` | `src/common/np_bitmap.h` | 面板位图（D2D DIB）；被钩子、桌面叠加、主程序三方复用 |
| `np::BuildPanelData` | 声明 `src/common/np_build.h:10`，实现 `np_build.cpp:199` | 由遥测 + 传感器 + 配置组装 `PanelData`（调用点 `np_hook.cpp:867`） |
