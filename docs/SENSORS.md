# NextPerf 传感器层与 ETW 层技术说明

> 覆盖代码：`src/sensors/np_vendor.h/.cpp`、`src/sensors/np_sensors.h/.cpp`、`src/etw/np_etw.h/.cpp`，
> 以及输出结构 `NPSensors`（`src/common/np_common.h`）。
>
> 本文只写**源码里的事实**与**本机实测过的结论**。凡是没能确认的一律写「未确认」，不编造。
>
> 配套速查表：[SENSORS-FUNCTIONS.md](SENSORS-FUNCTIONS.md)。
> 总入口：[CATALOG.md](CATALOG.md)（§2.6 传感器、§3.4 文件清单）。
> 数据契约逐字段说明：[DATA-STRUCTS.md](DATA-STRUCTS.md)。
> RT / Tensor 为什么做不出来：[RT_TENSOR.md](RT_TENSOR.md)。

---

## 0. 核对基准与阅读须知

### 0.1 行号基准（本次核对）

本文所有 `文件:行号` 都基于下面这个状态。**任务书里给的行数是更早版本的行数，已经过时**，本文按实际读到的行数写：

| 文件 | 行数（本次核对） | 任务书给的行数 |
| --- | --- | --- |
| `src/sensors/np_sensors.cpp` | **1064** | 917 |
| `src/sensors/np_sensors.h` | **199** | 131 |
| `src/sensors/np_vendor.cpp` | **342** | 294 |
| `src/sensors/np_vendor.h` | **271** | 216 |
| `src/etw/np_etw.cpp` | **339** | 281 |
| `src/etw/np_etw.h` | **121** | 78 |

行号会随任何一次改动漂移。**改这一层就必须同步改本文与 `SENSORS-FUNCTIONS.md`**（见第 7 节）。

> ⚠ 顺带一提：`CATALOG.md` §3.4「传感器与 ETW」那一行的文件行数（`~917 / 131`、`~294 / 216`、`~281 / 78`）
> **与本表不一致，是旧值**，需要一并更正（本文不改别人的文档，只在此标注）。
>
> ⚠ 本文引用的 `src/app/main.cpp`、`src/common/np_build.cpp` 行号属于**次级引用**（用来说明调用方），
> 那两个文件由别的迭代线在改动；以 `src/sensors/`、`src/etw/` 内的行号为准，app 层引用请随手复核。

### 0.2 标注约定

* **实测** = 写本文时在开发机（计算机名 `\\MSI`，24 个逻辑处理器，Intel 核显 + NVIDIA 独显混合显卡，
  与 README「测试环境」一节描述一致）上用 PDH 原生 API / `typeperf` / 程序自身日志验证过，证据写在对应小节。
* **源码注释** = 结论只来自代码注释，本文未能独立复现（例如注释里记的历史现象）。
* **未确认** = 没法从源码或本次实测确定的，明确标出，别当成事实用。

### 0.3 本文用到的验证手段（可复现）

* 原生 PDH 数组 API（与代码 `PdhGetFormattedCounterArrayW` 同一条路）：PowerShell `Add-Type` + P/Invoke `pdh.dll`，
  取 `\\GPU Engine(*)\\Utilization Percentage` 的实例名，确认大小写。
* `typeperf "<counter path>" -sc 1`：打印**原始** PDH 路径与实例名（注意 `Get-Counter` 的 `InstanceName`
  会把实例名显示成全小写，**不能**用它判断大小写，会被误导）。
* `Get-Counter`、注册表 `HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0\~MHz`。
* 程序自身日志 `%TEMP%\NextPerf.log` 的「数据来源」块（由 `src/app/main.cpp:160-219` 输出）。

---

## 1. 这一层是干什么的

### 1.1 一句话

**在不注入游戏、不写驱动、不要求管理员的前提下，从系统与厂商驱动里把「能拿到的最好的一份」硬件/系统指标
采出来，填进一个跨进程共享的 `NPSensors` 结构，供主程序面板与游戏内钩子读取。**

### 1.2 不注入也能采到的指标

| 类别 | 指标 | 主要来源 |
| --- | --- | --- |
| CPU | 占用率、每核当前频率、包功耗、温度、物理核/逻辑线程数 | `GetSystemTimes`、`CallNtPowerInformation`、PDH EMI、HWiNFO/WMI |
| 内存 | 已用/总量/百分比 | `GlobalMemoryStatusEx` |
| GPU | 名称、厂商、占用率、温度、热点、显存结温、功耗、功耗上限、核心频率、显存频率、风扇 %/RPM | NVML / NVAPI / ADL / HWiNFO / PDH |
| 显存 | 已用/总量/百分比 | NVML / DXGI / PDH / HWiNFO |
| NVAPI 域 | 图形引擎、显存控制器、视频引擎、PCIe 总线、未公开域 4..7 | NVAPI |
| 引擎级 | **每帧 GPU 执行时间**、compute 引擎占用、OFA（光流加速器）占用 | PDH（按游戏 pid） |
| RT/Tensor | 若厂商或第三方软件暴露则采信 | HWiNFO / LibreHardwareMonitor |
| 显示 | 分辨率、刷新率 | `EnumDisplaySettings`（在 `src/app/main.cpp:100-104`） |
| 帧计时（外置） | 帧率、帧时间、1% / 0.1% Low、present 计数 | ETW `Microsoft-Windows-DxgKrnl`（`src/etw/`） |

### 1.3 数据流与控制流

```
                        ┌──────────────── SensorHub（主程序内，单实例）────────────────┐
NVML / NVAPI / ADL ────►│ Init(): 一次性探测 + 建立 PDH 查询                          │
HWiNFO 共享内存 ───────►│ Poll(NPSensors&): 每轮先清空，再按指标逐个"选最优源"填入      │
PDH  ──────────────────►│ PollGameGpu(pid, frameDelta, out): 单独一路，按游戏 pid 取引擎数据│
WMI / LHM ─────────────►│                                                              │
CallNtPowerInformation ►└───────────────────────────────┬──────────────────────────────┘
GetSystemTimes / DXGI / 注册表                          │
                                                        ▼
                                     NPSensors ──► [Local\NextPerf_Sensors_v1] ──► 钩子（只读）
                                                        │
                                                        ▼
                                          面板行（src/common/np_build.cpp）

ETW（外置，不注入）: EtwMonitor ──► Result{fps, frameMs, low1, low01, presentN} ──► 只覆盖 Low 帧 + 提供帧计数
```

关键点：

* **传感器只在主程序采集一次**，钩子只读不采（`docs/ARCHITECTURE.md:16-17`）——
  避免多开游戏时 NVML/WMI 被重复初始化、互相打架。
* `SensorHub::Poll()` 开头就会 `NPClearSensors(&out)`（`np_sensors.cpp:1030`），
  把每个字段重置成 `-1` / `-273` / 0。**任何"写一次就指望一直留着"的字段都会被下一轮清掉**，
  需要跨轮保留的值必须自己存成员变量（这是「时有时无」类 bug 的根源，见 P2/P3）。
* `PollGameGpu()` 与 `Poll()` 是**两个入口**，`PollGameGpu` 必须在 `Poll` 之后调用
  （`main.cpp:94` 调 `Poll`，`main.cpp:156` 调 `PollGameGpu`）。

### 1.4 源码里写明的设计原则（`np_sensors.h:1-6`）

1. 所有数据源都是可选的，任何一个失败都不影响其他源；
2. 同一指标有多个源时按准确度排序：**厂商 SDK > 第三方监控软件共享内存 > WMI > PDH**；
3. UI 上会显示每个指标最终用的是哪个源，方便排查（实际落地为 `NPSensors::sourceText`
   + 每 5 秒一条的日志块；`NPSensors::sources` 位掩码是机器可读版本）。

> ⚠ 注意：原则 2 是**总原则**，具体到每个指标的真实顺序要看代码——例如 CPU 功耗实际是
> **PDH EMI 优先、HWiNFO 回退**（`np_sensors.cpp:749-764`），CPU 温度是 HWiNFO → LHM → ACPI。
> 以第 4 节的「自动优选总表」为准。

### 1.5 为什么"不注入也能采"很重要

* 独占全屏、Vulkan/OpenGL 游戏、注入被反作弊拒绝的场景下，**钩子那条路全断**，但 PDH/NVML/DXGI 照读不误
  —— 它们读的是系统计数器与驱动，与游戏怎么呈现无关（`README.md:185-189`）。
* ETW 帧计时也是外置的，连注入都不需要；它在本项目里已经落地（`src/etw/`），
  只用来修正 Low 帧与提供帧计数（见 3.6）。

### 1.6 谁在读这一层

| 消费者 | 位置 | 读什么 |
| --- | --- | --- |
| 面板行构造 | `src/common/np_build.cpp:257-467` | `NPSensors` 的 CPU/GPU/显存/内存/域/AI 引擎/来源文字 |
| 游戏内钩子 | 共享内存 `Local\NextPerf_Sensors_v1` | 同上（钩子只读） |
| 诊断日志 | `src/app/main.cpp:160-219` | 每个指标 + 它的来源（每 5 秒一条） |
| 自检 / UI 冒烟 | `src/app/main.cpp:783-798`、`858-859` | `Describe()` 与关键字段 |
| 端到端回归 | `tests/verify_metrics.py` | 日志里的「命中 N 个引擎实例」与 GPU 帧时间 > 0 |

---

## 2. 按数据源逐路拆解

### 2.0 总览

