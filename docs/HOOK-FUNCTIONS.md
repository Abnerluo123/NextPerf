# NextPerf 钩子层 函数 / 接口速查表

> 配套文档：[`HOOK.md`](HOOK.md)（钩子层主文档，含每个功能的实现逻辑与坑）、[`CATALOG.md`](CATALOG.md)（全项目文件编目）。
>
> ✅ **本文一律不引用源码行号（2026-10-11 起）**：全表只按**函数名 / 变量名**定位；原来的「位置」列已改成「所在文件」。
> 改动原因：旧的行号基准（`src/hook/np_hook.cpp` = 2755 行）早已失效 —— 它现在是 2971 行
> （`np_draw.cpp` = 625、`np_draw.h` = 119），而 `HOOK.md` 用的又是另一套基准，同一函数在两份文档里常差 11~43 行。
> 行号是每改一次代码就会漂移的**派生数据**，函数名才是稳定标识：把行号写进文档，等于留一份必然过期的地图。
> 这条口径债（`TODO.md` 的 **D1** / **T9**）到此结清，**后续新增内容也不许再写行号**。
> **请直接在源码里按函数名搜索定位**：
> ```
> Select-String -Path src\hook\np_hook.cpp -Pattern '^(HRESULT PresentCommon|bool BeginList|void __stdcall NpECL)'
> ```
> 「函数名 / 作用 / 被谁调用 / 注意」这四列是稳定的，`所在文件` 只回答「去哪个文件里搜」。
>
> 列含义：
> - **所在文件**：符号所在的源文件（**不含行号**；行号会漂移，一律不给）
> - **作用**：一句话
> - **被谁调用**：主要调用方（外部宿主调用单独标注）
> - **注意**：改这个函数时必须知道的事；`⚠` 表示踩过坑

---

## 1. 导出接口（DLL 边界）

| 函数名 | 所在文件 | 作用 | 被谁调用 | 注意 |
| --- | --- | --- | --- | --- |
| `DllMain` | `dllmain.cpp` | `ATTACH` → `NpHookAttach`；`DETACH` → `NpHookDetach` | Windows loader（注入器 `LoadLibrary`） | 先 `DisableThreadLibraryCalls`。**里面不做任何图形初始化**（loader lock） |
| `NpHookAttach(HMODULE)` | `np_hook.cpp` | 存 `gSelf`、`InitializeCriticalSection(&gCs)`、起 worker 线程（`Worker`） | `DllMain` 的 `DLL_PROCESS_ATTACH` 分支 | 立即返回；真正工作全在 worker 线程 |
| `NpHookDetach()` | `np_hook.cpp` | `gReady=false` → `RestoreAllHooks()` → 等 worker → `DeleteCriticalSection` | `DllMain` 的 `DLL_PROCESS_DETACH` 分支 | ⚠ **必须还原 vtable**（走的就是 `RestoreAllHooks`）；自卸载路径下不能等自己（`GetThreadId(gWorker) != GetCurrentThreadId()` 才等） |
| `gNpCountingOverlayDraws` | 定义 `np_hook.cpp`，声明 `np_draw.cpp` | `extern "C" thread_local bool`：叠加绘制期间抑制 draw 计数 | `NpDrawInst`/`NpDrawIdx` 读；两个 overlay 写 | ⚠ 必须 `thread_local`；定义必须在匿名命名空间**之外**（否则链接失败） |

---

## 2. 注入入口与 attach 流程

| 函数名 | 所在文件 | 作用 | 被谁调用 | 注意 |
| --- | --- | --- | --- | --- |
| `Worker(LPVOID)` | `np_hook.cpp` | attach 全流程 + 200ms 心跳循环 | `NpHookAttach` | 处理 `quit`/`detachPid`；每轮重跑 `DetectAi`；按策略探测；`!PresentHooked()` 时发最小遥测 |
| `OpenIpc()` | `np_hook.cpp` | 打开 Config/Sensors（只读）、按 pid 建 Telemetry（读写），`NPClearTelemetry` | `Worker` | 失败即 `return`（没有共享内存就没意义）。Telemetry 名按 PID 分开（`NPTelemetryShmName`） |
| `Cfg()` | `np_hook.cpp` | 返回 `NPConfig` **值拷贝** | 全文件多处 | 先 `NPDefaultConfig` 兜底；magic 不对时用默认值 |
| `Sens()` | `np_hook.cpp` | 返回 `NPSensors` **值拷贝** | `RenderPanel`、图历史采样、GPU 回填 | 先 `NPClearSensors` 兜底；要求 `magic` 正确且 `valid` |
| `DetectAi()` | `np_hook.cpp` | 按已加载模块置 `gAiModules`（DLSS SR/RR/FG、XeSS、FSR、DirectML、ORT） | `Worker`（attach 时 + 每轮心跳） | 每轮心跳都重跑（游戏可能刚加载 DLSS） |
| `EnsureRt()` | `np_hook.cpp` | 渲染线程上的图形初始化：`npg::GfxInit` + `npb::GfxInit` + `gPanel.Init` | `PresentCommon` | ⚠ **只能在渲染线程调**（单线程 D2D 工厂）；⚠ 少了 `npb::GfxInit()` = 面板永远画不出来（`npg::GfxInit` 里就是先调它） |

---

## 3. vtable 补丁原语与健全性检查

