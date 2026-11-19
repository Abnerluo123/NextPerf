# NextPerf —— 游戏性能计数器

> **一个面向"调画质"场景的 Windows 性能计数器。**
> 除了 FPS / 占用率 / 温度 / 显存这些常规项，它还想回答两个市面工具大多答不了的问题：
> **「这次掉帧到底是 GPU 还是 CPU」**、**「光追和 DLSS 到底吃掉了多少时间」**。
>
> ✨ 本轮已完成**单文件 U 盘便携化**（`NextPerf.exe` 内嵌 64/32 位钩子载荷，复制单文件即插即用）、**高还原 Apple Metal Performance HUD 紧凑高密度布局**（2 行设备顶栏 + 解耦对齐核心 4 行 + 10 格双通道示波器 + 多合一指标合流行 + 4 套 Apple 材质）、**Apple / macOS Dark 统一风格 `900×580` 紧凑 Direct2D 控制台**（带实时 `1:1` HUD 预览）以及全链路稳定性加固（`ResizeBuffers`/`ResizeBuffers1`、`NpDispatch`/`NpBuildAS` RT/AI 追踪、非侵入式 Vulkan IAT、`Ctrl+Shift+O/B/P/R/L` 热键与真值对照软件 `NextPerfBench.exe`）。详情见 [CHANGELOG](CHANGELOG.md) 与 [`docs/TODO.md`](docs/TODO.md)（main 分支改动 ⚠️ **未经用户完整测试**）。

---

## 它是怎么工作的

```
┌──────────────────────────────────────────┐
│  NextPerf.exe（主程序）                   │
│  传感器采集 · 注入器 · 桌面叠加 · 自绘界面 │
└───────────────┬──────────────────────────┘
                │ 共享内存（三个块，见下）
┌───────────────┴──────────────────────────┐
│  Config（双向）  Sensors（主→钩子）        │
│  Telemetry（钩子→主，**每个进程一份**）    │
└───────────────┬──────────────────────────┘
                │ CreateRemoteThread + LoadLibraryW
┌───────────────┴──────────────────────────┐
│  NextPerfHook.dll（注入游戏进程）          │
│  挂钩 Present · 采集帧数据 · 在游戏里画面板 │
└──────────────────────────────────────────┘
```

* **不注入**也能用：NVML / NVAPI / PDH / WMI 读硬件指标，在**桌面叠加窗口**上显示（支持屏幕四角摆放）。
* **注入**之后：挂钩 **DXGI（含 `Present`/`ResizeBuffers`）/ D3D11 / D3D12** 采集真实帧数据并把 HUD 画进游戏画面（独占全屏下亦可显示）；**Vulkan** 游戏经非侵入式 IAT 挂钩采集帧率/Low 帧并由桌面 HUD 自动衔接显示。
* **全局热键与跑分记录**：`Ctrl+Shift+O` 一键显隐叠加层、`Ctrl+Shift+B` 随时启停逐帧 Benchmark CSV 录制（写入 `%TEMP%\NextPerf_Bench_*.csv`）。
* **2D/3D 真值对照软件（`dist\NextPerfBench.exe`）**：编译同源产出的独立基准窗体（DX11/DX12，窗口标题栏实时显示 120 帧真值对比指标），配套自动化脚本 `tests/verify_ground_truth.py`。
* **32 位游戏**会自动换成 32 位钩子（走 WoW64 注入）。
* **微软商店 / Xbox 应用（UWP）游戏**也能注入（会往下找一层拿真实进程）。

---

## 本次更新（v1.6）—— 一次「打地基」的版本

> 这一版**几乎没有新增用户可见的功能**。它做的是**让这个项目从"只能靠人试"
> 变成"能自己验证自己"** —— 以及在这个过程中**抓到并修掉了一批真 bug**。

### 1. 完整的代码文档体系（新增 `docs/` 11 份文档）

以前这个项目只有代码和零散笔记，**改一处很容易忘掉另一处**（本项目已经因此吃过
好几次亏：改了配置项忘了加存取导致开关重启就丢、加了界面控件忘了改窗口高度导致
按钮被挤出窗口）。现在有了：