| 数据源 | 加载方式 | 依赖 | 需要管理员 | 主要贡献 |
| --- | --- | --- | --- | --- |
| NVML | `LoadLibraryW("nvml.dll")` 三路径回退 | NVIDIA 驱动 | 否 | GPU 占用/温度/功耗/频率/风扇/显存/编码解码 |
| NVAPI | `LoadLibraryW("nvapi64.dll")` + `nvapi_QueryInterface` | NVIDIA 驱动 | 否 | 4 个利用率域 + 未公开域 + 温度/转速 + **频率回退** |
| ADL | `LoadLibraryW("atiadlxx.dll")` | AMD 驱动 | 否 | 占用率/温度/频率（Overdrive5） |
| HWiNFO 共享内存 | `OpenFileMappingW("Global\\HWiNFO_SENS_SM2")` | 用户装 HWiNFO 且开启 Shared Memory | 否 | 主板级传感器：CPU 温度/功耗、GPU 热点/结温、风扇 |
| PDH | `PdhOpenQueryW` + 通配/星号计数器 | 系统自带 | 否 | 通用 GPU 引擎占用、显存占用、CPU 性能百分比、**按 pid 的 GPU 帧时间**、**CPU 功耗（EMI）** |
| CallNtPowerInformation | 运行时 `LoadLibraryW("powrprof.dll")` + `GetProcAddress` | 系统自带 | 否 | **CPU 当前频率**（每核 MHz） |
| WMI | `CoCreateInstance` + WQL | 系统自带 | 否（但 ACPI 热区常不给数） | ACPI 热区温度、LibreHardwareMonitor 读数 |
| 系统 API | 直接调用 | 系统自带 | 否 | CPU 占用、内存、显卡型号/厂商/显存总量、标称频率、核数 |

### 2.1 NVML（`np_vendor.cpp:8-56`，消费在 `np_sensors.cpp:786-918`）

**提供**：占用率（`gpu`/`memory`）、显存总量/已用、温度、功耗、功耗上限、核心/显存频率、风扇 %、
温度上限阈值、编码器/解码器利用率。

**关键函数与行号**

| 函数 | 行号 |
| --- | --- |
| `NvmlApi::Load()` | `np_vendor.cpp:8` |
| `NvmlApi::Unload()` | `np_vendor.cpp:50` |
| `SensorHub::PollGpuNvidia()` | `np_sensors.cpp:786` |

**实现逻辑与自动优选**

* 依次尝试 `nvml.dll` → `C:\Windows\System32\nvml.dll` →
  `C:\Program Files\NVIDIA Corporation\NVSMI\nvml.dll`（`np_vendor.cpp:10-18`）。
* 逐个 `GetProcAddress` 16 个导出（`np_vendor.cpp:22-37`）；**只有 `Init`、`DeviceGetHandleByIndex`、
  `DeviceGetUtilizationRates` 三个是硬要求**，缺任一个就整体卸载并返回 false（`np_vendor.cpp:40-42`）。
  `Init()` 非 0 同样整体失败（`:43-45`）。
* 多卡时**每 32 次轮询重挑一次「正在干活的那张卡」**：取 `u.gpu` 最大者；平时沿用上次结果以省开销
  （`np_sensors.cpp:794-807`）。
* 各指标独立写入，任一失败只影响自己：占用 `:817`、显存 `:823`、温度 `:830`、功耗 `:832`、
  功耗上限 `:834`、风扇 %`:836`、核心/显存频率 `:839-840`、温度上限 `:843`、编码/解码 `:848/852`。
* 写占用或显存成功时会 `out.sources |= NP_SRC_NVML`（`:819`），这是「最终用了哪个源」的机器可读标记。

**已知限制**

* GeForce 上**没有** RT / Tensor 域（`np_vendor.h:7-16`）。
* `nvmlDeviceGetEncoderUtilization` / `DecoderUtilization` / `TemperatureThreshold` / `FanSpeed`
  在部分型号/驱动上不导出或返回错误 —— 代码用 `if (ptr)` 逐个判断，缺失只是该指标留 `-1`。
* **读到的值目前没有消费者**：`maxGpuTemp()`、`encoderUtil()`、`decoderUtil()` 在整个仓库里
  没有任何调用点（本次核对 grep 全仓确认），即"采了但没显示"。见第 6.2 节。

### 2.2 NVAPI（`np_vendor.cpp:58-114`、`321-342`，消费在 `np_sensors.cpp:855-917`）

见 3.5 的深挖。这里只列限制：

* `NvAPI_Initialize` 失败（部分驱动会拒绝）→ 整体不可用，`available_` 里没有 `NP_SRC_NVAPI`（`np_vendor.cpp:98-100`）。
* **函数不是导出符号**，必须用 `nvapi_QueryInterface(interfaceId)` 换指针（`docs/ARCHITECTURE.md:70-72`）。
* 官方只公开 4 个利用率域；域 4..7 是**持续探测**，驱动不填就一直是 `bIsPresent = 0`。
* `GPU_GetUsages`（0x189A1FDF）虽然解析了指针（`np_vendor.cpp:93`），但**没有任何调用点**；
  `np_vendor.h:118` 还留着"下标 2 历史上是 3D 引擎占用，单位 1/100 %？实测为百分比"的不确定注释。
* `EnumPhysicalGPUs` 失败时只把 `gpuCount` 置 0，`loaded` 仍为 `true`（`np_vendor.cpp:102-104`）——
  于是 `Describe()` 会把 NVAPI 报成"可用"，而实际一个句柄都没有。判断能否用要同时看 `nvapi_.loaded`
  与 `nvapi_.gpuCount > 0`（`np_sensors.cpp:856` 就是这么写的）。

### 2.3 AMD ADL（`np_vendor.cpp:116-152`，消费在 `np_sensors.cpp:921-950`）

**提供**：占用率（`iActivityPercent`）、核心频率、显存频率、温度（Overdrive5）。

**实现与优选**

* `atiadlxx.dll` → `atiadlxx.dll` 失败再试 `atiadlxy.dll`（32 位兼容版，`np_vendor.cpp:121-122`）。
* 硬要求 `Main_Control_Create` + `Overdrive5_CurrentActivity_Get`（`:134-136`）；
  内存分配回调用自带的 `malloc` 包装（`:117`）。
* 多适配器时每 32 次轮询重选活动量最高的那个（`np_sensors.cpp:925-936`）。
* 单位换算：`iEngineClock / 100`（10kHz → MHz）、`iMemoryClock / 100`、`iTemperature / 1000`
  （`np_sensors.cpp:942-943`、`949`）。
* `Overdrive5` 是**旧接口**，新卡/新驱动上可能完全不返回数据（代码只判返回值，不做其它探测）。

**★ 一个容易忽略的分支问题**：`Poll()` 里是

```cpp
bool nvidia = (out.gpuVendor == 1) || nvml_.loaded;
bool amd    = (out.gpuVendor == 2) || adl_.loaded;
if (nvidia) PollGpuNvidia(out);
else if (amd) PollGpuAmd(out);      // np_sensors.cpp:1038-1042
```

即 **只要 NVML 加载成功，ADL 分支就永远不会执行**。混合显卡机器（本机就是 NVIDIA + Intel）上
`nvml_.loaded` 通常为真，于是：显卡名/占用/温度全部来自 NVIDIA 卡，**即使游戏实际跑在核显上**。
README 已如实写出这个未处理项（`README.md:217-218`）。

### 2.4 HWiNFO 共享内存（`np_vendor.cpp:154-313`，消费在 `np_sensors.cpp:718-724`、`979-994`、`1003-1006`）

**提供**：CPU Package 温度、CPU Package Power、GPU 热点、显存结温、GPU 功耗、GPU 风扇、
GPU Memory Used（MB→GB），以及（若存在）名字含 `Tensor` / `RT Core` 的读数。

**实现与优选**

* 共享内存名依次尝试 `Global\HWiNFO_SENS_SM2`、`Global\HWiNFO_SENS_SM`（`np_vendor.cpp:175`）。
* 布局按 HWiNFO 官方 v2 布局手写（`np_vendor.cpp:155-171`），**不依赖任何 HWiNFO 头文件**。
* 每次都重新解析整个读数表（`RefreshUnsafe`，`:237`），并用 **`Find` / `FindAny` 做大小写不敏感的子串匹配**
  （`NpContainsI`，`:289`）——所以查询关键字写成 `L"CPU Package"` / `L"Package"` 都能命中。
* 结构变化保护：clang 在 mingw 目标下不支持 MSVC 的 `__try/__except`，这里用**向量化异常处理 +
  `setjmp/longjmp`** 自建等价保护（`np_vendor.cpp:205-235`），防止 HWiNFO 正在改结构时把主程序带崩。
* 越界防护做了三层：签名 `'HWiS'`、`version >= 2`、`sizeSensor >= 264 && sizeReading >= 292`（`:239-241`）；
  再用 `VirtualQuery` 得到的映射大小反推**读数条数上界**与**传感器分组上界**（`:245-260`），
  `maxRead` 上限 4096。`type == 0xFFFFFFFF` 视为结束（`:270`）。

**已知限制**

* 用户必须在 HWiNFO 设置里打开 **Shared Memory Support**，否则 `loaded = false`，
  所有 `Find*` 直接返回 false（`np_sensors.cpp:718`、`:980`、`:1003`）。
* 字符串按 **ANSI / 当前代码页**解码（`NpHwStr`，`np_vendor.cpp:193-203`）——
  非中文/英文代码页下标签可能乱码，进而匹配不到。
* 温度/功耗的单位不做换算，直接采信 HWiNFO 给的数值（功耗按 W、温度按 ℃、`GPU Memory Used` 按 MB→/1024）。
  这是**约定**不是校验。

### 2.5 PDH（`np_sensors.cpp:104-361` 的 `PdhQuery`，注册在 `Init` 的 `:498-514`）

**提供**：通用 GPU 引擎占用率、显存占用、CPU 性能百分比、按 pid 的 GPU 执行时间、按 pid 的
compute/OFA 占用、CPU 包功耗（EMI）。

**`Init()` 注册的计数器（这是"这一层到底订阅了什么"的唯一权威清单）**

| 行号 | 注册方式 | 路径 | 用途 |
| --- | --- | --- | --- |
| `:499` | `AddWildcard` | `\GPU Engine(*)\Utilization Percentage` | （静态展开，见 P1） |
| `:504` | **`AddStarCounter`** | `\GPU Engine(*)\Running Time` | 游戏进程 GPU 帧时间的分子；成功则 `gameGpuOk_ = true` |
| `:505` | **`AddStarCounter`** | `\GPU Engine(*)\Utilization Percentage` | 全局最忙引擎；compute/OFA 分引擎占用 |
| `:508` | **`AddStarCounter`** | `\Energy Meter(*)\Power` | CPU 包功耗（EMI/RAPL） |
| `:509` | `AddWildcard` | `\GPU Adapter Memory(*)\Dedicated Usage` | 显存占用兜底 |
| `:510` | `AddWildcard` | `\GPU Adapter Memory(*)\Shared Usage` | 同上（**与 Dedicated 混在同一个求和里**，见 P6） |
| `:511` | `AddWildcard` | `\Processor Information(*)\% Processor Performance` | CPU 频率回退 |
| `:512` | `AddWildcard` | `\Thermal Zone Information(*)\Temperature` | **注册了但没有任何读取点**（死计数器） |
| `:513` | `Collect()` | — | 给速率型计数器做第一次基线 |