| 函数名 | 所在文件 | 作用 | 被谁调用 | 注意 |
| --- | --- | --- | --- | --- |
| `Patch(void** vt, int idx, void* hook, void** orig)` | `np_hook.cpp` | `VirtualProtect` → 写槽位 → 还原保护属性；`orig` 可空 | 所有安装点 + `RestoreAllHooks` | 返回 `false` 表示没装上；**所有调用方都检查返回值** |
| `namespace Vt`（下标枚举表） | `np_hook.cpp` | 交换链/工厂/队列/命令列表的 vtable 下标 | 全文件 | ⚠⚠ **别凭记忆数**，用 `python tests/vt_check.py` 核对。历史错过两次 |
| `PlausibleCodePtr(p, a, b)` | `np_hook.cpp` | 判「已提交 + `PAGE_EXECUTE*` + 不等于同表另两个槽」 | `PatchSwapChainVtable`、`EnsureD3D12Hooks` | 只做「像代码」判断，**不按模块名过滤**（见 §12.3 / HOOK.md §12.3） |
| `PtrOwner(p)` | `np_hook.cpp` | 返回 `模块名+偏移` 字符串（诊断） | 各处日志 | 用于把**实际归属模块**写进日志；`GetModuleHandleExA(FROM_ADDRESS)` 反查 |
| `InModule(HMODULE, void*)` | `np_hook.cpp` | 判指针是否属于某模块 | **无人调用** | 有意保留为教训备忘：D3D12 实现其实在 **`D3D12Core.dll`**，按 `d3d12.dll` 判断会让光追/DLSS 钩子全装不上。**不要顺手删** |
| `PointsIntoOurDll(const void*)` | `np_hook.cpp` | 指针是否落在本 DLL 的 PE 映像范围（用 PE 头算，不依赖 psapi） | `InstallDxgi` | 自递归防线：上一次卸载没还干净时「原函数」其实是上一份 DLL 的钩子 |

---

## 4. 异常保护（VEH + setjmp）

| 函数名 | 所在文件 | 作用 | 被谁调用 | 注意 |
| --- | --- | --- | --- | --- |
| `IsFatalCode(DWORD)` | `np_hook.cpp` | 白名单：只有真正的致命异常才兜 | `NpSeh` | ⚠ 必须筛：VEH 会收到 `DBG_PRINTEXCEPTION_C`（`OutputDebugString`）与 C++ 异常（`0xE06D7363`） |
| `NpSeh(PEXCEPTION_POINTERS)` | `np_hook.cpp` | VEH 回调：记录出错模块+偏移 + **崩溃上下文快照** → `longjmp(gJmp,1)` | Windows | ⚠ `thread_local gJmpArmed` 未置位时直接 `CONTINUE_SEARCH`；日志里的 `模块+偏移` 是定位驱动崩溃的唯一线索（实测 `nvwgf2umx.dll+0x330124`）；`SEH ctx:` 一行打出 api/inPresent/presentHooked/frames/dev/heapSwaps/merged |
| `SehReady()` | `np_hook.cpp` | 惰性注册 VEH，**保存句柄** | `PresentCommon`、`ProbeSwapChainVtable`、`NpDispatchRays`、`NpDispatch` | ⚠⚠ 句柄必须存下来并在卸载前注销，否则**重复注入 100% 闪退**（HOOK.md §22.1 坑 1） |
| `gJmp` / `gJmpArmed` | `np_hook.cpp` | `thread_local std::jmp_buf` / `bool` | 各处 `setjmp`/`longjmp` | ⚠ 必须每线程一份；`longjmp` 后不要碰 setjmp 之后声明的对象 |
| `gSehHandle` | `np_hook.cpp` | `PVOID`，`AddVectoredExceptionHandler` 返回值 | `SehReady` 写；`RestoreAllHooks` 里注销 | `RemoveVectoredExceptionHandler` 后把 `gSehReady` 复位 |
| `gDbg*` 崩溃上下文快照 | `np_hook.cpp` | 8 个 `volatile` 变量（api/inPresent/hooked/dev11/dev12/heapSwaps/merged/frames） | 写在 `UpdateTelemetryCommon` 开头；读在 `NpSeh`（`SEH ctx:` 那一行） | 为「只知崩在哪、不知当时在干什么」而加。⚠ `gDbgInPresent` 的旧说法（"从不赋值"）**已过期**，见下方 §16 |

---

## 5. vtable 探测与安装

| 函数名 | 所在文件 | 作用 | 被谁调用 | 注意 |
| --- | --- | --- | --- | --- |
| `ProbeSwapChainVtable()` | `np_hook.cpp` | 自造临时 D3D11 设备 + 8x8 隐藏窗口 + 交换链，拿 vtable 后立即销毁 | `Worker` | ⚠⚠ **有崩溃风险**：撞 SEH 即 `gProbePoisoned = true` 永久停手；窗口类注册在**宿主 exe** 模块；窗口必须**即时创建**（延迟创建会导致 `hwnd==null` → 探测什么都不做） |
| `gProbePoisoned` | `np_hook.cpp` | 探测已污染标志 | `ProbeSwapChainVtable` | 一旦置位**永久**不再探测（重试 = 多几次把游戏打崩的机会） |
| `PatchSwapChainVtable(IDXGISwapChain*)` | `np_hook.cpp` | 给一条交换链补 `Present`(8) / `Present1`(22)，记进 `gSwapVts` | 三个工厂跳板 + 探测 | 同一 vtable 只补一次；表满（`NP_MAX_SWAPVT=4`）返回 `false`；`Present1` 失败可接受 |
| `SwapVtFor(void** vt)` | `np_hook.cpp` | 按 vtable 指针查原函数条目 | `PresentCommon`、`PatchSwapChainVtable` | ⚠ blt / flip / composition 可能是**不同的类**，所以是小表不是单例 |
| `PresentHooked()` | `np_hook.cpp` | `gSwapVtCount > 0` | `Worker`、探测 | 是「Present 已挂上」的唯一判据 |
| `InstallDxgi()` | `np_hook.cpp` | 挂 `CreateSwapChain`(10) / `ForHwnd`(15) / `ForComposition`(24) | `Worker` | 只为读 vtable 创建工厂实例；**自递归防线**在 `PointsIntoOurDll`（读到自己的钩子就宁可不装） |
| `EnsureD3D12Hooks(ID3D12Device*)` | `np_hook.cpp` | 首次 D3D12 Present 时惰性安装：队列钩子 + 查询堆/fence/分配器环 + 叠加 + （可选）命令列表钩子 | `PresentCommon` | ⚠ 失败可重试但**必须防并发重入**（`installing` 原子闸）；5 次上限 + 2 秒冷却；临时队列/列表只借 vtable，用完 `Release` |