| 文档 | 内容 |
|---|---|
| **`docs/CATALOG.md`** | **总目录**：架构、功能定位表、文件清单、**10 条血泪教训** |
| `docs/HOOK.md` + `HOOK-FUNCTIONS.md` | 钩子层全部功能与实现逻辑（含 **63 条坑**） |
| `docs/APP.md` + `APP-FUNCTIONS.md` | 主程序层（注入器 / UI / 托盘 / 配置） |
| `docs/COMMON.md` | 统计与 Low 帧算法、面板渲染 |
| **`docs/DATA-STRUCTS.md`** | **共享内存逐字段数据契约**（谁写、谁读、能不能持久化） |
| `docs/SENSORS.md` + `SENSORS-FUNCTIONS.md` | 传感器与 ETW（含实证的坑） |
| `docs/LOGGING.md` | 日志规范与自检约定 |
| `docs/SIMULATOR.md` | 测试用模拟游戏说明 |

**目录开篇第一条就是硬性规矩**：**每次改代码必须同步更新文档**。
这不是形式主义 —— 理由写在里面了。

### 2. 日志与自检

* **日志规范**（`docs/LOGGING.md`）：两种日志各自的格式、位置、
  **以及"哪一端是最新行"**（主程序在末尾、钩子在开头 —— 这是一个很容易搞错、
  而且会直接导致看错日志的细节）。
* **机器可断言的自检**：关键不变量现在会打成
  `SELFCHECK <名字> = OK|FAIL`，测试脚本可以直接 grep：
  * `SELFCHECK unloadClean` —— 卸载时所有钩子补丁必须全部还原
    （有残留会让下次注入读到上一次的钩子 → 无限递归 → 栈溢出）
  * `SELFCHECK fpsLowLeAvg` —— 1% Low 不可能高于平均帧率（定义如此）
* **崩溃上下文**：钩子遇到异常时除了报出错地址，还会打出
  `inPresent / heapSwaps / frames / hookFlags` 等状态 ——
  **用来区分"崩在 Present 路径里"还是"崩在交换链重建窗口期"**。

### 3. 工作区内的模拟游戏（新增 `tests/sim/` 与 `tests/simvk/`）

**这一版最有价值的基建。** 以前验证任何结论都要真人开游戏、复现、截图、贴日志；
而且像「改分辨率」「切窗口」这类测试**会让用户的游戏闪退**，在用户机器上反复试
是不负责任的。现在有两个模拟游戏进程：

| 产物 | 后端 | 能力 |
|---|---|---|
| `tests/sim/sim.exe` | **DX11 + DX12** | 锁帧 / 垂直同步 / 分辨率切换 / 窗口状态切换 / CPU 与 GPU 负载注入 / **逐帧自报真实帧数据** |
| `tests/simvk/simvk.exe` | **Vulkan** | 同上（无 Vulkan SDK 环境下手写声明实现） |

**它们自己是渲染方，所以自己知道每一帧的真实情况**（帧间隔、CPU 耗时、
GPU 时间戳、Present 阻塞、交换链重建次数），并逐帧打成 JSON。
于是第一次可以问一个硬问题：**钩子采到的数字，和真实数字对得上吗？**

**首次对照结果**（DX11，60fps 垂直同步）：

| 指标 | 模拟器（真值） | 钩子 | 偏差 |
|---|---|---|---|
| 帧间隔 ms | 16.670 | 16.735 | **0.4%** |
| Present 阻塞 ms | 15.914 | 15.877 | **-0.2%** |
| CPU 帧时间 ms | 15.927 | 16.670 | 4.7%（口径差异） |
| GPU 帧时间 ms | 16.289 | **0.000** | **-100%** ⚠️ 见下（**已结案：模拟器负载太低，不是产品 bug**） |

> **结论：钩子的核心帧数据是准的**（帧间隔与 Present 阻塞偏差都在 1% 以内，
> 这两个正是帧时间 / Low 帧的基础量）。
> **GPU 帧时间读到 0 已结案**：模拟器每帧 GPU 工作量只有约 **0.005ms**，低于测量分辨率，
> 读 0 是**预期**的。用户后来在真机上确认这个指标是对的（见下面「已知问题 · 指标不准」表）。
> 教训写进 `CATALOG.md` 教训 14：**用工作量远低于测量分辨率的负载去验证一个测量值，等于没验证**。

**新增的测试脚本**：
* `tests/verify_framedata.py` —— 上面那张对照表的生成器
* `tests/sim_stress.py` —— 复现「改分辨率 / 开关垂直同步 / 切窗口」这类
  会触发交换链重建的操作，看崩不崩（支持 `--no-inject` 作对照组）
* `tests/sim/sim_drive.py` —— 模拟器的参考驱动 + 集成自测

### 4. 修掉的真 bug

这一版靠"通读代码 + 模拟器对照"抓到并修掉了这些：