**两套实例机制（必须分清，这是本层最容易踩的地方）**

| | `AddWildcard`（静态展开） | `AddStarCounter`（动态实例） |
| --- | --- | --- |
| 实现 | `PdhExpandCounterPathW` 展开后逐个 `PdhAddEnglishCounterW` | 直接 `PdhAddEnglishCounterW("...(*)")`，取值用 `PdhGetFormattedCounterArrayW` |
| 实例列表 | **在 AddCounter 那一刻冻结** | **每次取值时由 PDH 给出** |
| 新进程出现 | 看不到（要 `RefreshWildcard` 补） | 自动有 |
| 取值 API | `PdhGetFormattedCounterValue` | `PdhGetFormattedCounterArrayW` |
| 求和/取最大 | `SumWhere` / `SumWhere2` / `MaxWhere` | `SumStarCounter` / `MaxStarCounter` |

`RefreshWildcard`（`:151`）是补静态展开的方案，但**当前没有任何调用点**——按 (进程,引擎) 的读取需求
已经全部改用星号计数器，它的存在只是历史包袱（头文件 `np_sensors.h:36-41` 还留着当年的说明）。

**已知限制**

* 计数器名一律走 `PdhAddEnglishCounterW`（`:145`、`:180`、`:270`），**避免中文系统上对象名被本地化**——
  不要改成 `PdhAddCounterW`。
* `PdhCollectQueryData` 至少要两次采样才有速率型数据；`Init` 末尾那次 `Collect()` 就是基线（`:513`）。
* `SumStarCounter` / `MaxStarCounter` 会**过滤 `CStatus`**：只接受 `ERROR_SUCCESS`、
  `PDH_CSTATUS_VALID_DATA`、`PDH_CSTATUS_NEW_DATA`（`:301-303`、`:337-339`），其余视为脏样本。
* GPU 引擎实例**数量巨大**：源码注释记的是「实测枚举出 915 个实例」（`:369`），
  本次核对 `\GPU Engine(*)\Utilization Percentage` 拿到 **875 个实例**（实测），同一量级。

### 2.6 CallNtPowerInformation（`np_sensors.cpp:621-677`）

见 3.1。要点：**动态加载 `powrprof.dll`**，不链接 `powrprof.lib`。

### 2.7 WMI（`np_sensors.cpp:16-102` + 三处消费点）

**提供**：ACPI 热区温度（CPU 温度兜底）、LibreHardwareMonitor 的 CPU Package / Tctl 温度、
以及名字里含 `Tensor` / `RT Core` 的读数。

**实现与优选**

* GUID 自己声明（`kClsidWbemLocator` / `kIidIWbemLocator`，`np_sensors.cpp:16-19`）——
  **mingw 没有 `wbemuuid.lib`**，所以不能链接，必须硬编码这两个公开固定的 GUID。
* `WmiRows`（`:27-92`）是一个极简 WQL 查询器：`CoCreateInstance` → `ConnectServer` →
  `CoSetProxyBlanket`（`RPC_C_AUTHN_LEVEL_CALL` / `RPC_C_IMP_LEVEL_IMPERSONATE`）→ `ExecQuery`
  （`WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY`）→ 逐行把数值属性转 `double`。
  支持 `VT_BSTR`（用 `_wtof`）、`VT_R8/R4/I4/UI4/I8/UI8`（`:68-74`）。
* CPU 温度优先级（`PollCpu`，`:716-748`）：
  1. HWiNFO `Find(nullptr, L"CPU Package")` → `Find(L"CPU", L"Package")` → `FindAny(L"CPU (Tctl")`
     → `FindAny(L"CPU Package")`（`:719-722`）；
  2. 仍拿不到时查 `root\LibreHardwareMonitor` 的 `Sensor` 表，命中名字含 `cpu package` /
     `core (tctl` 的行（`:727-740`），成功则 `available_ |= NP_SRC_LHM`；
  3. 仍拿不到时查 `root\WMI` 的 `MSAcpi_ThermalZoneTemperature.CurrentTemperature`，
     要求 `> 1000`，按 `tk / 10.0 - 273.15` 换算（`:742-748`）。
* RT/Tensor（`:1007-1024`）：同一张 LHM `Sensor` 表，名字含 `tensor` → `hwTensorPct`，
  含 `rt core` → `hwRtPct`，来源位记 `NP_SRC_LHM`。

**已知限制**

* 所有 WMI 路径都以 `comInited_` 为前提（`CoInitializeEx` 在 `Init()` 里做，`:489-490`）。
* `MSAcpi_ThermalZoneTemperature` 在大量主板上**根本不提供**（README:471-472 已写明），
  拿不到就留 `-273`，面板显示 `—`。
* LHM 的 WMI 命名空间需要用户**显式开启**（README:407）。
* `WmiRows` 只取第一行结果（`WmiScalar` 用 `rows[0]`，`:98`）——ACPI 有多个热区时只读第一个。
* `WmiRows` 每行只记录**已成功取到**的属性；属性缺失不会报错，只会少一项。

### 2.8 系统通用 API

| 指标 | API | 行号 | 备注 |
| --- | --- | --- | --- |
| CPU 占用率 | `GetSystemTimes` 差分 | `np_sensors.cpp:682-699` | `total = dk + du`，`busy = (total - di)/total*100`，clamp 0~100；首轮无基线不输出 |
| 内存 | `GlobalMemoryStatusEx` | `:768-774` | `(TotalPhys - AvailPhys)` 算已用 |
| 显卡型号/厂商/显存总量 | `CreateDXGIFactory1` + `EnumAdapters1(0)` + `GetDesc1` | `:517-535` | 跨厂商通用；`VendorId` 0x10DE→NVIDIA、0x1002→AMD、0x8086→Intel（`:780-782`） |
| 标称频率 | 注册表 `HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0\~MHz` | `:540-548` | 本机实测 = **3072**（见 3.1 的口径问题） |
| 逻辑线程数 | `GetSystemInfo` | `:549-551` | |
| 物理核数 | `GetLogicalProcessorInformation` 统计 `RelationProcessorCore` | `:553-565` | 失败则退化为线程数 |
| 显示模式 | `EnumDisplaySettingsW` | `src/app/main.cpp:100-104` | 由 app 层写入 `NPSensors` |

---

## 3. 六个必须讲透的点

### 3.1 CPU 频率：`CallNtPowerInformation(ProcessorInformation=11)`

**为什么不用 PDH 的「标称 × 性能百分比」**（源码注释 `np_sensors.cpp:621-630`）：
后者依赖一个猜测的标称频率，误差直接乘进结果。注释里记的实测是
「PDH `Processor Frequency` 报 2300MHz、`% Processor Performance` 报 141%（=> 3259MHz）」，
而 `CallNtPowerInformation` 直接给每核当前 MHz，没有这层换算误差。

**实现（`np_sensors.cpp:631-677`）**

```cpp
struct NpProcessorPowerInfo {          // :632-639  按 MSDN 补齐（MinGW 头文件里没有）
    unsigned long Number, MaxMhz, CurrentMhz, MhzLimit, MaxIdleState, CurrentIdleState;
};
constexpr int kNpProcessorInformation = 11;   // :641  POWER_INFORMATION_LEVEL 枚举值
bool CpuFreqFromPowerInfo(double* mhzOut);    // :643
```

* **动态加载**：`LoadLibraryW(L"powrprof.dll")` + `GetProcAddress(..., "CallNtPowerInformation")`，
  且用函数内 `static` 只尝试一次（`:645-651`）。
* 缓冲区按 `GetSystemInfo().dwNumberOfProcessors * sizeof(...)` 自己算（`:654-659`），
  **不能依赖返回值给长度**：失败时返回的是 NTSTATUS（如 `0xC0000023` = `STATUS_BUFFER_TOO_SMALL`），
  不是所需字节数（`:660-662`）。
* 取**所有核里最快的那个**（任务管理器/各家工具展示的也是最高核心频率），`:664-675`。

**为什么必须动态 `GetProcAddress` 加载 `powrprof.dll`**（三条理由，缺一不可）

1. `build.bat` 的链接行是
   `-luser32 -lgdi32 -lshell32 -ladvapi32 -lole32 -loleaut32 -luuid -lcomctl32 -lcomdlg32 -lpdh -lpsapi -ldxgi -ld3d11 -ld2d1 -ldwrite -lshlwapi -lwinmm -lmsimg32 -ltdh`
   —— **没有 `-lpowrprof`**（`build.bat:71`，本次核对）。源码注释写明了「避免给 build.bat 增加
   `-lpowrprof` 依赖」（`:628`）。
2. **MinGW 头文件缺 `PROCESSOR_POWER_INFORMATION` 声明**，所以结构体在源码里按 MSDN 手写补齐
   （`:629`、`:632-639`）；同一个结构体在 `tests/emi_probe.cpp:25-37` 里又抄了一份。
3. 动态加载让"没有 powrprof.dll 的环境"（理论上不存在，但保持零外部依赖的项目风格）能优雅降级：
   `fn == nullptr` 就直接走 PDH 回退，不崩。

**回退路径（`:709-714`）**

```cpp
} else if (pdh_.SumWhere(L"Processor Information", L"_Total", &perfFreq) &&
           cpuBaseMHz_ > 0 && perfFreq > 0) {
    out.cpuClock = (float)(cpuBaseMHz_ * perfFreq / 100.0);
    cpuFreqFromNt_ = false;
}
```

**本机实测（本次核对）**

* `CallNtPowerInformation(11)` 返回 **0（成功）**，24 个逻辑处理器 × 24 字节 = 576 字节缓冲区；
  core 0 = 2700 MHz、core 2 = 2100 MHz（当时空闲）、最高核 2700 MHz、平均 2300 MHz。
* 故意给 48 字节缓冲区 → 返回 **0xC0000023**，确认"失败时不是长度"。
* 注册表 `~MHz` = **3072**；PDH `\Processor Information(_Total)\Processor Frequency` = **2300**；
  `% Processor Performance`(`_Total`) = **154% ~ 165%**（两次采样）；`_Total` 实例存在（typeperf 原始路径确认）。