---

## 6. 跳板函数（`extern "C"`，定义在 `np_hook.cpp`）

> 这些函数**签名必须与被 hook 的成员函数完全一致**（`__stdcall`、参数顺序），
> 否则栈会错位 —— 编译期也未必报错。

| 函数名 | 作用的 vtable 槽 | 被谁调用 | 注意 |
| --- | --- | --- | --- |
| `NpPresent` | `Present`(8) | 宿主进程的 DXGI | 纯转发到 `PresentCommon(..., isP1=false)` |
| `NpPresent1` | `Present1`(22) | 宿主 | 纯转发，`isP1=true` |
| `NpCreateSwapChain` | `IDXGIFactory::CreateSwapChain`(10) | 宿主 | 先 `Log`；成功且 `!gUnloading` 才补 Present |
| `NpCreateSwapChainHwnd` | `IDXGIFactory2::CreateSwapChainForHwnd`(15) | 宿主 | 同上；⚠ 若原函数缺失返回 `E_FAIL`（不会调用自己） |
| `NpCreateSwapChainComp` | `IDXGIFactory2::CreateSwapChainForComposition`(24) | 宿主 | 同上 |
| `NpECL` | `ID3D12CommandQueue::ExecuteCommandLists`(10) | 宿主提交线程 | ⚠ 只认 DIRECT 队列；锁定队列要 `AddRef`；只有 `q==gQueue12` 才插桩；`gFrameStarted` 用 CAS 抢且必须在 `EndList` 之前 |
| `NpDispatchRays` | `DispatchRays`(76) | 宿主提交线程 | ⚠ 槽位号用 `fetch_add` **返回值**；探针异常 → `gD12Broken` + **只转发一次**（历史 bug：转发两遍） |
| `NpBuildAS` | `BuildRaytracingAccelerationStructure`(72) | 宿主 | 只累加 `gAsBuilds` 后转发 |
| `NpDispatch` | `Dispatch`(14) | 宿主提交线程 | ⚠ 与 `DispatchRays` 同类坑；异常时复位 `gAiSpanOpen` 并永久停手 |
| `NpDrawInst` | `DrawInstanced`(12) | 宿主 + 我们自己的叠加 | `gNpCountingOverlayDraws` 时只转发不计数 |
| `NpDrawIdx` | `DrawIndexedInstanced`(13) | 宿主 + 叠加 | 同上 |
| `NpSetViewports` | `RSSetViewports`(21) | 宿主 | ⚠ 渲染分辨率**只认长宽比与输出一致**（`rel <= 1.05`）的最大视口 |

---

## 7. Present 主流程

| 函数名 | 所在文件 | 作用 | 被谁调用 | 注意 |
| --- | --- | --- | --- | --- |
| `PresentCommon(sc, sync, flags, pp, isP1)` | `np_hook.cpp` | 整个钩子层的心脏：防护 → 设备识别 → 分辨率/全屏 → 帧时间与帧内合并 → CPU 拆分 → Reflex → 时间戳收尾 → 遥测 → 叠加 → 调原函数 → **尾部收尾** | `NpPresent`/`NpPresent1` | ⚠⚠ **绝不在中间 `return`**（会跳过尾部收尾 → `gInPresent` 永远 `true` → 叠加永久失效）；⚠ `gUnloading` 判定必须在 `gTel` 解引用之前；⚠ 顺序：`setjmp`→`EnsureRt`→…→`CallOriginal`→CPU 拆分→复位状态 |

子步骤（都在 `PresentCommon` 内，**按出现顺序**；想看某一段就去源码里搜说明列里的标识符）：

| 逻辑段 | 说明 |
| --- | --- |
| 入口防护 | 反查原函数、`gUnloading`/`gTel`/`gInPresent` 三道闸、`SehReady`+`setjmp` |
| 首次接入/设备识别 | `GetDevice` 先 D3D11 后 D3D12，写 `processName` |
| 分辨率与全屏模式 | `GetDesc` / `GetFullscreenState` / `GetHwnd`+窗口样式 → `NP_PM_*` |
| 帧时间 + 帧内合并 | `DXGI_PRESENT_TEST` 过滤、`gRawStats`、阈值 `meanMs(240)*0.6` 夹 [2,30]、`gAbsorbThisPresent` |
| sim/submit 两段 | 只认 `gRenderTid`；渲染提交本质是排队 |
| Reflex 读取 | 设备只取一次；`SleepStatus` 1 秒一次；`Poll` 每帧 |
| `gLastPresentQpc` 推进 | **被合并时不推进**（合并机制本身） |
| `present diag` 5 秒诊断 | 调用数/real/test/sync 分布 + 帧时间极值 |
| 图表历史采样 | 每 8 帧一点；GPU 曲线**优先 PDH** `ss.gpuBusyMs` |
| 设备移除检测 | 每 128 帧一次 `GetDeviceRemovedReason` |
| GPU 时间戳收尾 | D3D12：`HarvestTimestamps` + `SubmitPendingResolve`；D3D11：`Ts11Tick` |
| 暂停判定 | `cfg.pauseHook`；⚠ 只跳过更新，**不 return** |
| 叠加：是否绘制 | `wantOverlay` 六条件；`updateHz` 限流；`settlePending` |
| 叠加：取缓冲并绘制 | ⚠ `GetCurrentBackBufferIndex()`；`stateOk=false` **只跳过本帧** |
| 叠加诊断日志 | `overlay NOT drawn` / `overlay drawn` / `overlay 5s` |
| 调原函数 + 计时 | `gJmpArmed=false`；`tPresent0`/`tPresent1`/`msInPresent` |
| **CPU Busy/Wait 拆分** | ⚠ 必须在 Present **返回之后**；三项同一次减法导出 |
| 尾部收尾 | `gRenderTid`/`gPresentRetQpc`/`gSimEndQpc=0`/`gFrameStarted=false`/`gInPresent=false` |

