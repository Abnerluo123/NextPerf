# NextPerf 主程序层（`src/app`）函数 / 接口速查表

> 配套文档：`docs/APP.md`（原理、为什么、坑与教训）。
> **行号基准**：`main.cpp` 1226 行、`ui.cpp` 939 行、`injector.cpp` 728 行、
> `settings.cpp` 184 行、`overlay.cpp` 135 行、`np_app.h` 177 行。
> 行数对不上就说明代码已改，请先核对再引用。
>
> 约定：`文件:行号` 里的行号是**定义处**（不是声明处）；
> 「调用方」只列本层（`src/app`）内的调用点，外部层（`src/hook`、`tests/`）另行标注。
> **改完代码请同步更新本表与 `docs/APP.md`、`docs/CATALOG.md`。**

---

## 0. 全局状态与常量

| 符号 | 文件:行号 | 作用 | 注意事项 |
| --- | --- | --- | --- |
| `npa::gApp` | `main.cpp:28`（定义）、`np_app.h:105`（声明） | 全层唯一的全局状态（配置/传感器/遥测/游戏列表/共享内存句柄/UI 标志） | **跨线程共享**：`injected`/`games`/`notice`/`telSlots` 必须用 `AppLock` 保护，见 `APP.md` §4.3 |
| `gInst` | `main.cpp:30` | 本进程 `HINSTANCE` | 与 `ui.cpp:135` 的同名 static 不是同一个 |
| `gTrayAdded` / `gTrayOk` | `main.cpp:31-32` | 「托盘图标已添加」/「注册成功」 | `gTrayOk` 经 `TrayAvailable()` 外露，决定 `WM_CLOSE` 语义 |
| `gOverlayOff` | `main.cpp:33` | 托盘「快速关闭叠加」开关 | 只关不立即开：重新打开要等下一个 120ms tick 的 `wantDesktop` |
| `gWantQuit` | `main.cpp:34` | `TrackPopupMenu` 模态循环吃掉了 `WM_QUIT` 时的补发标志 | 只在 `ShowTrayMenu` 里消费 |
| `gNid` | `main.cpp:35` | `NOTIFYICONDATAW` | `szTip` 手写截断到 127 字符 |
| `gFgHook` | `main.cpp:301` | WinEvent 前台钩子句柄 | 懒安装（`AppEnsureForegroundHook`），进程内不卸载 |
| `gAutoCand` / `gAutoCandSince` / `gAutoTried` / `gAutoScanMs` | `main.cpp:469-472` | 启发式自动注入的候选状态机 | **已停用**（`AppAutoInjectTick` 为空），目前是死变量 |
| `kAntiCheatKw[]` | `main.cpp:355` | 反作弊关键词表 | 只被 `NameIsAntiCheat` 使用，而后者已无调用方 |
| `gWatchThread` / `gWatchRun` | `injector.cpp:19-20` | 守护线程句柄 / 运行标志 | `volatile bool`，无事件对象，最坏多等 800ms |
| `gDetachAt` | `injector.cpp:493` | `detachPid` 请求的时间戳 | 0 = 没有请求；`GetTickCount()` 为 0 时替换成 1 |
| `gCleanedUp` | `main.cpp:717` | `CleanupAndExit` 幂等标志 | 防止重复清理 |
| `gW` / `gMain` / `gInst` / `gSelGame` / `gDragging` | `ui.cpp:133-137` | 控件数组 / 主窗口 / 实例 / 选中行 / 正在拖的滑杆下标 | `gW` 每次 `Layout()` 整体重建 |
| `gScroll` / `gListTop` / `gRowH` / `gVisibleRows` / `gTotalRows` | `ui.cpp:140-143`、`106` | 计数器列表滚动与行度量 | `gRowH` 只在 `Layout()` 里算，`VisibleRect` 依赖它 |
| `gPrevBmp` / `gPrevPanel` / `gPrevData` / `gPrevOk` | `ui.cpp:145-148` | 叠加预览用的位图 / 渲染器 / 数据 / 成功标志 | `UiDestroy` 必须释放（`ui.cpp:924`） |
| `gFontTitle/Head/Body/Small` | `ui.cpp:150` | 四个 GDI 字体 | `MakeFonts` 会先 `ReleaseFonts`，**不要漏 `DeleteObject`** |
| `gS` / `S(v)` | `ui.cpp:40-41` | DPI 缩放系数与缩放宏 | 所有布局尺寸都要过 `S()`，否则高分屏错位 |
| `kBack/kPanel/kPanel2/kBorder/kText/kDim` | `ui.cpp:22-27` | 主题色（黑白） | — |
| `kWinW` / `kWinH` / `kListH` | `ui.cpp:29/34/38` | 窗口宽 1000 / 高 730 / 列表高 400 | **加一行控件必须同步改 `kWinH`**；`kListH` 只定义这一处 |
| `kGroups[]` / `kGroupN` | `ui.cpp:98-105` | 计数器分组定义（帧率/CPU/GPU/系统/图表） | 决定列表总行数与组标题 |
| `gHwnd` / `gBmp` / `gPanel` / `gPd` / `gVisible` / `gW,gH` | `overlay.cpp:17-22` | 叠加窗口与其渲染资源 | `gVisible` 与窗口真实可见性由 `OverlaySetVisible` 同步 |
| `gPath` | `settings.cpp:10` | 配置文件的缓存路径 | `SettingsPath()` 只算一次 |
| `ProcEntry` / `DrawCtx` / `Ctx` / `Holder` / `Widget` / `CounterDef` / `CounterGroup` / `AppState::TelSlot` / `GameEntry` | `injector.cpp:463`、`overlay.cpp:24`、`main.cpp:264/450`、`injector.cpp:25`、`ui.cpp:116`、`ui.cpp:44/48`、`np_app.h:79/39` | 各文件的辅助结构体 | `TelSlot`/`GameEntry` 的生命周期见 `APP.md` §3.4/§5.4 |