> **口径不一致（本次核对发现）**：回退公式用的是 `cpuBaseMHz_`（注册表 **3072**），
> 而 `% Processor Performance` 是相对 PDH 自己的标称频率（**2300**）算的百分比，
> 两者不是同一个基准。于是 3072 × 1.65 ≈ **5070 MHz**，比同时刻 `CallNtPowerInformation`
> 报的最高核 2700 MHz 高出一大截（约 1.34 倍的系统性偏高来自 3072/2300）。
> 而 `np_sensors.h:153-155` 的注释写的是"回退到 PDH 路径时读数**可能系统性偏低**"——
> **与本机实测方向相反**。另外 `np_sensors.cpp:625-626` 注释里的算式（2300 × 141% => 3259MHz）
> 用的是 PDH 标称值，与代码实际用的注册表值也不是同一个数。
> 结论：**主路径可用，回退路径口径存疑**；要么统一基准，要么干脆去掉回退（留 `-1`）。

### 3.2 CPU 功耗：PDH `Energy Meter`（EMI / Intel RAPL）

**为什么优先 EMI**（`np_sensors.cpp:749-755`）：它是 Windows 自带的能量计量接口，走 Intel RAPL，
**免驱动、免管理员、不依赖用户装 HWiNFO**。

**三个必须记住的结论**

1. **PDH 对象名是 `Energy Meter`，不是 `Energy Meter Interface`**（`:506-507`、`:753`）。
   `\Energy Meter Interface(*)\...` 是错的，展开会失败。
2. **必须按实例过滤出 `PKG`（整包）**；`_Total` 恒为 0，`PP0` 只是核心，`DRAM` 是内存域（`:754`）。
   代码用 `MaxStarCounter(L"\\Energy Meter(*)\\Power", L"PKG", L"", &mw)`（`:757`）——
   关键字是**大小写敏感**的 `wcsstr`，所以写 `PKG` 而不是 `pkg` 是**必须**的。
3. **单位约毫瓦**，所以 `out.cpuPower = mw / 1000.0`（`:755`、`:758`）。

**本机实测（本次核对，`\\MSI`）**

```
typeperf "\Energy Meter(*)\Power" -sc 1        # 原始实例名（大小写敏感！）
  \Energy Meter(RAPL_Package0_DRAM)\Power =      0.000
  \Energy Meter(RAPL_Package0_PKG)\Power  =  39387.13     ← 约 39.4 W，毫瓦
  \Energy Meter(RAPL_Package0_PP0)\Power  =  31518.82     ← 仅核心
  \Energy Meter(RAPL_Package0_PP1)\Power  =      0.24
  \Energy Meter(_Total)\Power             =      0.000    ← ★ 恒为 0，印证注释
```

* 实例名是**大写** `RAPL_Package0_PKG` → 代码的 `L"PKG"` 过滤器能命中 **✅**（这一路是好的）。
* ⚠ 任务书里提到的 `RAPL_Package0_PKG` 全名与本次实测一致；但**源码里只写了 `PKG` 子串**，
  全名在仓库源码中并无记载，本文按实测补上。
* ⚠ **不要用 `Get-Counter` 判断实例名大小写**：它会把实例名显示成全小写
  （`rapl_package0_pkg`），照它改代码会把 `PKG` 写成 `pkg`，然后**永远读不到**。用 `typeperf` 或原生 PDH API。

**回退**：`hwinfo_.Find(L"CPU", L"Package Power")` → `FindAny(L"CPU Package Power")`（`:760-763`）。
非 Intel 平台（无 RAPL/EMI）时只能走 HWiNFO；两者都没有则 `cpuPower` 保持 `-1`（面板显示 `—`）。

### 3.3 GPU 帧时间：PDH `\GPU Engine(*)\Running Time`

**这是什么**：Windows 通过 PDH 按 **(进程, 引擎)** 暴露 GPU 累计执行时间，实例名形如

```
pid_1234_luid_0x00000000_0x0000ABCD_phys_0_eng_0_engtype_3D
```

**非管理员可读**（源码注释 `np_sensors.cpp:363-373`；本次核对 875 个实例、以普通权限读取成功）。
它是**驱动自己报的累计量**，所以与锁帧、NVIDIA Reflex、多线程提交**都无关**——
这正是靠 Present 钩子推算做不到的。

**算法（`PollGameGpu`，`:374-485`）**

```
每帧 GPU 忙时间 = (两次采样之间该进程 GPU 执行时间增量) / (这段时间的帧数增量)
```

逐步：

1. **先把上一次的值填回去**（`:377-379`）：`out.gpuBusyMs = lastBusyMs_` 等。
   因为 `Poll()` 每轮把结构体清成 `-1`，而新样本最快 100ms 才有 —— 不保留就会出现"时有时无"。
2. 前置守卫：`gameGpuOk_` 假（Init 时没加上计数器）、`pid == 0` 时给出诊断文字并返回（`:381-382`）。
3. `pdh_.Collect()`（`:388`）；构造 pid 关键字 `pid_%lu_`（`:390-391`）。
4. **先算 compute / OFA 分引擎占用**（`:398-417`），再算 Running Time。
   ⚠ 这一段**必须放在所有提前 return 之前**，否则"目标进程刚建基线的那一轮"和
   "Running Time 暂时读不到的那一轮"永远算不到 AI 引擎（`:394-397` 的教训注释）。
5. `SumStarCounter(L"\\GPU Engine(*)\\Running Time", kw, L"engtype_", &sec, &hits)`（`:426`），
   第二个关键字 `engtype_` 用来排除非引擎实例。
6. **单位是 100 纳秒**，`sec /= 1.0e7` 换算成秒（`:420-421`、`:434`）。
   忘了除会得到天文数字。
7. 基线管理：
   * 目标进程变了 → 重建基线并 `return`（`:442-448`）；
   * **帧计数（累计值）变小** → 说明钩子重新注入/pid 被复用，直接相减会**无符号下溢**成天文数字，
     算出的每帧 GPU 时间恒为 0；此处重建基线并 `lastBusyMs_ = -1`（`:450-459`）；
   * 间隔 `dms >= 100` 才产生新样本（`:464`）；
   * **间隔不足时保持基线不动**，等它累积（`:464-476`）——
     如果无条件推进基线，"界面轮询比阈值频繁"会导致**永远算不出值**。
8. 把留存值给出去（`:483`），而不是重置成 `-1`。

**诊断**：`gameGpuDiag_` 在每条失败路径与正常路径都会写一句人话（`:427-439`），
由 `main.cpp:202` 打到日志里（`PDH 未出数原因 : 命中 16 个引擎实例，累计 333.495 秒`——本机日志实测）。

**已知限制**

* 目标进程必须**已经用过 GPU**：刚建立基线、或游戏还在菜单里时不一定有 `Running Time` 实例。
* 该进程的引擎实例数量随引擎类型变化（本机日志实测：某进程命中 **16** 个引擎实例）。
* **帧数来自两个来源取大者**（`main.cpp:142-154`）：钩子的 `frameTotal` 与 ETW 的 `presentN`。
  原因：非管理员时注入失败 → 帧数恒 0 → GPU 帧时间永远算不出（`:144-148` 有实测日志记录）。
* 端到端回归：`tests/verify_metrics.py` 断言"先开 NextPerf、后开游戏"这个顺序下
  日志出现「命中 N 个引擎实例」且 GPU 帧时间 > 0（**只验证有值，不验证准不准**）。

### 3.4 AI 引擎：`engtype_compute` / `engtype_ofa`

**设计意图**（`:398-417`、`np_build.cpp:356-365`）：

* compute 引擎占用 = "游戏进程在 compute 引擎上的占比"，作为 AI 推理 / 超分的**代理指标**；
* OFA（光流加速器）= N 卡上 DLSS **帧生成**专用硬件单元，只要它在动就说明帧生成在工作，
  由系统计数器直接给出，不需要估算。

**实现**：两条 `SumStarCounter(L"\\GPU Engine(*)\\Utilization Percentage", pidKw, <engtype>, &v, nullptr)`
（`:400-401` 与 `:410-411`），结果分别落到 `out.engCompute` / `out.engOfa`，并留存到
`lastCompute_` / `lastOfa_`。

**🔴 当前这一路是坏的（本次核对发现，源码注释里没有）**

代码用的过滤器是**全小写**的 `L"engtype_compute"` 和 `L"engtype_ofa"`（`:401`、`:411`），
而 `wcsstr` **大小写敏感**，真实实例名是**大写开头**的：

| 用代码同款 API（`PdhGetFormattedCounterArrayW`）实测 875 个实例 | 命中数 |
| --- | --- |
| 含 `engtype_Compute`（真实名，如 `..._eng_5_engtype_Compute`） | **5** |
| 含 `engtype_compute`（代码用的关键字） | **0** |
| 含 `engtype_OFA`（真实名，如 `..._eng_8_engtype_OFA_0`） | **38** |
| 含 `engtype_ofa`（代码用的关键字） | **0** |
| 含 `engtype_`（`Running Time` 用的关键字，全小写但真实名里也是小写） | 875 ✅ |

程序自身日志（`%TEMP%\NextPerf.log`，本机实测，连续多个采样）也印证：

```
AI 引擎 : compute=-1.0% ofa=-1.0%  <- PDH \GPU Engine(*)\Utilization Percentage(engtype_compute / engtype_ofa)
（PDH 未出数原因 : 命中 16 个引擎实例，累计 333.495 秒）      ← 同一时刻 Running Time 是好的
```

**修法**（本文只写结论，不改代码）：把两个关键字改成 `L"engtype_Compute"` / `L"engtype_OFA"`，
或改用大小写不敏感的匹配；改完必须用真实游戏（或打开 DLSS 帧生成的进程）复验 `engOfa > 0`。

**为什么 `Running Time` 那条路不受影响**：它的第二个关键字是 `engtype_`，
真实名里这段前缀本身就是小写，所以能匹配全部引擎实例。

### 3.5 NVAPI 深挖

**动态加载（`np_vendor.cpp:71-105`）**

