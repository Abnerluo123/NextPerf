# NextPerf 代码总目录（CATALOG）

> **这是整个项目的总入口。**
>
> ## ⚠️ 给未来的 AI 与其他迭代者的第一条规矩
>
> **每次修改代码，必须同步更新本目录以及对应的功能文档。**
>
> 这不是"最好做"，是**必须做**。原因很实在：
> * AI 助手（包括我）会在长对话中被**压缩上下文**，压缩后我对自己改过什么
>   的"记忆"是不可靠的 —— 靠得住的是**仓库里的文档**。
> * 这个项目已经被"改了 A 忘了同步 B"坑过很多次：改了渲染分辨率的口径、
>   忘了同步 README 的已知问题；加了配置项、忘了加 `settings.cpp` 的存取
>   （导致开关重启就丢）；加了控件、忘了改窗口高度（按钮被挤出窗口）。
> * 文档不同步的代价是**下一次修改建立在对项目的错误认知上**。
>
> **改完代码请自问**：这个功能在下面哪一节？我更新那一节了吗？
> `docs/CATALOG.md` 与对应的 `docs/*.md` 都更新了吗？README 的
> 「已知问题」是否需要改？
>
> 如果是新增文件，**必须**在本文的「文件清单」里加一行。

---

## 0. 30 秒认识这个项目

**NextPerf** 是一个 Windows 游戏性能计数器（C++20，无 MSVC，用 **zig 0.16+** / `0.17.1`
交叉编译）。它做两件事：

1. **不注入**：用 NVML / NVAPI / PDH / WMI 等读硬件指标（CPU/GPU 占用、温度、
   功耗、频率、显存…），在**桌面叠加窗口**上显示。
2. **注入**：把一个钩子 DLL 注入到游戏进程，挂钩 **DXGI / D3D11 / D3D12**，
   采集**真实帧数据**（帧时间、GPU 帧时间、CPU 帧时间、1% / 0.1% Low…），
   并把面板**画在游戏画面里**（独占全屏下也能显示）。

```
                    ┌─────────────────────────────────────────┐
                    │  NextPerf.exe（主程序，src/app）         │
                    │  采传感器 · 注入器 · 桌面叠加 · 自绘界面  │
                    └───────────────┬─────────────────────────┘
                                    │ 共享内存（src/common/np_common.h）
                    ┌───────────────┴─────────────────────────┐
                    │  Config（双向）  Sensors（主→钩子）      │
                    │  Telemetry（钩子→主，**每个 PID 一份**）  │
                    └───────────────┬─────────────────────────┘
                                    │ CreateRemoteThread + LoadLibraryW
                    ┌───────────────┴─────────────────────────┐
                    │  NextPerfHook.dll（注入游戏，src/hook）   │
                    │  钩 Present/ResizeBuffers/Vulkan IAT    │
                    │  采集 · 游戏内/桌面 HUD                 │
                    └─────────────────────────────────────────┘
```

**三层共用一份数据契约**：`src/common/np_common.h`。任何结构体字段的改动
都要动三处（主程序、钩子、`tests/verify_inject.py` 里的 ctypes 镜像），
并且要让 `tests/struct_check.py` 过。

---

## 1. 功能文档索引（每个功能一份详解）