---

## 1. 进程启动 / 命令行 / 自检（`main.cpp`）

| 函数 | 文件:行号 | 作用 | 调用方 | 注意事项 |
| --- | --- | --- | --- | --- |
| `WinMain` | `main.cpp:929` | 入口：DPI → 清结构体 → 读配置 → 命令行分发 → GfxInit → 建共享内存 → 启动日志 → UI/叠加/守护线程/托盘 → 120ms 主循环 → 清理退出 | 系统 | 命令行分支必须在 `GfxInit` **之前**；`CleanupAndExit()` 后还要 `SettingsSave()`（`1223-1224`） |
| `RunSelfTest` | `main.cpp:756` | `--selftest`：D2D → 3 轮传感器 → 面板渲染到位图 → 共享内存 → DLL 存在性，写 `<exeDir>\..\selftest.txt` | `main.cpp:938` | **必须 `ExitProcess(fails)`**（`850`）；路径不能用 `DllPath()+"\\.."`（`758-762`）；退出码 = 失败项数 |
| `RunUiSmoke` | `main.cpp:855` | `--uismoke`：真建主窗口 + 叠加窗口跑 60 轮消息循环，确保 `WM_PAINT` 不崩 | `main.cpp:939` | 返回 1/2/3 分别对应 Gfx/Ui/Overlay 失败；结尾**必须硬退出**（`882-883`） |
| `RunInjectCli` | `main.cpp:908` | `--inject <pid>`：打印 `isWow64` → `InjectInto` → 退出码 0/1/3 | `main.cpp:941` | 参数解析只「跳过非数字」（`910-916`），不严格 |
| `EnableDpiAwareness` | `main.cpp:888` | 动态取 `SetProcessDpiAwarenessContext`，-4 → -2 → `SetProcessDPIAware` 逐级回退 | `main.cpp:932` | 必须在建窗口之前调用；不开会文字发糊 |
| （启动日志块，非函数） | `main.cpp:952-987` | 一次性写清 build/exe/dll/admin/日志路径/游戏列表，并 `Etw().Start()` | `WinMain` 内联 | `gApp.isAdmin` 在这里赋值；ETW 失败只写日志 |

---

## 2. 共享内存（`main.cpp` + `np_common.h`）

| 函数 | 文件:行号 | 作用 | 调用方 | 注意事项 |
| --- | --- | --- | --- | --- |
| `CreateShm`（static） | `main.cpp:38` | 创建并映射 Config / Sensors 两块共享内存 | `main.cpp:949`（WinMain）、`main.cpp:833`（self-test） | 只要 `shmCfg` 成功就算成功（`49`）；不填 `magic`，靠 `NPDefaultConfig`/`NPClearSensors` |
| `TryOpenTelemetry`（static） | `main.cpp:54` | 遍历 `gApp.injected`，为每个 pid 打开 `Local\NextPerf_Telemetry_v1_<pid>`，push 进 `telSlots` | `main.cpp:525`（`AppPublish`） | **只校验 `magic`，没校验 `version`**（见 APP.md G4）；已开过的跳过 |
| `PickTelemetry`（static） | `main.cpp:81` | 按 `tickMs` 最大者作为「当前游戏」，整块拷进 `gApp.telemetry` 并记 `telemetryPid` | `main.cpp:526` | `lastTick == 0` 时直接放弃 |
| `AppPublish` | `main.cpp:511` | 写 `pauseHook` → 整块 memcpy Config/Sensors → 锁内开/挑遥测 | `main.cpp:728`（退出广播）、`1024`（无条件）、`1117`（monitoring 内）、`872`（uismoke） | monitoring 时每 tick 走**两次**（幂等）；`pauseHook` 语义见 APP.md §3.3 |
| `TelemetryCloseAll` | `injector.cpp:689` | 解除映射 + 关闭全部遥测 slot | `main.cpp:740`（`CleanupAndExit`） | 内部加 `AppLock`；之后再清 `shmCfg/shmSens` |
| `NPTelemetryShmName`（inline） | `np_common.h:30` | 按 pid 拼遥测块名字（自带十进制转换，不依赖 `swprintf`） | `main.cpp:62`、钩子侧 | `n < 40` 时直接置空字符串 |
| `NPDefaultConfig`（inline） | `np_common.h:413` | 配置默认值（含 `magic`/`size`/`counters`/外观/行为） | `main.cpp:807`、`ui.cpp:449`、`settings.cpp:46` | **`version = 1` 是配置版本号，不是 `sizeof`**；`reserved[8]` 循环越界（APP.md G1）；**不重置** `autoInject`/`learnedAutoHook`/`detachPid`（G5） |
| `NPClearSensors`（inline） | `np_common.h:441` | 把 `NPSensors` 置成「空但合法」，`version = sizeof(NPSensors)` | `main.cpp:934`、`np_sensors.cpp:1030` | 会把 `screenW/H/refreshHz` 一起清 0 —— 所以显示模式必须写在 `Poll` **之后** |
| `NPClearTelemetry`（inline） | `np_common.h:472` | 清空遥测，`version = sizeof(NPTelemetry)` | `main.cpp:808`、`1184`、`injector.cpp:534` | 清 4096 帧 ×3 数组，别在热路径反复调 |
| `NPHistoryPush`（inline） | `np_common.h:223` | 推一个历史采样点（图表数据源，容量 256） | `main.cpp:1141`、`1145` | 主程序与钩子各自维护一份 |
| `NP_SHM_CONFIG` / `NP_SHM_SENSORS` / `NP_MAGIC` / `NP_MUTEX_SENSORS` | `np_common.h:22/23/47/24` | 共享内存名 / 魔数 / （未使用的）传感器互斥量名 | `main.cpp:40/45/66/84` | `NP_MUTEX_SENSORS` **全库无使用** |