1. `LoadLibraryW(L"nvapi64.dll")`，失败再试 `nvapi.dll`（32 位）。
2. 取导出 `nvapi_QueryInterface`；拿不到就卸载返回 false。
3. 用 **interface ID** 换函数指针（ID 表，`np_vendor.cpp:60-69`）：

| 常量 | ID | 函数 |
| --- | --- | --- |
| `kIdInitialize` | `0x0150E828` | `NvAPI_Initialize` |
| `kIdUnload` | `0xD22BDD7E` | `NvAPI_Unload` |
| `kIdEnumPhysicalGPUs` | `0xE5AC921F` | `NvAPI_EnumPhysicalGPUs` |
| `kIdGpuGetFullName` | `0xCEEE8E9F` | `NvAPI_GPU_GetFullName` |
| `kIdGpuThermalSettings` | `0xE3640A56` | `NvAPI_GPU_GetThermalSettings` |
| `kIdGpuDynamicPstates` | `0x60DED2ED` | `NvAPI_GPU_GetDynamicPstatesInfoEx` |
| `kIdGpuGetUsages` | `0x189A1FDF` | `NvAPI_GPU_GetUsages`（**解析了但没调用**） |
| `kIdGpuGetTachReading` | `0x5F608315` | `NvAPI_GPU_GetTachReading` |
| `kIdGpuGetAllClocks` | **`0xDCB616C3`** | `NvAPI_GPU_GetAllClockFrequencies` |

   注释说明后两个 ID「来自 PresentMon 自带的接口表」（`np_vendor.cpp:68`）。
4. 硬要求 `Initialize` + `EnumPhysicalGPUs`；`Initialize()` 非 0 整体失败（`:98-100`）。
   `EnumPhysicalGPUs` 失败只把 `gpuCount = 0`，`loaded` 仍为 true（`:102-104`，见 2.2 的坑）。
5. **句柄来源**：物理 GPU 句柄数组 `gpus[NVAPI_MAX_PHYSICAL_GPUS]`（`np_vendor.h:170`），
   消费端用 NVML 选出的 `nvmlIndex_` 去索引 NVAPI 数组，并 `min()` 到 `gpuCount-1`
   （`np_sensors.cpp:857`）——两套枚举的顺序**假设一致**，这是一个未验证的假设。

**`NV_GPU_CLOCK_FREQUENCIES` 结构体（`np_vendor.h:125-142`）**

```cpp
#define NVAPI_MAX_GPU_PUBLIC_CLOCKS 32
typedef struct {
    NvU32 version;          // = VER_2（官方 VER_3 与 VER_2 共用同一结构体，这里取更保守的 2）
    NvU32 clockTypeFlags;   // ClockType:4 | reserved:20 | reserved1:8  —— 位域在内存里就是普通 NvU32
    struct { NvU32 presentFlags; NvU32 frequency; } domain[32];   // bIsPresent:1 | reserved:31 / kHz
} NV_GPU_CLOCK_FREQUENCIES;
```

注释写明这是**逐字段对照 PresentMon 自带 `nvapi.h:5962`** 的权威定义（`np_vendor.h:127-130`）。

**domain 索引（`np_vendor.h:144-148`）—— ⚠ MEMORY 是 4 不是 1，极易搞错**

| 常量 | 值 | 含义 |
| --- | --- | --- |
| `NP_NVAPI_CLK_GRAPHICS` | **0** | 图形/核心频率 |
| `NP_NVAPI_CLK_MEMORY` | **4** | 显存频率 |
| `NP_NVAPI_CLK_PROCESSOR` | **7** | 处理器（定义存在，未使用） |
| `NP_NVAPI_CLK_VIDEO` | **8** | 视频（定义存在，未使用） |

**读取实现 `NvapiReadGpuClocks`（`np_vendor.cpp:321-342`）**

* `clk.clockTypeFlags = 0` = `CURRENT_FREQ`（当前频率，`:326`）。
* 只有 `presentFlags & 1` 才采信，`frequency / 1000.0` 由 **kHz → MHz**（`:333-339`）。
* 返回 true 表示"至少读到一个域"。

**它是 NVML 的频率回退路径（`np_sensors.cpp:902-915`）**

```cpp
if (out.gpuClock <= 0.0f || out.memClock <= 0.0f) {      // 默认 -1，<=0 表示"还没读到"
    if (NvapiReadGpuClocks(nvapi_, g, &cMhz, &mMhz)) {
        if (out.gpuClock <= 0.0f && cMhz > 0) out.gpuClock = (float)cMhz;
        if (out.memClock <= 0.0f && mMhz > 0) out.memClock = (float)mMhz;
        out.sources |= NP_SRC_NVAPI;
    }
}
```

为什么需要它：GPU 频率的主来源是 NVML，而 README 如实写了"换机器/换驱动，传感器很可能读不到数据"
（`README.md:215`、`np_sensors.cpp:903-906`）。NVAPI 走**同一个驱动但不依赖 NVML 那套库**，
补上这个弱点。注意 **NVML 成功时不覆盖**（只在 `<=0` 时补），所以不会互相打架。

**NVAPI 利用率域（`np_sensors.cpp:870-895`）**

* 官方 4 域：`NP_NVAPI_DOM_GPU=0`（图形）、`FB=1`（显存控制器）、`VID=2`（视频）、`BUS=3`（PCIe）
  （`np_vendor.h:177-181`）。
* `dom(i)` 辅助：`bIsPresent` 为 0 → `-1`；`percentage` 必须在 0~100 之间才采信，否则 `-1`（`:874-879`）。
* 域 4..7（`NVAPI_MAX_GPU_UTILIZATIONS = 8`，官方只用前 4 个）**每轮都探测**，
  有值就置 `domExtPresent` 位与 `nvapiExtSeen_`，并在 `sourceText` 里加 `| 扩展域:有`（`:884-892`、`:1059`）。
  这是 RT/Tensor 的"第三层"希望（`docs/RT_TENSOR.md:95-104`）。
* 若 NVML 没给占用率而 `domGpu >= 0`，用它顶上（`:894`）。

### 3.6 ETW：`Microsoft-Windows-DxgKrnl` 的 Present 事件

**为什么要它**（`np_etw.h:1-11`）：靠 Present 钩子推算帧时间踩了太多坑（每帧两次 Present 导致翻倍、
多线程提交区间重叠、Reflex 改变 Present 行为、后台流式线程搅乱"第一次提交"）。
PresentMon / RTSS 这类成熟软件的做法是订阅 `Microsoft-Windows-DxgKrnl` 的 ETW 事件 ——
数据是内核报的，与锁帧、Reflex、多线程提交全都无关。

**订阅是怎么建起来的（`np_etw.cpp:200-263`）**

| 步骤 | 代码 | 说明 |
| --- | --- | --- |
| provider GUID | `:13-15` | `{802EC45A-1E99-4B83-9920-87C98277BA9D}` |
| 关键字 | `:16-20` | `kKwPresent = 0x0000000008000000 \| 0x1`：**Present(0x8000000) + Base(0x1)** |
| 会话名 | `:22` | `NextPerfFrameTrace` |
| 属性 | `:204-217` | `WNODE_FLAG_TRACED_GUID`、**`ClientContext = 1`（QPC 时间戳）**、`EVENT_TRACE_REAL_TIME_MODE`、`BufferSize 64` / `Min 4` / `Max 24`、`FlushTimer 1` 秒 |
| 建会话 | `:221-225` | `StartTraceW`；若 `ERROR_ALREADY_EXISTS`（上次异常退出残留）先 `ControlTraceW(STOP)` 再建 |
| 打开 provider | `:239-240` | `EnableTraceEx2(sh, &kDxgKrnl, ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION, kKwPresent, 0, 0, nullptr)` |
| 消费者线程 | `:252-255` | `CreateThread` → `ConsumeLoop()` |
| 收事件 | `:265-283` | `OpenTraceW`（`PROCESS_TRACE_MODE_REAL_TIME \| PROCESS_TRACE_MODE_EVENT_RECORD`）+ `ProcessTrace`（阻塞直到 `CloseTrace`） |
| 停止 | `:285-305` | `stop_ = true` → `CloseTrace` → 等线程最多 2s → `ControlTraceW(STOP)` |

**为什么关键字要 `Present | Base`（关键实证，`:16-20`）**：

> 「只启用 Present 时实测抓到的是 `PresentQueuePacket` / `PresentHistory` 这类**记账事件**（每帧两次），
> 真正的 `Present_Start` 反而没进来 —— 它在 `Base` 下面。」

于是关键字把 `Base(0x1)` 一起打开。

**事件判定：不硬编码 ID，用 TDH 动态取名字（`:53-95`）**

* `TdhGetEventInformation` 两次调用（先取 `need`，再取内容），`:67-72`。
* **`TRACE_EVENT_INFO` 里没有 `EventNameOffset`**（作者一开始就写错过），事件名要靠
  `TaskNameOffset` + `OpcodeNameOffset` 组合，优先用 Task 名（`:74-79`）。
* ⚠ 名字必须拷进**调用方提供的缓冲**：TDH 给的名字指向 `ClassifyEvent` 的**局部 `std::vector`**，
  函数返回就析构了。原来的写法是直接把指针传回去再 `wcscpy`/`wcscmp` → **use-after-free**（`:58-62`）。
* 判定规则（`:86-94`）：名字是 `Present` 本体，或以 `Present_` 开头，**且 `opcode == 1`（Start）**。
  **绝不能**用 `wcsstr` 做子串匹配 —— 那样 `PresentHistory` / `PresentQueuePacket` /
  `PresentMultiPlaneOverlay` 这些记账事件都会被算成一次 present（每帧两次 → **帧率翻倍成 120fps**）。
* 每条事件先用 `{Id, Version, Opcode}` 三元组查缓存（最多 8 项），避免每条事件都走一次 TDH（`:83-87`、`:109-123`）。

**谁算一帧（`:97-124`）**

* ★ **没有目标进程时一条都不算**：`if (!m.targetPid_ || ev->EventHeader.ProcessId != m.targetPid_) return;`
  （`:100-103`）。原来 `targetPid_ == 0` 表示"全都算"，于是主程序还没注入任何游戏时，
  **全系统**（桌面、浏览器、播放器）的 present 都被算进来，Low 帧被污染成别的程序的数字。
* 时间戳直接取 `EVENT_HEADER.TimeStamp`（100ns、QPC 对齐，与 `QueryPerformanceCounter` 同源），
  差值 `/10000.0` 得 ms（`:140-142`、`:151`）。