---

## 8. 帧时间统计

| 函数名 | 所在文件 | 作用 | 被谁调用 | 注意 |
| --- | --- | --- | --- | --- |
| `NowQpc()` | `np_hook.cpp` | `QueryPerformanceCounter` 包装 | 各处 | — |
| `QpcMs(uint64_t)` | `np_hook.cpp` | QPC 差值 → 毫秒 | 各处 | 分母 `gQpcFreq` 由 `Worker` 填；为 0 时返回 0 |
| `np::FrameStats::push` | `np_stats.h` | 环形缓冲压入一帧 | `gStats`/`gRawStats` | 容量 4096 |
| `np::FrameStats::meanMs(window)` | `np_stats.h` | 窗口均值（**帧内合并的基准**） | `PresentCommon` | ⚠ 不能用中位数当合并基准（双峰振荡）；调用处夹 [2,30] |
| `np::FrameStats::medianMs` | `np_stats.h` | 中位数 | **当前无人调用** | 保留；注释说明为何不能用于合并基准 |
| `np::FrameStats::avgFps(600)` | `np_stats.h` | 平均 FPS | `UpdateTelemetryCommon` | ⚠ 临时缓冲必须是 `thread_local`（原 `static` 会算错） |
| `np::FrameStats::percentileMs / lowPercentileFps` | `np_stats.h` | 帧时间百分位 / 其倒数 | `UpdateTelemetryCommon` | 线性插值 `index=(n-1)*p`，与 PresentMon `StatisticsTracker.cpp` 的算法一致 |
| `np::FrameStats::recent / overRatio` | `np_stats.h` | 取最近 n 帧 / 超阈值比例 | `UpdateTelemetryCommon` 诊断块 | 采样率必须用 `frameTotal` 算，`count()` 会饱和 |
| `np::Ema(alpha)` | `np_stats.h` | 指数移动平均 | `frameMsAvg`、三个 CPU EMA | alpha 均为 0.10 |
| `gStats` / `gRawStats` | `np_hook.cpp` | 合并后序列 / **原始**序列 | `PresentCommon` | ⚠ 两者不可混用：`gRawStats` 只用来求合并阈值 |
| `gLastPresentQpc` / `gPrevPresentQpc` | `np_hook.cpp` | 上一次**记账** / 上一次**呈现** | `PresentCommon` | ⚠ 被合并时前者不推进、后者无条件推进（这是合并能成立的前提） |
| `gAbsorbThisPresent` / `gMergedCount` | `np_hook.cpp` | 本次是否被并入上一帧 / 累计合并数 | `PresentCommon` | `gMergedCount` 在诊断日志里清零 |

---

## 9. 遥测发布

| 函数名 | 所在文件 | 作用 | 被谁调用 | 注意 |
| --- | --- | --- | --- | --- |
| `UpdateTelemetryCommon(cfg, nowQpc, realPresent)` | `np_hook.cpp` | 完整遥测：崩溃上下文快照、状态、计数器、FPS 窗口计数、RT/Tensor 占比、Low 帧、PDH GPU 回填、图表、清零计数器 | `PresentCommon`（未暂停时） | ⚠ 开头刷新 `gDbg*` 崩溃快照；⚠ `fps` 窗口只数 `realPresent`；⚠ RT/Tensor 分母用 `frameMs` 不用 `gpuFrameMs`；⚠ 末尾必须清 `gDraws/gDispatches/gAsBuilds` |
| `PublishMinimalTelemetry()` | `np_hook.cpp` | 心跳：`tickMs`/`gfxApi`/`processName`/`lastError` | `Worker`（仅 `!PresentHooked()`） | 把「已注入但没数据」变成一句能看懂的话；按 API 分支写文案 |
| `GuessApiFromModules()` | `np_hook.cpp` | 按模块猜 API（含 Vulkan/OpenGL） | `PublishMinimalTelemetry` | 与 `HostApiGuess` 不同：后者排除了本 DLL 自己导入的模块 |
| `NPClearTelemetry` / `NPClearSensors` | `np_common.h` | 结构体清零 + 写 `sizeof` 版本 | `OpenIpc`；`Sens()` | ⚠ `version` 写的是 `sizeof`，读方用它做布局自检 |

---

## 10. D3D12 时间戳与命令列表资源

