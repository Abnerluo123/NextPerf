# 已知缺口与待办总表（TODO）

> **这份文件是「还没做完 / 还没验证 / 条件性可用」的唯一权威清单。**
>
> 其他文档各自的「待办 / 未确认 / 已知问题」小节**只保留技术细节与调查过程**；
> **状态一律以本表为准**。两处不一致时，先怀疑本表过期（或那一节过期），**两边都改**。
>
> 维护规矩（和 `docs/CATALOG.md` 第一条同一个性质）：
> * 做完一条**立刻**把状态改成 ✅，并写清证据（哪次实测 / 哪行日志 / 哪个提交）；
> * **不要把「其实已经修好、只是文档没改」的东西留在这里** —— 本项目已经因此吃过至少 6 次亏。

---

## 0. 状态口径（统一用词，不要混用）

| 标记 | 含义 | 需要的证据 |
| --- | --- | --- |
| ✅ **已验证** | 在真机 / 真游戏上实测通过 | 用户实测报告、日志行、截图 |
| 🔨 **已实现·未验证** | 代码写完、自动化测过，但**没有**在真实场景验证 | 宿主/单元测试 |
| ⚠️ **条件性可用** | 依赖外部条件；条件不满足时按「读不到」处理 | 写清条件 |
| ❌ **不可用** | 接口/硬件根本不提供，**不要再试** | 出处（API/文档限制） |
| 🚧 **未实现** | 明确要做、还没做 | — |
| ❓ **口径未定** | 有值，但含义/口径没定论 | — |
| 🧹 **待清理** | 死代码/死字段/过期文案，不影响功能 | grep 结果 |

**面板上的数值标签**也统一成这一套（实现在 `src/common/np_build.cpp`）：
`(硬件)` · `(实测)` · `AI 已启用 · 估算中` · `无 DXR` · `无 AI` · `需注入` · `启用 N 次` · `—`。

**日志读法口径**（`docs/LOGGING.md` 第 1 节）：钩子日志**不是按时间排的**（4MB 回绕 + 多进程共用一个文件），
一律**按关键字搜**（pid / exe 名 / `SEH` / `DETACH` / `watchdog`），并且**一复现就先复制一份**。

---

## 1. 指标：能不能出数，取决于什么

> 「默认开」= 在 `NP_ALL_COUNTERS` 里（`src/common/np_common.h`）。不在里面的**要去界面手动勾**。