---

## 3. 传感器与数据来源（本层入口 + 外部 API）

| 函数 | 文件:行号 | 作用 | 调用方 | 注意事项 |
| --- | --- | --- | --- | --- |
| `AppPollSensors` | `main.cpp:93` | `hub.Poll` → 显示模式 → 取 pid → ETW 目标/覆盖 Low 帧 → 帧数取 max → `PollGameGpu` → 每 5 秒数据来源日志 | `main.cpp:1116`、`871` | **顺序不能乱**（显示模式在 `Poll` 后）；`injected` 读取要加锁；无 pid 时 `gpuBusyMs = -1` |
| `ApiName`（static） | `main.cpp:529` | `gfxApi` → 字符串（D3D11/D3D12/D3D9/Vulkan/OpenGL/?） | `main.cpp:580`（状态栏） | 只用于状态栏文案 |
| `HookHint`（static） | `main.cpp:541` | 按 `hookFlags` 说明「卡在哪一步」 | `main.cpp:593` | 文案即用户可见的诊断结论，改动要谨慎 |
| `np::SensorHub::Init` | `src/sensors/np_sensors.cpp`（声明 `np_sensors.h:92`） | 一次性探测所有数据源 | `main.cpp:948`、`858`、`783` | 失败不致命 |
| `np::SensorHub::Poll` | 声明 `np_sensors.h:94`（实现 `np_sensors.cpp:1028`） | 填一个完整 `NPSensors` | `main.cpp:94`、`785`、`859` | **开头会 `NPClearSensors`**（清空一切） |
| `np::SensorHub::PollGameGpu` | 声明 `np_sensors.h:106`（实现 `np_sensors.cpp:374`） | 按 (pid, 帧数) 算 PDH GPU 忙时间 / compute / OFA | `main.cpp:156` | 需要 `frameCount`，所以要先用 `max(钩子, ETW)` 算好 |
| `np::SensorHub::Describe` | 声明 `np_sensors.h:112` | 数据源总览文本（写日志 + 状态栏） | `main.cpp:216`、`597`、`788` | — |
| `np::SensorHub::gameGpuDiag` | 声明 `np_sensors.h:109` | 上一次 `PollGameGpu` 没出数的原因 | `main.cpp:202` | 诊断用 |
| `np::SensorHub::Shutdown` | 声明 `np_sensors.h:93` | 释放厂商 SDK / PDH / WMI | `main.cpp:747`、`846`、`881` | 必须在 `GfxShutdown` 之后一起收尾 |
| `np::Etw()` / `Start` / `Stop` / `SetTargetPid` / `Snapshot` / `active` / `status` | `src/etw/np_etw.h:38/39/43/64/40/68` | 外置帧计时（内核 DxgKrnl Present 事件） | `main.cpp:119/136/152/724/983` | 需要管理员；**Low 帧之外一律不覆盖**；`status()` 返回拷贝（线程安全） |
| `np::BuildPanelData` | `src/common/np_build.cpp:199`（声明 `np_build.h:10`） | 把 `NPSensors + NPTelemetry + NPHistory + NPConfig` 整理成面板行 | `main.cpp:809`、`ui.cpp:393`、`overlay.cpp:98` | 三处显示通路共用，保证排版一致 |
| `npb::GfxInit` / `GfxShutdown` / `Factory` / `SetRasterScale` / `RasterScale` / `PanelBitmap::Render` | `src/common/np_bitmap.cpp:17/29`、`np_bitmap.h:19/25/26/34` | Direct2D/DirectWrite 工厂与 BGRA 位图渲染 | `main.cpp:780/746/943`、`ui.cpp:391/398`、`overlay.cpp:87/95/105` | 叠加定位用 `RasterScale()` 换算物理像素 |

---

## 4. 主循环 / 状态文本 / 线程锁（`main.cpp`、`injector.cpp`）