| 函数名 | 所在文件 | 作用 | 被谁调用 | 注意 |
| --- | --- | --- | --- | --- |
| `InitTs12(dev, q)` | `np_hook.cpp` | 建查询堆（64 槽）、4 个 readback、fence、`NP_ALLOC_RING=32` 个分配器、一条命令列表并 `Close()` | `EnsureD3D12Hooks` | 任一失败返回 `false`（会走日志分支）；`gTsFreq` 拿不到退 1000000 |
| `BeginList()` | `np_hook.cpp` | 取分配器环下一个槽 → 检查 fence → `Reset` 分配器与命令列表 → 返回 `gList` | `PresentCommon` 的叠加绘制路径、`SubmitPendingResolve` | ⚠ `try_lock`（抢不到就放弃本帧，**绝不阻塞**）；⚠ **绝不等 GPU**；⚠ 连续失败 > 600 → `gD12Broken`；任何 `Reset` 失败 → `gD12Broken` |
| `EndList(q)` | `np_hook.cpp` | `Close` → `ExecuteCommandLists` → `Signal(gFence)` → 记 `gAllocFence` → 同步叠加 fence → 解锁 | 同 `BeginList` 的两处 | ⚠ 没拿锁时直接返回（不要瞎解锁）；`Close` 失败 → `gD12Broken` |
| `HarvestTimestamps()` | `np_hook.cpp` | fence 完成后 `Map` readback，算 GPU/RT/AI 毫秒并写遥测 | `PresentCommon` | ⚠ 单批 > 50ms 判为配对出错，丢弃；`0 < gpuMs < 1000` 才写；RT/Tensor 百分比**不在这里算** |
| `SubmitPendingResolve(q, rtCount, aiUsed, batchCount)` | `np_hook.cpp` | 找一个空 `Pending` 槽，`ResolveQueryData(0, TS_SLOTS)` 到 readback | `PresentCommon` | 4 个槽都忙就**放弃这一帧**（不等待） |
| `CanTimestamp(cl)` | `np_hook.cpp` | 列表是否允许插时间戳（必须 `DIRECT`） | `NpDispatchRays`、`NpDispatch` | ⚠ bundle 与直连列表**共用 vtable**，bundle 上 `EndQuery` 非法 → 必须用 `GetType()` 过滤 |
| `gD12Lock` / `gD12Locked` | `np_hook.cpp` | 全局命令列表的互斥 + 线程重入标记 | `BeginList`/`EndList` | ⚠ 用 `try_lock`；`thread_local` 标记防止同线程重入 |
| `gD12Broken` | `np_hook.cpp` | D3D12 侧永久停手标志 | 多处读写 | 语义是**永久的**；置位后帧时间路径继续工作 |
| `gAlloc[32]` / `gAllocFence[32]` / `gAllocCur` / `gBeginListFails` | `np_hook.cpp` | 分配器环与状态 | `BeginList`/`EndList` | ⚠ 环太小（3/8）会让叠加**永久停画** |
| `gPending[4]` | `np_hook.cpp` | readback 延迟收割槽 | `HarvestTimestamps`/`SubmitPendingResolve` | `struct Pending` 定义在同处 |
| `InitTs11(dev)` / `Ts11Tick(seq)` | `np_hook.cpp` | D3D11 三缓冲时间戳 | `PresentCommon` | `GetData` 返回 `S_OK` 才算就绪；要求 `!dj.Disjoint` 且 `0 < ms < 1000` |

---

## 11. 叠加绘制（`src/hook/np_draw.*`）

### 11.1 全局与编排

| 函数名 | 所在文件 | 作用 | 被谁调用 | 注意 |
| --- | --- | --- | --- | --- |
| `npg::GfxInit()` | `np_draw.cpp` | `npb::GfxInit()` + `LoadLibraryW("d3dcompiler_47.dll")` 取 `D3DCompile` | `EnsureRt` | 运行时编译着色器，不依赖预编译 blob |
| `npg::GfxShutdown()` | `np_draw.cpp` | `npb::GfxShutdown()` + 释放 d3dcompiler | `SelfUnloadNow` | `gCompiler`/`gD3DCompile` 一并清空 |
| `CompileShader(entry, target, out)` | `np_draw.cpp` | 编译内置全屏三角形 VS/PS | `Overlay11::Init` / `Overlay12::Init` | 源码 `kShaderSrc` 在同文件顶部；错误 blob 会被 `Release` |
| `Overlay11::Draw(...)` | `np_draw.cpp` | D3D11 路径：动态纹理上传 + 每帧新建 RTV + `Draw(3,0)` | `PresentCommon` | ⚠⚠ **RTV 每帧新建、本帧放掉**（长期引用后台缓冲会让游戏 `ResizeBuffers` 失败 → 闪退）；⚠ 必须绑 `CULL_NONE` 光栅状态；⚠ 视口定位面板矩形 |
| `Overlay12::Record(...)` | `np_draw.cpp` | D3D12 路径：**常驻上传环** → `CopyTextureRegion` → 屏障 → 全屏三角形 | `PresentCommon`（由 `BeginList`/`EndList` 包住） | ⚠⚠ 描述符堆**换堆**而不是覆盖槽位；⚠ 换堆后 `settle_ = 30`；⚠ 上传缓冲**不再每帧新建**（见下 `uploadRing_`） |
| `DrawCb(rt, ud)` | `np_hook.cpp` | D2D 回调：`PanelRenderer::Render` | `PanelBitmap::Render` | `DrawCtx` 定义在同一个文件里 |
| `RenderPanel(cfg)` | `np_hook.cpp` | `BuildPanelData` → `Measure` → `PanelBitmap::Render` → 记 `gBw/gBh` | `PresentCommon` | 尺寸变化时记一条 `panel logical size` 诊断日志 |
| `gBw` / `gBh` | `np_hook.cpp` | 面板纹理**物理像素**尺寸（含光栅倍率） | 叠加路径 | `gBw>0 && gBh>0` 是「可以画」的前置条件 |

### 11.2 `Overlay11`（`np_draw.h`）