| 指标 | 来源 | 前置条件 | 默认开 | 本机现状（2026-10-11） | 怎么出数 |
| --- | --- | --- | --- | --- | --- |
| 帧时间 / FPS | 钩子（Present 间隔） | 注入成功 + D3D | ✅ | ✅ 正常（60.0fps / 16.7ms） | 注入即可 |
| 1% / 0.1% Low | 钩子（1000ms 窗口平均） | 同上 | ✅ | ✅ 正常 | 注入即可 |
| CPU 帧时间 | 钩子（CPUBusy+CPUWait） | 同上 | ✅ | ✅ 正常 | 注入即可 |
| GPU 帧时间 | **PDH** `\GPU Engine(pid_*)\Running Time` | 非管理员即可；**必须先开游戏后开 NextPerf**（通配实例冻结坑） | ✅ | ✅ 正常（2.8~4.7ms） | 注入即可 |
| GPU 占用 / 温度 / 功耗 / 频率 / 显存 | NVML | NVIDIA 驱动 | ✅ | ✅ 正常 | — |
| CPU 占用 / 内存 | `GetSystemTimes`（**整机**口径）/ `GlobalMemoryStatusEx` | — | ✅ | ✅ 正常；★ 2026-10-11 起占用率做了 **1 秒级指数平滑**（原来瞬时窗口约 120ms，会乱跳） | — |
| **CPU 频率** | `CallNtPowerInformation` | 系统自带 | ✅ | ✅ 正常（3821 MHz） | — |
| CPU 功耗 | PDH `\Energy Meter(*)\Power`（RAPL，按实例含 `PKG`） | Intel 平台；否则要 HWiNFO | ✅ | ✅ 正常（21.9 W） | — |
| **CPU 温度** | HWiNFO 共享内存 → LHM(WMI) → ACPI 热区 | **要装 HWiNFO 并开 Shared Memory Support**，或装 LHM 并开其 WMI 命名空间 | ✅ | ❌ 显示 `--`（三条路都没通） | 开 HWiNFO 共享内存 |
| **GPU 热点 / 显存结温** | **只有 HWiNFO** | 同上 | ❌ 默认关 | ❌ 不显示 | 开 HWiNFO + 手勾「GPU 热点」 |
| **风扇%** | NVML → NVAPI tach → HWiNFO | 型号/驱动可能都不给 | ❌ 默认关 | ⚠️ 未验证 | 手勾「风扇」；不行就开 HWiNFO |
| **显存带宽 / 视频引擎 / PCIe 总线** | NVAPI `GetDynamicPstatesInfoEx` 的 FB/VID/BUS 域 | **需要驱动肯分发函数指针** | ❌ 默认关 | ❌ 恒 `-1`（本机驱动拒绝分发，BUILD.md 记为预期降级） | 目前无解；见 §2 |
| **RT Core 负载** | ① 硬件计数器（HWiNFO/LHM 里含 `RT Core` 的读数）② **引擎 pass 实测**（`DispatchRays` 前后插时间戳） | ① 民用卡不提供 ② **游戏必须用 DXR + D3D12 + `deepEngineHook=1`** | ✅ | 面板显示 `无 DXR`（**re8 没有光追**，属正确显示） | 用**支持光追的游戏**验证实测路径（《漫威蜘蛛侠》PC 版支持） |
| **Tensor / AI 负载** | ① 硬件计数器（HWiNFO/LHM 里含 `Tensor`）② **AI pass 实测**（首次 draw 之后的 compute 区间） | ① 民用卡不提供 ② 游戏里有 AI pass（DLSS/FSR/XeSS）+ D3D12 | ✅ | 面板显示 `无 AI`（re8 未开 DLSS） | 开 **DLSS 帧生成**的游戏验证实测路径 |
| **AI 引擎（光流 / Compute）** | PDH `\GPU Engine(*)\Utilization Percentage` + `engtype_OFA` / `engtype_Compute` | 大小写 bug 已修（`53037bd`） | 行随数据出现 | ✅ **数据是通的**（日志 `ofa=0.0%` 是真读数）；但面板**只在 > 0 时显示**，所以 `0.0%` 时整行消失 | 开 DLSS 帧生成看非零 |
| CPU Busy / Wait | 钩子（同一次减法导出） | 注入 | ❌ 默认关 | ✅ 有值 | 手勾（判断 CPU 瓶颈要用它） |
| Draw / Dispatch 次数、AI 模块列表、数据源、显卡型号 | 钩子 / 传感器 | 注入 | ❌ 默认关 | ✅ 有值 | 手勾 |
| Vulkan / OpenGL 游戏的帧数据 | — | — | — | ❌ 拿不到（**设计边界**，不是故障） | UI 有说明 |

---

## 2. 功能缺口（🚧 未实现）