* 单帧过滤：`0.02 < ms < 1000`（超过 1 秒不是一帧，是切出去/加载），`:152-153`。

**统计口径（`:147-197`）**

* `frameMs`：最近一帧的 present 间隔（环形缓冲 2048 条，`np_etw.h:106`）。
* `fps`：**最近 1 秒窗口的计数帧率**（`winN_ * 1e7 / Δt`），锁 60 时就是稳定 60.0，不做逐帧换算（`:162-169`）。
* `low1Fps` / `low01Fps`：窗口内**最慢 1% / 0.1%** 帧的平均帧时间换算回 FPS（`:187-196`）；
  样本 < 32 不算（`:178`）。
* **限流**：`Snapshot()` 每 120ms 会被主线程调 3 次，而 `RecalcLow` 要在**持锁**状态下全排序 2048 个样本，
  持锁的另一边是 ETW 消费者线程（一卡就丢事件）。所以 `RecalcLow` 限流到 **5Hz（200ms）**（`:179-184`）。

**线程安全**

* 一把 `CRITICAL_SECTION` 保护全部状态；`OnPresent` / `SetTargetPid` / `Snapshot` 都进锁（`:148`、`:309`、`:324`）。
* `status_` 跨线程（消费者线程会写 `OpenTrace` 失败原因）：**`status()` 返回拷贝**而不是引用，
  否则主线程读、消费者线程写同一个 `std::string` 是 UB（SSO 缓冲/堆指针撕裂会崩，`np_etw.h:66-68`）。
* ★ **锁在构造函数里初始化**，不在 `Start()` 里懒初始化：`Snapshot()` / `SetTargetPid()` / `Stop()`
  都可能在 `Start()` 之前被调用（非管理员时 `Start` 失败；`--uismoke` 路径压根不调 `Start`，
  但主循环照样会 `SetTargetPid`）—— 在**全零的 `CRITICAL_SECTION`** 上 `EnterCriticalSection` 是 UB，
  会去等一个 NULL 信号量（`np_etw.h:29-33`、`np_etw.cpp:29-32`）。

**权限与"对反作弊游戏 ACCESS_DENIED"（**不要尝试绕过**）**

* 创建 ETW 实时会话**需要管理员**（`np_etw.h:9-11`）。非管理员时 `StartTraceW` 返回 `ERROR_ACCESS_DENIED`，
  代码把它翻译成 `"创建 ETW 会话被拒（需要管理员运行 NextPerf）"`（`:226-235`），`Start()` 返回 false，
  调用方只写一行日志继续跑（`main.cpp:982-986`），**绝不阻塞程序**。
* **反作弊会让这条路也断**：EasyAntiCheat / BattlEye / Vanguard 这类反作弊主动阻止外部注入，
  这是它们的**设计目标**；而且部分反作弊启动后会让 `StartTraceW` **直接返回 `ACCESS_DENIED`**
  —— PresentMon 有专门 issue 记录（`README.md:199-204`，引 GameTechDev/PresentMon#573）。
* **本工具不会尝试绕过反作弊**（既有封号风险，也不符合工具定位，`README.md:204`）。
  这条是硬约束：**任何"提权/换 provider/绕开校验去拿事件"的想法都不要做**。
* ⚠ **诊断文案的局限**：代码只把 `ACCESS_DENIED` 归因为"需要管理员"（`:228-230`），
  **没有区分"反作弊拦截"**。以管理员运行却被反作弊挡住时，日志会误导人去查权限。
  排查顺序建议：先确认 `admin=yes`（`main.cpp:972`），再看是否有反作弊在跑。

**它在应用层怎么被用（`src/app/main.cpp`）**

| 用途 | 行号 | 说明 |
| --- | --- | --- |
| 跟随目标进程 | `:117-119` | `np::Etw().SetTargetPid(pid)`；pid 来源优先 `telemetryPid`，否则 `injected.back()` |
| **只覆盖 Low 帧** | `:121-140` | 仅 `fpsLow1` / `fpsLow01` 用 ETW 值；帧率/帧时间/GPU 时间**不覆盖**（"多覆盖一次只会把对的数弄坏"） |
| 提供帧计数 | `:142-154` | `frameCount = max(钩子 frameTotal, ETW presentN)`，喂给 `PollGameGpu` |
| 诊断日志 | `:170-193` | 帧率/帧时间/Low/累计 present/窗口样本数 + **匹配到的事件名**（最多 6 个，用来定位"帧率翻倍"） |
| 启动 / 停止 | `:982-986` / `:722-724` | 启动时 `Start()`；退出时显式 `Stop()`，否则系统里留一个同名会话 |

**当前验证状态（要如实说）**

* 会话能建、事件能收（需要管理员实测）；但**"匹配到的事件名到底是不是 `Present`/`Present_Start`"
  在本项目里仍未在真机确认**（`docs/CODE-REVIEW-2026-10-09.md:140`：「运行时要靠用户的管理员实测确认」）。
  代码已经把事件名打进日志，拿到日志即可判定。
* ETW 的数据**不准**用于覆盖帧率/帧时间/GPU 时间——这是上一版翻车后的硬规则（`main.cpp:129-130`）。
* ⚠ 文档不一致：`docs/ARCHITECTURE.md:205-208` 还写着"**为什么不用 ETW**？…需要权限…
  所以选注入"，那是**过时结论**（当时 ETW 还没落地）。改这一层时请一并修正该段。

---

## 4. 自动优选总表（以代码为准）

| 指标 | 优先级顺序（←先 / 后→） | 关键行号 |
| --- | --- | --- |
| CPU 占用率 | `GetSystemTimes` 差分（唯一来源） | `np_sensors.cpp:682-699` |
| CPU 频率 | `CallNtPowerInformation` → PDH `% Processor Performance × cpuBaseMHz_` | `:706-714` |
| CPU 温度 | HWiNFO（4 个关键字依次） → LibreHardwareMonitor(WMI) → ACPI 热区(WMI) | `:718-748` |
| CPU 功耗 | **PDH `Energy Meter(*)Power` 实例含 `PKG`** → HWiNFO Package Power | `:756-764` |
| 内存 | `GlobalMemoryStatusEx`（唯一来源） | `:768-774` |
| GPU 名称 | NVML `DeviceGetName` → NVAPI `GPU_GetFullName` → DXGI `GetDesc1` | `:812-814`、`:859-863`、`:777` |
| GPU 厂商 | DXGI `VendorId`（0x10DE/0x1002/0x8086） | `:780-782` |
| GPU 占用率 | NVML → NVAPI `domGpu` → ADL → PDH **最忙引擎** | `:817-820`、`:894`、`:941`、`:956-968` |
| GPU 温度 | NVML → NVAPI ThermalSettings（仅当 `<-200`）→ ADL | `:830-831`、`:864-869`、`:948-949` |
| GPU 功耗 | NVML → HWiNFO | `:832-833`、`:989-990` |
| GPU 功耗上限 | NVML（唯一来源） | `:834-835` |
| GPU 核心/显存频率 | NVML `ClockInfo` → **NVAPI `GPU_GetAllClockFrequencies`** → ADL | `:838-841`、`:908-915`、`:942-943` |
| GPU 风扇 | NVML `%` → NVAPI `TachReading`(RPM) → HWiNFO `%` | `:836-837`、`:897-900`、`:991` |
| 显存总量 | NVML → DXGI `DedicatedVideoMemory` | `:823-826`、`:778` |
| 显存已用 | NVML → PDH `GPU Adapter Memory` 求和 → HWiNFO `GPU Memory Used` | `:823-827`、`:969-975`、`:992-993` |
| GPU 热点 / 显存结温 | HWiNFO（唯一来源；**普通 GPU 温度不从 HWiNFO 取**） | `:983-988` |
| NVAPI 利用率域 | NVAPI（唯一来源） | `:870-895` |
| 编码器 / 解码器 | NVML（唯一来源，且**当前无消费者**） | `:846-853` |
| 硬件 RT / Tensor | HWiNFO（名字含 `Tensor`/`RT Core`） → LHM WMI | `:996-1025` |
| GPU 帧时间 | PDH `Running Time`（唯一在传感器层的来源；钩子推算值只在面板层兜底） | `:426-484`、`np_build.cpp:342-354` |
| AI 引擎 compute/OFA | PDH `Utilization Percentage`（**当前因大小写不匹配而失效**，见 3.4） | `:398-417` |

**「最终用了哪个源」怎么表达**

* 机器可读：`NPSensors::sources`（`NP_SENSOR_SRC` 位组合，`np_common.h:138-148`）。
  `Poll` 结尾会把 `available_`（探测到的源）与 `out.sources`（本轮真正用到的源）**或**在一起（`:1060`），
  所以这个字段同时表达"有"与"用了"，**不能**用它判断某个指标具体来自谁。
* 人可读：`NPSensors::sourceText`（`:1049-1061`），形如
  `GPU:NVML | CPU:系统计时器 | 温度:HWiNFO/WMI | 域:NVAPI | 扩展域:有`，
  面板行 `NP_C_SENSOR_SRC` 显示它（`np_build.cpp:466-467`）。
* 最详细的是日志：`SensorHub::Describe()`（`:605-618`）+ 每 5 秒的「数据来源」块（`main.cpp:160-219`）。

---

## 5. 坑与教训（逐条，全部来自源码注释与本次实测）

### P1 通配实例在 `AddCounter` 那一刻就冻结 ★★★

* **现象**：`\GPU Engine(*)` 的实例列表被固定；游戏在 NextPerf **之后**启动时，
  该进程的引擎实例根本不在查询里，求和永远找不到 → **GPU 帧时间整场为空**。
  用户实测的原始描述：「先开游戏再开 NextPerf 就有数据，先开 NextPerf 再进游戏就没有。」
* **根因**：`PdhAddCounter` 时通配符被展开成 N 个独立计数器，实例列表从此不再变化。
* **正确做法**：用**实例名带 `*` 的计数器** + `PdhGetFormattedCounterArrayW`，
  实例列表**每次取值时**由 PDH 给出，新进程自动就有（`AddStarCounter` / `SumStarCounter` / `MaxStarCounter`）。