| Bug | 性质 |
|---|---|
| **`NPDefaultConfig()` 越界写 16 字节** | `reserved[]` 被逐步缩小到 `[4]`，而清空循环还写 `i < 8`。后果是**静默**清掉紧邻的 `sensors.magic/version` → 发布的传感器块 `magic=0` 被钩子忽略。已改成跟着 `sizeof` 走 |
| **自动学习时忘了加锁** | `games.push_back` 会让守护线程的迭代器失效 → 随机崩溃。界面那条路径有锁，学习这条漏了 |
| **游戏列表仍用跨进程时钟判活** | 正是那个会算出「42 亿毫秒」的判据。已改为读主循环算好的 `telemetryLive` |
| **AI 引擎两路 100% 失效** | PDH 实例名大小写错了（`engtype_compute` vs 真实 `engtype_Compute`），而 `wcsstr` 大小写敏感 → 命中 0 个 → 永远显示 `-1.0%` |
| **崩溃上下文里 `inPresent` 恒为 0** | 变量只声明从没赋值，比不报还糟（会误导定位） |

**最新迭代（main 未发布，⚠️ 未经用户完整测试，详见 [CHANGELOG](CHANGELOG.md)）**：
* 修复 **ETW** `Start()` 事件过滤栈缓冲区溢出、`SetTargetPid()` 未清 `saw171_`/事件名/直方图、`OnEvent()`/`diagText()` 缺锁竞态；
* 修复 **Common** `NPDefaultConfig()` 补齐运行时开关清零 + 编译期 `static_assert` 锁定结构体尺寸、`PanelRenderer::Measure()` 首帧未调 `EnsureFormats()` 返回 `240x110` 兜底及取消勾选不收窄、`np_bitmap` 改用多线程 D2D 工厂；
* 修复 **Hook** `DllMain(DLL_PROCESS_DETACH)` 进程终止期 loader lock 死锁风险、`PresentCommon` `NPHistoryPush` 参数错位与 `setjmp` 恢复、`SelfUnloadNow()` 释放 `gInfoQueue12`/`gReflexDev`/`nvapi` 句柄、`Cfg()`/`Sens()` 布局校验、`Overlay12::OnBackBuffer()` 交换链重建立即感知 + 8 槽 SRV 环 + 4MB 上传环预分配（`sim_stress --api=dx12` 12 步零 SEH 通过）及 `UpdateTelemetryCommon()` `fpsLowLeAvg` 统计序；
* 修复 **App/Sensors/Tests** `detachPid` 从 `games` 循环拆出且允许空名单认领 `--inject` 进程、`TryOpenTelemetry`/`PickTelemetry` 校验 `version == sizeof(NPTelemetry)`、`CleanupAndExit` 注销 `gFgHook`、`WM_DPICHANGED` 重设窗口尺寸、`PollCpu()` 改读 PDH 标称频率、`verify_quit_noinject.py` 配置文件名（`config.json`）、`struct_check.py` 全三结构体校验以及 DX12 模拟器 `Resize()` 资源重建缺陷。
* **2026-11-19 缺陷修复、功能扩展与控制台 Direct2D 重构（main 未发布，⚠️ 未经用户完整测试）**：
  * **传感器/vendor/ETW 修复**：修正 `NV_GPU_DYNAMIC_PSTATES_INFO_EX` 72 字节布局恢复 RTX GPU/FB/VID/BUS 域读取；无 NVML 时回退 NVAPI；`PollCpu` 防无符号整数下溢；PDH 单采集去重与 WMI 缓存退避；DXGI 优选最大非软件独显；HWiNFO 44 字节紧致头与重连；`np_etw` 停会话重填、锁安全关闭与诊断状态保留。
  * **Hook/HUD 稳健性与功能扩展**：补齐 `Overlay11::Draw` 状态保存恢复、`Overlay12` Resize 200ms fence 等待、`NormalizeRtvFormat` 视口裁剪、`PatchSafe` 防递归、`gUnloading`/`calledOrig` D3D12 卸载防护、`fpsLow01` 3 秒窗、`pauseHook` 仅跳渲染，以及 Vulkan IAT `p99`/ETW/曲线/`vkIat=0` 自检；新增 **GPU/CPU 瓶颈指示**（`NP_C_BOTTLENECK`）、系统时钟、曲线折线图、`4` 套场景预设、`4` 套视觉主题、表头/`[min max]` 极值开关、**横向精简条** HUD 及仅后台隐藏模式。