| # | 缺口 | 影响 | 备注 / 下一步 |
| --- | --- | --- | --- |
| F1 | **没有挂钩 `ResizeBuffers`** | 交换链重建只能靠副作用感知（描述符堆换满 8 槽 / `heapSwaps`）；全屏切换、分辨率变更、`ResizeBuffers` 全收不到通知 | 全仓确认 `src/hook/` 里 `ResizeBuffers` **只出现在注释里（非注释行 0）**。做之前必须用 `tests/vt_check.py` 核出正确 vtable 下标 |
| F3 | **Low 帧 / 帧时间的"帧"定义**（③ 大修，进行中） | **已完成主体**：帧不再按 Present 调用间隔算，改成**累积相位重建**（`N = round(t/vblank)`，帧时长 = `(N−N_prev)×vblank`），只在同步呈现 + 刷新率已知 + 近 5 秒对齐率 ≥75% 时启用；重复呈现（`adv==0`）不当新帧，真掉帧（`adv≥2`）如实报。**实测（工作区内）**：帧时长变成 16.67/33.33 这种精确的消隐整数倍、平均帧时长与真实帧率一致、`dup` 计数证明重复呈现被吸收 | ⏳ **待真机验收**（前提②：真游戏 + 驱动面板交叉验证）：跑一局《漫威蜘蛛侠》，面板帧时间 P50 应 ≈16.67ms、`1% Low` 应回到 ~59（原来是 42~46）。验收通过后做下面两条 |
| F3a | **`1% Low` 与 `0.1% Low` 恒等**（上一条的后续） | 回看只有约 10 个 1 秒窗口，`ceil(10×1%)` 与 `ceil(10×0.1%)` 都是 1 → 两行同一个数。**③ 落地后**帧序列已是"显示帧"，可以换成 PresentMon 口径（滑动 1 秒窗内对**帧**取 P99/P99.9 → 600 帧下 1% 取 6 帧、0.1% 取 1 帧，两行才真正不同） | 待 F3 真机验收后做 |
| F3b | **窗口边界每帧重切导致数值翻动** | `lowWindowed` 从最新一帧往回切窗口 → 每来一帧所有窗口平移，"最差窗口"会在相邻两个之间翻 | 改成**固定窗口网格**（每满 1000ms 固定关一个窗），与 F3a 一起做 |
| F2 | **钩子日志没有真正的轮转** | 4MB 覆盖式回绕 → 文件**不按时间排**，多进程/多会话互相穿插；查问题时现场容易被冲掉 | `LOGGING.md` roadmap 已记。不能简单改名（钩子持有旧句柄，见 SIMULATOR.md 第 1 条坑），要做得多进程配合（例如按 pid 分文件） |
| F3 | **ETW 的 Present 事件名没在真机确认** | 现在靠 TDH 动态取名 + 打日志，理论上对，但没人看过它匹配到的是不是 `Present_Start` | 需要**管理员**跑一次（会话能建），日志会打出匹配到的事件名 |
| F4 | **反作弊拦截与"没权限"无法区分** | `StartTraceW` 都返回 `ACCESS_DENIED`，日志只归因为"需要管理员" | 已知反作弊（EAC/BE/Vanguard）会拦 ETW；**明确不绕过** |
| F5 | **ETW 只覆盖 Low 帧** | 帧率/帧时间/GPU 时间一律用钩子的（有意设计，不是缺口） | 见 `DATA-STRUCTS.md` |
| F6 | **混合显卡没处理** | `nvml_.loaded` 为真时永远走 NVIDIA 分支 → 游戏跑在核显上也会读独显的卡名/占用/温度 | README 已如实记为未处理 |
| F7 | **帧生成（DLSS-FG）的 OFA 值未在真实场景验证** | AI 引擎行的 `光流 x%` 目前只在日志里见过 0.0 | 需要一个开帧生成的游戏 |
| F8 | **`cfg.autoInject` 是死开关** | 字段/JSON 键都在，但 `AppAutoInjectTick()` **故意留空**，界面也没有它的开关 | 有意废弃（曾把梯子/微信/NextProbe 自己当游戏注入）；留着是为将来做启发式 |
| F9 | **DCGM profiling**（数据中心卡） | 民用卡用不上 | RT_TENSOR.md 第 3 层 |
| F10 | **HDR / 多显示器的桌面叠加未验证** | 叠加定位到前台窗口所在显示器，HDR 下颜色未验证 | 未见记录 |

---

## 3. 已实现但**未在真实场景验证**（🔨）

| # | 项 | 现状 | 怎么验 |
| --- | --- | --- | --- |
| V1 | **RT Core 引擎 pass 实测**（`DispatchRays`/`BuildAS` 时间戳 → `rtLoad`） | 代码已接（`deepEngineHook=1` 默认开），模拟器不做光追 → 只有代码级验证 | 用支持光追的 D3D12 游戏（《漫威蜘蛛侠》PC 版）看 RT Core 行是否出现 `x% (实测)` |
| V2 | **Tensor / AI pass 实测**（首次 draw 之后的 compute 区间 → `tensorLoad`） | 同上 | 开 DLSS/FSR 后看 Tensor 行是否 `x% (实测)` |
| V3 | **AI 引擎行的非零值** | 修好大小写后**没见过非零**（`ofa=0.0%`、`compute=-1.0%`） | 开 DLSS 帧生成 |
| V4 | **HWiNFO 里的 RT/Tensor 字段** | 代码会找标签含 `Tensor` / `RT Core` 的读数；**消费级卡上 HWiNFO 并没有这两个字段** | 开 HWiNFO 共享内存后看面板是否出现 `(硬件)`；大概率不会 |
| V5 | **看门狗的"正例"** | 反例验过（正常会话不输出），正例（渲染线程真卡住）没验 | 造一个"只挂起渲染线程"的靶子 |
| V6 | **各指标的"准不准"** | 自动化只断言"有值"；只有帧时间/GPU 帧时间与模拟器做过数值对照 | 需要另一套可信来源对照 |
| V7 | **`--selftest` / `--uismoke` 路径下的 ETW 行为** | 未见专项验证 | 跑一次看日志 |
| V8 | **NVML 索引 ↔ NVAPI 句柄顺序一致** | 代码假设两套枚举顺序一致（用 `nvmlIndex_` 索引 `nvapi_.gpus[]`） | 多卡机器上核对 |
| V9 | **32 位游戏注入链** | 有 `verify_x86_inject.py`，但只确认"日志里出现了钩子" | 真 32 位游戏 |