| 函数 | 文件:行号 | 作用 | 调用方 | 注意事项 |
| --- | --- | --- | --- | --- |
| `AppLock::AppLock` / `~AppLock` | `injector.cpp:33` / `34` | RAII 进出 `AppMutex()` 的 `CRITICAL_SECTION` | 全层十余处（清单见 APP.md §4.3） | 可重入；**锁内不做慢操作**（尤其不能放 `InjectInto`） |
| `AppMutex`（static） | `injector.cpp:24` | 函数局部静态的 `CRITICAL_SECTION` 持有者 | `AppLock` | C++11 起局部静态初始化线程安全；**不要换成全局对象**（初始化顺序问题） |
| `UpdateStatus`（static） | `main.cpp:548` | 生成 `gApp.statusText`：notice 优先 → 已接管 → 已注入未接管（`lastError`/`HookHint`）→ 未注入 | `main.cpp:1215` | 判活用**帧数增长**（`554-560`），不要改回 `tickMs` 差值 |
| `AppLog` | `injector.cpp:72` | 写 `%TEMP%\NextPerf.log` + `OutputDebugStringA`，每行带时间与 pid | 全层（含热点路径） | 每条都开关一次文件句柄；>1MB 时用 `GENERIC_WRITE`+`SetEndOfFile` 截断（`90-105`） |
| `AppLogPath` | `injector.cpp:65` | `%TEMP%\NextPerf.log` 全路径 | `main.cpp:699/973`、`injector.cpp:86/98/102` | `TEMP` 取不到时退化为当前目录 |
| `SetNotice` | `injector.cpp:111` | 写「给用户看」的提示，`noticeMs`（默认 12000ms）内不被状态栏刷新覆盖 | 全层十余处 | **必须内部加锁**（守护线程也会调）；会额外写一条日志 |
| `BuildStamp` | `injector.cpp:44` | 取 exe 文件时间戳当构建标识，结果缓存 | `ui.cpp:903`（标题）、`main.cpp:968`（启动日志） | 不用 `__DATE__/__TIME__`（clang `-Wdate-time` 会报错） |
| `AdminRelaunch` | `injector.cpp:121` | `ShellExecuteW("runas")` 提权重启自己 | `main.cpp:691`、`ui.cpp:507` | 返回 ≤32 视为用户拒绝；成功后调用方负责退出 |
| `ApiName` / `HookHint` | 见 §3 | — | — | — |

---

## 5. 注入器（`injector.cpp`）

| 函数 | 文件:行号 | 作用 | 调用方 | 注意事项 |
| --- | --- | --- | --- | --- |
| `InjectInto` | `injector.cpp:296` | 完整注入流程：选 DLL → `OpenProcess` → 写路径 → 取 `LoadLibraryW` → `CreateRemoteThread` → 等 10s → 登记 | `injector.cpp:637`（ScanNow）、`ui.cpp:546`、`main.cpp:924`（CLI） | 幂等（已注入直接返回 true）；**超时绝不释放远程内存**；`STILL_ACTIVE` 不算失败；收尾用 `AppLock` |
| `IsInjected` | `injector.cpp:153` | 打开 `Local\NextPerf_Injected_<pid>` 互斥量判断是否已注入 | `injector.cpp:298/616/621/640` | 互斥量由**钩子 DLL** 创建；钩子自卸载后即变假 |
| `InjectMutexName`（static） | `injector.cpp:147` | 拼互斥量名 | `IsInjected` | 名字格式与钩子侧必须一致 |
| `ProcessIsWow64` | `injector.cpp:287` | 判断目标是否 32 位进程跑在 64 位系统上 | `injector.cpp:313/385`、`main.cpp:922` | `InjectInto` 里被调用两次（冗余，见 G10） |
| `NpResolve32LoadLibraryW`（static） | `injector.cpp:226` | 走目标 **32 位 PEB** → `Ldr` → 模块链表 → `kernel32.dll` → 导出表，取 32 位 `LoadLibraryW` 绝对地址 | `injector.cpp:386` | **所有偏移按 32 位硬编码**（`Ldr +0x0C`、`InMemoryOrderModuleList +0x14`、`DllBase +0x18`、`BaseDllName +0x2C/+0x30`）；遍历上限 256；`next==head` 停止 |
| `NpExportAddr32`（static） | `injector.cpp:186` | 在 32 位模块里按导出名找函数，返回绝对地址 | `NpResolve32LoadLibraryW`（`264`） | `AddressOfNameOrdinals` 每项 **2 字节**、`AddressOfFunctions`/`AddressOfNames` 每项 **4 字节** |
| `NpReadRemote`（static） | `injector.cpp:177` | 跨进程读，要求读满 `n` 字节 | `NpExportAddr32`、`NpResolve32LoadLibraryW` | 返回 false 表示读失败或读不满 |
| `ExplainWin32`（static） | `injector.cpp:275` | 错误码 → 中文说明（5/87/126/127/299/1008） | `InjectInto` 各处 | 用户看到 `err=5` 没有意义，**新增错误码请补这里** |
| `DllPath` | `injector.cpp:135` | `gApp.dllPath` = `<exeDir>\NextPerfHook.dll`（顺带缓存 `exeDir`） | `injector.cpp:300`、`main.cpp:835/836/957` | 32 位变体在 `InjectInto` 内部按「最后一个 `.` 前插 `32`」推导 |
| `EnumProcesses`（static） | `injector.cpp:468` | `CreateToolhelp32Snapshot` 拿全部 (pid, 小写 exe 名) | `injector.cpp:566/516` | **不需要打开目标进程**，所以一定有名字；返回 vector，锁外调用 |
| `ExeNameOf`（static） | `injector.cpp:455` | 从路径取文件名并转小写 | `injector.cpp:508/580` | `settings.cpp:104-110` 里**就地重写了一遍**（因为它是 static）——改这里记得看那边 |
| `ProcessAlive` | `injector.cpp:699` | 进程是否存活（`GetExitCodeProcess == STILL_ACTIVE`） | `main.cpp:505`、`ui.cpp:538`、`injector.cpp:673` | 需要 `QUERY_LIMITED_INFORMATION`；打不开就当死 |
| `ProcessImagePath` | `injector.cpp:710` | 完整路径（`QueryFullProcessImageNameW`） | `ProcessExeName` | 拿不到返回空串 |
| `ProcessExeName` | `injector.cpp:722` | 进程名（路径最后一段，**原样大小写**） | `main.cpp:251/288`、`ui.cpp:544`、`injector.cpp:301` | 需要小写时调用方自己转（如 `main.cpp:252`） |
| `WatchThread`（static） | `injector.cpp:644` | 守护线程：`Sleep(800)` → `InjectorScanNow` | `InjectorInit` | 与钩子侧 200ms 轮询周期不同，改动要回头看 2.5 秒契约 |
| `InjectorInit` | `injector.cpp:652` | 置 `gWatchRun` 并 `CreateThread` | `main.cpp:993` | 返回值目前无人检查 |
| `InjectorShutdown` | `injector.cpp:658` | 清标志 + `WaitForSingleObject(2000)` + `CloseHandle` | `main.cpp:735` | 最坏等 800ms；必须在释放共享内存之前调 |