| 函数名 | 所在文件 | 作用 | 被谁调用 | 注意 |
| --- | --- | --- | --- | --- |
| `Overlay11::Init(dev)` | `np_draw.cpp` | 编译 VS/PS、采样器、**预乘 alpha** 混合、`CULL_NONE` 光栅状态 | `PresentCommon` | ⚠ D3D11 默认 `CullMode=BACK`，全屏三角形正好是背面 → 不建这个状态则 `Draw` 成功但屏幕无内容 |
| `Overlay11::Release()` | `np_draw.cpp` | 释放全部成员 | `SelfUnloadNow` | — |
| 成员 | `np_draw.h` | `vs_/ps_/tex_/srv_/blend_/samp_/raster_/rtv_/rtvSrc_/texW_/texH_` | — | ⚠ `texW_/texH_` 必须在纹理与 SRV 都建好之后才写（见 `Overlay11::Draw` 里的赋值处）；`rtvSrc_` 是历史遗留（现在不再 `AddRef` 后台缓冲） |

### 11.3 `Overlay12`（`np_draw.h`）

| 函数名 | 所在文件 | 作用 | 被谁调用 | 注意 |
| --- | --- | --- | --- | --- |
| `Overlay12::Init(dev)` | `np_draw.cpp` | 根签名（SRV 表 + 静态采样器）、SRV 堆（1）、RTV 堆（8）、fence | `EnsureD3D12Hooks` | PSO **不在这里建**（依赖后台缓冲格式） |
| `Overlay12::EnsurePso(dev, rtvFormat)` | `np_draw.cpp` | 按后台缓冲格式建/换 PSO | `Overlay12::Record` | 旧 PSO 走 `Retire` 延迟回收 |
| `Overlay12::Retire(IUnknown*)` | `np_draw.cpp` | 挂到 fence 队列延迟回收（上限 16） | `EnsurePso`、`Record`、换堆 | ⚠ 立刻 `Release` 一个「已录进命令列表但 GPU 没执行完」的资源 = 释放正在使用的显存 |
| `Overlay12::OnFrameCompleted()` | `np_draw.cpp` | fence 确认后真正 `Release` | `Overlay12::Record` | 每帧回收一次 |
| `Overlay12::Release()` | `np_draw.cpp` | 清空 pending + **释放上传环** + 释放全部 + **重置 `psoFormat_`** | `SelfUnloadNow` | ⚠ 不重置 `psoFormat_` 会让重新 `Init` 后的 `EnsurePso` 误判；⚠ 上传环不挂在 `pending_` 上，必须在这里显式放 |
| `Overlay12::uploadRing_` / `uploadRingSize_` / `uploadRingFence_` / `uploadRingNext_` | `np_draw.h` | **常驻上传缓冲环**（4 个）：按 fence 确认 GPU 用完才复用；尺寸变了才重建；全忙就跳过这一帧的面板更新 | `Overlay12::Record` | ⭐ 2026-10-11 根因修复：原来每帧 `CreateCommittedResource` 一个约 300KB UPLOAD 资源，是 `nvwgf2umx.dll+0x330124` 崩溃的根因；顺带省掉约 18MB/s 分配抖动 |
| `Overlay12::fence()` / `fenceValuePtr()` | `np_draw.h` | 给 `EndList` 用来 `Signal` 并写值 | `EndList` | 值必须与 `gFenceVal` 同步推进 |
| `Overlay12::HeapSwaps()` | `np_draw.h` | RTV 堆被换过几次（诊断） | 叠加 5s 日志（`overlay 5s`） | 暴涨说明游戏在频繁重建交换链 |
| `Overlay12::settlePending()` / `tickSettle()` | `np_draw.h` | 交换链重建后的静默帧计数 | `PresentCommon` | 置位点是 `Overlay12::Record` 换堆那一支（`settle_ = 30`） |
| 成员 | `np_draw.h` | `root_/pso_/vsBlob_/psBlob_/psoFormat_/tex_/srvHeap_/rtvHeap_/rtvSlots_[8]/rtvSlotsN_/rtvHeapSwaps_/settle_/drawSkips_/rtvHeapSize_/texW_/texH_/fence_/fenceValue_/pending_[16]/pendingN_` | — | `drawSkips_` 只声明未使用；`rtvSlots_` 是 `struct RtvSlot { ID3D12Resource* res; int slot; }` 数组 |

---

## 12. NvAPI Reflex（`src/hook/np_reflex.h`，header-only）