| 文档 | 覆盖范围 | 什么时候看它 |
|---|---|---|
| [`CATALOG.md`](CATALOG.md) | **总目录**（本文） | 任何时候，先看这里 |
| [`TODO.md`](TODO.md) | **已知缺口与待办总表**（未实现 / 未验证 / 条件性可用 / 死代码 / 口径债）——**状态唯一权威来源** | 想知道"还有什么没做完、什么读不到、为什么"时 |
| [`CLEANUP.md`](CLEANUP.md) | **本机环境改动台账与清理清单**（装过什么、改过什么、还剩什么） | 装/改环境时**必须**记一行；项目收尾清电脑时照着删 |
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | 整体架构、Present 是怎么挂上的 | 想理解整体设计 |
| [`HOOK.md`](HOOK.md) | **钩子层**：注入后干了什么、怎么挂钩、怎么采集、怎么画、怎么卸载 | 改钩子前必读 |
| [`HOOK-FUNCTIONS.md`](HOOK-FUNCTIONS.md) | 钩子层函数速查表 | 找函数在哪 |
| [`APP.md`](APP.md) | **主程序层**：主循环、注入器、UI、托盘、配置 | 改主程序前必读 |
| [`APP-FUNCTIONS.md`](APP-FUNCTIONS.md) | 主程序层函数速查表 | 找函数在哪 |
| [`COMMON.md`](COMMON.md) | **公共层**：数据契约、统计与 Low 帧算法、面板渲染、JSON | 动结构体/算法前必读 |
| [`DATA-STRUCTS.md`](DATA-STRUCTS.md) | **共享内存逐字段说明**（数据契约） | 动任何结构体字段前**必读** |
| [`SENSORS.md`](SENSORS.md) | **传感器层 + ETW**：各路数据源与降级策略 | 改传感器前必读 |
| [`SENSORS-FUNCTIONS.md`](SENSORS-FUNCTIONS.md) | 传感器层函数速查表 | 找函数在哪 |
| [`LOGGING.md`](LOGGING.md) | **日志规范**（含"日志文件在哪"的重要警告） | 加日志、看日志前必读 |
| [`SIMULATOR.md`](SIMULATOR.md) | **模拟游戏测试宿主**（DX11/DX12/Vulkan） | 想在不依赖真人的情况下验证时 |
| [`BUILD.md`](BUILD.md) | 构建说明 | 第一次构建 |
| [`RT_TENSOR.md`](RT_TENSOR.md) | RT Core / Tensor 负载为什么做不出来 | 想碰这两个指标时 |

---

## 2. 按功能分类（功能 → 文件 → 关键函数）

> 详细实现逻辑见对应的功能文档。这里只做**定位**用。

### 2.1 注入链路

| 功能 | 文件 | 关键函数 |
|---|---|---|
| 64 位与便携单文件释放 | `src/app/injector.cpp`、`src/app/hook_embed.S` | `ExeDir`、`DllPath`、`DllPath32`、`HookEmbedInfo`、`ExtractEmbeddedHook`、`InjectInto`、`IsInjected` |
| **32 位游戏注入（WoW64）** | `src/app/injector.cpp` | `NpExportAddr32`、`NpResolve32LoadLibraryW`、`NpReadRemote` |
| 自动注入已添加的游戏 | `src/app/injector.cpp` | `InjectorScanNow`（**按 exe 名匹配**） |
| 请求钩子自卸载 | `src/app/injector.cpp`、`src/app/main.cpp` | `RequestHookDetach`、`QueueHookDetach`（`gDetachQueue`）、`RemoveGameLocked`、`ForgetLearned` |
| 便携配置读写 | `src/app/settings.cpp` | `SettingsPath`（支持 `NextPerf.portable` / 同目录 `config.json`）、`SettingsLoad`、`SettingsSave` |
| 注入的钩子入口 | `src/hook/dllmain.cpp`、`src/hook/np_hook.cpp` | `DllMain`、worker 线程 |
| 命令行注入 | `src/app/main.cpp` | `RunInjectCli`（`--inject <pid>`） |

### 2.2 挂钩（hook）

| 功能 | 文件 | 关键函数 |
|---|---|---|
| 交换链 vtable 探测 | `src/hook/np_hook.cpp` | `ProbeSwapChainVtable`（自造临时交换链 + SEH 保护 + `gProbePoisoned`） |
| DXGI 工厂钩子 | `src/hook/np_hook.cpp` | `InstallDxgi`（`CreateSwapChain` / `ForCreateSwapChainForHwnd` / `ForComposition`） |
| 交换链 vtable 钩子 | `src/hook/np_hook.cpp` | `PatchSwapChainVtable`、`PatchSafe`（含 `PointsIntoOurDll` 防递归）、`SwapVtFor` |
| Present / Present1 | `src/hook/np_hook.cpp` | `PresentCommon`（**核心**，含 4 角定位 `cfg.fpsCap & 3u`、`fpsLow01` 3 秒窗、`pauseHook` 跳过渲染保留收尾） |
| `ResizeBuffers` / `ResizeBuffers1` | `src/hook/np_hook.cpp`、`src/hook/np_draw.cpp` | `NpResizeBuffers`、`NpResizeBuffers1`、`BeforeSwapchainResize`、`Overlay11/12::OnResizeBuffers`（含 200ms fence 等待） |
| Vulkan 帧遥测（非侵入 IAT） | `src/hook/np_hook.cpp` | `InstallVulkan`、`PatchModuleVkIat`、`NpVkQueuePresentKHR`、`NpVkCreateSwapchainKHR`、`gVkProcAddrHandedOut`（含 `p99`/ETW/曲线/`vkIat=0` 自检） |
| D3D12 命令队列/命令列表 | `src/hook/np_hook.cpp` | `EnsureD3D12Hooks`、`NpDispatchRays`/`NpBuildAS`(`TS_RT_BASE`)、`NpDispatch`(`TS_AI_BASE` 多槽)、`gUnloading`+`calledOrig` |
| **还原所有补丁（幂等）** | `src/hook/np_hook.cpp` | `RestoreAllHooks`（**两条卸载路径都必须调**） |
| 干净卸载 | `src/hook/np_hook.cpp` | `SelfUnloadNow`、`NpHookDetach`、`detachPid` 机制 |