---

## 6. 进程扫描 / HookControl / 遥测回收（`injector.cpp`）

| 函数 | 文件:行号 | 作用 | 调用方 | 注意事项 |
| --- | --- | --- | --- | --- |
| `InjectorScanNow` | `injector.cpp:560` | 一轮看护：按 exe 名匹配游戏 → 更新 `pid/injected` → 认领已注入进程 → 收集目标 → **锁外**注入 → 回填 | `injector.cpp:647`（守护线程）、`main.cpp:687/999`、`ui.cpp:436` | `!monitoring` 或没有游戏时直接 return（**`g.pid` 不会清零**）；`EnumProcesses` 在锁外；注入在锁外；学习条目要 `learnedAutoHook` |
| `RequestHookDetach` | `injector.cpp:495` | 置 `cfg.detachPid = pid` 请求该钩子自卸载 | `injector.cpp:523`（`ForgetGameAt`） | 钩子 200ms 内响应 → `SelfUnloadNow`；请求 2.5 秒后由 `InjectorScanNow` 清掉 |
| `ForgetGameAt` | `injector.cpp:505` | 移除第 index 条游戏：先卸载钩子 → 摘 `injected` → 清遥测 → 删条目 → 落盘 | `injector.cpp:553`、`ui.cpp:498` | `pid == 0` 时**必须现场按名字查进程**；`erase` 用倒序；全程 `AppLock` |
| `ForgetLearned` | `injector.cpp:549` | 清空所有 `learned` 条目（逐条走 `ForgetGameAt`），返回条数 | `ui.cpp:456` | **倒序遍历**（`ForgetGameAt` 会 erase）；手动添加的保留 |
| `InjectorTick` | `injector.cpp:667` | 主循环每 tick：清死进程 + 收掉不再需要（pid 不在 `injected` 里）的遥测 slot | `main.cpp:1152` | 整段 `AppLock`；**真正的 `UnmapViewOfFile` 在这里**，不是「陈旧遥测回收」那段 |

---

## 7. 前台窗口跟踪（`main.cpp`）

| 函数 | 文件:行号 | 作用 | 调用方 | 注意事项 |
| --- | --- | --- | --- | --- |
| `IsUwpFrameHost`（static） | `main.cpp:243` | 窗口是不是 UWP 外壳（类名 `ApplicationFrameWindow`，兜底按进程名 `applicationframehost`） | `ResolveForegroundPid`（`262`） | 进程名兜底要打开进程，别放进每帧路径 |
| `ResolveForegroundPid`（static） | `main.cpp:257` | 取前台窗口对应的**真实应用 pid**（UWP 往下找一层子窗口） | `AppRememberForeground`（`284`）、`AppTrackForeground`（`498`） | 子窗口取「第一个 pid 与壳不同」的；找不到退回壳 pid |
| `AppRememberForeground`（static） | `main.cpp:283` | 记住「最近一个不是自己的前台进程」（`forePid`/`foreName`） | `ForegroundWinEvent`、`AppTrackForeground` | 过滤自己；名字拿不到写 `(未知进程)` |
| `ForegroundWinEvent`（static CALLBACK） | `main.cpp:294` | WinEvent 回调：每次前台变化都记一次 | 系统（`SetWinEventHook`） | 只认 `EVENT_SYSTEM_FOREGROUND` + `OBJID_WINDOW` + `CHILDID_SELF` |
| `AppEnsureForegroundHook`（static） | `main.cpp:304` | 懒安装前台钩子（一次即可） | `AppTrackForeground`（`495`） | `WINEVENT_OUTOFCONTEXT \| WINEVENT_SKIPOWNPROCESS` |
| `AppTrackForeground` | `main.cpp:494` | 每个 tick：装钩子 → 记当前前台 → 若 `forePid` 已退出则作废 | `main.cpp:1015` | 轮询只是兜底，**主路径是 WinEvent**（轮询会漏 ALT+TAB） |
| `NameLooksNonGame`（static） | `main.cpp:326` | 浏览器/聊天工具/IDE 等拒绝清单 | **无调用方**（死代码） | 随启发式自动注入一起停用 |
| `NameIsAntiCheat` / `AnyAntiCheatRunning`（static） | `main.cpp:366` / `376` | 反作弊关键词匹配 / 「当前有反作弊进程在跑」（5 秒缓存） | `AnyAntiCheatRunning` 无调用方 | 规则：**只检测 + 放弃，绝不绕过** |
| `WindowLooksLikeGame`（static） | `main.cpp:404` | 窗口特征判据：`WS_EX_NOREDIRECTIONBITMAP` 或窗口矩形与某显示器完全一致 | **无调用方**（死代码） | 保留了调研结论（不要枚举目标模块） |
| `RealGameWindow`（static） | `main.cpp:445` | UWP 壳窗口 → 真实子窗口 | **无调用方**（死代码） | 判游戏特征必须用真实窗口 |
| `AppAutoInjectTick`（static） | `main.cpp:488` | **空函数**（占位） | `main.cpp:1016` | 停用理由见 APP.md §9.4；**不要**在没想清判据前复活它 |