* **单文件 U 盘便携与 Apple Metal HUD / 控制台视觉重构（2026-11-19，main 未发布，⚠️ 未经用户完整测试）**：
  * **单文件即可带上 U 盘使用**：编译期通过 `hook_embed.S` 将 64 位与 32 位钩子 DLL 内嵌进 `dist\NextPerf.exe`（仅复制这一个 `.exe` 到任何电脑/U 盘即可运行与注入，按内容哈希自动释放到 `%TEMP%\NextPerf\hooks\<hash>`；同目录放 `NextPerf.portable` 或 `config.json` 时配置也跟随 U 盘）。
  * **还原 Apple Metal Performance HUD 排版**：2 行设备顶栏（显卡名/`[分辨率]`、`缩放 Direct|Composited`/`刷新率`）+ 核心 4 行（`FPS:` / `Pre:` / `GPU:` / `Mem:` + 右侧 `[min max]` / `[VRAM]`）+ 10 格双通道示波器图，默认去掉多行散乱小图，并收敛为 4 套克制 Apple 材质（`Metal 黑` / `液态暗灰` / `浅雾透明` / `极简纯字`）。
  * **Apple / macOS Dark 统一控制台 UI**：控制台（`ui.cpp`）与 HUD 采用一致的中性深灰层级与 Apple Blue/Green/Red/Orange 系统语义色，改用分段控制器、macOS 滑块开关、分组细线列表与中性画布实时 HUD 预览，同时保留全功能四标签页（概览与游戏 / HUD 外观 / 监控指标 / 系统与诊断）、5 组全局热键（`Ctrl+Shift+O/B/P/R/L`）、每显示器 DPI、真实 `frames[]` 分位数 CSV `# SUMMARY` 与诊断快照剪贴板复制。

### 5. 关于「改分辨率 / 切窗口闪退」的调查（结论已更新）

早期怀疑是「没有挂钩 `ResizeBuffers`」导致后台缓冲引用没释放。
**后续在可复现靶子上证伪了**：崩溃发生在第一次交换链重建之前，与 `ResizeBuffers` 无关。
完整的调查与排除清单见下面的「已知问题 · 闪退」一节。

## 已知问题（必读）

> 📋 **完整清单（含"哪些数据读不到、需要什么条件、哪些是死代码、哪些口径还没统一"）在
> [`docs/WHY-VSYNC.md`](docs/WHY-VSYNC.md) —— **给玩家看的科普**：为什么"锁帧 + 垂直同步"决定流畅度
> （实测：平均 105fps 反而比 稳 60fps 更卡），以及怎么用面板的几个数判断自己顺不顺。

> [`docs/TODO.md`](docs/TODO.md)** —— 那份是**状态唯一权威来源**，本节只保留结论与细节。
> 两处说法不一致时，以 `docs/TODO.md` 为准，并把两处都改掉。

### 指标不准 / 不可用

| 指标 | 状态 | 说明 |
|---|---|---|
| **GPU 帧时间** | ✅ 可用 | 用户实测确认正确。<br>（曾一度怀疑它有问题：模拟器对照读到 0 —— 那是**模拟器负载太低**造成的，<br>它每帧 GPU 工作量只有约 0.005ms，低于测量分辨率，读 0 是预期的。）|
| **帧时间** | ✅ 可用 | = 相邻两次 Present 的间隔（`msBetweenPresents`）。**每一次真实 Present 都记账**，不做帧内合并<br>（合并曾两次被判定为回归并回退）。模拟器对照：平均值偏差 0%、P50 偏差 3% |
| **CPU 帧时间** | ✅ 可用 | 显示 **`cpuBusy`**（= 上一帧 Present 返回 → 本帧 Present 开始，即 CPU 真正干活的时间）。<br>**不含排队时间** —— 这一行的用途是判断 CPU 是不是瓶颈，而排队（垂直同步等待）不是 CPU 在干活。<br>⚠️ 开低延迟（Reflex）时该口径仍会受调度影响，判断 CPU 瓶颈请先关低延迟 |
| **1% / 0.1% Low** | ✅ 可用 | **照 Intel PresentMon 的口径**：在**滑动窗口内直接取百分位**（1% Low = 窗口内帧时间的 P99 取倒数；0.1% Low 窗口 3 秒）。<br>**为什么不用严格百分位**：真实游戏会把一帧拆成多次 Present（实测 8.4/24.9ms 交替、平均恰好 16.65），<br>严格百分位会落在 24.9ms 那一半上 → 显示 40 多，与体感不符。<br>显示时**向下取整**（59.6 → 59，避免和「满帧 60」混淆） |
| **RT Core 负载** | 🔨 已接通·待真机 DXR 复验 | 已接通 `DispatchRays` 与 `BuildRaytracingAccelerationStructure` (`TS_RT_BASE`) 时间戳实测 + HWiNFO 硬件读数 |
| **Tensor / AI 负载** | 🔨 已接通·待真机 DLSS 复验 | 已修复 `NpDispatch` 多槽 `TS_AI_BASE` 区间采集（`sim_dx12` 已实测出数）并增加 PDH `engOfa`/计算引擎回退 |
| **Vulkan 帧数据** | 🔨 已实现·待真机 Vulkan 游戏复验 | 通过非侵入式 IAT 挂钩采集 `vkQueuePresentKHR` 帧率/Low 帧，桌面叠加自动衔接显示 |