| 函数 / 结构 | 所在文件 | 作用 | 被谁调用 | 注意 |
| --- | --- | --- | --- | --- |
| `np::NVFrameReport` | `np_reflex.h` | 逐字段对齐 `nvapi.h` 的帧报告（sim/submit/present/driver/os/gpu 各起止时刻 + `gpuActiveRenderTimeUs` / `gpuFrameTimeUs`） | `ReflexReader::Poll` 输出 | ⚠⚠ **不能改字段顺序/宽度**，否则解析全是垃圾 |
| `np::NVLatencyParams` / `NVGetSleepStatusParams` | `np_reflex.h` | NVAPI 入参结构（64 帧报告 / 低延迟状态） | `Poll` / `SleepStatus` | `NvBool` 是 32 位整数 |
| `NP_NVAPI_VERSION(s, v)` | `np_reflex.h` | `sizeof(s) \| (v << 16)` | 两个入参填充 | 版本号必须与结构大小绑定 |
| `ReflexReader::Init()` | `np_reflex.h` | 载入 `nvapi64.dll`（失败退 `nvapi32.dll`）→ `nvapi_QueryInterface` → `NvAPI_Initialize` → 取两个接口 | `PresentCommon` | 幂等；`NvAPI_Initialize` 返回非 0 视为失败；**没有对应的 `FreeLibrary`**（见 HOOK.md §21.7） |
| `ReflexReader::ok()` | `np_reflex.h` | `getLatency_ != nullptr` | `PresentCommon` | — |
| `ReflexReader::Poll(dev, out)` | `np_reflex.h` | 取最近一帧报告：从 64 帧里挑 `frameID` 最大且时刻非 0 的；剔除「结束早于开始」 | `PresentCommon` | 返回 `false` 表示这次没数据（游戏没用 Reflex）→ 调用方必须保持回退路径 |
| `ReflexReader::SleepStatus(dev, &on)` | `np_reflex.h` | **驱动直证**低延迟是否开启 | `PresentCommon`（1 秒一次） | 返回 `true` 才代表「驱动明确回答了」；比用 CPU Wait 推断可靠（那是旁证） |
| `ReflexReader::hasSleepStatus()` | `np_reflex.h` | 该接口是否可用 | `PresentCommon` | — |
| 接口 ID 常量 | `np_reflex.h` | `NvAPI_Initialize=0x0150E828`、`NvAPI_D3D_GetLatency=0x1A587F9C`、`NvAPI_D3D_GetSleepStatus=0xAEF96CA1` | `Init` | GetLatency 的 ID 来自 Intel PresentMon 的 `nvapi_interface_table.h` |
| `gReflex` / `gReflexDev` / `gReflexTried` | `np_hook.cpp` | 读取器实例 / D3D 设备（AddRef 后**不释放**）/ 只试一次标记 | `PresentCommon` | 字段写入：`simMs`/`submitMs` 被 Reflex 值与启发式值**共用**（读到就覆盖） |

---

## 13. 卸载与还原

| 函数名 | 所在文件 | 作用 | 被谁调用 | 注意 |
| --- | --- | --- | --- | --- |
| `RestoreAllHooks()` | `np_hook.cpp` | **幂等**还原：记日志 → 注销 VEH → 交换链表 → 工厂 → 队列 → 命令列表 → 指针置空 | `SelfUnloadNow`、`NpHookDetach` | ⚠⚠ 必须从**两条路径**都调（只写在自卸载里会让补丁留在原地 → 下次注入自递归栈溢出）；⚠ VEH 注销是「重复注入 100% 闪退」的根因修复（函数开头那一段） |
| `SelfUnloadNow()` | `np_hook.cpp` | `gUnloading=true` → `Sleep(150)` → `RestoreAllHooks` → `Sleep(250)` → 释放图形/采集资源 → 释放互斥量与共享内存 → `FreeLibraryAndExitThread` | `Worker`（`quit` / `detachPid` 两条判定） | ⚠ 顺序不能乱（先直通、再还原、排空后才允许 `FreeLibrary`）；**不返回** |
| `detachPid` 判定 | `np_hook.cpp`（`Worker` 循环内） | `gCfg->detachPid == GetCurrentProcessId()` → `SelfUnloadNow()` | worker 循环（200ms 轮询） | 字段定义 `np_common.h`；**为什么不用 `CreateRemoteThread`** 见 `np_common.h` 里该字段的注释（ASLR + 跨位数） |
| `quit` 判定 | `np_hook.cpp`（`Worker` 循环内） | 主程序退出 → `SelfUnloadNow()` | worker 循环 | 条件含 `magic == NP_MAGIC` |
| `gWorker` / `gInjectedMutex` | `np_hook.cpp` | worker 线程句柄 / 防重复注入互斥量 | `NpHookAttach` / `Worker` / `NpHookDetach` | 互斥量在 `Worker` 里 `CreateMutexW` 创建并持有到进程结束；`NpHookDetach` 里**不能等自己** |

---

## 14. 日志与诊断

| 函数名 | 所在文件 | 作用 | 被谁调用 | 注意 |
| --- | --- | --- | --- | --- |
| `Log(fmt, ...)` | `np_hook.cpp` | 写 `%TEMP%\NextPerfHook.log`，前缀 = 墙钟 + 进程名 + pid + tid | 全文件 | ⚠⚠ 写满 512KB 后**回到文件开头覆盖** → **最新的行在开头，结尾是旧内容**，排查别只看末尾。格式/编码约定见 `docs/LOGGING.md` 第 2 节 |
| 关键诊断日志（按**日志文本**检索，不给行号） | 钩子层各处 | `render-thread gfx init`（`EnsureRt`）；`reflex:`（`PresentCommon`，含 `init=`/`ok=`）；`reflex sleep status`；`reflex frame`；`present diag 5s`；`overlay NOT drawn` / `overlay drawn` / `overlay 5s`；`cpu split avg`（以上都在 `PresentCommon`）；`RestoreAllHooks:`（`RestoreAllHooks`）；`attach:` / `probe attempt` / `probe disabled`（`Worker`）；**`SEH` + `SEH ctx:`**（`NpSeh`） | — | 这些是事后定位问题的**唯一证据**，改动相关路径时不要删 |

---

## 15. 共享内存接口速查（`src/common/np_common.h`）