---

## 8. 可信名单学习 / 陈旧遥测回收（`main.cpp` 主循环内联）

| 逻辑块 | 文件:行号 | 作用 | 注意事项 |
| --- | --- | --- | --- |
| 学习验证块 | `main.cpp:1026-1114` | 三条件（`NP_HOOK_PRESENT` + `gfxApi != UNKNOWN` + `frameTotal` 持续 3 秒增长）→ 取进程名 → 去重 → 入库 | `sLearnPid/sLearnSince/sLearnFt` 是**函数局部 static**；同一 pid 只判一次；**push_back 没加 `AppLock`**（G2） |
| 进程名获取（主路径） | `main.cpp:1057-1069` | `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)` + `QueryFullProcessImageNameW` | UWP 也能查；取最后一段并转小写 |
| 进程名获取（兜底） | `main.cpp:1076-1092` | `CreateToolhelp32Snapshot` 遍历同名 pid 取 `pe.szExeFile` | **不加兜底会静默跳过学习**，很隐蔽 |
| 陈旧遥测回收 | `main.cpp:1171-1191` | `attached && pid && 帧数 2.5 秒不增长` → 清 `telemetry` + 从 `injected` 摘掉 | 与 `InjectorTick` 分工：这里不清 slot 映射 |
| overlay 判活 / `wantDesktop` | `main.cpp:1164-1196` | `attached = attached && 帧数在增长`；`wantDesktop = !off && monitoring && (mode==2 \|\| (!attached && mode!=1))` | **一律不要用跨进程时间戳** |
| overlay 诊断日志 | `main.cpp:1197-1212` | 每 5 秒打印判定依据 | 排查 HUD 问题的第一手证据，别删 |
| simulate 假数据 | `main.cpp:1118-1143` | `cfg.simulate` 时造正弦+噪声数据 + 推历史 | 开发预览用；**正式版应删除**（源码注释里写明） |

---

## 9. 界面（`ui.cpp`）

| 函数 | 文件:行号 | 作用 | 调用方 | 注意事项 |
| --- | --- | --- | --- | --- |
| `UiCreate` | `ui.cpp:883` | 算 DPI → 注册 `NextPerfMainCls` → `CreateWindowExW` → 按客户区把窗口尺寸补到 `kWinW×kWinH` → `Layout()` | `main.cpp:989`、`860` | 标题带 `BuildStamp()`；失败返回 false（调用方直接退出） |
| `UiDestroy` | `ui.cpp:924` | 释放预览位图/渲染器/字体 | `main.cpp:737`、`879` | 不销毁窗口本身（窗口由消息循环的销毁流程处理） |
| `UiRefresh` | `ui.cpp:930` | 节流 200ms：重渲染预览 + `InvalidateRect` | `ui.cpp:677`（`WM_TIMER`）、`main.cpp:874` | 与 `WM_TIMER` 的 250ms 双击节流 |
| `UiWindow` | `ui.cpp:922` | 返回主窗口句柄 | `main.cpp:656/676/995/1001` | — |
| `MainProc`（static） | `ui.cpp:669` | 窗口过程：`WM_CREATE/WM_TIMER/WM_ERASEBKGND/WM_DPICHANGED/WM_MOUSEMOVE/WM_LBUTTONDOWN/WM_LBUTTONUP/WM_MOUSEWHEEL/WM_PAINT/WM_CLOSE/NP_WM_TRAY/WM_COMMAND/WM_DESTROY` | 系统 | `WM_CLOSE` 在托盘不可用时改成真退出；`WM_COMMAND` 只在 `l == 0` 时处理托盘命令 |
| `Layout`（static） | `ui.cpp:235` | `gW.clear()` 后重建全部控件（左：计数器列表 + 游戏行 + 按钮；右：滑杆 + 循环选择 + 按钮） | `ui.cpp:688`（`WM_DPICHANGED`）、`916`（`UiCreate`） | 加行必改 `kWinH`；尺寸只在 `Layout`/`WM_PAINT` 各算一次，**别重复写魔法数** |
| `Add`（static） | `ui.cpp:206` | 注册一个控件，并做**同类型**重叠检测 + `AppLog` 警告 | `Layout` 内多处 | 只查同类型（父子包含关系不算 bug）；`W_GROUP/W_CHECK` 走 `push_back` 绕过它 |
| `Hit`（static） | `ui.cpp:420` | 按注册顺序找第一个命中的控件 | `ui.cpp:723`（`WM_LBUTTONDOWN`） | **先注册的吃掉点击**；`W_GROUP` 不可点 |
| `VisibleRect`（static） | `ui.cpp:402` | 控件 → 屏幕矩形（列表行按 `gScroll` 折算，不可见返回 false） | `Hit`、`DrawCounterList`、`WM_MOUSEMOVE` | 列表外的控件不参与命中与绘制 |
| `DoAction`（static） | `ui.cpp:431` | 动作分发：`A_START/A_STOP/A_SAVE/A_DEFAULT/A_FORGET/A_ADDGAME/A_REMGAME/A_QUIT/A_ELEVATE/A_LOGDIR/A_INJECTFRONT` | `ui.cpp:746` | 结尾统一 `InvalidateRect`；`A_ADDGAME` 必须在 `AppLock` 内 `push_back` 并同步填 `name` |
| `DrawCounterList`（static） | `ui.cpp:556` | 画计数器勾选列表（分组标题、复选框对勾、细分隔线、迷你滚动条） | `ui.cpp:783`（`WM_PAINT`） | 滚动条进度用 `gScroll / (gTotalRows - gVisibleRows)` |
| `DrawGameList`（static） | `ui.cpp:607` | 画游戏列表与右侧状态文案 | `ui.cpp:784` | `path` 为空时必须回退 `name`；⚠ 状态判活仍是旧的跨进程时钟写法（G3） |
| `DrawPreview`（static） | `ui.cpp:648` | 把预览位图 `AlphaBlend` 到预览框（等比、**只缩不放**） | `ui.cpp:787` | `fit > 1.0f` 时截到 1.0，避免糊 |
| `RenderPreview`（static） | `ui.cpp:389` | `BuildPanelData` → `Measure` → 渲染到位图 | `UiRefresh`（`935`） | 懒 `GfxInit`；任何一步失败置 `gPrevOk = false` |
| `PrevDraw`（static） | `ui.cpp:384` | `PanelBitmap::Render` 的回调，调 `PanelRenderer::Render` | `RenderPreview` | 与真实叠加用的是同一条渲染通路 |
| `MakeFonts` / `ReleaseFonts`（static） | `ui.cpp:159` / `152` | 建/删四个 GDI 字体（雅黑 UI，灰阶抗锯齿） | `WM_CREATE`、`WM_DPICHANGED`、`UiDestroy` | `MakeFonts` 内部先 `ReleaseFonts`，不会泄漏 |
| `FillR` / `StrokeR` / `Txt`（static） | `ui.cpp:174` / `185` / `195` | 圆角填充 / 圆角描边 / 文本（`TRANSPARENT` 背景 + `DrawTextW`） | 绘制各处 | 每次都 `Create`+`Delete` GDI 对象，别放进逐像素循环 |