### 2.3 帧数据采集与算法

| 功能 | 文件 | 关键函数 |
|---|---|---|
| 帧时间采集 | `src/hook/np_hook.cpp` | `PresentCommon` 里的 record 段 |
| ~~帧内重复 Present 的合并~~ | `src/hook/np_hook.cpp` | ❌ **已回退（不再吸收）**：吸收那一行被注释掉（`// gAbsorbThisPresent = true;`），只有 `gMergedCount` 仍在自增 —— 所以日志里的 `frame merge: … merged=N` 是「**满足阈值的次数**」，**不是真的合并了 N 帧**。判据仍取自 `gRawStats.meanMs(240)` |
| **Low 帧（PresentMon 百分位口径）** | `src/hook/np_hook.cpp` + `src/etw/np_etw.cpp` | **滑动窗口内直接取百分位**（1% Low 窗口 1 秒、0.1% Low 窗口 3 秒；线性插值 `index=(n−1)p`、倒数指标排名反转）。管理员运行时窗口内的样本来自 **ETW 驱动侧显示事件**，并由主程序经 `NPConfig.reserved[]` 中继给钩子（见 `np_common.h` 的 `NPConfigSetEtwRelay`）。`lowWindowed`（窗口平均）**已退休**，只留在 `np_stats.h` 作历史对照 |
| 环形缓冲与百分位 | `src/common/np_stats.h` | `FrameStats`、`push`、`recent`、`percentileMs` |
| **CPU Busy / Wait 拆分** | `src/hook/np_hook.cpp` | Present 返回后**同一次减法**导出三个值 |
| GPU 帧时间 | `src/sensors/np_sensors.cpp` | PDH `\GPU Engine(pid_*)\Running Time` |
| 低延迟状态（Reflex） | `src/hook/np_reflex.h` | `ReflexReader`、`NvAPI_D3D_GetSleepStatus` |

### 2.4 叠加层绘制

| 功能 | 文件 | 关键函数 |
|---|---|---|
| 游戏内叠加（D3D11） | `src/hook/np_draw.cpp` | `Overlay11::Draw`（状态保存/恢复 + RTV 每帧新建本帧释放 + `NormalizeRtvFormat` + 视口裁剪） |
| 游戏内叠加（D3D12） | `src/hook/np_draw.cpp` | `Overlay12::Record` / `OnFrameCompleted`（描述符堆"换堆" + 200ms fence 回收） |
| 面板渲染（共用） | `src/common/np_panel.cpp` | `PanelRenderer`（Apple Metal HUD 2 行顶栏、核心 4 行 `[min max]`/`[VRAM]`、10 格双通道示波器、`ThemeFor` 4 套 Apple 材质、`LayoutHorizontalChips`） |
| 面板行组装 | `src/common/np_build.cpp` | `BuildPanelData`、`ShortGpuName`、`NPComputeBottleneck`、`NP_PRESET_*`、Apple Metal 核心/扩展排版 |
| 桌面叠加窗口 | `src/app/overlay.cpp` | `OverlaySetVisible`、`OverlayUpdate`（每显示器 DPI `Shcore!GetDpiForMonitor` + `overlayMode == 3` 仅后台） |

### 2.5 主程序与界面