---

## 4. 口径未定（❓）

| # | 项 | 现状 | 影响 |
| --- | --- | --- | --- |
| Q1 | **显存"已用"的 PDH 回退口径** | `\GPU Adapter Memory(*)\Dedicated Usage` **和** `Shared Usage` **加在一起** | 可能超过 DXGI 专用显存 → 百分比被 clamp；本机走 NVML 不受影响 |
| Q2 | **AI 引擎行 0.0% 时整行消失** | `engOfa > 0` 才显示，`==0` 不显示 | "看不到这一行" ≠ "读不到"（日志里其实有值） |
| Q3 | **HWiNFO 的单位与字段语义** | 温度/功耗/显存**直接采信不换算** | 换 HWiNFO 版本/语言可能变 |
| Q4 | **NVAPI 未公开域 4..7 是什么** | 每轮探测，有值就采信 | 官方未公开 |
| Q5 | **`gVpW/gVpH` 的 32 位标量竞态** | 注释认可"影响可忽略" | 要严谨可换 `std::atomic` |
| Q6 | **CPU 频率回退的标称值** | `np_sensors.cpp:763` 硬编码 `kNominalMHz = 2300.0`，**代码里就一个 TODO** | 主路径（`CallNtPowerInformation`）可用；回退路径口径存疑（实测偏高约 1.34×） |

---

## 5. 死代码 / 死字段 / 过期文案（🧹）

> 这些**不影响功能**，但会误导后来者。清理它们会改 `sizeof` 或日志，按 `DATA-STRUCTS.md` 的清单走。

**完全没用过的宏/字段**
`NP_MUTEX_SENSORS`（定义了从未使用）、`NP_MOD_LEN` / `NP_MAX_MODULES`（无人使用）、
`NPConfig::opacity` / `fpsCap` / `pollMs`（写了没人读；`pollMs` 甚至有 UI 控件，实际节拍固定 120ms）、
`textColor` / `accentColor` / `warnColor`（渲染器不读，行颜色是 `LevelRGB()` 硬编码）、
`GameEntry::hooked`、`AppState::overlayVisible` / `uiVisible` / `lastPoll` / `lastOverlay`（只用声明，无读写）。

**实现了但没接线 / 无调用点**
`PdhQuery::RefreshWildcard`、`SumWhere2`、`SumInstance`、`MaxWhere`、
`SensorHub::SetOverride` / `override_`（"手动指定优先源"未接线）、`available()`、`sources()`、
`gameGpuAvailable()`、`maxGpuTemp()`、`encoderUtil()` / `decoderUtil()`（编解码占用采到了没人显示）、
`nvapiExtSeen()`、`NvmlApi::DeviceGetPerformanceState`、`NvapiApi::GPU_GetUsages`、
`InModule()`（**有意保留作教训备忘，不要顺手删**）、
`np::FrameStats::lowPct` / `lowIntegral` / `medianMs` / `RunningMax`（保留供对照）、
`np_build` 的 `HistRange()` / `HystWarn()`、`Overlay11::rtvSrc_`（历史遗留，原做法违反 DXGI 规则）、
`Overlay12::drawSkips_`（只声明不计数）、`gCs`（`EnterCriticalSection` 全文件没有）、`gCpuStartQpc`（只写不读）、
`hwinfoTried_` / `cpuFreqFromNt_`（只写不读）。

**注册了但没人读的 PDH 计数器**
`\Thermal Zone Information(*)\Temperature`（`np_sensors.cpp:512`，全仓无读取点）。

**过期文案**
`main.cpp:317` 附近的注释仍写着"由用户在界面上主动打开 `cfg.autoInject`"，而界面已无此开关、功能也故意留空。

---

## 6. 工程与测试缺口