---

## 10. 托盘 / 退出（`main.cpp`）

| 函数 | 文件:行号 | 作用 | 调用方 | 注意事项 |
| --- | --- | --- | --- | --- |
| `AddTray`（static） | `main.cpp:603` | `Shell_NotifyIconW(NIM_ADD)` + 记录 `gTrayAdded/gTrayOk` + 失败时提示 | `main.cpp:995` | 失败必须留日志与用户提示（「关闭窗口将直接退出」） |
| `ShowTrayMenu`（static） | `main.cpp:625` | 建右键菜单并 `TrackPopupMenu`，处理抢前台/`WM_NULL`/`WM_QUIT` 三个坑 | `AppTrayNotify`（`660`） | 模态循环会吃 `WM_COMMAND`/`WM_QUIT`；`gWantQuit` 在此补发 |
| `TrayAvailable` | `main.cpp:653` | 托盘是否真的注册成功 | `ui.cpp:857` | `false` 时 `WM_CLOSE` = 真退出 |
| `AppTrayNotify` | `main.cpp:655` | 托盘鼠标事件：右键→菜单；左键单击→快速开关叠加；双击→显隐窗口 | `ui.cpp:865`（`NP_WM_TRAY`） | 左键从关闭态恢复时不会立即显示，要等下一个 tick |
| `AppTrayCommand` | `main.cpp:675` | 菜单命令分发：`NP_TRAY_SHOW/OVERLAY/MONITOR/ELEVATE/LOGDIR/EXIT` | `ui.cpp:871`（`WM_COMMAND`, `l == 0`） | `ELEVATE`/`EXIT` 都要 `EndMenu()` + `PostQuitMessage(0)` |
| `CleanupAndExit`（static） | `main.cpp:718` | 幂等的顺序清理：停 ETW → `quit=1` + `AppPublish` + `Sleep(900)` → 删托盘 → 停守护线程/叠加/UI → 关遥测 → 关共享内存 → `GfxShutdown`/`hub.Shutdown` | `main.cpp:693/707/1223` | **顺序不能改**；`Sleep(900)` 是留给钩子自卸载的 |
| `NP_WM_TRAY` / `NP_TRAY_*` | `np_app.h:22-30` | 托盘回调消息（`WM_USER+100`）与命令 ID（1001-1006） | `main.cpp`、`ui.cpp` | 新增命令 ID 要与菜单项、`AppTrayCommand` 三处同步 |

---

## 11. 桌面叠加（`overlay.cpp`）

| 函数 | 文件:行号 | 作用 | 调用方 | 注意事项 |
| --- | --- | --- | --- | --- |
| `OverlayInit` | `overlay.cpp:45` | 注册 `NextPerfOverlayCls` + 建分层/穿透/置顶/不抢焦点窗口 + 初始化渲染器 | `main.cpp:990`、`861` | **渲染器失败必须先 `DestroyWindow` 再返回 false**，否则窗口永远挂在桌面上 |
| `OverlayShutdown` | `overlay.cpp:73` | 销毁窗口 + 释放位图/渲染器 | `main.cpp:736`、`878` | — |
| `OverlaySetVisible` | `overlay.cpp:79` | 记录 `gVisible` 并 `ShowWindow(SW_SHOWNOACTIVATE/SW_HIDE)` | `main.cpp:665/683/688/877/1213`、`ui.cpp:435/441` | 可见性最终由主循环 `wantDesktop` 决定 |
| `OverlayUpdate` | `overlay.cpp:85` | 按前台窗口显示器 DPI 光栅化 → 建面板数据 → 渲染位图 → `UpdateLayeredWindow` 定位到右上角 | `main.cpp:1214`、`873` | 不可见时**直接返回**（省 CPU）；透明度已在像素里，`SourceConstantAlpha = 255` |
| `OverlayProc`（static） | `overlay.cpp:34` | 窗口过程：吞掉 `WM_CLOSE`/`WM_DESTROY` | 系统 | 保证窗口只能由程序自己销毁 |
| `DrawCb`（static） | `overlay.cpp:29` | `PanelBitmap::Render` 回调 | `OverlayUpdate`（`105`） | — |

