# NextPerf 传感器层 / ETW 层 —— 函数与接口速查表

> 配套文档：[SENSORS.md](SENSORS.md)（原理、自动优选策略、坑与教训）。
> 总入口：[CATALOG.md](CATALOG.md)；数据契约逐字段说明：[DATA-STRUCTS.md](DATA-STRUCTS.md)。
> 本表只回答四个问题：**叫什么、在哪、干什么、谁在调、注意什么**。
>
> **行号基准**：`np_sensors.cpp` 1064 行 / `np_sensors.h` 199 行 / `np_vendor.cpp` 342 行 /
> `np_vendor.h` 271 行 / `np_etw.cpp` 339 行 / `np_etw.h` 121 行（本次核对的实际行数）。
> 行号会随改动漂移 —— **改了就要改本表**。
>
> 图例：⚠ = 易踩坑；🔴 = 当前存在缺陷；❌ = 已实现但**没有任何调用方**（死代码）；
> ★ = 关键路径，别乱动。

---

## 1. `SensorHub` —— 传感器总入口（`np_sensors.h:90-197` / `np_sensors.cpp`）

| 函数 | 文件:行号 | 作用 | 调用方 | 注意事项 |
| --- | --- | --- | --- | --- |
| `SensorHub::Init()` | `np_sensors.cpp:488` | `CoInitializeEx` + 动态加载 NVML/NVAPI/ADL + 打开 HWiNFO + 建 PDH 查询（注册 7 条计数器）+ DXGI 取卡 + 注册表取标称频率 + 核数 + `ProbeSources()` | `main.cpp:948`（WinMain）、`main.cpp:783`（`--selftest`）、`main.cpp:858`（`--uismoke`） | 永远返回 true（失败靠"数据源不可用"表达）；末尾 `pdh_.Collect()` 是速率型计数器的基线（`:513`）；`gameGpuOk_` 只在此处置位（`:504`） |
| `SensorHub::Shutdown()` | `np_sensors.cpp:572` | 依次卸载 NVML/NVAPI/ADL、关 HWiNFO、关 PDH 查询、`CoUninitialize` | `main.cpp:747`（`CleanupAndExit`）、`main.cpp:846`、`main.cpp:881` | 必须先于进程退出；`PdhQuery::Close` 会连 `stars_` 一起清（见 P9） |
| ★ `SensorHub::Poll(NPSensors&)` | `np_sensors.cpp:1028` | **每轮主入口**：先 `NPClearSensors` 清空并把一切置 `-1`，再依次 `PollCpu/PollRam/(PollGpuNvidia|PollGpuAmd)/PollGpuGeneric/PollHwinfoExtras/ProbeRtTensorHardware`，最后拼 `sourceText` 并把 `available_` 或进 `sources` | `main.cpp:94`（`AppPollSensors`，主循环 120ms 一跳）、`main.cpp:785`、`main.cpp:859` | ⚠ **开头会清空整个结构体**，写在 `Poll` 之前的字段会被覆盖（`main.cpp:95-96` 有踩坑注释）；⚠ GPU 分支是 `if (nvidia) ... else if (amd)`，NVML 可用时 ADL 永不执行（`:1038-1042`） |
| ★ `SensorHub::PollGameGpu(pid, frameDelta, out)` | `np_sensors.cpp:374` | 按游戏 pid 读 PDH：算「每帧 GPU 忙时间」（`Running Time` 差分 ÷ 帧数增量）、compute / OFA 引擎占用 | `main.cpp:156`（`AppPollSensors`，**必须在 `Poll` 之后**） | ⚠ 与 `Poll` 是两个入口，调用顺序不能反；⚠ `frameDelta` 是**累计帧数**不是增量（`:446`）；⚠ 新样本最快 100ms 一个，值靠 `lastBusyMs_/lastCompute_/lastOfa_` 跨轮保留（P2）；⚠ 帧计数回退要重建基线（P4） |
| `SensorHub::gameGpuDiag()` | `np_sensors.h:109` | 返回上一次没出数的原因（空 = 正常） | `main.cpp:202`（写日志） | 正常路径也会写一句（`命中 N 个引擎实例，累计 X 秒`），方便对照 |
| ❌ `SensorHub::gameGpuAvailable()` | `np_sensors.h:107` | 返回 `gameGpuOk_`（`Running Time` 计数器是否加上） | **无调用方** | 想判断可用性时用它，别去猜 |
| ❌ `SensorHub::available()` | `np_sensors.h:111` | 返回探测到的数据源位掩码 `available_` | **无调用方** | UI 实际用的是 `sourceText` |
| ❌ `SensorHub::SetOverride()` / `overrideMask()` | `np_sensors.h:115-116` | 「手动指定优先数据源」的开关 | **无调用方**（`override_` 全程无人读） | 功能**未接线**：改了也不会生效 |
| `SensorHub::Describe()` | `np_sensors.cpp:605` | 把 `available_` 拼成 `"NVML+NVAPI+PDH"` 这样的短串 | `main.cpp:216`、`main.cpp:597`、`main.cpp:788` | ⚠ 反映的是"**探测到**"而不是"**本轮用到**"；NVAPI `EnumPhysicalGPUs` 失败时仍会报可用（P11） |
| ❌ `SensorHub::sources()` | `np_sensors.h:126` | 返回 `srcInfo_`（每个源的 present/used/note） | **无调用方** | 结构化探测详情，目前只在 `ProbeSources` 里填 |
| `SensorHub::ProbeSources()` | `np_sensors.cpp:581` | 把 6 个源的探测结果填进 `srcInfo_`，并累加 `available_` | `Init()`（`:568`） | 判断条件分别为 `nvml_.loaded`、`nvapi_.loaded`、`adl_.loaded`、`hwinfo_.loaded`、`pdh_.Count() > 0`、`comInited_` |
| `SensorHub::PollCpu()` | `np_sensors.cpp:680` | CPU 占用率（`GetSystemTimes` 差分）、核数/线程数、频率（Nt → PDH 回退）、温度（HWiNFO → LHM → ACPI）、功耗（EMI → HWiNFO） | `Poll()`（`:1035`） | 频率回退口径有争议（见 SENSORS.md 3.1）；`cpuBaseValid_` 首轮不输出占用率 |
| `SensorHub::PollRam()` | `np_sensors.cpp:767` | 内存用量 + 从 DXGI 写显卡名/显存总量/厂商 | `Poll()`（`:1036`） | 它写的 `gpuName` 随后可能被 NVML/NVAPI 覆盖（优先级见 SENSORS.md 第 4 节） |
| `SensorHub::PollGpuNvidia()` | `np_sensors.cpp:786` | NVML 全量指标 + NVAPI 域/温度/转速/**频率回退** | `Poll()`（`:1041`） | 每 32 轮询重挑"最忙的卡"（`:794`）；NVML 拿不到频率（`<=0`）才调 NVAPI（`:908`） |
| `SensorHub::PollGpuAmd()` | `np_sensors.cpp:921` | ADL Overdrive5：占用率/温度/核心与显存频率 | `Poll()`（`:1042`，**仅当 NVIDIA 分支没走**） | 单位：`/100` → MHz、`/1000` → ℃ |
| `SensorHub::PollGpuGeneric()` | `np_sensors.cpp:953` | 跨厂商兜底：自己 `Collect()` 一次；GPU 占用率取**最忙引擎**；显存占用用 PDH 求和 | `Poll()`（`:1044`） | ⚠ 只在对应字段 `< 0` 时才覆盖；⚠ 显存是 `Dedicated + Shared` 混算（未定论） |
| `SensorHub::PollHwinfoExtras()` | `np_sensors.cpp:979` | HWiNFO 补充：GPU 热点、显存结温、GPU 功耗、风扇、显存已用 | `Poll()`（`:1045`） | 内部会 `hwinfo_.Refresh()`（重新解析整块共享内存）；只填空字段 |
| `SensorHub::ProbeRtTensorHardware()` | `np_sensors.cpp:996` | 硬件级 RT/Tensor：HWiNFO 名字含 `Tensor`/`RT Core` → LHM WMI | `Poll()`（`:1046`） | 民用卡上**通常拿不到**（见 `docs/RT_TENSOR.md`）；拿不到就保持 `-1` |
| `SensorHub::WmiScalar()` | `np_sensors.cpp:94` | 跑一条 WQL，取**第一行**指定数值属性 | `PollCpu`（`:744`，ACPI 热区） | `const` 成员；只读第一行（多热区只取第一个） |

---

## 2. `PdhQuery` —— PDH 查询与求和（`np_sensors.h:22-88` / `np_sensors.cpp:104-361`）

### 2.1 生命周期与注册

| 函数 | 文件:行号 | 作用 | 调用方 | 注意事项 |
| --- | --- | --- | --- | --- |
| `PdhQuery::Open()` | `np_sensors.cpp:105` | `PdhOpenQueryW(nullptr, 0, &query_)` | `Init()`（`:498`） | 幂等（`opened_`） |
| `PdhQuery::Close()` | `np_sensors.cpp:112` | `PdhCloseQuery` + 清 `counters_` **和 `stars_`** | `Shutdown()`（`:577`） | ⚠★ **必须一起清 `stars_`**：否则 `AddStarCounter` 以为加过了，继续用野 `HCOUNTER`（P9） |
| ★ `PdhQuery::AddStarCounter(path)` | `np_sensors.cpp:264` | 注册**动态实例**计数器（路径含 `(*)`），取值走 `PdhGetFormattedCounterArrayW` | `Init()`：`:504`（Running Time）、`:505`（Utilization）、`:508`（Energy Meter Power） | ★ 需要"游戏后启动也能读到"的计数器**必须**用这个，不能用 `AddWildcard`（P1）；同名重复调用直接返回已存在句柄的状态 |
| ⚠ `PdhQuery::AddWildcard(path)` | `np_sensors.cpp:124` | `PdhExpandCounterPathW` 展开通配符后逐个 `PdhAddEnglishCounterW`（**实例列表从此冻结**） | `Init()`：`:499`、`:509`、`:510`、`:511`、`:512` | ⚠ 只适合"启动时就已经存在且不会新增"的实例（如 `Processor Information`）；用它读按进程的 GPU 引擎数据 = P1 bug |
| ❌ `PdhQuery::RefreshWildcard(path)` | `np_sensors.cpp:151` | 重新展开通配符，把**新出现的实例**补进查询（内部判重避免重复求和） | **无调用方**（已被 `AddStarCounter` 取代） | 历史方案，保留实现；`np_sensors.h:36-41` 有当年的说明 |
| `PdhQuery::Collect()` | `np_sensors.cpp:189` | `PdhCollectQueryData` + 逐个 `PdhGetFormattedCounterValue` 填 `value/valid` | `Init()`（`:513`）、`PollGameGpu`（`:388`）、`PollGpuGeneric`（`:954`） | 速率型计数器**至少要两次**采集才有值 |
| `PdhQuery::Count()` | `np_sensors.h:79` | 已注册的**静态**计数器数量（不含 `stars_`） | `ProbeSources()`（`:599`） | 只要不为 0 就认为 PDH 源可用 |

### 2.2 取值 / 求和（**这里全部是口径问题**）

| 函数 | 文件:行号 | 作用 | 调用方 | 注意事项 |
| --- | --- | --- | --- | --- |
| ★ `SumStarCounter(path, kw1, kw2, out, hitCount)` | `np_sensors.cpp:277` | 读**动态实例数组**，对实例名同时含 `kw1`/`kw2` 的项**求和**；过滤 `CStatus` | `PollGameGpu`：`:400`（compute）、`:410`（OFA）、`:426`（Running Time） | ⚠ 大小写**敏感**（`wcsstr`）→ 见 🔴3.4；⚠ `Running Time` 单位 **100ns**，调用方自己 `/1e7`（P6）；`hitCount` 回传命中数用于诊断 |
| ★ `MaxStarCounter(path, kw1, kw2, out)` | `np_sensors.cpp:313` | 同上但取**最大值** | `PollCpu`（`:757`，EMI `PKG`）、`PollGpuGeneric`（`:964`，最忙引擎） | ⚠ **占用率百分比不能跨引擎求和**（会恒为 100%），必须用这个（P5）；EMI 用它是为了在多个 `PKG` 实例里取整包 |
| `SumWhere(objKw, instKw, out)` | `np_sensors.cpp:215` | 对**静态**计数器求和：按对象名 + 实例名子串过滤 | `PollCpu`（`:709`，`Processor Information` 的 `_Total`）、`PollGpuGeneric`（`:971`，`GPU Adapter Memory`） | ⚠ `instKw` 为空表示"全都要"→ `:971` 因此把 `Dedicated + Shared` 加在一起（未定论，见 SENSORS.md 第 8 节）；依赖 `PathHasObject` |
| ❌ `SumWhere2(objKw, kw1, kw2, out)` | `np_sensors.cpp:230` | 实例名同时含两个关键字才计入（为 `\GPU Engine(*)` 那种长实例名设计） | **无调用方** | 动态实例方案落地后不再需要 |
| ❌ `SumInstance(counterKw, kw1, kw2, out)` | `np_sensors.cpp:247` | 按「计数器名 + 实例名」过滤求和（区分同实例名下的不同计数器） | **无调用方** | `np_sensors.h:51-55` 记录了它的存在理由（`Running Time` 与 `Utilization Percentage` 实例名相同） |
| ❌ `MaxWhere(objKw, instKw, instOut, out)` | `np_sensors.cpp:348` | 静态计数器里取最大值，并回传实例名 | **无调用方** | |
| `PathHasObject(path, obj)`（static） | `np_sensors.cpp:204` | 从 `\\Computer\Object(Instance)\Counter` 里解析对象名并比较 | `SumWhere` / `SumWhere2` / `MaxWhere` | ⚠ 它要求路径带 `\\计算机名` 前缀。**本次实测 `PdhExpandCounterPathW` 确实带前缀**（`\\MSI\...`）→ 它能工作；`np_sensors.h:55` 说它会失败的注释**已过时**（P8） |
| `PdhQuery::Counter`（结构） | `np_sensors.h:24-30` | `path / instance / handle / value / valid` | — | `instance` 只在静态展开那一路填 |
| `PdhQuery::StarCounter`（结构） | `np_sensors.h:85-86` | `path + handle` | — | 私有；与 `counters_` 分开存 |

---

## 3. NVML（`np_vendor.h:54-77` / `np_vendor.cpp:8-56`）

| 函数 / 成员 | 文件:行号 | 作用 | 调用方 | 注意事项 |
| --- | --- | --- | --- | --- |
| `NvmlApi::Load()` | `np_vendor.cpp:8` | 三路径找 `nvml.dll` → `GetProcAddress` 16 个导出 → 校验必需项 → `Init()` | `SensorHub::Init()`（`np_sensors.cpp:492`） | 硬要求只有 `Init`/`DeviceGetHandleByIndex`/`DeviceGetUtilizationRates`（`:40-42`）；`Init()` 非 0 也整体失败（`:43-45`） |
| `NvmlApi::Unload()` | `np_vendor.cpp:50` | `Shutdown()` + `FreeLibrary` | `SensorHub::Shutdown()`（`:573`） | 先调 `Shutdown` 再卸库 |
| `DeviceGetCount` / `DeviceGetHandleByIndex` | `np_vendor.cpp:24-25` | 枚举设备 | `PollGpuNvidia`：`:790`、`:799`、`:810` | 用 `_v2` 后缀版本，旧版签名不同 |
| `DeviceGetName` | `np_vendor.cpp:26` | 显卡名（ANSI）| `:813` | 结果拷贝进 `gpuName`（UTF-8 由 `NPCopyStr` 处理） |
| `DeviceGetUtilizationRates` | `np_vendor.cpp:27` | `gpu`/`memory` 占用 % | `:801`（挑卡）、`:817` | 挑卡依据就是它 |
| `DeviceGetMemoryInfo` | `np_vendor.cpp:28` | 显存 total/free/used（字节） | `:823` | 换算 GiB 除以 `1024^3` |
| `DeviceGetTemperature` | `np_vendor.cpp:29` | GPU 温度（℃） | `:830` | 传感器枚举 `NVML_TEMPERATURE_GPU=0` |
| `DeviceGetPowerUsage` / `EnforcedPowerLimit` | `np_vendor.cpp:30-31` | 功耗 / 功耗上限（**毫瓦**） | `:832`、`:834` | 都要 `/1000` → W |
| `DeviceGetClockInfo` | `np_vendor.cpp:32` | 核心/显存频率（MHz） | `:839-840` | 用 `NVML_CLOCK_GRAPHICS=0` / `NVML_CLOCK_MEM=2` |
| `DeviceGetFanSpeed` | `np_vendor.cpp:33` | 风扇 % | `:836` | 拿不到时回退 NVAPI 的 RPM |
| ❌ `DeviceGetPerformanceState` | `np_vendor.cpp:34` | P-State | **从未调用** | 解析了指针但没接线 |
| `DeviceGetTemperatureThreshold` | `np_vendor.cpp:35` | 温度上限（`GPU_MAX`） | `:843` | 写进 `maxGpuTemp_` |
| `DeviceGetEncoderUtilization` / `DecoderUtilization` | `np_vendor.cpp:36-37` | 编码/解码器占用 %（含采样周期） | `:848`、`:852` | 值写进 `encUtil_/decUtil_`，但❌**无调用方**（面板看不到） |

---

## 4. NVAPI（`np_vendor.h:79-181, 269-271` / `np_vendor.cpp:58-114, 316-342`）

| 函数 / 常量 | 文件:行号 | 作用 | 调用方 | 注意事项 |
| --- | --- | --- | --- | --- |
| interface ID 表 | `np_vendor.cpp:60-69` | 9 个 `NvU32` ID（`Initialize 0x0150E828`、`EnumPhysicalGPUs 0xE5AC921F`、`GetDynamicPstatesInfoEx 0x60DED2ED`、**`GetAllClockFrequencies 0xDCB616C3`** …） | `Load()` | 后两个来自 PresentMon 的接口表；**改 ID 等于换 API，必须核对来源** |
| `NvapiApi::Load()` | `np_vendor.cpp:71` | `nvapi64.dll`（→ `nvapi.dll`）+ `nvapi_QueryInterface` → 逐个 ID 换指针 → `Initialize()` → `EnumPhysicalGPUs()` | `SensorHub::Init()`（`:493`） | ⚠ 每个 ID 都要判 `== 0 && fn`；⚠ `EnumPhysicalGPUs` 失败**不会**让 `loaded=false`，只把 `gpuCount=0`（P11） |
| `NvapiApi::Unload()` | `np_vendor.cpp:107` | `NvAPI_Unload` + `FreeLibrary` | `Shutdown()`（`:574`） | |
| `NvapiApi::QueryInterface` | `np_vendor.h:156` | `NvAPI_QueryInterface_t`：唯一入口 | `Load()` | 函数**不是**导出符号，必须走它（`docs/ARCHITECTURE.md:70-72`） |
| `NV_GPU_CLOCK_FREQUENCIES` | `np_vendor.h:132-139` | `version` + `clockTypeFlags` + `domain[32]{presentFlags, frequency(kHz)}` | `NvapiReadGpuClocks` | 逐字段对照 PresentMon 的 `nvapi.h:5962`；位域在内存里就是普通 `NvU32` |
| `NV_GPU_CLOCK_FREQUENCIES_VER_2` | `np_vendor.h:141-142` | 版本宏（官方 VER_3 与 VER_2 同结构体，取更保守的 2） | 同上 | 用 `NP_MAKE_NVAPI_VERSION`（`np_vendor.h:93-94`）拼 `sizeof<<16 \| ver` |
| **domain 索引** | `np_vendor.h:144-148` | `GRAPHICS=0` / **`MEMORY=4`** / `PROCESSOR=7` / `VIDEO=8` | `NvapiReadGpuClocks` | ⚠★ **MEMORY 是 4 不是 1**，照常识写 1 会读到垃圾（源码原话：「极易搞错」） |
| ★ `NvapiReadGpuClocks(nv, gpu, coreMhz, memMhz)` | `np_vendor.cpp:321` | 直读 GPU 核心/显存频率，**kHz → MHz**（`/1000`） | `PollGpuNvidia`（`np_sensors.cpp:910`） | `clockTypeFlags = 0` 表示 `CURRENT_FREQ`；只有 `presentFlags & 1` 才采信；返回 true = 至少读到一个域。**只在 NVML 值为 `<=0` 时作为回退**（不覆盖 NVML） |
| `GPU_GetDynamicPstatesInfoEx` | `np_vendor.cpp:91`；消费 `np_sensors.cpp:870-895` | 4 个公开利用率域 + 探测域 4..7 | `PollGpuNvidia` | `dom(i)`：`bIsPresent==0` 或百分比不在 0~100 → `-1`；探测到扩展域会置 `nvapiExtSeen_` 并在 `sourceText` 里加 `扩展域:有` |
| `GPU_GetThermalSettings` | `np_vendor.cpp:89`；消费 `np_sensors.cpp:864-869` | 温度（仅当 NVML 没给时：`out.gpuTemp < -200`） | `PollGpuNvidia` | 版本宏 `NV_GPU_THERMAL_SETTINGS_VER_2`（`np_vendor.h:107`） |
| `GPU_GetTachReading` | `np_vendor.cpp:95`；消费 `np_sensors.cpp:897-900` | 风扇转速（RPM） | `PollGpuNvidia` | 仅在 NVML 风扇 % 拿不到时用 |
| `GPU_GetFullName` | `np_vendor.cpp:87`；消费 `np_sensors.cpp:859-863` | 显卡名（NVML 失败时的第二来源） | `PollGpuNvidia` | 短串上限 `NVAPI_SHORT_STRING_MAX = 64` |
| ❌ `GPU_GetUsages` | `np_vendor.cpp:93`；结构 `np_vendor.h:119-123` | 历史用法域数组（下标 2 曾是 3D 占用） | **从未调用** | `np_vendor.h:118` 的单位注释本身写着"？实测为百分比"，语义未确认 |
| ❌ `maxGpuTemp()` / `encoderUtil()` / `decoderUtil()` | `np_sensors.h:193-195` | 访问器 | **无调用方** | 值算出来了但没显示 |

---

## 5. AMD ADL（`np_vendor.h:183-241` / `np_vendor.cpp:116-152`）

| 函数 | 文件:行号 | 作用 | 调用方 | 注意事项 |
| --- | --- | --- | --- | --- |
| `NpAdlMalloc(size)` | `np_vendor.cpp:117` | `__stdcall` 分配回调（ADL 要求） | ADL 内部经 `Main_Control_Create` | 必须 `__stdcall`，否则 32 位下栈会坏 |
| `AdlApi::Load()` | `np_vendor.cpp:119` | `atiadlxx.dll` → `atiadlxy.dll`；`GetProcAddress` 6 个导出；`Main_Control_Create` + `Adapter_NumberOfAdapters_Get` | `SensorHub::Init()`（`:494`） | 硬要求 `Main_Control_Create` + `Overdrive5_CurrentActivity_Get`（`:134-136`）；`adapterCount` 负数会被夹成 0（`:141`） |
| `AdlApi::Unload()` | `np_vendor.cpp:146` | `Main_Control_Destroy` + `FreeLibrary` | `Shutdown()`（`:575`） | |
| `Overdrive5_CurrentActivity_Get` | `np_vendor.cpp:130`；消费 `np_sensors.cpp:930`、`:940` | 占用率 + 频率 | `PollGpuAmd` | 单位：`iEngineClock/100` → MHz、`iMemoryClock/100` → MHz；**Overdrive5 是旧接口，新卡可能不返数** |
| `Overdrive5_Temperature_Get` | `np_vendor.cpp:131`；消费 `np_sensors.cpp:948` | 温度 | `PollGpuAmd` | `iTemperature/1000` → ℃；调用前才判 `if (ptr)` |
| `NPAdapterInfo` / `NPADLPMActivity` / `NPADLTemperature` | `np_vendor.h:187-221` | 自声明结构体（不依赖 ADL SDK 头） | 同上 | 每个结构体调用前都要填 `iSize = sizeof(...)`（`np_sensors.cpp:929`、`:939`、`:947`） |

---

## 6. HWiNFO 共享内存（`np_vendor.h:243-267` / `np_vendor.cpp:154-313`）

| 函数 | 文件:行号 | 作用 | 调用方 | 注意事项 |
| --- | --- | --- | --- | --- |
| `HwinfoCtx::Open()` | `np_vendor.cpp:173` | 试 `Global\HWiNFO_SENS_SM2` → `Global\HWiNFO_SENS_SM`，`MapViewOfFile`，成功后立刻 `Refresh()` | `SensorHub::Init()`（`:496`） | 用户必须在 HWiNFO 里开 **Shared Memory Support**；返回 false 就整个源不可用 |
| `HwinfoCtx::Close()` | `np_vendor.cpp:186` | `UnmapViewOfFile` + `CloseHandle` + 清 `readings` | `Shutdown()`（`:576`） | |
| ★ `HwinfoCtx::Refresh()` | `np_vendor.cpp:224` | **带异常保护**地解析整块共享内存 | `PollHwinfoExtras`（`:981`） | ⚠ 保护是「VEH + `setjmp/longjmp`」（clang 在 mingw 下没有 `__try/__except`）；⚠ 它是**最后一道防线**，不能当正常路径用（P10） |
| `HwinfoCtx::RefreshUnsafe()` | `np_vendor.cpp:237` | 真正的解析：校验签名 `'HWiS'`、`version>=2`、`sizeSensor>=264`、`sizeReading>=292`；用 `VirtualQuery` 的映射大小反推上界；逐条读 label/unit/value | `Refresh()` | ⚠★ `offReading`/`offSensor` 是对方进程写的，**不可信**：下溢会读到映射外（P10）；`maxRead` 上限 4096；`type == 0xFFFFFFFF` 结束 |
| `NpHwinfoHeader` | `np_vendor.cpp:162-171` | v2 头布局 | `RefreshUnsafe` | 手写布局（`signature/version/revision/pollTime/offSensor/offReading/sizeSensor/sizeReading`） |
| `NpHwStr(s, maxLen)` | `np_vendor.cpp:193` | ANSI → `std::wstring`（`CP_ACP`） | `RefreshUnsafe` | ⚠ HWiNFO 用**当前代码页**；非中英文代码页可能乱码导致匹配失败 |
| `HwinfoCtx::Find(sensorKw, labelKw, out)` | `np_vendor.cpp:298` | 传感器分组名 + 读数名**都要**匹配（大小写不敏感子串） | `PollCpu`（`:719-722`、`:761-762`）、`PollHwinfoExtras`（`:983-990`） | 传 `nullptr` 表示"该字段不限" |
| `HwinfoCtx::FindAny(labelKw, out)` | `np_vendor.cpp:308` | 只匹配读数名 | `PollCpu`（`:721`）、`PollHwinfoExtras`（`:984-993`）、`ProbeRtTensorHardware`（`:1004-1005`） | 命中**第一条**即返回（顺序取决于 HWiNFO 的排列） |
| `NpContainsI(hay, needle)` | `np_vendor.cpp:289` | 大小写不敏感子串匹配 | `Find` / `FindAny` | 空 needle 视为匹配（`return true`） |
| `NpSehHandler` / `NpSehInstall` | `np_vendor.cpp:212` / `:216` | 向量化异常处理 + 一次性安装 | `Refresh()` | `thread_local` 的 `jmp_buf`（`:210-211`）——**不能改成全局**（同 hook 层的教训） |

---

## 7. CPU 频率：`CallNtPowerInformation`（`np_sensors.cpp:621-677`）

| 函数 / 常量 | 文件:行号 | 作用 | 调用方 | 注意事项 |
| --- | --- | --- | --- | --- |
| `NpProcessorPowerInfo` | `np_sensors.cpp:632-639` | 按 MSDN 补齐的结构体（MinGW 头文件里没有） | `CpuFreqFromPowerInfo` | 字段顺序 `Number/MaxMhz/CurrentMhz/MhzLimit/MaxIdleState/CurrentIdleState` 不能错 |
| `kNpProcessorInformation = 11` | `np_sensors.cpp:641` | `POWER_INFORMATION_LEVEL` 里的 `ProcessorInformation` | 同上 | 写错数字会拿到别的信息类 |
| ★ `CpuFreqFromPowerInfo(mhzOut)` | `np_sensors.cpp:643` | 运行时 `LoadLibraryW("powrprof.dll")` + `GetProcAddress("CallNtPowerInformation")`；缓冲区按逻辑处理器数自算；取**最高核** MHz | `PollCpu`（`:706`） | ⚠ **不能链接 `-lpowrprof`**（`build.bat:71` 里没有），因为 MinGW 头文件缺声明（`:628-629`）；⚠ 失败时返回的是 **NTSTATUS**（`0xC0000023`）**不是所需长度**（`:660-662`，本机实测确认）；成功时 `buf[i].MaxMhz` 在本机等于当前频率，**别拿它当真实最大值** |
| `cpuBaseMHz_` | `np_sensors.h:156` | 注册表 `~MHz`（CPU 标称频率），PDH 回退用 | `PollCpu`（`:712`） | 本机实测 = 3072；与 PDH 自己的标称 2300 不一致 → 回退口径存疑（SENSORS.md 3.1） |
| `cpuFreqFromNt_` | `np_sensors.h:155` | 标记本次频率是否来自 Nt 路径 | 仅内部写 | 没有消费者；注释说 false 时"可能系统性偏低"，本机实测方向相反 |

---

## 8. WMI 辅助（`np_sensors.cpp:15-102`）

| 函数 / 常量 | 文件:行号 | 作用 | 调用方 | 注意事项 |
| --- | --- | --- | --- | --- |
| `kClsidWbemLocator` / `kIidIWbemLocator` | `np_sensors.cpp:16-19` | 硬编码 GUID | `WmiRows` | ⚠ mingw 没有 `wbemuuid.lib`，**必须**自己声明（P17）；抄错 = WMI 全废 |
| `WmiRow` | `np_sensors.cpp:22-25` | 一行的数值属性 + 字符串属性 | `WmiRows` / 消费点 | 只记录**成功取到**的属性 |
| `WmiRows(ns, wql, numProps, strProps, out)` | `np_sensors.cpp:27` | 极简 WQL 查询：`CoCreateInstance` → `ConnectServer` → `CoSetProxyBlanket` → `ExecQuery(FORWARD_ONLY\|RETURN_IMMEDIATELY)` → 逐行转 `double` | `WmiScalar`（`:97`）、`PollCpu`（`:727`）、`ProbeRtTensorHardware`（`:1009`） | 支持 `VT_BSTR/R8/R4/I4/UI4/I8/UI8`（`:68-74`）；返回 `!out.empty()`；⚠ 每次调用都会新建 WMI 连接（有开销，别放进高频路径） |
| `WmiScalar(ns, wql, prop, out)` | `np_sensors.cpp:94` | 取第一行的某个属性 | `PollCpu`（`:744`） | `const` 成员 |

---

## 9. ETW：`EtwMonitor`（`np_etw.h:27-119` / `np_etw.cpp`）

| 函数 / 常量 | 文件:行号 | 作用 | 调用方 | 注意事项 |
| --- | --- | --- | --- | --- |
| `Etw()` | `np_etw.cpp:24` | 全局单例（函数内 `static`） | `main.cpp`（多处）、`OnEvent`/`OnBuffer` | 静态局部 → 首次调用时构造，构造里初始化锁（`np_etw.cpp:29-32`） |
| `kDxgKrnl` GUID | `np_etw.cpp:13-15` | `Microsoft-Windows-DxgKrnl {802EC45A-1E99-4B83-9920-87C98277BA9D}` | `Start()` | |
| `kKwPresent` | `np_etw.cpp:16-20` | `Present(0x8000000) \| Base(0x1)` | `Start()` | ★ 只开 `Present` **实测抓到的是记账事件**（每帧两次），真正的 `Present_Start` 在 `Base` 下面 |
| `kSessionName` | `np_etw.cpp:22` | `NextPerfFrameTrace` | `Start`/`ConsumeLoop`/`Stop` | 会话名全局唯一；残留靠 `ERROR_ALREADY_EXISTS` 兜（`:222-225`） |
| `EtwMonitor::EtwMonitor()` | `np_etw.cpp:29` | **构造函数里 `InitializeCriticalSection`** | 单例构造 | ⚠★ 不能挪回 `Start()` 懒初始化：非管理员 / `--uismoke` 路径会在全零 CS 上 `EnterCriticalSection`（UB） |
| `~EtwMonitor()` | `np_etw.cpp:34` | `Stop()` + `DeleteCriticalSection` | 进程退出 | |
| `SetStatus(const char*)` | `np_etw.cpp:39` | 加锁写 `status_` | `Start`（`:233`、`:245`、`:257`、`:261`）、`ConsumeLoop`（`:275`） | ⚠ 消费者线程也写它 → 必须加锁 |
| `status()` | `np_etw.cpp:45` | 加锁读并**返回拷贝** | `main.cpp:173`、`main.cpp:984/986` | ⚠★ 返回引用会让主线程与消费者线程读写同一个 `std::string`（UB） |
| ★ `Start()` | `np_etw.cpp:200` | 建实时会话（`ClientContext=1` QPC、`EVENT_TRACE_REAL_TIME_MODE`、`FlushTimer=1`）→ `EnableTraceEx2(DxgKrnl, INFORMATION, kKwPresent)` → 起消费者线程 | `main.cpp:983`（WinMain，启动时一次） | ⚠ **需要管理员**；`ERROR_ACCESS_DENIED` → 状态文案"创建 ETW 会话被拒"，返回 false，**不阻塞程序**；⚠ 反作弊也会让它 ACCESS_DENIED —— **不要尝试绕过**；⚠ 同会话残留先 STOP 再建（`:222-225`） |
| `Stop()` | `np_etw.cpp:285` | `stop_=true` → `CloseTrace` → 等消费者线程最多 2s → `ControlTraceW(STOP)` | `~EtwMonitor`、`main.cpp:724`（`CleanupAndExit`） | ⚠ 不显式停会留同名会话（`main.cpp:722-723` 的原话）；`:246`、`:258` 在失败路径上也会调 |
| `ConsumeLoop()` | `np_etw.cpp:265` | 消费者线程：`OpenTraceW`（REAL_TIME + EVENT_RECORD）→ `ProcessTrace`（阻塞）→ `CloseTrace` | `Start` 起的线程（`:252-255`） | 失败时 `SetStatus("OpenTrace 失败")` 并 `active_=false` |
| `OnBuffer(lf)` | `np_etw.cpp:145` | 返回 `stop_ ? FALSE : TRUE` 结束 `ProcessTrace` | ETW 运行时回调 | `__stdcall` |
| ★ `ClassifyEvent(ev, opOut, nameOut, nameCap)`（static） | `np_etw.cpp:64` | 用 TDH 动态取事件名（`TaskNameOffset` → `OpcodeNameOffset` 兜底），判定是否一次 present | `OnEvent`（`:117`） | ⚠★ **名字必须拷进调用方缓冲**（TDH 的名字指向局部 vector，返回即析构 → use-after-free）；⚠ `TRACE_EVENT_INFO` 里**没有** `EventNameOffset`；⚠ 判定 = 名字是 `Present` 或以 `Present_` 开头（**不能用 `wcsstr` 子串**）+ `opcode==1`（P14） |
| ★ `OnEvent(ev)` | `np_etw.cpp:97` | 过滤 `targetPid_` → 查 `{Id,Version,Opcode}` 缓存 → 未命中则 `ClassifyEvent` → 记录事件名（去重，最多 6）→ `OnPresent(TimeStamp)` | ETW 运行时回调 | ⚠★ **`targetPid_ == 0` 时一条都不算**（否则全系统 present 污染 Low 帧，P12）；命中缓存时不重新取名，所以事件名只在首次记录 |
| `OnPresent(qpc100ns)` | `np_etw.cpp:147` | 累计 `presentN_`；算帧间隔（`0.02 < ms < 1000` 才入环形缓冲）；1 秒窗口计数算 `fps_` | `OnEvent`（`:142`） | 全程持锁；时间戳单位 100ns（`/10000` → ms） |
| `RecalcLow()` | `np_etw.cpp:173` | 对环形缓冲排序，取最慢 1% / 0.1% 的平均帧时间 → 换算 FPS | `Snapshot`（`:329`） | ⚠ 样本 < 32 直接返回；⚠★ **限流 200ms（5Hz）**：它是在持锁状态下全排序 2048 个样本，会拖住消费者线程（P16） |
| `SetTargetPid(pid)` | `np_etw.cpp:307` | 切换跟踪目标；**重建全部基线**（`lastQpc_/winStart_/winN_/ringN_/frameMs_/fps_/low*/presentN_`） | `main.cpp:119`（`AppPollSensors`，每轮） | pid 未变时直接返回；不重建基线会把两个进程的间隔算成一帧 |
| `Snapshot(Result*)` | `np_etw.cpp:321` | 加锁取一份快照（`frameMs/fps/presentN/inWindow/low1Fps/low01Fps/evNames`），内部会调 `RecalcLow` | `main.cpp:136`（覆盖 Low 帧）、`main.cpp:152`（取帧计数）、`main.cpp:172`（诊断日志） | 返回 `presentN > 0` 表示"有数据"；⚠ 主线程 120ms 内会调 3 次（`:179`） |
| `EtwMonitor::Result` | `np_etw.h:50-63` | 快照结构（含 `evNames[6][96]` 诊断事件名） | `main.cpp:132`、`:151`、`:171` | `evNames` 是用来定位"帧率翻倍"的，别删 |
| `EvKey` / `cachedKey_` / `cachedVal_` | `np_etw.h:84-87`、`np_etw.cpp:109-123` | 事件判定缓存（最多 8 项） | `OnEvent` | 避免每条事件都走 TDH |

**ETW 与它层的接口约定（`main.cpp`）**：只覆盖 Low 帧（`:136-139`）、只提供帧计数（`:142-154`）、
其余一律不覆盖（`:121-130` 的翻车教训）。

---

## 10. `NPSensors` 字段 → 生产者对照（`src/common/np_common.h:236-300`）

| 字段 | 默认（`NPClearSensors`，`:441-470`） | 谁会写 | 位置 |
| --- | --- | --- | --- |
| `tickMs` / `valid` | 0 / 0 | `SensorHub::Poll` | `np_sensors.cpp:1031-1032` |
| `cpuUsage` | `-1` | `PollCpu`（`GetSystemTimes` 差分） | `:694` |
| `cpuTemp` | `-273` | `PollCpu`（HWiNFO → LHM → ACPI） | `:723`、`:735`、`:746` |
| `cpuPower` | `-1` | `PollCpu`（EMI `PKG` → HWiNFO） | `:758`、`:763` |
| `cpuClock` | `-1` | `PollCpu`（Nt → PDH 回退） | `:707`、`:712` |
| `cpuCores` / `cpuThreads` | 0 / 0 | `PollCpu`（`Init` 里探测） | `:700-701` |
| `ramUsedGB` / `ramTotalGB` / `ramPct` | `-1` | `PollRam`（`GlobalMemoryStatusEx`） | `:771-773` |
| `gpuName` | `""` | `PollRam`(DXGI) → `PollGpuNvidia`(NVML) → NVAPI | `:777`、`:814`、`:862` |
| `gpuVendor` | 0 | `PollRam`（DXGI VendorId） | `:780-782` |
| `gpuUsage` | `-1` | NVML → NVAPI `domGpu` → ADL → PDH 最忙引擎 | `:818`、`:894`、`:941`、`:965` |
| `gpuTemp` | `-273` | NVML → NVAPI → ADL | `:831`、`:868`、`:949` |
| `gpuHotspot` / `gpuMemTemp` | `-273` | `PollHwinfoExtras`（仅 HWiNFO） | `:985`、`:988` |
| `gpuPower` / `gpuPowerLimit` | `-1` | NVML / HWiNFO；上限仅 NVML | `:833`、`:990`、`:835` |
| `gpuClock` / `memClock` | `-1` | NVML → NVAPI → ADL | `:839-840`、`:911-912`、`:942-943` |
| `gpuFanPct` / `gpuFanRpm` | `-1` | NVML % → NVAPI RPM → HWiNFO % | `:837`、`:899`、`:991` |
| `vramUsedGB` / `vramTotalGB` / `vramPct` | `-1` | NVML / DXGI(总量) / PDH(求和) / HWiNFO | `:824-826`、`:778`、`:972-974`、`:993` |
| `domGpu` / `domFb` / `domVid` / `domBus` | `-1` | NVAPI 4 域 | `:880-883` |
| `domExt[4]` / `domExtPresent` | `-1` / 0 | NVAPI 域 4..7 探测 | `:885-892` |
| `hwRtPct` / `hwTensorPct` / `hwRtTensorSrc` | `-1` / `-1` / 0 | `ProbeRtTensorHardware`（HWiNFO → LHM） | `:1004-1021` |
| ★ `gpuBusyMs` | `-1` | `PollGameGpu`（PDH `Running Time` 差分） | `:377`、`:483` |
| 🔴 `engCompute` / `engOfa` | `-1` | `PollGameGpu`（PDH，**过滤器大小写不匹配 → 实际永远 `-1`**） | `:402`、`:412` |
| `screenW/H` / `refreshHz` | 0 | app 层 `EnumDisplaySettingsW` | `main.cpp:100-104` |
| `sources` / `sourceText` | 0 / `""` | 各 Poll 函数按需 `\|=`；`Poll` 末尾拼文字 | `:819`、`:893`、`:944`、`:966`、`:1060-1061` |
| `version` | `sizeof(NPSensors)` | `NPClearSensors` | `np_common.h:448` |

**消费点**：面板行 `src/common/np_build.cpp:257-269`（CPU）、`:306-340`（GPU）、`:342-366`（GPU 帧时间 / AI 引擎）、
`:367-377`（显存 / NVAPI 域）、`:381-401`（RT/Tensor）、`:457-459`（内存）、`:466-467`（数据源文字）。

---

## 11. 其它关键常量 / 宏

| 名称 | 位置 | 值 / 含义 |
| --- | --- | --- |
| `NP_SENSOR_SRC` 位枚举 | `np_common.h:138-148` | `NVML=1<<0`、`NVAPI=1<<1`、`ADL=1<<2`、`HWINFO=1<<3`、`LHM=1<<4`、`PDH=1<<5`、`WMI=1<<6`、`INTEL=1<<7` |
| `NP_NVAPI_DOM_*` | `np_vendor.h:177-181` | GPU=0 / FB=1 / VID=2 / BUS=3 |
| `NP_NVAPI_CLK_*` | `np_vendor.h:144-148` | GRAPHICS=0 / **MEMORY=4** / PROCESSOR=7 / VIDEO=8 |
| `NVAPI_MAX_GPU_UTILIZATIONS` | `np_vendor.h:89` | 8（官方只用前 4） |
| `NV_GPU_CLOCK_FREQUENCIES_VER_2` | `np_vendor.h:141-142` | `(sizeof<<16) \| 2` |
| `NP_ADL_OK` | `np_vendor.h:185` | 0 |
| `NP_HIST_CAP` | `np_common.h:205` | 256（历史曲线容量，与传感器层无关但常一起看） |
| 链接库（主程序） | `build.bat:71` | `... -lpdh ... -ltdh ...`，**没有 `-lpowrprof`**、没有 `-lwbemuuid` |
| 编译源（主程序） | `build.bat:75` | 含 `src\etw\np_etw.cpp`、`src\sensors\np_vendor.cpp`、`src\sensors\np_sensors.cpp` |

---

## 12. 死代码 / 未接线清单（改这一层前先看一眼）

**❌ 无调用方**：`PdhQuery::RefreshWildcard`（`np_sensors.cpp:151`）、`SumWhere2`（`:230`）、
`SumInstance`（`:247`）、`MaxWhere`（`:348`）、`SensorHub::SetOverride`/`overrideMask`（`np_sensors.h:115-116`）、
`SensorHub::available()`（`:111`）、`SensorHub::sources()`（`:126`）、`SensorHub::gameGpuAvailable()`（`:107`）、
`maxGpuTemp()`/`encoderUtil()`/`decoderUtil()`（`:193-195`）、`nvapiExtSeen()`（`:196`）、
`NvmlApi::DeviceGetPerformanceState`（`np_vendor.cpp:34`）、`NvapiApi::GPU_GetUsages`（`:93`）。

**已注册但无人读**：PDH `\Thermal Zone Information(*)\Temperature`（`np_sensors.cpp:512`）。

**只写不读的成员**：`hwinfoTried_`（`np_sensors.h:160`，只在 `:495` 置 true）、
`cpuFreqFromNt_`（`:155`，只在 `:708`、`:713` 写）、`override_`（`:147`）。

**🔴 已知缺陷（不影响编译，但功能是死的）**：`engtype_compute`（`:401`）/ `engtype_ofa`（`:411`）
大小写与真实实例名不符 → AI 引擎两个数永远是 `-1`（详见 SENSORS.md 3.4 与 6.1）。

---

## 13. 维护规则

1. 本表与 `SENSORS.md` **必须**和代码同一次改动一起更新。
2. 新增/删除 PDH 计数器时，同步更新 `SENSORS.md` 2.5 的注册清单（那是"这一层订阅了什么"的权威列表）。
3. 新增 `NPSensors` 字段时：同步 `SENSORS.md` 第 10 节、`tests/struct_check.py`、Python ctypes 镜像，
   并确认 `NPClearSensors` 给了正确的"不可用"默认值。
4. 任何"手工指定优先源"的功能在 `override_` 真正接进 `Poll` 之前，不要写进用户文档。
5. `docs/CATALOG.md` 已存在：把本层的数据源、PDH 计数器路径、`NPSensors` 字段、
   诊断入口（`gameGpuDiag()` / `sourceText` / `Describe()` / `Etw().status()`）登记进 §2.6，
   并**更正 §3.4 里本层三个文件的旧行数**（真实值见本表开头的「行号基准」）。