* **证据**：`np_sensors.h:36-40`、`:58-70`，`np_sensors.cpp:500-504`，`docs/CODE-REVIEW-2026-10-09.md:59`；
  回归用例 `tests/verify_metrics.py` 专门复现"先开 NextPerf、后开游戏"的顺序。

### P2 值必须跨轮询保留，否则界面"时有时无" ★★★

* **现象**：`Poll()` 每轮把 `NPSensors` 清成 `-1`，而新样本最快 100ms 才有；
  界面每秒轮询十几次 → 绝大多数轮询拿到 `-1`，偶尔闪一下有值。
* **正确做法**：用成员 `lastBusyMs_` / `lastCompute_` / `lastOfa_` 保存上次算出的值，
  **每次轮询开头先填回去**（`np_sensors.cpp:375-379`、`:483`）。
* **证据**：`np_sensors.h:181-183`，`docs/CODE-REVIEW-2026-10-09.md:61`。

### P3 采样基线不能无条件推进 ★★★

* **现象**：如果函数结尾无条件把基线推到最新，而界面轮询比 100ms 阈值更频繁，
  间隔**永远到不了阈值** → **永远算不出值**。
* **正确做法**：间隔不足时**保持基线不动**，等它累积（`np_sensors.cpp:464-476`）。
* **相关坑**：目标进程变化时曾"先 `Collect` 一次、紧接着又 `Collect` 一次"，
  两次采样间隔几乎为 0，对速率型计数器只会产生一个无意义的样本 —— 已去掉（`:384-387`）。

### P4 帧计数是无符号累计值，回退会下溢 ★★

* **现象**：钩子被重新注入 / 进程重启后复用了同一个 pid → 帧计数变小 →
  相减**无符号下溢**成天文数字 → 「每帧 GPU 时间」恒为 0（界面显示 `0.00 ms`）。
* **正确做法**：`frameDelta < lastPidFrameTotal_` 时重建基线并把 `lastBusyMs_` 置 `-1`
  （`np_sensors.cpp:450-459`）。

### P5 GPU 占用率不能跨引擎求和 ★★★

* **现象**：`\GPU Engine(*)` 下每个引擎（3D/Copy/Video/…）各自报 0~100%，
  几十上百个实例求和必然几百 → 被 `clamp` 到 100 → **没有 NVML/ADL 的机器上"GPU 占用率"恒等于 100%**，
  一个永远不动的假数。
* **正确做法**：取**最忙的那个引擎**（`MaxStarCounter`），与任务管理器口径一致；
  并且必须走动态实例计数器（静态展开的实例表在游戏后才启动时看不到）。
* **证据**：`np_sensors.h:71-76`，`np_sensors.cpp:956-968`。

### P6 `Running Time` 单位是 100 纳秒，不是秒 ★★★

* PDH 对累积计数器**不做换算**，`\GPU Engine(*)\Running Time` 直接给 FILETIME 那种 100ns 累计量；
  必须 `/1e7`（`np_sensors.cpp:419-421`、`:434`）。忘了除会得到天文数字。
* 经验值：本次核对看到的量级是几十万到十几亿（如 `180298`、`1514286377`），
  除以 1e7 后是"自开机以来累计秒数"的量级（程序日志实测：`累计 333.495 秒`）。

### P7 `Energy Meter` 的 `_Total` 恒为 0；对象名不是 `Energy Meter Interface` ★★★

* 实例必须按 `PKG` 过滤才能拿到"整包"功耗；`_Total` 恒为 0，`PP0` 只是核心，`DRAM` 是内存域
  （`np_sensors.cpp:753-757`）。
* 单位约**毫瓦**，要 `/1000`。本次实测：`RAPL_Package0_PKG = 39387`（≈39.4 W）、`_Total = 0`、`PP0 = 31518`。
* 关键字匹配是**大小写敏感**的 `wcsstr`；真实实例名是**大写** `RAPL_Package0_PKG`。
  **不要用 `Get-Counter` 的 `InstanceName` 判断大小写**（它显示全小写）。

### P8 同一实例名下混着不同的计数器名，必须按计数器名过滤 ★★

* `\GPU Engine(*)` 下 `Running Time`（累计秒/100ns）与 `Utilization Percentage`（百分比）
  的**实例名一模一样**；混在一起求和就是把秒和百分号加到一起（`np_sensors.h:51-55`）。
* 头文件同时记着：「**不用 `PathHasObject`** —— 本地展开出来的路径不带 `\\计算机名` 前缀，它会解析失败。」
  **本次核对否定了这半句**：`PdhExpandCounterPathW` 在本机返回的是
  `\\MSI\GPU Adapter Memory(luid_...)\Dedicated Usage` —— **带机器名前缀**，`PathHasObject` 能正常工作。
  也就是说 `SumWhere`（`:215`）这一路是可用的，头文件那句注释已经过时，容易误导后来人。

### P9 `PdhQuery::Close()` 必须连 `stars_` 一起清 ★★

* `PdhCloseQuery` 之后 `stars_` 里的 `HCOUNTER` 就是**野句柄**；而 `AddStarCounter` 见到相同路径
  会直接返回 true（以为加过了）→ 下一次 `Init` 全程拿野句柄去 `PdhGetFormattedCounterArrayW`：
  轻则读不到数，重则访问已释放内存（`np_sensors.cpp:117-121`）。

### P10 HWiNFO 共享内存是"别人写的内存"，一个字都不能信 ★★★

* 读数元素下界实际是 **292 字节**（曾经只判 128 → 越界读，`docs/CODE-REVIEW-2026-10-09.md:90`）。
* `offReading` 是对方进程写进来的值：一旦 `offReading >= total`，
  `(total - offReading)` 会**无符号下溢**成天文数字，循环就会读映射之外的地址。
  现在按 `VirtualQuery` 得到的映射大小反推上界（`np_vendor.cpp:245-260`）；
  传感器分组的 `sensorIndex` 同理（`:256-260`）。
* 异常处理器（VEH + `setjmp/longjmp`）**只是最后一道防线**，不能当成正常路径依赖（`:249-252` 的原话）。

### P11 `NVAPI_QueryInterface` 拿到的指针可能为 0；`loaded` 不等于"能用" ★

* 每个 ID 都要判 `QueryInterface(...) == 0 && fn`（`np_vendor.cpp:80-96`）。
* `EnumPhysicalGPUs` 失败时 `loaded` 仍为 `true`、`gpuCount = 0`（`:102-104`）——
  消费端要判 `nvapi_.loaded && nvapi_.gpuCount > 0`（`np_sensors.cpp:856`）。
* README 还记录了「部分新驱动版本会拒绝分发函数指针」（`README.md:404`）→ 自动降级，不影响其它指标。

### P12 没有目标进程时 ETW 一条都不能算 ★★

* `targetPid_ == 0` 曾经表示"全都算"，于是没注入游戏时**全系统**的 present 都被算进来，
  Low 帧被污染成别人的数字（`np_etw.cpp:100-103`，`main.cpp:133-135`）。
* 同理，覆盖 Low 帧前必须判 `pid != 0`（`main.cpp:136`）。

### P13 ETW 事件名是"取到的指针活不过函数返回" ★★

* `TdhGetEventInformation` 返回的名字指向**局部缓冲**；把指针传回调用方再 `wcscpy`/`wcscmp`
  就是 use-after-free（堆一复用就会写坏别人的数据）。名字必须拷进**调用方提供的缓冲**
  （`np_etw.cpp:58-62`、`:81-84`）。

### P14 ETW 事件匹配不能用子串 ★★

* 名字里含 `Present` 就算一帧 → `PresentHistory` / `PresentQueuePacket` 这些**记账事件**混入
  → 每帧两次 → **帧率翻倍成 120fps**。
* 正确做法：`Present` 本体或以 `Present_` 开头（边界匹配）+ `opcode == 1`（`np_etw.cpp:86-94`）。

### P15 跨线程共享的状态：锁的初始化时机与返回拷贝 ★★

* `CRITICAL_SECTION` 在**构造函数**里初始化（不能等 `Start()`）：非管理员 / `--uismoke` 路径下，
  `Snapshot`/`SetTargetPid`/`Stop` 会先于 `Start` 被调用，在全零的 CS 上 `EnterCriticalSection` 是 UB。
* `status()` 返回**拷贝**：消费者线程也会写它，主线程同时读同一个 `std::string` 会因
  SSO 缓冲/堆指针撕裂而崩（`np_etw.h:66-68`、`np_etw.cpp:274-275`）。

### P16 不要在持锁状态下做重活 ★

* `RecalcLow` 要对 2048 个样本全排序，而它是在 `Snapshot` 持锁时调用的；
  持锁的另一边是 ETW 消费者线程，一卡就丢事件 → 限流到 5Hz（`np_etw.cpp:179-184`）。

### P17 WMI 的 GUID 必须自己声明 ★

* mingw 没有 `wbemuuid.lib`，`CoCreateInstance` 用的 CLSID/IID 只能硬编码
  （`np_sensors.cpp:15-19`）。值本身是公开固定的，抄错会直接导致 WMI 全部不可用。

### P18 `NPSensors::version` 是结构体大小，是唯一的布局自检 ★★

* `NPClearSensors` 写的是 `(uint32_t)sizeof(NPSensors)`（`np_common.h:448`）；
  曾经写死 `1`，于是 app 写 / 钩子读这一路**完全没有布局自检**，往中间插字段两边照样跑但读出错位字节。
  现在读方对一下大小就能立刻发现不一致。往 `NPSensors` 里加字段时，
  **`tests/struct_check.py` 与 Python ctypes 镜像也要一起改**。

### P19 混合显卡上"读到的卡"未必是"在出图的那张卡" ★

* NVML 加载成功 → ADL 分支永不执行（`np_sensors.cpp:1038-1042`）；
  多卡时按 `u.gpu` 最大者挑卡，每 32 轮询重挑一次（`:794-807`）。
* 本机就是核显 + 独显（`README.md:239`）；若游戏实际由核显出图，读数会与实际渲染的 GPU 对不上
  （`README.md:217-218` 如实记录为**未处理**）。

### P20 换机器/换驱动就会读不到 ★★

* README 明确写了这条（`README.md:215`、`:251-252`）：「PDH 的 GPU 引擎计数器、
  NVML / NVAPI 的可用字段都随驱动变化。**换驱动后若读不到数据，请优先怀疑这里。**」