---

## 12. 配置存取（`settings.cpp`）

| 函数 | 文件:行号 | 作用 | 调用方 | 注意事项 |
| --- | --- | --- | --- | --- |
| `SettingsPath` | `settings.cpp:12` | `%APPDATA%\NextPerf\config.json`（目录现建；失败退化到当前目录），结果缓存 | `settings.cpp:47/181` | 换个位置等于用户配置丢失，别乱改 |
| `ReadFile` / `WriteFile`（static） | `settings.cpp:25` / `37` | 整文件读写（`_wfopen`） | `SettingsLoad`/`SettingsSave` | 失败静默；写失败也没有提示（未确认是否需要） |
| `SettingsLoad` | `settings.cpp:45` | 先 `NPDefaultConfig` 铺默认 → 逐键覆盖 → `cfgVersion` 升级补位 → 读 `games`/`learned` | `main.cpp:936` | 文件缺失/解析失败**保留默认值**；新增字段必须**同时**加读与写 |
| `SettingsSave` | `settings.cpp:138` | 整体重写 JSON（含 `cfgVersion = 5`、`games` 路径数组、`learned` 名字数组） | `main.cpp:950/1103/1224`、`ui.cpp:445/491`、`injector.cpp:545` | **运行态字段绝不持久化**（`pauseHook`/`detachPid`/`quit` 等，清单见 APP.md §13.2） |
| `np::Json::parse` / `dump` / `mkObj` / `mkNum` / `mkStr` / `mkArr` / `find` / `numOr` | `src/common/np_json.h:180/41/…` | 极简 JSON（只服务配置持久化） | `settings.cpp` | 不支持完整 JSON 规范；`mkArr` 返回 `Value` 不是 `Json` |

---

## 13. Win32 / 系统 API 使用要点（本层特有）

| API | 用在哪 | 关键点 |
| --- | --- | --- |
| `CreateFileMappingW` / `MapViewOfFile` | `main.cpp:39-48` | 页文件支撑；`Local\` 命名空间 |
| `OpenFileMappingW(FILE_MAP_READ)` | `main.cpp:63` | 遥测只读打开（主程序不写） |
| `NtQueryInformationProcess(26)` | `injector.cpp:237` | `ProcessWow64Information` 给的是**目标 32 位 PEB** |
| `CreateRemoteThread` | `injector.cpp:401` | 线程函数地址：64 位目标用本进程 `LoadLibraryW`，32 位目标必须用 PEB 解析出来的那个 |
| `WaitForSingleObject(th, 10000)` | `injector.cpp:415` | **超时不得释放远程内存** |
| `SetWinEventHook(EVENT_SYSTEM_FOREGROUND)` | `main.cpp:306` | 不会漏前台变化；`WINEVENT_OUTOFCONTEXT` 避免注入式回调 |
| `EnumChildWindows` | `main.cpp:265/452` | 解 UWP 壳窗口 → 真实应用窗口 |
| `CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS)` | `main.cpp:383/1077`、`injector.cpp:470` | 不需要打开目标进程，UWP/受保护进程也有名字 |
| `QueryFullProcessImageNameW` | `main.cpp:1062`、`injector.cpp:717` | 只要 `QUERY_LIMITED_INFORMATION`，UWP 也能查 |
| `EnumDisplaySettingsW` / `EnumDisplayDevicesW` | `main.cpp:100/421` | 显示模式；判「窗口 == 整屏」也用它 |
| `UpdateLayeredWindow(ULW_ALPHA)` | `overlay.cpp:131` | 分层窗口唯一的输出方式；位置/尺寸用物理像素 |
| `Shell_NotifyIconW(NIM_ADD/NIM_DELETE)` | `main.cpp:614/732` | 失败必须降级（`WM_CLOSE` = 真退出） |
| `ShellExecuteW("runas")` | `injector.cpp:125` | 返回值 ≤32 = 失败/被拒 |
| `TrackPopupMenu` | `main.cpp:643` | 模态循环吃 `WM_COMMAND`/`WM_QUIT`，见 §10 |
| `GetOpenFileNameW` | `ui.cpp:470` | 添加游戏 exe；需要 `comdlg32` |
| `AllocateAndInitializeSid` + `CheckTokenMembership` | `main.cpp:961-964` | 判定管理员（比 `IsUserAnAdmin` 稳） |

---

## 14. 改动检查清单（提 PR 前对一遍）

1. 新增/修改了跨线程共享的数据？→ 按 `APP.md` §4.3 的清单逐点补 `AppLock`。
2. 动过 UI 布局？→ 复核 `kWinH`（右列按钮底边目前 726/730），并在日志里搜「UI 重叠警告」。
3. 动过共享内存结构体？→ 只在末尾加字段或复用 `reserved[]`；同步 `sizeof` 自校验语义与读方校验。
4. 新增配置字段？→ 读 + 写 + UI + `cfgVersion` 升级四处都改；**运行态字段不要落盘**。
5. 动过任何轮询周期？→ 复核 2.5 秒契约（`detachPid` 超时 / 陈旧遥测 / overlay 判活）。
6. 动过注入失败路径？→ 保证每条失败路径都有 `AppLog` + `SetNotice`。
7. **更新 `docs/APP.md` 与 `docs/CATALOG.md`**，并在 `docs/APP-FUNCTIONS.md` 里同步行号。