### 闪退 —— **已修复（2026-10-11 用户实测确认）**

**现象**：D3D12 游戏可能闪退，崩溃点固定在 NVIDIA 用户态驱动的一个偏移上
（`nvwgf2umx.dll+0x330124`）。

> ⚠️ **2026-10-11 新证据：它不只发生在"反复切换设置"时。** 用户实测：自动注入进 re8 后
> **正常游玩约 8 秒**就崩（`frames=466`、`heapSwaps=0`、没改过分辨率），签名与之前完全一致：
> ```
> SEH: code=0xc0000005 at nvwgf2umx.dll+0x330124
> SEH ctx: api=12 inPresent=1 presentHooked=1 frames=466 dev11=0 dev12=1 heapSwaps=0
> ```
> `inPresent=1`，而我们的 VEH 在调用**原始 Present** 期间是**故意解除武装**的
> （`gJmpArmed=false`，见 `PresentCommon`）——所以这个访问违例发生在**驱动内部**，我们抓不到。
> ✅ **根因已定位并修复（v1.6.4）**：历史二分显示崩溃需要「绑定我们的 RTV」与「上传那一半」
> 同时存在，而把 `CopyTextureRegion` 去掉**仍然崩** —— 于是上传那一半里剩下的**唯一每帧动作**
> 就是**每帧新建一个约 300KB 的 UPLOAD 资源**。现已改成**常驻上传缓冲环**（4 个，按 fence 复用，
> 见 `np_draw.h`），把这个动作从每帧路径上删掉，顺带省掉约 18MB/s 的分配抖动。
>
> **用户实测确认**：连续 **2755 帧 / 60fps ≈ 53 秒**，期间还含**一次交换链重建**（`heapSwaps=1`），
> **零 `SEH`、零闪烁**；而修复前同一场景 **466 帧（约 8 秒）就崩**。
>
> ⚠️ 观测窗口是**一次约 1 分钟的正常游玩**，不等于长期证伪。若将来在别处复现，继续往下走：
> 装 Windows 的「图形工具」可选功能
> （`DISM /Online /Add-Capability /CapabilityName:Tools.Graphics.DirectX~~~~0.0.1.0`，**需要管理员**），
> D3D12 验证层会把"哪一次调用用错了"直接写进 `NextPerfHook.log`（转储代码早已在钩子里）。
> 注意：验证层必须在**游戏创建设备之前**启用才对该设备有效 → 诊断时要**尽早注入**
> （与现在的 10 秒延迟冲突），需要单独一轮诊断构建。

**已经查到的**（全部有工作区内的可复现实验，见 `tests/_bisect.py`）：

* 闪退**可以在工作区内复现**：`python tests\sim_stress.py --api=dx12`，约 250 帧必崩，
  崩溃偏移与真游戏日志一致 —— 不需要真人开游戏就能验证。
* **与 `ResizeBuffers` 无关**。早期的怀疑（没挂钩它 -> 引用没释放 -> 重建失败）被证伪：
  在可复现的靶子上，崩溃发生在**第一次交换链重建之前**，而且 `heapSwaps=0`。
* **不是**设备不一致、设备被移除、RTV 创建失败、描述符堆大小、槽位用尽。
* 触发条件是「**绑定我们自己的 RTV**」与「**上传那时的那一半操作**」**同时存在** ——
  但这两半**单独都不崩**，也排除了 draw、各条资源屏障、视口、
  `CopyTextureRegion`、描述符堆绑定、PSO / 根签名 / 根描述符表。**根因尚未定位。**

**为什么当时没修（历史记录）**：

* 唯一试出来的规避办法是**关掉 D3D12 的游戏内叠加** —— 但那等于砍掉核心功能，
  **不能在 DX12 游戏里叠加，这个工具就没有意义**，所以没有这么做。
