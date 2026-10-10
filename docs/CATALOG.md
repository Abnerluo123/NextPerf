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

**NextPerf** 是一个 Windows 游戏性能计数器（C++20，无 MSVC，用 **zig 0.16.0**
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
                    │  钩 Present/ResizeBuffers · 采集 · 画面板 │
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
| 64 位注入 | `src/app/injector.cpp` | `InjectInto`、`IsInjected` |
| **32 位游戏注入（WoW64）** | `src/app/injector.cpp` | `NpExportAddr32`、`NpResolve32LoadLibraryW`、`NpReadRemote` |
| 自动注入已添加的游戏 | `src/app/injector.cpp` | `InjectorScanNow`（**按 exe 名匹配**） |
| 请求钩子自卸载 | `src/app/injector.cpp` | `RequestHookDetach`、`ForgetGameAt`、`ForgetLearned` |
| 注入的钩子入口 | `src/hook/dllmain.cpp`、`src/hook/np_hook.cpp` | `DllMain`、worker 线程 |
| 命令行注入 | `src/app/main.cpp` | `RunInjectCli`（`--inject <pid>`） |

### 2.2 挂钩（hook）

| 功能 | 文件 | 关键函数 |
|---|---|---|
| 交换链 vtable 探测 | `src/hook/np_hook.cpp` | `ProbeSwapChainVtable`（自造临时交换链 + SEH 保护 + `gProbePoisoned`） |
| DXGI 工厂钩子 | `src/hook/np_hook.cpp` | `InstallDxgi`（`CreateSwapChain` / `ForCreateSwapChainForHwnd` / `ForComposition`） |
| 交换链 vtable 钩子 | `src/hook/np_hook.cpp` | `PatchSwapChainVtable`、`Patch`、`SwapVtFor` |
| Present / Present1 | `src/hook/np_hook.cpp` | `PresentCommon`（**核心**） |
| ResizeBuffers | `src/hook/np_hook.cpp` | （见 `HOOK.md`） |
| D3D12 命令队列/命令列表 | `src/hook/np_hook.cpp` | `InstallD3D12`（注意函数实现在 **`D3D12Core.dll`**） |
| **还原所有补丁（幂等）** | `src/hook/np_hook.cpp` | `RestoreAllHooks`（**两条卸载路径都必须调**） |
| 干净卸载 | `src/hook/np_hook.cpp` | `SelfUnloadNow`、`NpHookDetach`、`detachPid` 机制 |

### 2.3 帧数据采集与算法

| 功能 | 文件 | 关键函数 |
|---|---|---|
| 帧时间采集 | `src/hook/np_hook.cpp` | `PresentCommon` 里的 record 段 |
| **帧内重复 Present 的合并** | `src/hook/np_hook.cpp` | 基准取自 `gRawStats.meanMs(240)`，避免正反馈 |
| **Low 帧（Intel 权威口径）** | `src/common/np_stats.h` | `lowPercentileFps`（1 秒滑动窗口 + 对 FPS 取 P1） |
| 环形缓冲与百分位 | `src/common/np_stats.h` | `FrameStats`、`push`、`recent`、`percentileMs` |
| **CPU Busy / Wait 拆分** | `src/hook/np_hook.cpp` | Present 返回后**同一次减法**导出三个值 |
| GPU 帧时间 | `src/sensors/np_sensors.cpp` | PDH `\GPU Engine(pid_*)\Running Time` |
| 低延迟状态（Reflex） | `src/hook/np_reflex.h` | `ReflexReader`、`NvAPI_D3D_GetSleepStatus` |

### 2.4 叠加层绘制

| 功能 | 文件 | 关键函数 |
|---|---|---|
| 游戏内叠加（D3D11） | `src/hook/np_draw.cpp` | `Overlay11::Draw`（**RTV 每帧新建、本帧释放**） |
| 游戏内叠加（D3D12） | `src/hook/np_draw.cpp` | `Overlay12::Record`（**描述符堆"换堆"+ fence 延迟回收**） |
| 面板渲染（共用） | `src/common/np_panel.cpp` | `PanelRenderer`（字号/缩放/列宽锁定） |
| 面板行组装 | `src/common/np_build.cpp` | 分组、指标、颜色、开关 |
| 桌面叠加窗口 | `src/app/overlay.cpp` | `OverlaySetVisible`、`OverlayUpdate`（分层窗口 + 鼠标穿透） |

### 2.5 主程序与界面

| 功能 | 文件 | 关键函数 |
|---|---|---|
| 主循环 / 托盘 | `src/app/main.cpp` | 窗口过程、`NP_TRAY_*` 消息 |
| 自绘界面 | `src/app/ui.cpp` | `Add`、`Layout`、`W_CYCLE`、`A_*` 动作分发、**控件重叠检测** |
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