| # | 项 | 现状 | 下一步 |
| --- | --- | --- | --- |
| T1 | ~~**"退出时不再注入"没有自动化回归**~~ ✅ **已补（2026-10-11）** | `tests/verify_quit_noinject.py`：预置配置让看护线程有维护目标 → 手动注入宿主 → 发「退出」→ 断言退出后 `attach:` 次数**不再增长**、主程序 ≤10s 退出。已纳入 `run_all.py` | ⚠ 沙箱里改不了 `%APPDATA%`（用例会优雅跳过）；沙箱的 TEMP 每次命令一换，主/钩子日志落点会被回收，所以**前置条件的确证**在沙箱里不可靠，真机正常 |
| T2 | ~~**看门狗没有正例测试**~~ ✅ **已补（2026-10-11）** | `tests/verify_watchdog.py`：只 `SuspendThread` **渲染线程**（不挂 worker）→ 8 秒内应出现 `watchdog: 已有 X.Xs 没有 Present 调用` → 恢复后应出现 `watchdog: Present 已恢复`。实测已通过 | 就是这个用例顺带抓出了"日志编码写错"（见 D11） |
| T3 | **`struct_check.py` 只检查 `NPTelemetry`** | `NPConfig` / `NPSensors` 没有镜像比对；且 `NPSensors` 字段宽度和 444 ≠ `sizeof` 448（尾部填充），直接加会**误报** | 先修脚本比较方式，再补两个结构体 |
| T4 | **C++ 侧没有读方 `version`/`size` 校验** | 钩子/主程序只看 `magic`（G4 属实，grep 无 `->version` 比较） | 两侧各加一行判断 |
| T5 | **`sizeof(NPConfig/NPSensors/NPTelemetry)` 没有编译期断言** | 无 `static_assert` | 加三行 |
| T6 | **`run_all.py` 只断言"有值"** | 7 项用例都不验"准不准" | 见 V6 |
| T7 | **`NPConfig::reserved` 只剩 4 个槽位** | 再无槽位可用时加字段**必须**改 `sizeof` → 两侧必须同时升级 | 建议改成递增布局版本号 |
| T8 | **"恢复默认"不重置运行时开关**（G5） | `NPDefaultConfig` 不碰 `autoInject`/`pauseHook`/`learnedAutoHook`/`detachPid`（已在代码里核实） | 确认是"有意"还是漏了，并在注释写明 |
| T9 | **文档行号漂移** | `HOOK.md` 头部写 2755 行、§1.3 写 2744；两份文档同一函数行号互不相同；`CATALOG.md` 的 `src/app` 行数也是旧估值 | ✅ **已根治（2026-10-11）**：两份文档共 707 处行号引用全部清掉，改为只引用函数名/变量名（见 §8 D1、`CATALOG.md` §6 的维护约定） |
| T10 | **`tests/verify_quit_noinject.py` 用错了配置文件名** | 它预置 `%APPDATA%\NextPerf\settings.json`，而程序读的是 **`config.json`**（`settings.cpp` 的 `SettingsPath()`）→ 看护线程永远拿不到维护目标，**用例的前置条件恒不成立**，主判据有可能空跑 | 🚧 待修：文件名改成 `config.json`，并**顺手备份/还原它**（主程序经「退出」路径结束时会 `AppSaveSettings()` 重写该文件）。已记进 `docs/CLEANUP.md` §7 的 C1/C2 |
| T11 | ~~**"DX12 被判成 Vulkan"没有自动化覆盖**~~ ✅ **已补（2026-10-11）** | 新用例 `tests/verify_dx12_decoyvk.py` + 测试宿主新开关 `host_run.exe --decoy-vulkan`（照常跑 DX12、额外加载 `vulkan-1.dll`）。**修复前的钩子跑它必 FAIL**（已实测：`hostApi=20` / `probe disabled` / `attached=0`），修复后 `hostApi=12` / `probe attempt 1/3` / `attached=1` | 已纳入 `run_all.py` |
| T12 | **`verify_metrics.py` 依赖"应用有一个游戏目标"** | 该用例用裸 `LoadLibrary` 注入，应用要靠**学习机制**才知道该轮询哪个进程；而学习又依赖游戏名单/前台等条件。2026-10-11 用户清空名单后，应用日志里**根本没有「数据来源」块** → 用例判 FAIL。⚠ 这**不是产品问题**（清空名单后不轮询是合理的），但用例的失败信息会误导 | 让用例自己造目标（像 T1 那样预置名单，且**必须备份/还原 `config.json`**），或把"没有目标"与"目标存在但读不到 PDH"两种失败分开报 |

---

## 7. 已经修好的口径错误（留档：别再写回去）

> 2026-10-11 晚做了一次**全文档扫描**（4 个子代理分头通读 11 份文档 + 我自己在代码里逐条核实），
> 下表是那一轮**已修**的条目；紧接着的 §9 是**还没修**的。