* 曾以为触发条件是「反复改分辨率 / 反复开关垂直同步 / 反复切窗口全屏」，于是判断
  「很少有玩家会反复做这些操作，**正常玩不受影响**」。
  ⚠️ **这个判断是错的**：2026-10-11 用户实测**正常游玩约 8 秒就崩**
  （`frames=466`、`heapSwaps=0`、全程没改过设置）。这条错误假设已随手上的修复一并作废。
* 要真正定位，需要的下一步是装上 D3D12 的「图形工具」让验证层直接报错
  （本机没装，`D3D12GetDebugInterface` 不可用）。验证层的接入代码已经写好，
  装上之后崩溃原因会直接出现在 `NextPerfHook.log` 里。

**临时的自我保护**（已经实现并实测有效）：

* **切换窗口 / 无边框 / 全屏**时，若判定交换链刚被重建过，
  会**静默若干帧**不往游戏的命令列表里录命令，避开驱动那个崩溃点。
* ⚠️ 这个静默机制的触发条件是「**RTV 槽位攒满 8 个**」，而槽位是**按后台缓冲对象**逐个累加的
  —— 每次交换链重建都会换掉一整批后台缓冲对象、各占一个新槽，所以它**会**触发，
  但**要等到第二~三次重建之后**：**第一次重建完全没有保护**（而崩溃恰恰发生在重建那段窗口期）。
  **准确说法是「来得太晚」，不是「从未生效」。** 已在 `docs/HOOK.md` 与 `docs/COMMON.md`
  记过；要彻底修好应当**补上 `ResizeBuffers` 钩子**，由它显式触发静默，而不是靠副作用猜。

**如果你遇到了闪退**：先确认用的是 **v1.6.4 或更新**（更早的版本请更新）。
若在 v1.6.4 之后仍然复现，请**先复制一份 `%TEMP%\NextPerfHook.log`** 再报告 ——
**下一步是装 D3D12 验证层**（`DISM /Online /Add-Capability /CapabilityName:Tools.Graphics.DirectX~~~~0.0.1.0`，需管理员），
它会把「哪一次调用用错了」直接写进日志。注意验证层必须在**游戏创建设备之前**启用，
所以要单独做一轮「尽早注入」的诊断构建（与现在的 10 秒延迟冲突）。

**最新状态更新**：`IDXGISwapChain::ResizeBuffers` / `IDXGISwapChain3::ResizeBuffers1` 钩子、`BeforeSwapchainResize()` 200ms fence 等待及 `Overlay11/12::OnResizeBuffers()` 已在 `main` 中实现并通过 `vt_check.py` / `sim_stress.py`（dx11/dx12），真机多游戏场景仍在持续验收中（详见 [`docs/TODO.md`](docs/TODO.md)）。

### 关窗后进程不退 / 状态栏跳变（2026-10-11，生化危机8 DX12 实测）

**现象**：钩子开着 → 点叉关游戏 → 窗口消失，但**后台进程没被清理**；同时 NextPerf 的
状态栏在「钩上 / 没钩上」之间**每 2.5 秒跳一次**。

> ✅ **结论（2026-10-11 用户 A/B 实测）："进程不退"是游戏自己的问题，不是我们的。**
> **完全不启动 NextPerf**、正常玩一次并关窗，进程同样退不干净。
> 钩子侧证据一致：看门狗证明渲染线程**没有**卡在我们的 Present 路径里，而且日志里完全没有
> `DETACH: enter`（进程是被任务管理器结束的，`TerminateProcess` 不走 `DllMain`）。
> 下面那条"退出时反复注入"是**另一件事**、确实是我们自己的 bug，已修 —— 两件事不要混。

* **状态栏跳变这一半已定位、已修**（在 main，未发布）：判定"钩子已死"后，主程序把上一次
  的帧数记成了 0；下一次读到的**冻结帧数**与 0 不相等，就被当成"又长了"→ 复活 → 2.5 秒
  后再判死，无限翻。现在只清计时器、保留冻结帧数，改法见 `CHANGELOG.md`。
* **进程不退：已经抓到一条确凿的、属于我们自己的原因，并做了根因修复**（在 main，未发布）。
  **NextPerf 退出时看护线程还在往游戏里反复注入** —— 实测日志（你点「退出」之后）：
  ```
  21:07:30.150  quit from UI button
  21:07:30.794  inject pid 20124 (re8.exe): remote LoadLibrary returned 0xE2F50000
  21:07:31.618  inject pid 20124 (re8.exe): remote LoadLibrary returned 0xE2F50000
  ```
  每次注入的钩子一启动就看到 `cfg.quit=1` → 立刻自卸载 → 注入标记互斥量消失 →
  看护线程 800ms 后**又注入**，0.8 秒一轮。**往一个正在关停的进程里反复 `LoadLibrary`，
  撞上 loader lock 就是永久挂死** —— 正是"窗口没了、进程退不掉"。
  修法：**先停看护线程、再广播退出**（`CleanupAndExit` 换了顺序），并加两道注入闸门
  （退出中不注入 / 正在请求卸载的 pid 不注入）。