| 功能 | 文件 | 关键函数 |
|---|---|---|
| 主循环 / 托盘 | `src/app/main.cpp` | 窗口过程、`NP_TRAY_*` 消息（含 `NP_TRAY_LAYOUT`）、`pollMs`/`updateHz` 独立计时 |
| 控制台界面（Direct2D） | `src/app/ui.cpp` | Apple / macOS Dark 统一风格四标签页控制台（Direct2D + DirectWrite）、`UiCanvas`、实时 Metal HUD 预览、异步扫描/注入线程 |
| 前台窗口跟踪 | `src/app/main.cpp` | `AppTrackForeground`、`ForegroundWinEvent`、`ResolveForegroundPid`（UWP 剥壳） |
| **可信名单自动学习** | `src/app/main.cpp` | 三条验证：`NP_HOOK_PRESENT` + `gfxApi != UNKNOWN` + `frameTotal` 增长 |
| **陈旧遥测回收** | `src/app/main.cpp` | 帧数 2.5 秒不增长 → 判定钩子已死 → 清空遥测 |
| 配置存取 | `src/app/settings.cpp` | `SettingsLoad`、`SettingsSave` |

### 2.6 传感器

| 功能 | 文件 | 关键函数 |
|---|---|---|
| 传感器总调度与自动优选 | `src/sensors/np_sensors.cpp` | `SensorHub::Init` / `Poll` |
| CPU 频率 | `src/sensors/np_sensors.cpp` | `CpuFreqFromPowerInfo`（`CallNtPowerInformation`，**动态加载 powrprof**） |
| CPU 功耗 | `src/sensors/np_sensors.cpp` | PDH **`Energy Meter`**（不是 `Energy Meter Interface`） |
| NVAPI（显存频率等） | `src/sensors/np_vendor.cpp` | `NvapiReadGpuClocks` |
| ETW（DxgKrnl Present） | `src/etw/np_etw.cpp` | 见 `SENSORS.md` |

---

## 3. 文件清单（新增文件必须在这里加一行）

### 3.1 主程序（`src/app/`）

| 文件 | 职责 |
|---|---|
| `main.cpp` | 主循环、托盘（含 `NP_TRAY_LAYOUT`）、全局热键 `Ctrl+Shift+O/B/P/R/L`、CSV 实时分位数 `# SUMMARY`、诊断剪贴板、前台跟踪、`telSlots` 回收 |
| `ui.cpp` | Apple / macOS Dark 统一风格 Direct2D + DirectWrite 紧凑四标签页控制台（`900×580`）、实时 `1:1` Metal HUD 预览、异步扫描/注入线程 |
| `injector.cpp` | 内嵌 64/32 位钩子 DLL 哈希释放（`%TEMP%\NextPerf\hooks\<hash>` + AppContainer ACL）、WoW64 32 位注入、看护扫描、多 PID `gDetachQueue` |
| `hook_embed.S` | `.incbin` 将 `NextPerfHook.dll` 与 `NextPerfHook32.dll` 内嵌至 `NextPerf.exe` 只读段，支持单 EXE U 盘便携使用 |
| `settings.cpp` | 配置 JSON 存取（`cfgVersion = 8` 紧凑 Metal HUD 迁移 + 便携模式同目录配置 + 无锁快照落盘） |
| `overlay.cpp` | 桌面分层叠加窗口（4 角定位 + 每显示器 DPI + 仅后台模式） |
| `np_app.h` | `AppState` / `GameEntry` / `BenchmarkSummary` / 便携内嵌查询接口 |

### 3.2 钩子（`src/hook/`）

| 文件 | 职责 |
|---|---|
| `np_hook.cpp` | **钩子主体**：探测、`PatchSafe`、DXGI/D3D12/Vulkan IAT 挂钩、采集、遥测、`gUnloading` 安全卸载 |
| `np_draw.cpp` / `.h` | 游戏内叠加绘制（D3D11 状态保存恢复 + D3D12 200ms fence 等待与 `OnResizeBuffers`） |
| `np_reflex.h` | NVAPI Reflex / 低延迟状态 |
| `dllmain.cpp` | DLL 入口 |
| `np_hook.h` | 导出声明 |

> ⚠ 按 §6「不再写行号」约定，本表仅维护模块职责，不记录易漂移的物理行数。

### 3.3 公共（`src/common/`）

| 文件 | 职责 |
|---|---|
| `np_common.h` | **数据契约**：`NPConfig` / `NPSensors` / `NPTelemetry` / 位域辅助与预设常量 |
| `np_stats.h` | `FrameStats` 环形缓冲、百分位、Low 帧 |
| `np_panel.cpp` / `.h` | Direct2D 面板渲染器（4 套主题、面积折线填充、横向精简条布局） |
| `np_build.cpp` / `.h` | 面板行组装、`NPComputeBottleneck`、Preset 与新指标 |
| `np_json.h` | 极简 JSON 解析（含越界与推进保护） |
| `np_bitmap.cpp` / `.h` | 位图工具 |