| 曾经的错误说法 | 事实 | 修在哪 |
| --- | --- | --- |
| DATA-STRUCTS 说 1% Low「最终口径 = `lowPercentileFps`」 | 主路径是 **`lowWindowed`（窗口平均）**，百分位只在窗口数 < 4 时兜底（`np_hook.cpp` 1186-1191） | `DATA-STRUCTS.md`（本轮） |
| HOOK.md §16.4 说「钩子层不再调用 `lowWindowed`」 | **它正是主路径** —— 这句把主次写反了 | `HOOK.md` §16.4 + §23.3 表（本轮） |
| COMMON §3.5/§8.2.11、DATA-STRUCTS §4.1/§4.2、CATALOG §2.3 说「已做帧内重复 Present 合并」 | **合并已回退**：吸收那一行被注释掉（`// gAbsorbThisPresent = true;`），只有 `gMergedCount` 还在自增 → 日志 `merged=N` 是「满足阈值的次数」。**README 原本就是对的** | 三份文档（本轮） |
| COMMON §8.4.26、DATA-STRUCTS 三处说面板「CPU 帧时间 = `cpuBusyAvg + cpuWaitAvg`」 | 面板实际显示 **`cpuBusyAvg`**（`np_build.cpp:296`，`cpuFrameMs` 仅兜底）。⭐ `cpuFrameMs` **字段**确实是 Busy+Wait —— **同名不同物**，必须分开写 | 两份文档（本轮） |
| README 说 GPU 帧时间「0.000 ⚠️ 待查、尚未定性」 | 早已结案：模拟器每帧 GPU 仅约 0.005ms，**低于测量分辨率**，读 0 是预期 | `README.md`（本轮） |
| README 闪退节「正常玩不受影响」「目前没有根治办法」 | 前者已被实测**否掉**（正常玩 8 秒就崩）；后者随 v1.6.4 修复作废 | `README.md`（本轮） |
| CHANGELOG v1.6.4 验证表写「真实游戏实测 ❌ 未做」 | 与同段开头冲突 —— 用户**已用该版**实测（2755 帧 / 53 秒 / 零 SEH） | `CHANGELOG.md`（本轮） |
| APP.md G13 没有结案标记，仍写「D3D12 叠加目前不可靠」 | 结案：根因就是 G15 那条（**不是「注入太早」**），10 秒延迟只作安全边际保留 | `docs/APP.md`（本轮） |
| SENSORS.md §4 表说 AI 引擎「**当前**因大小写不匹配而失效」 | 已在 `53037bd` 修复 | `docs/SENSORS.md`（本轮） |
| BUILD.md 手动编译命令缺 `src/etw/np_etw.cpp` 与 `-ltdh` | 照抄**会链接失败**（`build.bat` 里有） | `docs/BUILD.md`（本轮） |
| CATALOG 文件行数（~1000 / ~813 / ~587 …） | 真值 **1265 / 941 / 772 / 184 / 135 / 187**（LF 口径）。原值疑似用 `Get-Content` 数的 —— 那个口径会吃掉 LF-only 文件的空行 | `docs/CATALOG.md`（本轮） |
| LOGGING §1 有两个「6.」编号；§4 有 `ests/verify_reinject.py` 错字（吞了 t） | 排版问题，会干扰检索与引用 | `docs/LOGGING.md`（部分修：编号已改 7） |
| Low 帧"最终采用口径③ 严格百分位"、口径④"无调用点" | 实际用**口径④ 窗口平均**（1000ms 窗口 / 回看约 10s），口径③ 只在窗口数不足 4 个时兜底 | `COMMON.md` §3.4/§8.2/§9.2、`CATALOG.md` §2.3/§5-10、README |
| AI 引擎 `engtype` 大小写 bug"是现存缺陷" | 已在 `53037bd` 修复（`engtype_Compute`/`engtype_OFA`）；**真机复验仍未做**（见 §3 V3） | `SENSORS.md` §3.4/§6.1、`SENSORS-FUNCTIONS.md` |
| APP.md §17 把 G1/G2/G3 记为"本次未改" | 都已修好（`np_common.h` 越界、学习路径加锁、UI 改用 `telemetryLive`） | `APP.md` §17（连 G9 一起） |
| `ARCHITECTURE.md` 的判活片段用跨进程时钟 `(now - tickMs) < 2500` | 已改成帧计数增长 + `gApp.telemetryLive` | `ARCHITECTURE.md` §2.4 |
| README 说静默机制"挂在一个永远不成立的条件上、等于从未生效" | 触发条件是 RTV 槽位攒满 8 个，而槽位按后台缓冲对象累加 → **会触发，只是要等到第二~三次重建**；准确说法是"来得太晚" | README 已知问题 |
| 数据来源日志文案写"最近1200帧样本推算"、`CPU 帧时间 <- 帧周期 - Present 阻塞`、`engtype_compute/engtype_ofa` | 三处都已按代码实际口径改正 | `src/app/main.cpp` |
| （2026-10-11 深夜）**纯 DX12 游戏被判成 Vulkan → 完全没数据**（《漫威蜘蛛侠》实测） | 根因：钩子自己静态导入 `d3d12.dll`/`d3d11.dll`，而 `SelfImports()` 把它们全跳过 → 最强 D3D 信号**永远失效** → `vulkan-1.dll` 命中 → `probe disabled` → `swapVt=0`（一次都没挂上）。**修法**：用 `D3D12Core.dll`（只在真建有 D3D12 设备时出现、我们没导入）作首要判据；`GuessApiFromModules()` 与 `HostApiGuess()` 口径统一 | `src/hook/np_hook.cpp`；回归用例 `tests/verify_dx12_decoyvk.py`（**修复前必 FAIL**）；`docs/HOOK.md` §7 教训 3 + §22 坑 6b |
| （2026-10-11 深夜）**CPU 帧时间掉到 0.几 ms**（《漫威蜘蛛侠》实测"太异常"） | ✅ **已修并经用户实测确认**（v1.6.5）：根因是 `gPresentRetQpc`（Busy 与模拟阶段的基准）漏在 `realPresent` 判断之外 → 被每帧那次 `DXGI_PRESENT_TEST` 探测调用的返回覆盖 → **`Busy + Wait` 只剩半个帧周期**（0.35+7.88=8.23 vs 16.7ms）；面板显示 `cpuBusyAvg` 故为 0.3ms。**修法**：只有真实呈现才推进该基准（帧记账的 `gLastPresentQpc` 本来就这么判）。用户验收原话：**"这个版本的 CPU 帧时间比以往任何一个版本都要正确"** | `src/hook/np_hook.cpp`；回归用例 `tests/verify_present_test.py`（`host_run.exe --probe-present`，修复前比值 0.48 / 修复后 1.09）；`docs/HOOK.md` §22 坑 6c |
| （2026-10-11 深夜）**CPU 占用率"乱跳"**（用户："蜘蛛侠会波动到 50%"） | ✅ **已改（待用户验收）**：指标本身没错 —— 它是**整机口径**（`GetSystemTimes`，100% = 24 逻辑处理器跑满），20-30% ≈ 5-7 线程、50% ≈ 12 线程，两游戏差异来自引擎多线程程度。问题在**刷新窗口**：原来是两次轮询的瞬时差分，窗口仅约 120ms。修法：按真实间隔做 1 秒级指数平滑（`alpha = 1 − exp(−dt/500ms)`） | `src/sensors/np_sensors.cpp` + `.h`；`docs/SENSORS.md` §3.1、`DATA-STRUCTS.md` 的 `cpuUsage` |