* **看门狗当场给出了答案**（21:16 那次复现）：
  ```
  watchdog: 已有 5.2s 没有 Present 调用 —— 游戏侧停住了，不是卡在我们的 Present 路径里
  ```
  而且**没有**任何"PresentCommon 进入后未返回"的行 → **我们的 Present 路径被排除**。
  随后的 A/B（不启动 NextPerf 也一样退不干净）确认这是**游戏自身的问题** —— 到此结案。

**下次复现怎么定位**（这次钩子日志回绕把现场冲掉了一半，只能靠推断）：

1. 复现后**立刻把 `%TEMP%\NextPerfHook.log` 复制一份**（回绕随时会发生；上限已从 512 KB
   提到 4 MB，读法见 `docs/LOGGING.md` 第 1 节）；
2. 搜 **`watchdog:`** —— 这一行会直接给结论：
   * `watchdog: PresentCommon 进入后 X.Xs 未返回 (stage=…)` → **卡在我们这条路径里**，
     `stage` 还会告诉你卡在哪一段（`0=帧统计段` / `1=叠加绘制段` /
     `2=原始 Present(驱动)` / `3=收尾段`）；
   * `watchdog: 已有 X.Xs 没有 Present 调用` → **游戏侧停住了**，不是卡在我们的 Present 路径里
     （别再动钩子的 Present 代码，去查游戏的关停）。
3. 顺手搜 `DETACH: enter`（判断 DLL 卸载有没有走到、卡没卡在 `RestoreAllHooks`）。

### 自动注入会让游戏闪退（2026-10-11，生化危机8 DX12 实测）

**现象**：让 NextPerf 自动盯 re8 → 游戏**启动即闪退**；但**进到游戏里再手动注入**不闪退。

**判断**：自动注入是"进程一出现就注入"，那时游戏自己**正在初始化 D3D 设备/交换链**，
而我们的钩子在那时还要**额外建一个 D3D11 设备 + 隐藏窗口**去探测交换链 vtable
（`ProbeSwapChainVtable`）—— 两边一起动 driver，代价就是闪退。

**暂时这样处理**（在 main，未发布）：探测到进程后**先等 10 秒**再自动注入
（`injector.cpp` 的 `kAutoInjectDelayMs`，日志里会打 `auto-inject delay: 等 pid=… …`）。
手动注入不受影响 —— 那本来就是"游戏已经在跑"的场景。

> ⚠️ 这**只是时序缓解**，不是根因修复：根因是"为了拿 vtable 必须自建 D3D 设备"这件事在
> 游戏初始化期不安全。要根治得换探测方式，属于下一轮的事。

## 使用

```
dist\NextPerf.exe
```

1. **单文件即插即用**：只需拷贝 `dist\NextPerf.exe`（内嵌 64/32 位钩子 DLL，无需额外下载钩子文件；若希望配置文件随 U 盘携带，在同目录建一个空文件 `NextPerf.portable` 即可）。打开后默认开始监控，桌面角落出现 Apple Metal 风格 HUD 面板；主窗口包含 **概览与游戏 / HUD 外观 / 监控指标 / 系统与诊断** 四标签页及实时 HUD 预览。
2. **想看在游戏里的数据**（帧率、帧延迟，以及独占全屏下的显示），三条路：
   * **游戏已经在跑**：切到游戏窗口后按 `Ctrl+Shift+B`（或切回 NextPerf「概览与游戏」标签页点 **注入前台游戏**）；
   * **想让它自动盯**：在「概览与游戏」添加目标 exe —— ⚠️ 为避开初始化窗口期，自动守护会在探测到进程约 10 秒后动手；**商店 / UWP 游戏**请使用前台注入；
   * **命令行**：`NextPerf.exe --inject <pid>`（退出码 0=成功 / 1=参数错 / 3=失败）。