| 文件 | 行数 | 职责 |
|---|---|---|
| `main.cpp` | ~1000 | 主循环、托盘、命令分发、前台跟踪、自动学习、陈旧遥测回收 |
| `ui.cpp` | ~813 | 自绘 D2D 界面、控件系统、动作分发 |
| `injector.cpp` | ~587 | 注入（含 WoW64 32 位）、看护扫描、HookControl |
| `settings.cpp` | ~158 | 配置 JSON 存取 |
| `overlay.cpp` | ~111 | 桌面分层叠加窗口 |
| `np_app.h` | ~125 | `AppState` / `GameEntry` 等 |

### 3.2 钩子（`src/hook/`）

| 文件 | 行数 | 职责 |
|---|---|---|
| `np_hook.cpp` | ~2100 | **钩子主体**：探测、挂钩、采集、遥测、卸载 |
| `np_draw.cpp` / `.h` | ~500 / 74 | 游戏内叠加绘制（D3D11 / D3D12 两套） |
| `np_reflex.h` | ~99 | NVAPI Reflex / 低延迟状态 |
| `dllmain.cpp` | 20 | DLL 入口 |
| `np_hook.h` | 5 | 导出声明 |

### 3.3 公共（`src/common/`）

| 文件 | 行数 | 职责 |
|---|---|---|
| `np_common.h` | ~344 | **数据契约**：NPConfig / NPSensors / NPTelemetry / 宏 |
| `np_stats.h` | ~208 | `FrameStats` 环形缓冲、百分位、Low 帧 |
| `np_panel.cpp` / `.h` | ~408 / 89 | 面板渲染器 |
| `np_build.cpp` / `.h` | ~396 / 7 | 面板行组装 |
| `np_json.h` | ~205 | 极简 JSON |
| `np_bitmap.cpp` / `.h` | ~86 / 33 | 位图工具 |

### 3.4 传感器与 ETW

| 文件 | 行数 | 职责 |
|---|---|---|
| `src/sensors/np_sensors.cpp` / `.h` | ~917 / 131 | NVML/PDH/WMI/HWiNFO 等采集与优选 |
| `src/sensors/np_vendor.cpp` / `.h` | ~294 / 216 | NVAPI / ADL 动态加载与结构定义 |
| `src/etw/np_etw.cpp` / `.h` | ~281 / 78 | ETW 会话（DxgKrnl Present） |

### 3.5 测试（`tests/`）

| 文件 | 职责 |
|---|---|
| `run_all.py` | 回归总入口 |
| `verify_inject.py` | 注入端到端 + **ctypes 结构体镜像** |
| `verify_reinject.py` | **重复注入不闪退**（守 VEH / vtable 残留那条链） |
| `verify_x86_inject.py` | 32 位注入 |
| `verify_frontinject.py` / `verify_early.py` / `verify_metrics.py` | 其它注入场景 |
| `struct_check.py` | 校验 ctypes 镜像与 C++ 结构体一致 |
| `merge_algo_check.py` | 帧内合并算法的数值验证 |
| `low_recover_check.py` | Low 帧恢复时间的量化（历史分析） |
| `panel_width_check.py` / `row_metrics_check.py` | 面板布局 |
| `shot_overlay.py` / `shot_ui.py` / `diag.py` / `vt_check.py` / `try_inject.py` | 截图与诊断工具 |
| `host_run.cpp` | **旧的**简易测试宿主（将被 `sim/` 取代） |
| `sim/` | **新的模拟游戏**（DX11/DX12，见 `SIMULATOR.md`） |
| `simvk/` | **新的 Vulkan 模拟游戏**（见 `SIMULATOR.md`） |

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
    70 秒"。最终采用 **Intel PresentMon 权威口径**（1 秒滑动窗口 + 对 FPS 取 P1）
    —— 有源码出处，不要凭感觉改。

---

## 6. 文档维护约定

* **本文（CATALOG.md）**：新文件、新功能、新文档都要在这里登记。
* **功能文档**：改哪个功能就改哪份文档，实现逻辑与"为什么"要写进去。
* **`DATA-STRUCTS.md`**：动字段必须改。
* **`LOGGING.md`**：改日志格式必须改。
* **`SIMULATOR.md`**：改模拟器接口必须改。
* **README 的「已知问题」**：修好了要把条目移走或标注，不要留着过期信息
  （曾长期写着"1% Low 不准"，其实早已修好）。
* 文档用**中文**写，技术名词保留英文。**行号要真实**，改了代码记得同步。