### 3.4 传感器与 ETW

| 文件 | 职责 |
|---|---|
| `src/sensors/np_sensors.cpp` / `.h` | NVML/NVAPI/PDH/WMI/HWiNFO 等采集与优选（含去重、缓存退避、DXGI 优选与 HWiNFO 重连） |
| `src/sensors/np_vendor.cpp` / `.h` | NVAPI / ADL 动态加载与结构定义（含 72 字节 `NV_GPU_DYNAMIC_PSTATES_INFO_EX`） |
| `src/etw/np_etw.cpp` / `.h` | ETW 会话（DxgKrnl Present、锁安全关闭、`184/171` 基准重置与诊断保留） |

### 3.5 测试（`tests/`）

| 文件 | 职责 |
|---|---|
| `run_all.py` | 回归总入口 |
| `verify_inject.py` | 注入端到端 + **ctypes 结构体镜像** |
| `verify_reinject.py` | **重复注入不闪退**（守 VEH / vtable 残留那条链） |
| `verify_x86_inject.py` | 32 位注入 |
| `verify_frontinject.py` / `verify_early.py` / `verify_metrics.py` | 其它注入场景 |
| `struct_check.py` | 校验 ctypes 镜像与 C++ 结构体一致 |
| `verify_dx12_decoyvk.py` / `verify_present_test.py` / `verify_watchdog.py` / `verify_quit_noinject.py` | 2026-10-11 补的四条回归（`--decoy-vulkan` 的 DX12 识别、`--probe-present` 的帧基准、看门狗正例、退出不再注入），都在 `run_all.py` 里 |
| `repro_low_frame.py` | **诊断工具（不在套件里）**：起宿主 `--jitter-present` 复现"显示 60fps 但 Present 间隔乱跳"，打出"显示事件探测 / vblank / 对齐率 / 重复呈现次数"——这是 ③「以显示帧为准」的取数工具 |
| `merge_algo_check.py` | 帧内合并算法的数值验证 |
| `low_recover_check.py` | Low 帧恢复时间的量化（历史分析） |
| `panel_width_check.py` / `row_metrics_check.py` | 面板布局 |
| `shot_overlay.py` / `shot_ui.py` / `diag.py` / `vt_check.py` / `try_inject.py` | 截图与诊断工具 |
| `_exitprobe.py` | **复现「钩子开着时关窗口，进程退不掉」**（发 WM_CLOSE 给测试宿主，再看进程是否退出 + 抓 DETACH 日志）。只结束自己启动的进程，**不会** taskkill 用户的 NextPerf |
| `_bisect.py` / `_readmecrash.py` / `_adddebuglayer.py` | D3D12 闪退的二分定位 / README 用崩溃复现 / 装调试层（`_` 前缀 = 内部探针） |
| `host_run.cpp` | **旧的**简易测试宿主（将被 `sim/` 取代） |
| `verify_ground_truth.py` | **真值对照验证**（对比 `sim.exe` DX11/DX12 真值与 `NPTelemetry` 及 Compute Dispatch） |
| `sim/` | **2D/3D 模拟基准程序**（DX11/DX12，同步导出为 `dist\NextPerfBench.exe`，窗口标题实时显示真值） |
| `simvk/` | **新的 Vulkan 模拟游戏**（见 `SIMULATOR.md`） |

### 3.6 仓库根

| 文件 | 职责 |
|---|---|
| `README.md` | 用户向说明 + **「已知问题」**（过期条目比没有更糟，见 §6） |
| `CHANGELOG.md` | **变更记录**：`main` 上还没发布的东西、以及每个发布版本包含什么。未发布段落**必须**标注「未经用户完整测试」 |
| `build.bat` | 构建脚本（**GBK + CRLF**，改成 UTF-8/LF 会让 `cmd.exe` 解析出错） |

> `dist/` 不在版本库里（`.gitignore`），二进制通过 GitHub Release 分发 ——
> 所以「改了什么」和「发布了什么」是两件事，别混为一谈。

---

## 4. 构建与验证（最短路径）