* 所以每个指标都必须能独立失败、必须有诊断日志（`gameGpuDiag_`、`sourceText`、「数据来源」块）。

---

## 6. 本次核对发现的新问题（源码注释里没有）

以下三条是写本文时**新发现**的，源码注释与既有文档都没有记载。本文**只记录，不改代码**。

### 6.1 AI 引擎（compute / OFA）过滤器大小写不匹配 → 这一路实际是死的 🔴

* 位置：`np_sensors.cpp:401`（`L"engtype_compute"`）、`np_sensors.cpp:411`（`L"engtype_ofa"`）。
* 证据：用代码同款 API 实测 875 个实例，`engtype_Compute` 命中 **5** 个、`engtype_OFA` 命中 **38** 个，
  而小写写法命中 **0** 个；程序自身日志每次都打 `compute=-1.0% ofa=-1.0%`，
  同一时刻 `Running Time`（关键字 `engtype_`）正常出数并"命中 16 个引擎实例"。
* 影响：面板「AI 引擎」行的 Compute/OFA 两个数**永远不会出现**；
  `docs/RT_TENSOR.md` 里"OFA 只要在动就说明帧生成在工作"的能力**目前拿不到**。
* 建议修法：`L"engtype_Compute"` / `L"engtype_OFA"`（或改成大小写不敏感匹配），
  并用真实 DLSS 帧生成场景复验。**详见 3.4。**

### 6.2 一批"采了没接线 / 写了没读"的死代码与死字段

| 位置 | 状态 |
| --- | --- |
| `\Thermal Zone Information(*)\Temperature`（`np_sensors.cpp:512`） | 注册进 PDH 查询，**全仓没有任何读取点** |
| `PdhQuery::RefreshWildcard`（`:151`） | 实现完整，**无调用方**（已被星号计数器取代） |
| `PdhQuery::SumWhere2`（`:230`）、`SumInstance`（`:247`）、`MaxWhere`（`:348`） | 实现完整，**无调用方** |
| `SensorHub::SetOverride` / `overrideMask`（`np_sensors.h:115-116`） | 「手动指定优先数据源」**没接线**，`override_` 全程无人读 |
| `SensorHub::available()`（`:111`）、`sources()`（`:126`）、`gameGpuAvailable()`（`:107`） | 无调用方（UI 只用了 `sourceText` 与 `Describe()`） |
| `maxGpuTemp()` / `encoderUtil()` / `decoderUtil()`（`:193-195`） | 值算出来了，**无调用方**，面板看不到 |
| `nvapiExtSeen()`（`:196`） | 无调用方（但 `nvapiExtSeen_` 被 `Describe()` 末端用到了 `:1059`） |
| `nvml_.DeviceGetPerformanceState`（`np_vendor.cpp:34`）、`nvapi_.GPU_GetUsages`（`:93`） | 解析了函数指针，**从未调用** |
| `hwinfoTried_`（`np_sensors.h:160`） | 只在 `Init` 里置 true（`:495`），从未被读 |
| `PdhQuery::Counter::instance`（`np_sensors.h:26`） | 只在静态展开那一路用；星号计数器那一路不用它 |

这些不影响正确性，但会**误导后来人**（以为某功能已经接好了）。清理或接线都要在本文件留痕。

### 6.3 文档与代码不一致（改这一层时顺手修）

| 位置 | 问题 |
| --- | --- |
| `docs/ARCHITECTURE.md:205-208` | 还写着「**为什么不用 ETW**」的旧结论；实际 `src/etw/` 已落地并从 `main.cpp:982` 启动 |
| `np_sensors.h:55` | 「本地展开出来的路径不带 `\\计算机名` 前缀」——**与实测相反**（`PdhExpandCounterPathW` 返回带 `\\MSI\` 前缀） |
| `np_sensors.h:153-155` | 「回退到 PDH 路径读数可能**系统性偏低**」——本机实测是**偏高**（注册表 3072 vs PDH 标称 2300） |
| `np_sensors.cpp:625-626` | 注释的算式用 PDH 标称 2300，而代码用的是注册表 `cpuBaseMHz_`(3072)，口径不一致 |
| `README.md:216` 与 `README.md:411` | 数据源优先级两处写法不同（`NVML → NVAPI → ADL → PDH → WMI → HWiNFO` vs `厂商 SDK > 共享内存 > PDH > WMI`），且都不是逐指标的精确顺序 → 以本文第 4 节为准 |
| `README.md:403-409` | 没有列出 ETW 与 `CallNtPowerInformation` 两个源 |
| `docs/CODE-REVIEW-2026-10-09.md:131` | VRAM 求和口径列为「未确认/未修」（见 P6/未确认清单），至今仍未定论 |
| `docs/CATALOG.md:191-193` | §3.4 里这三个文件的行数还是旧值（`~917 / 131`、`~294 / 216`、`~281 / 78`），与本文 0.1 节实测的行数不符；`CATALOG.md` 自己要求「行数要真实」 |

---

## 7. 给未来的自己 / 其他迭代者的叮嘱

1. **改这一层的任何一行，都要同步改 `docs/SENSORS.md` 和 `docs/SENSORS-FUNCTIONS.md`。**
   本文所有行号都会漂移；改完请把受影响的行号、函数表、优先级表一起更新。
2. **`docs/CATALOG.md` 也要一起更新。** 它已经存在（本次核对末尾由并行的文档工作建好，
   §1 的功能文档索引里已经登记本文与 `SENSORS-FUNCTIONS.md`，§2.6 是本层的定位表），
   但 **§3.4 的文件行数还是旧值**，需要更正。本层要登记进 `CATALOG.md` 的条目：
   * 数据源：NVML / NVAPI / ADL / HWiNFO / PDH / CallNtPowerInformation / WMI / LHM / ETW / DXGI
   * PDH 计数器路径：`\GPU Engine(*)\Running Time`、`\GPU Engine(*)\Utilization Percentage`、
     `\Energy Meter(*)\Power`、`\GPU Adapter Memory(*)\Dedicated|Shared Usage`、
     `\Processor Information(*)\% Processor Performance`、`\Thermal Zone Information(*)\Temperature`（当前死）
   * 输出字段：`NPSensors` 全部字段 + 单位 + `-1/-273` 的"不可用"约定
   * 诊断入口：`gameGpuDiag()`、`sourceText`、`Describe()`、「数据来源」日志块、`Etw().status()`
3. **任何数据源都必须"能独立失败"**：一个源崩了不能影响别的源，也不能让程序退出。
   新增源时把失败原因写进 `gameGpuDiag_` 或 `sourceText`，否则排查时会重新经历"权限问题"式的误判。
4. **不要相信"看起来像"的实例名/单位**：`Running Time` 是 100ns、`Energy Meter` 是毫瓦、
   利用率不能跨引擎求和、`_Total` 可能是 0。改动前先用 `tests/emi_probe.cpp`
   这类独立探针程序在真机上确认（**不要**用 `Get-Counter` 判断实例名大小写）。
5. **反作弊**：ETW 被反作弊拒绝（`ACCESS_DENIED`）是**设计目标**，不是 bug。**不要尝试绕过。**
   要做的只是把诊断写清楚（区分"没管理员"与"被反作弊拦截"）并优雅降级。
6. **加字段就改 `version` 自检与镜像**：动 `NPSensors` 必须同步 `tests/struct_check.py`
   与 Python ctypes 镜像，否则读方会拿到错位字节而"看起来一切正常"。
7. **回归要跑 `python tests/run_all.py`**，其中的 `tests/verify_metrics.py` 是唯一覆盖
   "传感器链路端到端出值"的用例（它只验证**有值**，不验证**准不准**——
   准确性需要在真机上与另一套可信来源对照，本项目从未做过）。
8. **优先级/口径写在文档里，也要写在代码注释里**：本层的历史 bug 几乎全是"口径"问题
   （求和 vs 最忙、100ns vs 秒、毫瓦 vs 瓦、冻结 vs 动态实例），不是语法错误。

---

## 8. 未确认清单（别把下面这些当事实用）

| 项 | 说明 |
| --- | --- |
| ETW 实际匹配到的事件名 | 会话需要管理员才能建，本项目尚未在真机确认匹配到的是 `Present_Start`；代码已把事件名打进日志，拿到 `%TEMP%\NextPerf.log` 即可判定（`docs/CODE-REVIEW-2026-10-09.md:140`） |
| ETW Low 帧数值准确性 | 需要与游戏内 OSD / 另一套可信来源对照，未做 |
| 指标**数值是否准确** | 端到端用例只断言"有值"。GPU 帧时间与 NVIDIA 驱动面板的数**不保证相同**（`README.md:119`） |
| 其他硬件/驱动上的行为 | 本项目只在一台机器（`\\MSI`，24 逻辑处理器，核显+RTX 5080 Laptop，Windows 11 26H2）上验证过；`README.md:215` 明确写"换机器/换显卡/换驱动，传感器很可能读不到数据" |
| `NVAPI domain` 4..7 到底是什么 | 官方未公开；`domExtPresent` / `nvapiExtSeen_` 只是"厂商将来填了就采信"的探测结果，语义未确认 |
| NVML 索引与 NVAPI 句柄顺序是否一致 | 代码假设 `nvapi_.gpus[nvmlIndex_]` 对应同一张卡（`np_sensors.cpp:857`），未验证 |
| `\Processor Information(_Total)\% Processor Performance` 的 `_Total` 是否所有机器都有 | 本机有（typeperf 确认）；别的机器/别的核心数下未确认，没有它则 CPU 频率回退路径失效 |
| HWiNFO 单位与字段语义 | 代码按"W / ℃ / MB"直接采信，未做单位校验；不同 HWiNFO 版本/语言可能变化 |
| `GPU Adapter Memory` 的求和口径 | `Dedicated Usage` 与 `Shared Usage` 被加在一起（`np_sensors.cpp:969-975`），可能超过 DXGI 报的专用显存使百分比被 clamp；是否有意为之**未确认**（`docs/CODE-REVIEW-2026-10-09.md:131`） |
| `--uismoke` / `--selftest` 路径下 ETW 行为 | `--uismoke` 不调 `Etw().Start()` 但主循环会 `SetTargetPid`（靠构造函数初始化锁兜住，`np_etw.h:29-33`）；实际运行未见异常，但未做专项验证 |