---

## 8. 还没修的口径债（本轮扫描发现，**已登记未动手**）

> 这些都是"文档之间/文档与代码之间还对不上"的地方。按 `CATALOG.md` 的规矩：
> 动它们的时候**顺便把两边都改掉**，别只改一处。

| # | 项 | 现状 | 建议 |
| --- | --- | --- | --- |
| D1 | ~~**HOOK.md 与 HOOK-FUNCTIONS.md 系统性行号漂移**~~ ✅ **已根治（2026-10-11）** | `HOOK.md` **423 处 → 0**、`HOOK-FUNCTIONS.md` **284 处 → 0**；两份文档头部都改成新政策：**不引用行号，只引用函数名/变量名/日志文本**（结构完整、解释文字一条未删） | 后续新增内容**也不许再写行号**；行号只在绝对必要时以"某函数内"描述 |
| D11 | **日志编码写错：文档说 GBK，实际是 UTF-8** ✅ **已修** | 按 GBK 解会得到 `宸叉湁 5.2s` 这种乱码 → **中文关键字永远匹配不上**（`_exitprobe.py` 因此一直没察觉；新写的看门狗用例第一次跑也栽在这） | 已改 `LOGGING.md` 编码表与 PowerShell 读法；`_exitprobe.py`/`verify_watchdog.py`/`verify_quit_noinject.py`/`sim_stress.py`/`_bisect.py` 全部改成 UTF-8 优先、GBK 兜底 |
| D12 | **`Get-Content` 会吃掉 LF-only + 无 BOM UTF-8 文件的换行** | 本机默认按 GBK 解码这种文件：`(Get-Content docs\HOOK-FUNCTIONS.md).Count` = **278**，真值 **307**。这正是 `CATALOG.md` 行数写错的根源（已于本轮更正） | 数行数用 **LF 字节数** 或 `-Encoding utf8` / `[IO.File]::ReadAllLines`；`Select-String` 不受影响 |
| D13 | **沙箱 TEMP 每次命令一个、结束即回收** | 我的测试脚本里，主程序/钩子日志的落点会因此漂移（表现为"读不到日志"），一度被误判成产品问题 | 测试**不要依赖单一日志路径**：按 mtime 在多个候选里挑；判据尽量落在跟着被测进程走的那份日志上 |
| D2 | 日志字段 `frame merge: … merged=N` **误导** | 它其实是"满足合并阈值、但**没有吸收**"的次数（合并已回退）。子代理就是照它得出"合并生效"的错误结论 | 改名 `wouldMerge=`（**改它要重新构建 + 重测**，放下一个版本） |
| D3 | **`sim.exe` 切垂直同步时自己就崩**（不注入也崩） | 而 `sim_stress.py` 把 vsync 当 12 条重建场景之一 → **对照组的"不注入是稳的"前提不成立** | 先修模拟器（或把 vsync 从默认场景里摘掉并注明） |
| D4 | **钩子 vs 模拟器自报的数值对照（ground truth）从未做** | `SIMULATOR.md` §5.1 自己写着这是"模拟器最大的用途" | 一次对照就能把"准不准"从 ❓ 变成 ✅（`verify_framedata.py` 只比了帧间隔与阻塞） |
| D5 | **干净卸载链没有自动化** | `verify_reinject.py` 名义测重复注入，但日志显示**从未走到卸载路径**（任务管理器结束不走 `DllMain`）；`run_all.py` 只跑 7 项，不含 `verify_reinject` / `_exitprobe` / `sim_stress` / `verify_framedata` | 用 `detachPid` 显式卸载并断言 `SELFCHECK unloadClean = OK` |
| D6 | `simvk` 的独占全屏 / MAILBOX / FIFO_RELAXED 分支跑不到 | 本机枚举出 0 个模式、只有 IMMEDIATE 可用 | 换机器或换驱动再补，或明确标"未覆盖" |
| D7 | ETW 三项未确认 | 真机事件名是否 `Present_Start`；反作弊的 `ACCESS_DENIED` 与"没权限"**不可区分**；`--selftest/--uismoke` 下 ETW 行为未验 | 以管理员跑一次，把匹配到的事件名打进日志 |
| D8 | `NPConfig::version` 恒为 1，另两个结构体是 `sizeof` | 三份文档对"自校验字段"的说法不一致（`version` vs `size`） | 统一约定并在 `np_common.h` 注释里写死 |
| D9 | 混合显卡、HDR、多显示器未验证 | NVIDIA 驱动存在时永远走 NVIDIA 分支 → 游戏跑核显也会读独显 | 需要一台双显卡机器 |
| D10 | `NP_GFXAPI` 声明了 Vulkan / OpenGL / D3D9，但 CATALOG §0 只说挂 DXGI/D3D11/D3D12 | 这几档是否实现，三份文档都没说 | 明确写成"枚举保留、未接入"或删枚举 |

---

## 9. 一句话版：现在还剩什么

* **拿不到的数**：RT/Tensor **硬件**占用率（民用卡不提供，❌ 永久）；NVAPI 的 FB/VID/BUS 三域（本机驱动拒绝，❌ 暂时）；
  Vulkan/OpenGL 游戏的帧数据（设计边界）。
* **要外部条件才能拿到的数**：CPU 温度、GPU 热点、显存结温、风扇%（开 HWiNFO 共享内存）；
  ETW Low 帧（管理员）。
* **实现了但没在真实验证过的数**：RT / Tensor / AI 的**引擎 pass 实测**（需要支持光追或 DLSS 帧生成的游戏）。
* **看得到"无 DXR / 无 AI"不等于坏了**：re8 既没有光追也没开 DLSS —— 那两行显示的是**正确结论**。
* **要手动勾才出现的行**：GPU 热点、显存带宽、视频引擎、PCIe 总线、Draw 数、AI 模块、风扇、数据源、显卡型号、CPU Busy/Wait。
* **真正的功能欠账**：`ResizeBuffers` 钩子（F1）、日志轮转（F2）、两条自动化回归（T1/T2）。