```powershell
# 环境（每次新开的终端都要先做）
. "C:\Users\Abner\Documents\deepseek-harness\default-workspace\tools\env.ps1"

cd NextPerf
cmd /c build.bat                              # 构建
cmd /c tests\build_hosts.bat                  # 构建测试宿主（删掉 host_run.exe 会导致 3 项假失败）
python tests\run_all.py --skip-build          # 回归（SUITE=0 为通过）
python tests\struct_check.py                  # 结构体镜像一致性
```

**⚠️ 构建前先确认 `dist\NextPerf.exe` 没被占用**（用户可能正开着 NextPerf）。
**绝对不要为了解开占用去杀用户的进程**（`Get-Process NextPerf | Stop-Process`）。
被占用时只报告，让用户自己关。

---

## 5. 血泪教训（写在最显眼处，避免重复踩）

这些都是**真实发生过**的，不是理论：

1. **跨进程比较时钟不可靠。** 钩子写的 `GetTickCount64()` 时间戳，主程序拿自己
   的时钟去减，会算出负数或天文数字。凡是"钩子还活着吗"这类判断，
   **一律用「帧计数是否增长」**，不要用时间戳差。
   （曾导致「桌面 HUD 不跟随」查了很久。）
2. **改共享内存结构体 = 改契约。** 必须同步 `tests/verify_inject.py` 的 ctypes
   镜像并跑 `struct_check.py`。`NPConfig` 加字段要复用 `reserved[]`，
   **保持 `sizeof` 不变**（共享内存用 `version == sizeof(...)` 自校验）。
3. **加了配置项就要加存取。** `learnedAutoHook` 曾因漏了 `settings.cpp`
   存取而"重启就丢"；`bgColor` 曾"只有写没有读"。加完请跑一遍
   字段与 `settings.cpp` 的比对。
4. **加 UI 控件行必须同步改 `kWinH`。** 否则底部按钮被挤出窗口
   （「退出监视」按钮就这样消失过一次）。
5. **两个控件画在同一矩形上，点击会被先注册的吃掉。** `Add()` 里现在有
   重叠检测（只对同类型控件报警）。
6. **绝不能在 `PresentCommon` 中间提前 `return`。** 尾部有一整套收尾状态
   （`gInPresent` / `gFrameStarted` / 时间戳…），中断它会让叠加与时间戳
   **永久失效**。要"跳过"就只跳过更新。
7. **卸载必须干净，两条路径都要还原。** `RestoreAllHooks()` 是幂等的，
   `SelfUnloadNow` 和 `DllMain(DLL_PROCESS_DETACH)` 都要调；
   `AddVectoredExceptionHandler` 注册的处理器**必须注销**
   （否则进程级链表里留下野指针 → 下次注入崩）。
8. **不要打开目标的进程去"判断它是不是游戏"。** 商店/UWP 进程
   `OpenProcess` 可能失败（实测把商店版生化危机8 误判成"不是游戏"），
   枚举受保护进程的模块还可能把它搞崩。要用**窗口属性**或
   **系统级进程快照**（`CreateToolhelp32Snapshot` 不需要打开目标）。
9. **不要用启发式猜"是不是游戏"。** 窗口化游戏与普通窗口在窗口属性上
   没有区别。主流工具（RTSS/Steam/Game Bar/Playnite）**清一色靠数据库**。
   本项目因此**移除了**靠特征猜的自动注入，只认**可信名单**。
10. **改显示口径要三思。** Low 帧曾因擅自换成"68 秒窗口平均"而"卡顿后钉住
    70 秒"。**当前实际用的是 Intel PresentMon 动态查询的「滑动窗口百分位」口径**
    （`np_hook.cpp::UpdateTelemetryCommon` 调用 `lowPercentileFps(99.0/99.9, n1s)`，
    线性插值 + 倒数指标排名反转；管理员运行时由 ETW 经 `NPConfig.reserved[]` 中继 1s/3s 窗口值）；
    `lowWindowed` **已退休**，只留在 `np_stats.h` 作历史对照。
    **改口径前先读代码**，并同步 `COMMON.md`、`DATA-STRUCTS.md` 与 `README.md`。