| 符号 | 所在文件 | 作用 | 钩子层使用点 | 注意 |
| --- | --- | --- | --- | --- |
| `NP_SHM_CONFIG` / `NP_SHM_SENSORS` | `np_common.h` | 共享内存名字 | `OpenIpc` | 只读映射（`FILE_MAP_READ`） |
| `NPTelemetryShmName(pid, out, n)` | `np_common.h` | 拼 `Local\NextPerf_Telemetry_v1_<pid>` | `OpenIpc` | 自带十进制转换，不用 `swprintf` |
| `NPConfig` | `np_common.h` | 配置块 | `Cfg()` | 钩子关心的字段：`pauseHook`、`overlayMode`、`updateHz`、`deepEngineHook`、`vtableProbe`、`quit`、`detachPid`、`offsetX`/`offsetY`、`scale` |
| `NPSensors` | `np_common.h` | 传感器块 | `Sens()` | 钩子只真正用 `gpuBusyMs`（GPU 曲线/源头回填）、`cpuUsage`/`gpuUsage`（图表） |
| `NPTelemetry` | `np_common.h` | 遥测块（钩子写、主程序读） | `UpdateTelemetryCommon`、`PublishMinimalTelemetry`、各跳板 | ⚠ `version` = `sizeof`，跨进程 ABI；新增字段只能加在末尾或用 `reserved` |
| `NP_HOOK_*` 标志 | `np_common.h` | `PRESENT/QUEUE/CMDLIST/TIMESTAMP/OVERLAY/REFLEX/REFLEX_KNOWN` | 各安装点 + `Worker`（心跳里刷新 `hookFlags`） | 主程序判断「挂到哪一步」的唯一依据 |
| `NP_C_CPU_BUSY` / `NP_C_CPU_WAIT` | `np_common.h` | CPU 两个半边的计数器位 | 面板 | **故意不在 `NP_ALL_COUNTERS`** 里（默认不显示） |
| `NPHistoryPush` | `np_common.h` | 游戏内图表环形缓冲压入（**12 个参数**） | `PresentCommon` | 参数顺序：`fps,avg,low1,low01,ucpu,ugpu,latFrame,latCpu,latGpu,latBusy,latWait` |
| `NPCopyStr` | `np_common.h` | 安全字符串拷贝 | `SelfImports`、`processName` 各处 | 保证 `'\0'` 结尾 |

---

## 16. 未使用 / 历史遗留（清理前先读这里）

| 符号 | 所在文件 | 状态 | 说明 |
| --- | --- | --- | --- |
| `InModule(HMODULE, void*)` | `np_hook.cpp` | **无人调用** | 有意保留为教训备忘（D3D12 在 `D3D12Core.dll`）。**不要顺手删** |
| `gCs` | `np_hook.cpp`（初始化在 `NpHookAttach`、销毁在 `NpHookDetach`） | 死代码 | 全文件**没有 `EnterCriticalSection`**（已 grep 确认） |
| `gCpuStartQpc` | `np_hook.cpp` | 只写不读（写点在 `NpECL`） | 原为「本帧 CPU 起点」 |
| `gDbgInPresent` | `np_hook.cpp` | ⚠ **本条已过期**：历史上只声明、从不赋值（`SEH ctx:` 的 `inPresent=` 恒为 0），**现在 `PresentCommon` 入口置 1、出口置 0**（`// 镜像给崩溃快照`） | `inPresent=1` 已是可信信息 —— 2026-10-11 的 D3D12 崩溃就是靠它判定"崩在原始 Present 调用里" |
| `Overlay11::rtvSrc_` | `np_draw.h` | 历史遗留 | 曾用于缓存并 `AddRef` 后台缓冲 —— 那个做法已被判定为**严重违反 DXGI 规则**（见 `Overlay11::Draw` 里的注释） |
| `Overlay12::drawSkips_` | `np_draw.h` | 只声明不计数 | 原计划用于「闪烁是不是因为我们没画上去」的诊断（该诊断现由 `overlay 5s` 日志承担） |
| `np::FrameStats::lowPct` / `lowIntegral` / `medianMs` | `np_stats.h` | 当前**不调用** | 保留供对照；⚠ 但同文件的 **`lowWindowed` 是钩子层的主路径**（`PresentCommon` 里 `lowWindowed(1.0/0.1, 1000ms, look)`），**不要**把它也算进"未使用" |
| `TS_BATCH_BASE` / `TS_BATCH_PAIRS` 与 `HarvestTimestamps` 里的批次求和 | `np_hook.cpp`（`TS_BATCH_*` 定义在文件头部，批次求和在 `HarvestTimestamps` 里） | **已不再产生数据** | 逐批夹取已整个拆掉，`batchCount` 现在恒由 `gBatchCount`（恒 0）传入 |
| `gVpW` / `gVpH` | `np_hook.cpp` | 32 位标量竞态（注释认可可忽略） | 要严谨可换 `std::atomic<uint32_t>` |
| `np_reflex.h` 的 `dll_` | `np_reflex.h` | 无 `FreeLibrary` | 见 HOOK.md §21.7 |
| `TEXT("Local\\NextPerf_SensorsMutex_v1")`（`NP_MUTEX_SENSORS`） | `np_common.h` | 钩子层不使用 | 主程序侧用；钩子只读 Sensors 快照，不加锁 |

---

## 17. 相关但**不在**钩子层的接口

| 符号 | 所在文件 | 说明 |
| --- | --- | --- |
| `np::EtwMonitor` / `np::Etw()` | `src/etw/np_etw.h` / `np_etw.cpp` | ETW 帧计时，**外置**（不注入游戏），由主程序驱动。钩子层不含任何 ETW 代码 |
| `np::PanelRenderer` | `src/common/np_panel.h` | 真正的 D2D 面板绘制（`PanelRenderer::Render` 由 `DrawCb` 调用） |
| `npb::PanelBitmap` / `npb::RasterScale()` / `SetRasterScale()` / `GfxInit()` / `GfxShutdown()` / `Factory()` | `src/common/np_bitmap.h` | 面板位图（D2D DIB）；被钩子、桌面叠加、主程序三方复用 |
| `np::BuildPanelData` | 声明 `src/common/np_build.h`，实现 `src/common/np_build.cpp` | 由遥测 + 传感器 + 配置组装 `PanelData`（调用点在 `RenderPanel` 里） |