3. **HUD 与热键**：可在「HUD 外观」切换 `Metal 竖版` / `极简横条`、4 套 Apple 材质、四角锚点及滑杆微调，在「监控指标」切换 `Metal 精简` / `Metal 标准` / `深度调优` / `全部启用` 预设；支持全局热键：
   * `Ctrl+Shift+O`：切换 HUD 显示模式 / `Ctrl+Shift+B`：注入当前前台进程 / `Ctrl+Shift+P`：暂停或恢复采集 / `Ctrl+Shift+R`：开始或停止 CSV Benchmark 录制（带真实帧分位数 `# SUMMARY`） / `Ctrl+Shift+L`：切换横纵向布局。
4. 关掉主窗口默认最小化到系统托盘；托盘右键支持监控启停、打开「诊断」或一键切换 HUD 布局，控制台退出按钮可完全退出并安全通知已注入钩子卸载。

**自检**（不开窗口，跑一遍传感器和探测，结果写 `selftest.txt`）：

```
dist\NextPerf.exe --selftest
```

---

## 开发

```powershell
# 构建（需要 zig 0.16.0；本项目不用 MSVC / MinGW）
cd NextPerf
cmd /c build.bat

# 构建测试宿主
cmd /c tests\build_hosts.bat
cmd /c tests\sim\build_sim.bat        # 模拟游戏（DX11/DX12）
cmd /c tests\simvk\build_simvk.bat    # 模拟游戏（Vulkan）

# 回归
python tests\run_all.py --skip-build  # SUITE=0 为通过
python tests\struct_check.py          # 校验共享内存结构体镜像一致
python tests\sim\sim_drive.py         # 模拟器自带的集成自测
python tests\verify_framedata.py      # 用模拟器的真值校验钩子帧数据
```

**⚠️ 构建前先确认 `dist\NextPerf.exe` 没被占用**（你可能正开着 NextPerf）。
**不要为了解开占用去杀用户的进程。**

### 代码结构

```
src/app/        主程序：主循环、注入器、界面、托盘、配置
src/hook/       注入到游戏里的钩子：探测、挂钩、采集、画面板、卸载
src/common/     两层共用的数据契约与算法（**改结构体要动三处**）
src/sensors/    传感器：NVML / NVAPI / ADL / HWiNFO / PDH / WMI
src/etw/        ETW（DxgKrnl Present 事件）
tests/          测试脚本 + 两个模拟游戏
docs/           **代码文档（改代码请同步更新）**
```

**开始阅读代码前，请先看 [`docs/CATALOG.md`](docs/CATALOG.md)** ——
它是总目录，包含功能定位表、文件清单和一份"血泪教训"清单（
例如"绝不能在 Present 处理函数中途 return"、"不要跨进程比较时钟"）。

---

## 测试环境

| 项 | 值 |
|---|---|
| 操作系统 | Windows 11 家庭版中文版 · `26H2` · Build `26300.9457` |
| CPU | Intel Core Ultra 9 275HX（24 逻辑处理器） |
| 内存 | 32 GB |
| 显示适配器 | **混合显卡**：Intel 核显 + NVIDIA RTX 5080 Laptop（16 GB） |
| 显卡驱动 | **617.42 —— Studio 驱动** |
| 显示 | 内建单屏 3840×2400 @ 60Hz |
| 编译器 | zig 0.16.0（`zig c++ -target x86_64-windows-gnu` / `x86-windows-gnu`），**无 MSVC / MinGW / Vulkan SDK** |

实测游戏：《生化危机8》（微软商店版，DX12）、《漫威蜘蛛侠》（DX12）。

---

## 参考过的开源项目

* **[Intel PresentMon](https://github.com/GameTechDev/PresentMon)** ——
  **CPU Busy / CPU Wait 的拆分公式来自它**（MetricsCalculatorCpuGpu.cpp）：
    msCPUBusy = 上一帧 Present 返回 → 本帧 Present 开始；msCPUWait = 本帧在 Present 内部停留的时长。
    本项目的「CPU 帧时间」取其中的 **Busy**（判断 CPU 瓶颈用，不含排队）。
    ⚠️ Low 帧**没有**采用它的严格百分位口径 —— 真实游戏会把一帧拆成多次 Present，
    严格百分位会落在假的短帧/长帧上；本项目用**窗口平均（1000ms）**更贴近体感。
  CPU Busy / Wait 的拆分公式也来自它。
* **NVAPI / NVML** —— 显存、频率、利用率；接口 ID 与结构体定义以
  PresentMon 里的 `nvapi_interface_table.h` / `nvapi.h` 为准（那是权威列表）。

---

## 许可与免责

本项目为个人学习与自用工具，**按现状提供，不保证任何指标准确**。
注入行为可能被反作弊判定为异常，**请自行评估风险**；
**本项目不会、也不打算尝试绕过任何反作弊机制**。