11. **★★ 「先加诊断再改行为」—— 一行日志推翻了我推演三轮的解释（2026-10-11）**

    **这段保留下来，因为它同时示范了错误做法和正确做法。**

    *错误做法*：我看到「模拟器 1113 帧、钩子只记账 241 帧、P50 正好翻倍」，
    就推演出「帧内合并吃掉真帧 + 正反馈（均值变大→阈值变大→吃更多）」，
    还把它当结论**写进了本文件**。那是**错的**。

    *正确做法*：在合并判定处加一条 5 秒一次的诊断日志，把输入与判定一起打出来：

        frame merge: presents=301 rawMean=16.67ms thresh=10.00ms merged=0 frames=300
        frame merge: presents=300 rawMean=16.67ms thresh=10.00ms merged=0 frames=600

    **`merged=0`（一帧都没合并）、`presents≈frames`（每帧恰好一次 Present）、
    `rawMean=16.67 / thresh=10.00`（全部正确）** —— 结论直接被否掉。
    重跑对照后：**P50 偏差 3%、平均偏差 0%**，之前那个坏结果**根本没复现**。

    **留下的规矩**：
    * 看到"数字不对"时，**先去把输入和中间量打出来**，不要靠读代码推演；
    * **推演出来的机制不能直接写进文档当结论** —— 必须先用观测证实；
    * 加诊断日志的成本极低（几行），而错误结论一旦进文档，
      会误导后来者（包括上下文被压缩后的自己）。

12. **★ 「平均值对」不代表「分布对」**（同一次调查的副产物）
    `tests/verify_framedata.py` 比的是平均值，一度显示 0.4% 偏差、看起来很好；
    但**分布**（P50/P99/总帧数）一测就露出问题。
    以后凡是验证时间序列类指标，**必须同时比分布与样本数**，不能只看平均值。

13. **★ 不要用"真值也这样"来放过一个 bug**
    调查中我发现模拟器真值的 1% Low 也只有 47 fps，一度判断"可能不是 bug"。
    那是错的：真值 47 是这台机器的真实抖动，而钩子把它放大成了 26~43 ——
    **两者不是一回事**。看到"真值也不好看"时，要接着比**偏差**，而不是就此收手。

14. **★ 用远低于分辨率的负载去验证测量值，等于没验证（2026-10-11）**

    模拟器对照显示钩子的 GPU 帧时间是 0，我据此把它当成产品缺陷写进了 README。
    **用户实测确认它是对的。**

    原因：模拟器每帧 GPU 工作量只有约 **0.005ms**（单三角形 + 清屏），
    **低于这条测量路径的分辨率** —— 读 0 是**预期的**。

    这和教训 #11（用不触发某段代码的目标验证那段代码）是同一类错误：
    **验证手段必须落在被测对象的有效量程/触发条件之内**，
    否则"测不出来"会被误读成"东西坏了"。

---

## 6. 文档维护约定

* **本文（CATALOG.md）**：新文件、新功能、新文档都要在这里登记。
* **功能文档**：改哪个功能就改哪份文档，实现逻辑与"为什么"要写进去。
* **`DATA-STRUCTS.md`**：动字段必须改。
* **`LOGGING.md`**：改日志格式必须改。
* **`SIMULATOR.md`**：改模拟器接口必须改。
* **`CHANGELOG.md`（仓库根）**：**发布打 tag 时**把「未发布」段挪成该版本段落；
  **`main` 上出现未发布的提交时**（哪怕只是文档）也要在「未发布」段登记一行。
  未发布段落**必须**写明「未经用户完整测试」——本项目发布版的要求是用户在自己机器上
  完整测过，`main` 领先不等于可以发布。
* **README 的「已知问题」**：修好了要把条目移走或标注，不要留着过期信息
  （曾长期写着"1% Low 不准"，其实早已修好）。
* **`CLEANUP.md`（本机环境台账）**：**只要在会话工作区之外装了 / 改了 / 删了任何东西**
  （pip·uv 包、SDK、目录、文件、环境变量、系统功能……），就必须在它的台账里记一行 ——
  **用户项目收尾时要照着它清电脑**。删掉之后**不要删记录**，把状态改成「已删」并写清核实方式。
* **不再写行号**（2026-10-11 起）：引用代码一律用**函数名 / 变量名 / 日志文本**。
  `HOOK.md` 与 `HOOK-FUNCTIONS.md` 里的 423 + 284 处行号已全部清掉（见 `TODO.md` 的 D1），
  原因是它们对照的快照早已过期、每改一次就漂一次。行数只作量级参考，别当准数。
* 文档用**中文**写，技术名词保留英文。**不写行号**（见上），改了代码记得同步。
