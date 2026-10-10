# NextPerf 主程序层（`src/app`）技术文档

> **维护铁律（先读这一段）**
>
> 1. **每次改 `src/app` 下的任何代码，必须同步更新本文档（[`APP.md`](APP.md) 与
>    [`APP-FUNCTIONS.md`](APP-FUNCTIONS.md)）以及总目录 [`CATALOG.md`](CATALOG.md)。**
>    文档写错了比没写更糟 —— 下一个人会按错的说明去改代码。
> 2. 行号会漂移。本文档所有行号基于**当前工作区的文件内容**，各文件行数如下，
>    行数对不上就说明代码已经改过，请先把受影响的条目核对一遍再继续：
>
>    | 文件 | 文档撰写时的行数 |
>    | --- | --- |
>    | `src/app/main.cpp` | 1226 |
>    | `src/app/ui.cpp` | 939 |
>    | `src/app/injector.cpp` | 728 |
>    | `src/app/settings.cpp` | 184 |
>    | `src/app/overlay.cpp` | 135 |
>    | `src/app/np_app.h` | 177 |
>
>    （注意：任务书里给的 1000 / 813 / 587 / 158 / 111 行是**过期数字**，
>     实际是上表这些。以 `read` 工具看到的行号为准。）
> 3. 拿不准的结论写「未确认」，**不要编造**。本文档里已经这样标注了几处。
> 4. 本文档偶尔会引用**其它层**的行号（`src/hook`、`src/sensors`、`src/common`）。
>    那些文件不归本层维护、**会随别的改动漂移**（撰写期间 `src/hook/np_hook.cpp`
>    就正在被并行修改，行号已经变过一次）。对不上时以
>    `docs/HOOK.md`、`docs/SENSORS.md`、`docs/DATA-STRUCTS.md` 为准；
>    `src/app` 自己的行号才是本文档负责的范围。

---

## 1. 这一层是干什么的

### 1.1 一句话

`NextPerf.exe` 是**常驻主程序**：它一个进程负责**采传感器 → 写共享内存 → 注入游戏进程 →
读回游戏内钩子写出的遥测 → 画桌面叠加窗口/托盘/设置界面**。
游戏内那块面板是 `NextPerfHook.dll` 画的，本层只负责「喂数据 + 收数据 + 看护它」。

### 1.2 在进程模型里的位置

```
┌──────────────────────────────────────┐                       ┌───────────────────────────────┐
│ NextPerf.exe（本层 src/app）          │      共享内存           │ NextPerfHook[32].dll（src/hook）│
│                                      │                       │                               │
│ SensorHub ──► NPSensors ─────────────┼──► [Config]/[Sensors] ─┼─► 读配置/传感器               │
│ gApp.cfg  ───────────────────────────┼──► [Config](含 detach/  │   决定画什么、何时自卸载        │
│                                      │      pause/quit 指令)  │                               │
│ 面板/图表 ◄── NPTelemetry ◄──────────┼──◄ [Telemetry_<pid>] ──┼── 每帧写帧时间/延迟/RT/Tensor  │
│ UI / 托盘 / 桌面叠加 / 注入器 / ETW   │   （每 PID 一份）      │   Present 钩子 + 游戏内叠加     │
└──────────────────────────────────────┘                       └───────────────────────────────┘
```

三块共享内存是**全部**的进程间通信（`src/common/np_common.h:1-10`）。
这样设计的好处（源码注释原话）：**钩子 DLL 不需要自己再去打开 NVML/NVAPI/WMI，
主程序一个进程负责采集，钩子只负责「画」**；同时注入多个游戏也不会重复初始化厂商 SDK。

### 1.3 文件清单与职责

| 文件 | 职责 |
| --- | --- |
| `main.cpp` | `WinMain`、命令行分发、共享内存创建/发布、主循环（120ms tick）、托盘、前台跟踪、学习逻辑、陈旧遥测回收、清理退出、`--selftest`/`--uismoke`/`--inject` |
| `ui.cpp` | 自绘深色设置窗口：计数器勾选列表（可滚动）、游戏列表、滑杆/循环选择/按钮、叠加预览、全部动作分发 |
| `injector.cpp` | 注入器与进程看护（守护线程 800ms 一轮）、32 位 WoW64 注入、HookControl（卸载请求/移除条目/清空学习名单）、日志与提示、进程信息小工具 |
| `settings.cpp` | `%APPDATA%\NextPerf\config.json` 读写 + 旧配置版本升级 |
| `overlay.cpp` | 桌面分层叠加窗口（`WS_EX_LAYERED` + 鼠标穿透），`UpdateLayeredWindow` 输出 |
| `np_app.h` | `AppState` / `GameEntry` / 托盘消息与命令 ID / 全部跨文件函数声明 / `AppLock` |

### 1.4 边界：这一层不做什么

* **不做 Present/D3D 钩子**（在 `src/hook`），也不画游戏内面板。
* **不做面板排版**：面板内容由 `np::BuildPanelData()`（`src/common/np_build.cpp:199`）
  统一生成，绘制由 `np::PanelRenderer`（`src/common/np_panel.cpp`）完成 ——
  **主程序与钩子共用同一份排版代码，保证桌面叠加与游戏内叠加长得一模一样**。
* **不做传感器驱动的实现**（在 `src/sensors`），本层只调用 `SensorHub`。
* **不与反作弊对抗**：遇到反作弊只检测 + 放弃，见 §9.4。

---

## 2. 进程启动与命令行分发

### 2.1 `WinMain` 启动顺序（`main.cpp:929`）

```
EnableDpiAwareness()                        // 929-932，必须在建窗口之前
NPClearSensors / NPClearTelemetry           // 934-935：把结构体置成「空但合法」（magic/version 已填）
SettingsLoad()                              // 936：加载配置（文件缺失/解析失败就静默保留默认值）
  ├─ --selftest  -> RunSelfTest()           // 938（它自己会再 NPDefaultConfig 一次，见 807）

  ├─ --uismoke   -> RunUiSmoke()            // 939
  └─ --inject    -> RunInjectCli()          // 941（在 GfxInit 之前返回：注入不需要图形）
npb::GfxInit()                              // 943：Direct2D/DirectWrite 工厂
gApp.hub.Init()                             // 948：探测所有传感器数据源
CreateShm()                                 // 949
SettingsSave()                              // 950：立刻落盘一次，保证配置文件存在（哪怕用户没改过）
[启动日志块]                                 // 952-987，见 2.6
UiCreate()                                  // 989
OverlayInit()                               // 990（失败只弹一次 MessageBox，不致命）
InjectorInit()                              // 993：起守护线程
AddTray(UiWindow())                         // 995
gApp.monitoring = true; OverlaySetVisible(true); InjectorScanNow();  // 997-999
while (GetMessageW(...)) { ...120ms tick... } // 1005-1221
CleanupAndExit(); SettingsSave();            // 1223-1224
```

**为什么命令行分支在 `GfxInit` 之前**：`--inject` 是给自动化验证/脚本用的，它不需要窗口、
不需要 D2D；提前返回可以让 CI 在没有桌面会话时也能跑（`main.cpp:940` 注释）。
`--selftest` 会自己 `GfxInit`，`--uismoke` 也会（`main.cpp:857`）。

### 2.2 DPI 感知（`EnableDpiAwareness`，`main.cpp:888`）

先 `GetProcAddress(user32, "SetProcessDpiAwarenessContext")` 动态取，
按 `PER_MONITOR_AWARE_V2`（-4）→ `SYSTEM_AWARE`（-2）→ `SetProcessDPIAware()` 逐级回退。
**为什么动态取**：这样在老的 Windows 上也不会因为缺导出而加载失败；
**为什么必须开**：不开的话整张窗口会被 DWM 拉伸，文字发糊（`main.cpp:887`）。
UI 侧还有一份自己的缩放：`ui.cpp:40-41` 的 `gS = dpi/96` 与 `S(v)` 宏，
在 `UiCreate`（`ui.cpp:887-891`）和 `WM_DPICHANGED`（`ui.cpp:683-692`）里更新。

### 2.3 `--selftest`（`RunSelfTest`，`main.cpp:756`）

不弹窗口，跑一遍「D2D 初始化 → 传感器采集 3 轮 → 面板渲染到位图 → 创建共享内存 → 检查 DLL」，
把结果写到 **`<exeDir>\..\selftest.txt`**（即项目根，故意不塞进 `dist`）。

* 报告同时写三处：文件、`OutputDebugStringA`、`stderr`（`main.cpp:764-774`）。
* **坑（源码注释里明写的）**：不能写成 `DllPath() + "\\..\\selftest.txt"` ——
  `DllPath()` 返回的是 `...\NextPerfHook.dll`，把 dll 当目录再退一级是非法路径，
  `_wfopen` 一直失败，报告只进了 stderr（`main.cpp:758-762`）。
* ⚠ 上面那句 `DllPath();`（`main.cpp:762`）**不是废调用**：它的副作用是把
  `gApp.exeDir` / `gApp.dllPath` 填好，紧接着的 `_wfopen` 用的就是 `gApp.exeDir`。
  **删掉它会得到一个相对路径 `\..\selftest.txt`**（跟着当前工作目录走）。
* **退出码** = 失败项数；且**必须 `ExitProcess(fails)` 硬退出**（`main.cpp:847-851`）：
  这条路径不进主消息循环、也不走 `CleanupAndExit`，直接 `return` 偶尔会把进程留在系统里
  （`--uismoke` 就会），**残留进程会锁住 `dist\NextPerf.exe` 让后续构建失败**。
* `WinMain` 里写成 `return RunSelfTest() ? 2 : 0;`（`main.cpp:938`），实际上 `RunSelfTest`
  从不返回（内部 `ExitProcess`），这行只是给编译器一个交代。

### 2.4 `--uismoke`（`RunUiSmoke`，`main.cpp:855`）

真的创建主窗口 + 叠加窗口，跑 60 轮「PeekMessage 抽干消息队列 → `AppPollSensors` →
`AppPublish` → `OverlayUpdate` → `UiRefresh`」再 `Sleep(30)`（`main.cpp:865-876`），
目的只有一个：**确保 `WM_PAINT` 这条路径不会崩**。结束时同样 `ExitProcess(0)`（`main.cpp:882-883`）。
返回码约定：1 = GfxInit 失败，2 = UiCreate 失败，3 = OverlayInit 失败。

### 2.5 `--inject <pid>`（`RunInjectCli`，`main.cpp:908`）

* 参数解析很土：**从字符串里跳过所有非数字字符**，剩下的当 pid（`main.cpp:910-916`）。
  `strstr(cmdLine, "--inject")` 只做包含匹配（`main.cpp:941`），所以 `--inject 1234` 与
  `--inject=1234` 都能用。
* 退出码：`0` 成功 / `1` 参数不对或进程不存在 / `3` 注入失败（`main.cpp:907` 注释 + 926）。
* 存在意义（`main.cpp:904-906` 注释）：**让注入（尤其是 32 位目标的 WoW64 注入）可以被
  自动化验证**，不必去点界面。

### 2.6 启动日志：一次性写清运行环境（`main.cpp:952-987`）

启动时把 `build`（`BuildStamp()`，取自 exe 文件时间戳）、`exe` 路径、`hook dll` 路径与
**是否存在**、`admin` 是否管理员、日志路径、游戏列表逐条、以及非管理员时的提示
（「若游戏以管理员启动，注入会被拒 err=5」）全部写进 `%TEMP%\NextPerf.log`。

* **为什么**（`main.cpp:952-953` 注释）：用户反馈问题时只要给这一个文件，
  就能排除掉**版本不对、DLL 不存在、权限不够**这些最常见的原因。
* `BuildStamp()` 的由来（`np_app.h:14-18`）：用 exe 文件时间戳而不是 `__DATE__/__TIME__`，
  因为 clang 为了可复现构建会把后者当错误（`-Werror,-Wdate-time`），而且取文件时间本来就更准。
  **踩过的坑**：磁盘上同时存在两份 NextPerf，习惯性打开了旧的那份，于是所有修复看起来都
  「没生效」，白排查一整轮 —— 所以标题栏和启动日志都显示构建时间。
* 顺带在此处：`gApp.isAdmin` 用 `AllocateAndInitializeSid` + `CheckTokenMembership` 判定
  （`main.cpp:958-966`），并尝试 `np::Etw().Start()`（`main.cpp:983-986`，
  失败只写日志，**绝不阻塞程序**）。

---

## 3. 共享内存：创建与发布

### 3.1 三块内存的所有权

| 名字 | 结构体 | 写 | 读 | 备注 |
| --- | --- | --- | --- | --- |
| `Local\NextPerf_Config_v1` | `NPConfig` | 主程序 | 所有钩子 | **同时是主程序→钩子的指令通道**（`quit` / `pauseHook` / `detachPid`） |
| `Local\NextPerf_Sensors_v1` | `NPSensors` | 主程序 | 所有钩子 | 只在主程序采集一次 |
| `Local\NextPerf_Telemetry_v1_<pid>` | `NPTelemetry` | 该 pid 的钩子 | 主程序 | **必须每 PID 一份** |

名字与 `NPTelemetryShmName()` 在 `src/common/np_common.h:22-45`。
`Local\` 前缀是会话命名空间：普通权限即可创建，不会被 UAC 完整性级别拦住。

**为什么 Telemetry 必须按 PID 分开**（`np_common.h:18-21`）：同时注入两个游戏时，
共用一个名字会让两个钩子往同一块内存里互相覆盖，主程序读到的就是**一锅粥**。

> `NP_MUTEX_SENSORS`（`np_common.h:24`）在**整个代码库里没有任何地方使用**，
> 属于遗留常量。写传感器时**没有加锁**，靠的是「整块 memcpy + 读方整块拷贝」的约定
> （读方见 `src/hook/np_hook.cpp:636-642`：先 `c = *gCfg;`、`s = *gSens;` 拷出来再用）。

### 3.2 `CreateShm`（`main.cpp:38`）

`CreateFileMappingW(INVALID_HANDLE_VALUE, ...)` 建页文件支撑的映射 + `MapViewOfFile`。
两个映射各自独立，**只要 `shmCfg` 成功就算成功**（`main.cpp:49`）。
`--selftest` 也会调用它来验证这一环（`main.cpp:833`）。

### 3.3 发布：`AppPublish`（`main.cpp:511`）

```cpp
gApp.cfg.pauseHook = gApp.monitoring ? 0u : 1u;   // 511-515
if (gApp.shmCfg)  { NPConfig c = gApp.cfg; memcpy(gApp.shmCfg, &c, sizeof(NPConfig)); }  // 516-519
if (gApp.shmSens) memcpy(gApp.shmSens, &gApp.sensors, sizeof(NPSensors));                // 520
AppLock lk;                                       // 524-526
TryOpenTelemetry(); PickTelemetry();
```

* **`pauseHook` 的语义（用户报的第三个 bug 的修复）**：点「退出监视」如果只关桌面 HUD，
  游戏内面板照旧在画、数据照旧在读。现在 `pauseHook` 会让钩子**跳过叠加绘制与遥测更新
  （= 停止读取）**，只保留最小心跳（`main.cpp:512-515`，钩子侧 `src/hook/np_hook.cpp:1542-1558`）。
* **为什么 `AppPublish` 要在每个 tick 都无条件调用**（`main.cpp:1017-1023`）：
  原来它只在 `monitoring` 为真时调用，于是用户「退出监视」后**配置再也不发布**，
  钩子永远收不到 `pauseHook`。`AppPublish` 幂等、开销极小（几十字节 memcpy），无条件调用最稳。
* 一个可观察的冗余：monitoring 时每个 tick 会走**两次** `AppPublish`（`main.cpp:1024` 与 `1117`），
  幂等所以无害，改代码时不要误以为只有一处。
* 先写 Config 再写 Sensors，中间没有原子性保证 —— 钩子侧能看到「新配置 + 旧传感器」的瞬间，
  设计上可以接受（配置变化不影响传感器语义）。
* 写到共享内存时用的是**本地副本整块 memcpy**（`NPConfig c = gApp.cfg;`），
  与之对应读方也是整块拷贝 —— 单向撕裂被两侧的「整块」约定压住了。
  至于为什么用本地副本而不是直接 `memcpy(gApp.shmCfg, &gApp.cfg, ...)`：**源码没有写明，未确认**
  （推测与「写的时候 gApp.cfg 可能被同进程其他地方改动」有关）。

### 3.4 每 PID 一份遥测：`telSlots`

`AppState::telSlots`（`np_app.h:79-85`）里每条记录：`{pid, map, view, lastTick}`。

* `TryOpenTelemetry`（`main.cpp:54`）：遍历 `gApp.injected`，**已经开过的跳过**；
  用 `NPTelemetryShmName(pid)` 打开，**只校验 `magic == NP_MAGIC`**，
  然后记下 `lastTick = v->tickMs`，push 进 `telSlots`。
  > 注意：这里**没有校验 `version`（= `sizeof(NPTelemetry)`）**。
  > 布局自校验在钩子→主程序这一路目前**没有真正执行**。见 §17。
* `PickTelemetry`（`main.cpp:81`）：遍历所有 slot，用 `v->tickMs` 当新鲜度，
  **取最大的那一份**整体拷进 `gApp.telemetry`，并记 `gApp.telemetryPid`。
  `lastTick == 0` 时直接放弃（没有任何钩子写过数据）。

**为什么按 `tickMs` 挑而不是按「最后一个注入的」**：同时注入多个游戏时，
用户关心的是**正在前台玩的那个**，而它正好就是数据最新的那个。

### 3.5 `version = sizeof` 自校验：三种结构**不一样**，别记混

| 结构 | `version` 写什么 | 位置 |
| --- | --- | --- |
| `NPSensors` | **`(uint32_t)sizeof(NPSensors)`** | `np_common.h:448`（`NPClearSensors`） |
| `NPTelemetry` | **`(uint32_t)sizeof(NPTelemetry)`** | `np_common.h:474`（`NPClearTelemetry`） |
| `NPConfig` | **`1`（配置 schema 版本号，不是 sizeof）** | `np_common.h:415` |

* 为什么 Sensors/Telemetry 要写 `sizeof`（`np_common.h:305-308`、`443-447`）：
  读方拿它和自己算出来的大小对一下，就能立刻发现**结构体布局对不上**。
  曾经往 `NPTelemetry` 中间插了一个字段而 Python 那边的 `ctypes` 结构体没同步，
  结果读到的全是错位字节（钩子明明工作正常却报「未挂上」）。
  `NPSensors` 原来是 `version = 1`，等于**完全没有自检**，后来补齐成 `sizeof`。
* ⚠ **`version == sizeof` 只能抓「大小变了」，抓不到「字段顺序/宽度换了但总大小不变」**
  （`tests/struct_check.py:1-11` 原话：两边字段总数相同、总大小也相同，只是排列不同，
  所以 `version == sizeof(...)` 那道大小自检**抓不到**，结果读到的全是错位字节 ——
  查了很久）。所以**动过结构体字段顺序/宽度之后，必须跑 `python tests/struct_check.py`**，
  它按字段名、顺序、宽度逐项比对 C++ 头与 Python ctypes 镜像（目前只覆盖 `NPTelemetry`）。
* `NPConfig` 的 `version` 是**配置格式版本**；`size = sizeof(NPConfig)` 才是它的自校验字段
  （`np_common.h:416`）。
* 新增共享内存字段的铁律：**加在结构体末尾**（或复用 `reserved[]`），
  这样 `sizeof` 不变、旧 DLL 不失配。`autoInject` 就是这么加进去的
  （`np_common.h:182-185`：「复用原来的 `reserved[0]` 槽位 —— `NPConfig` 的大小完全不变」）。

### 3.6 释放顺序（`CleanupAndExit`，`main.cpp:718`）

见 §12.3。

---

## 4. 线程模型与主循环节奏

### 4.1 三条执行流

| 执行流 | 周期 | 干什么 |
| --- | --- | --- |
| **主线程**（消息循环） | 120ms tick + 窗口消息 | 前台跟踪、发布共享内存、学习、采传感器、`InjectorTick`、叠加/状态刷新（`main.cpp:1005-1221`） |
| **守护线程**（`WatchThread`） | 800ms（`injector.cpp:644-650`） | `InjectorScanNow()`：扫描进程、按 exe 名匹配、自动注入、清理 `detachPid` |
| **UI 定时器**（`WM_TIMER` id=1） | 250ms（`ui.cpp:673`），`UiRefresh` 内部再节流到 200ms（`ui.cpp:930-937`） | 重渲染预览位图 + `InvalidateRect` |

ETW 监控是**第四条**（`np::EtwMonitor` 自己的消费线程，`src/etw/np_etw.cpp`），
不属于本层实现，本层只调 `SetTargetPid/Snapshot/Start/Stop`。

### 4.2 主循环 120ms tick 的固定顺序（`main.cpp:1012-1217`）

```
now = GetTickCount64(); if (now - lastTick < 120) -> 只派发消息，跳过整块   // 1012-1013
AppTrackForeground()          // 1015 前台窗口（WinEvent 钩子兜底 + 前台进程退出作废）
AppAutoInjectTick()           // 1016 空函数，见 §9.4
AppPublish()                  // 1024 无条件发布（含 pauseHook）
[学习块]                       // 1037-1114 见 §7
if (monitoring) {
    AppPollSensors()          // 1116 采传感器 + ETW + PDH 每进程 GPU
    AppPublish()              // 1117（本 tick 第二次，幂等）
    [simulate 假数据 或 NPHistoryPush]   // 1118-1151
    InjectorTick()            // 1152 清死进程 + 收遥测映射
    [帧计数判活 + 陈旧遥测回收]  // 1164-1193 见 §8
    wantDesktop = ...         // 1194-1196
    OverlaySetVisible(wantDesktop); OverlayUpdate(); UpdateStatus();  // 1213-1215
}
```

**为什么 tick 用 `>= 120` 而不是定时器**：消息循环本来就是阻塞的，
用「处理完一条消息就看一下时间」的方式最省事，也不会因为大量消息而饿死刷新逻辑。

### 4.3 `AppLock`：唯一的跨线程互斥（`np_app.h:129-138`，实现 `injector.cpp:24-34`）

```cpp
struct AppLock { AppLock(); ~AppLock(); };   // 内部是一把 CRITICAL_SECTION
```

`CRITICAL_SECTION` 可重入，**同一线程嵌套加锁是安全的**（`np_app.h:134`）。

**为什么必须有它**（`np_app.h:129-133` 原话）：守护线程每 800ms 跑一次 `InjectorScanNow`，
会往 `gApp.injected` 里 `push_back`、改写 `gApp.games[i].pid`；而主线程每 120ms 就在
读/遍历它们。`std::vector` 的 `push_back` 会扩容，**扩容之后另一边正在用的迭代器/引用
就是野指针 —— 直接崩，而且崩的是主程序。**

**当前必须加锁的访问点清单**（改代码时按这个清单核对）：

| 位置 | 保护对象 |
| --- | --- |
| `main.cpp:110-116`（`AppPollSensors`） | 读 `telemetryPid` / `injected` |
| `main.cpp:524-526`（`AppPublish`） | `TryOpenTelemetry` / `PickTelemetry` |
| `main.cpp:565-574`（`UpdateStatus`） | `gApp.notice`（读 + 清） |
| `main.cpp:1181-1188`（陈旧遥测回收） | `telemetry` + `injected` |
| `main.cpp:483-488`（UI `A_ADDGAME`） | `gApp.games.push_back` |
| `injector.cpp:111-119`（`SetNotice`） | `gApp.notice` 写 |
| `injector.cpp:443-449`（`InjectInto` 收尾） | `injected.push_back` |
| `injector.cpp:527-538` / `539-542`（`ForgetGameAt`） | `injected` / `telemetry` / `games.erase` |
| `injector.cpp:561-564`、`573-634`（`InjectorScanNow`） | `games` / `injected` |
| `injector.cpp:671-686`（`InjectorTick`） | `injected` / `telSlots` |
| `injector.cpp:690-696`（`TelemetryCloseAll`） | `telSlots` |

**「锁里不做慢操作」的原则**（`injector.cpp:566-571`）：`InjectorScanNow` 里
`EnumProcesses()` 在锁外做、把本轮的注入目标先收集到 `toInject`，
**出了锁再调 `InjectInto`** —— 因为 `InjectInto` 里那个 `WaitForSingleObject` 最长等 10 秒，
如果在锁里做，主线程的界面刷新（`AppPublish` / `AppPollSensors` 都要拿这把锁）会一起卡死。

> **已知缺口**：学习逻辑在 `main.cpp:1093-1107` 直接把新条目 `gApp.games.push_back(g)`，
> **没有加 `AppLock`**。这正好违反 `A_ADDGAME`（`ui.cpp:483-488`）注释里写明的规则，
> 守护线程此时若正在 `for (auto& g : gApp.games)` 里遍历就会踩迭代器失效。见 §17。

### 4.4 钩子存活判据 & overlay mode 自动切换（`main.cpp:1164-1213`）

```cpp
uint64_t ft = gApp.telemetry.frameTotal;
if (ft != sLastFt) { sLastFt = ft; sLastGrowMs = now; }        // 1166-1170
bool attached = gApp.telemetry.attached && sLastGrowMs != 0 &&
                (now - sLastGrowMs) < 2500;                    // 1192-1193
bool wantDesktop = !gOverlayOff && gApp.monitoring &&
                   (gApp.cfg.overlayMode == 2 ||
                    (!attached && gApp.cfg.overlayMode != 1)); // 1194-1196
```

* `overlayMode`：`0 = 自动`（钩子在线→游戏内画，不在线→桌面叠加退回）、
  `1 = 强制游戏内`、`2 = 强制桌面叠加`（`np_common.h:173`）。
* 每 5 秒把判定依据整条打进日志（`main.cpp:1197-1212`）：
  `mode / attached / tele / ft / growAge / pid / monitoring / off / wantDesktop` ——
  **这是排查「HUD 不消失 / 不出现」的第一手证据**，不要在重构时顺手删掉。
* ⚠ `docs/ARCHITECTURE.md:195` 里写的还是旧判据 `(now - telemetry.tickMs) < 2500`，
  **已经过期**，以本节为准（该文件不在本次改动范围内，未修改）。

---

## 5. 注入器（`injector.cpp`）

### 5.1 `InjectInto(DWORD pid)`（`injector.cpp:296`）

流程（每一步失败都会 `AppLog` + `SetNotice`，让用户看到**原因**）：

| 步骤 | 行号 | 关键点 |
| --- | --- | --- |
| ① 自检 | 297-298 | `pid == 0` 或等于自己 → false；已注入过 → 直接 true（幂等） |
| ② 拼日志身份 | 300-308 | `"pid 1234 (game.exe)"`，进程名拿不到就只写 pid |
| ③ **选 DLL** | 313-329 | 32 位目标换 `NextPerfHook32.dll`，见 §5.3 |
| ④ 存在性检查 | 331-336 | `GetFileAttributesW`，缺 DLL 直接给提示 |
| ⑤ `OpenProcess` | 338-356 | 先申请**最小权限集** `QUERY_LIMITED_INFORMATION\|CREATE_THREAD\|VM_OPERATION\|VM_WRITE\|VM_READ`；失败再退回 `PROCESS_ALL_ACCESS`，两次错误码都记日志；`err=5` 专门提示「用托盘菜单以管理员身份重启」 |
| ⑥ `VirtualAllocEx` | 358-367 | 写 DLL 全路径（`(len+1)*sizeof(wchar_t)`） |
| ⑦ `WriteProcessMemory` | 369-378 | |
| ⑧ 取远端函数地址 | 380-399 | 默认用**本进程** `kernel32!LoadLibraryW`；32 位目标改为走 PEB 解析，见 §5.3 |
| ⑨ `CreateRemoteThread` | 401-413 | |
| ⑩ 等 10 秒 | 415-428 | 见下面的「铁律」 |
| ⑪ 判结果 | 431-439 | `STILL_ACTIVE(259)` **不算失败**（大进程加载慢）；只有明确的 `0` 才是载入失败 |
| ⑫ 登记 | 443-451 | `AppLock` 内 `gApp.injected.push_back(pid)` + `SetNotice` |

**铁律：远端内存只在确认远端线程结束后才释放**（`injector.cpp:419-428`）：

```cpp
if (wait == WAIT_OBJECT_0) VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
else AppLog("... 远端 LoadLibrary 10 秒未返回，保留远程内存不释放（泄漏 %llu 字节）", ...);
```

超时（`WAIT_TIMEOUT`）说明 `LoadLibraryW` **可能还在读**这段 DLL 路径字符串，
这时 `VirtualFreeEx` 等于让游戏进程去读已释放的页 —— **直接把它打崩**。
按「宁可功能退化也不能崩游戏」的原则，这种情况**故意只泄漏一页**。

**错误码人话翻译**：`ExplainWin32`（`injector.cpp:275-285`）把 5 / 87 / 126 / 127 / 299 / 1008
翻成中文说明。为什么值得单独写一个函数（`injector.cpp:274`）：
**用户看到 "err=5" 没有意义**。注意 299 `ERROR_PARTIAL_COPY` 的注释是
「目标很可能是 32 位进程」—— 这是早期「检测到 32 位就直接拒绝」年代留下的提示。

### 5.2 `IsInjected` 与命名互斥量（`injector.cpp:147-157`）

* 互斥量名：`Local\NextPerf_Injected_<pid>`（`InjectMutexName`）。
* `IsInjected` = `OpenMutexW(SYNCHRONIZE, FALSE, name)` 成功即认为已注入。
* 创建方在**钩子 DLL 里**（`src/hook`，`ARCHITECTURE.md:85`）：钩子在自己的 `Worker` 里建这个
  命名互斥量，主程序靠它判断「是否已注入」。
* **副作用（要记住）**：互斥量随钩子进程存活；钩子自卸载后句柄关闭、互斥量销毁，
  于是 `IsInjected` 变假 —— 这正是 `ForgetGameAt` 之后能重新注入的前提。
* `InjectorScanNow` 还会用 `IsInjected` **反向认领**：遍历所有进程，
  凡是已经被注入过（互斥量存在）的都补进 `gApp.injected`（`injector.cpp:619-626`），
  这样即使主程序重启，也能重新接管那些还挂着旧钩子的进程。

### 5.3 32 位 WoW64 注入（`injector.cpp:159-272`）

**为什么不能直接用我们自己的 `LoadLibraryW`**（`injector.cpp:161-165` 原话）：
我们是 64 位程序，往 32 位目标注入时用的是 64 位 kernel32 里的地址，
**在 32 位进程的地址空间里不存在**，远端线程起不来。
实测现象：目标进程里始终没有我们的模块，钩子日志也不生成。

正解四步（`injector.cpp:166-171`）：① 拿目标 32 位 PEB → ② 走 `Ldr` 找 `kernel32.dll` 的
`DllBase` → ③ 解析它的导出表取 **32 位 `LoadLibraryW`** → ④ 用这个地址 `CreateRemoteThread`。

**选 DLL**（`injector.cpp:310-329`）：

```cpp
if (ProcessIsWow64(pid)) {                 // 313
    // "NextPerfHook.dll" -> "NextPerfHook32.dll"（在最后一个 '.' 前插 "32"）
    ...
    if (GetFileAttributesW(p32) != INVALID) dll = p32;
    else { SetNotice("目标是 32 位进程，但程序目录下没有 NextPerfHook32.dll ..."); return false; }
}
```

32 位进程**加载不了** 64 位 DLL（反之亦然）。原来的行为是检测到 32 位目标就**直接拒绝注入**，
现在两边都编了，改用 32 位那份（`injector.cpp:311-312`）。
`build.bat` 用 `-target x86-windows-gnu` 编译出 `dist\NextPerfHook32.dll`；
`dist\NextPerfHook64.dll` 只是 `NextPerfHook.dll` 的**同名副本**（给外部注入工具识别用），
**本层代码从不引用它**。

**`NpResolve32LoadLibraryW`（`injector.cpp:226`）用到的全部 32 位硬编码偏移**
（`injector.cpp:172` 警告：所有偏移都按 **32 位** 布局硬编码，64 位指针宽度不同、偏移也不一样）：

| 项 | 32 位偏移 | 行号 |
| --- | --- | --- |
| `PEB->Ldr` | `+0x0C` | 241 |
| `PEB_LDR_DATA->InMemoryOrderModuleList` | `+0x14` | 245 |
| `LDR_DATA_TABLE_ENTRY->InMemoryOrderLinks`（链表节点就在 entry 内 `+0x08`） | `+0x08` | 251（`entry = cur - 0x08`） |
| `LDR_DATA_TABLE_ENTRY->DllBase` | `+0x18` | 254 |
| `...->BaseDllName.Length`（`UNICODE_STRING`） | `+0x2C` | 255 |
| `...->BaseDllName.Buffer` | `+0x30` | 256 |

* PEB 地址来自 `NtQueryInformationProcess(proc, ProcessWow64Information /*26*/, ...)`
  （`injector.cpp:235-237`），函数指针从 `ntdll.dll` 动态取（`227-233`）。
* 遍历上限 256 个模块，遇到 `next == 0 || next == head` 停止（`248`、`268-269`）。
* 名字匹配：`nameLen >= 24 && < 260`（`kernel32.dll` = 12 字符 = 24 字节），
  转小写后 `wcscmp(nm, L"kernel32.dll")`（`258-265`）。
* `NpExportAddr32`（`186`）手工走 PE 导出表：`IMAGE_DOS_HEADER` → `IMAGE_NT_HEADERS32` →
  `DataDirectory[EXPORT]` → `IMAGE_EXPORT_DIRECTORY` → `AddressOfNames`（每条 4 字节 RVA）
  → `AddressOfNameOrdinals`（每条 **2** 字节）→ `AddressOfFunctions`（每条 4 字节）
  → 返回 `modBase + fnRva`（绝对地址）。所有读取都经 `NpReadRemote`（`177`）。
* 目标：**走目标 PEB 找它自己的 `LoadLibraryW`** —— 不依赖 ASLR 推算、不写 inline hook。

### 5.4 `InjectorScanNow`：按 exe 名匹配已添加的游戏（`injector.cpp:560`）

```
① 锁内快速退出：!monitoring 或 games 为空 -> return        // 561-564
② EnumProcesses()（锁外，纯本线程数据）                     // 566
③ 锁内：为每个 GameEntry 重新计算「现在什么状态」           // 573-617
     want = g.name 非空 ? g.name : ExeNameOf(g.path)        // 580（学习条目只有 name）
     if (g.learned && !cfg.learnedAutoHook) continue;       // 584（学习条目要用户开开关）
     线性查 procs 找同名进程（跳过自己） -> pid              // 585-589
     ★ 处理 detachPid 的清理（见 §6.4）                      // 596-610
     g.pid = pid;  g.injected = pid && IsInjected(pid);     // 615-616
     （pid 变化时打一行 watch: 日志）                        // 611-614
④ 锁内：认领已被注入的进程 / 收集本轮要注入的 pid           // 619-633
     for (proc : procs):
        if (IsInjected(proc.pid))  -> 补进 gApp.injected（不重复）
        else if (某个 GameEntry 的 pid == proc.pid) -> toInject.push_back
⑤ 锁外：逐个 InjectInto(pid)，完事再锁内回填 g.injected      // 636-641
```

* **为什么按 exe 名匹配而不是全路径**（`np_app.h:41-43` + `injector.cpp:578-580`）：
  商店（UWP）应用的 exe 在 `WindowsApps` 下，**普通用户读不了**，
  学习来的条目只能按名字匹配，所以 `GameEntry::path` 允许为空。
* **为什么每轮重算而不是增量**：`InjectorScanNow` 的产出就是 UI 上「待注入 / 运行中 / 注入失败」
  这一栏的**解释**（`injector.cpp:575-576`：用户看到的「待注入」原来没有任何解释）。
* **`monitoring == false` 时直接 return** 的副作用：`g.pid` 会保持旧值（不会被清零），
  这正是 `ForgetGameAt` 里需要「当场按名字查一遍进程」的原因（见 §6.2）。

### 5.5 守护线程（`injector.cpp:644-665`）

```cpp
static DWORD WINAPI WatchThread(LPVOID) { while (gWatchRun) { Sleep(800); InjectorScanNow(); } }
bool InjectorInit()      { gWatchRun = true; gWatchThread = CreateThread(...); }
void InjectorShutdown()  { gWatchRun = false; WaitForSingleObject(gWatchThread, 2000); CloseHandle(...); }
```

* 标志是 `volatile bool gWatchRun`（`injector.cpp:20`），**没有事件对象**：
  最坏情况下退出要多等 800ms，`WaitForSingleObject` 给了 2 秒余量。
* 与钩子侧对 `detachPid` 的轮询周期（**200ms**，`src/hook/np_hook.cpp:2671`）不同，
  改动任何一侧的周期都要回头看 §6.4 的 2.5 秒契约。

### 5.6 进程信息小工具

| 函数 | 行号 | 实现要点 |
| --- | --- | --- |
| `ProcessAlive` | `699` | `OpenProcess(QUERY_LIMITED_INFORMATION)` + `GetExitCodeProcess == STILL_ACTIVE` |
| `ProcessImagePath` | `710` | `QueryFullProcessImageNameW`；拿不到返回空 |
| `ProcessExeName` | `722` | `ProcessImagePath` 的最后一段（带扩展名，**原样大小写**） |
| `ProcessIsWow64` | `287` | `IsWow64Process`，需要 `QUERY_LIMITED_INFORMATION` |
| `ExeNameOf`（static） | `455` | 纯字符串取文件名 + **转小写**（`::towlower`），给内部匹配用 |
| `EnumProcesses`（static） | `468` | `CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS)`，名字统一转小写 |

* 一律用 `PROCESS_QUERY_LIMITED_INFORMATION` 而不是 `PROCESS_VM_READ`：
  **商店/UWP/受保护进程也能查**（`main.cpp:1055-1056` 有同样的说明）。
* `EnumProcesses` **不需要打开目标进程**，所以「一定有名字」——
  这也是学习逻辑拿不到路径时的兜底手段（见 §7.2）。

---

## 6. HookControl：钩子生命周期控制

`np_app.h:145-157` 的说明：用户要求「写成类，为后续单独删某个游戏也实现这个功能」，
目前以自由函数形式提供同一入口，**后续要扩成类也只改这里**。

```cpp
void RequestHookDetach(DWORD pid);   // 请求该 pid 的钩子自卸载
void ForgetGameAt(int index);        // 移除第 index 条游戏记录（先卸载再删 + 落盘）
int  ForgetLearned();                // 清空「学习来的」条目（逐条走同一入口）
```

### 6.1 `RequestHookDetach`（`injector.cpp:495`）

```cpp
gApp.cfg.detachPid = pid;
gDetachAt = GetTickCount() ? GetTickCount() : 1;   // 记请求时间戳（0 是保留值）
AppLog("detach: 请求 pid=%lu 的钩子自卸载", pid);
```

**为什么不直接 `CreateRemoteThread` 调 `NpHookDetach`**（`np_common.h:196-198` 原话）：
那需要解析远端导出地址（ASLR 下要自己算偏移），而且跨位数（32 位游戏）还得另做一套。
**钩子本来每帧/每轮就在读 `NPConfig`，用这个字段既简单又天然支持跨位数。**

钩子侧的响应链（`np_hook.cpp:2674-2682`，守卫线程 200ms 一轮）：
`detachPid == GetCurrentProcessId()` → `SelfUnloadNow()` →
`RestoreAllHooks`（还原 vtable 补丁）+ 注销 VEH + `FreeLibraryAndExitThread`，
**这正是「反复注入不闪退」用的干净卸载**（`injector.cpp:485-487`）。

### 6.2 `ForgetGameAt(int index)`（`injector.cpp:505`）

```
① index 越界 -> return                                             // 506
② 取 pid 与名字（name 优先，空则 ExeNameOf(path)）                   // 507-509
③ ★ pid == 0 时**当场按名字查一遍进程**                             // 515-520
④ 查到了 pid:
     RequestHookDetach(pid)                                        // 523
     锁内：从 gApp.injected 摘掉该 pid（倒序遍历 + erase）           // 527-531
           若 telemetry.pid == pid -> NPClearTelemetry(&gApp.telemetry) // 533-536
           gApp.games.erase(begin()+index)                          // 537
⑤ 没查到 pid：锁内只 erase 条目                                     // 539-542
⑥ 日志 + SettingsSave()                                             // 544-545
```

三个「为什么」都是源码注释里写明的：

* **为什么必须先卸载再删条目**（`injector.cpp:502-504`）：不卸载的话，学习逻辑会立刻
  从还活着的数据里把它重新学回来 —— 这正是用户报的「清空后又自己回来」。
* **为什么 `pid == 0` 要现场查进程**（`injector.cpp:511-514`）：`g.pid` 是由
  `InjectorScanNow` 填的，而它在 `!monitoring` 时直接 return；所以监视关着、
  或条目刚学习完还没被扫过时 pid 都是 0 —— 原来 `if (pid)` 不成立就**根本没请求卸载**
  （用户报的「清空没有正确退出 hook」）。
* **为什么还要摘 `gApp.injected`**（`injector.cpp:525-526`）：不摘的话 `IsInjected` 仍为真
  → 界面显示「已接管」，而且自动注入不会再处理它（用户报的「接管状态没清空」）。
* **为什么还要清遥测**（`injector.cpp:532`）：不清的话 `attached` 仍为真，
  面板还以为钩子活着。

### 6.3 `ForgetLearned`（`injector.cpp:549`）

倒序遍历（`size_t i = size; i-- > 0;`）逐条判断 `learned`，
命中就调 `ForgetGameAt((int)i)` 并计数 —— **倒序是必须的**：`ForgetGameAt` 会 `erase`，
正序遍历会跳条目/越界。返回清掉的条数，UI 用它拼提示（`ui.cpp:456-457`）。
**手动添加的条目不动**（`ui.cpp:453`）。

### 6.4 `detachPid` 的清理契约（`injector.cpp:488-493`、`590-610`）

> **为什么不能只在「pid 已退出」时清**：游戏还活着的话 `detachPid` 会一直留着，
> **下次再注入这个游戏，钩子一看 `detachPid == 自己` 就立刻又自卸载。**
> 2.5 秒足够钩子（200ms 一轮）完成 `SelfUnloadNow`。

清理发生在 `InjectorScanNow` 锁内（`596-610`）：两种情况都清 ——
① 目标 pid 已不在进程快照里（退出了）；② 请求发出超过 2.5 秒（`gDetachAt` 时间戳）。
顺带解决 **pid 复用**问题：清掉之后就不会误伤复用同一 pid 的新进程。

`gDetachAt` 的赋值有 `GetTickCount() ? GetTickCount() : 1`（`injector.cpp:498`）——
**0 被当作「没有请求」的保留值**，所以 0 时替换成 1。

---

## 7. 可信名单的自动学习（v1.6）

位置：主循环 tick 内，`main.cpp:1026-1114`。用户的原话被直接抄在注释里
（`main.cpp:1029-1031`）：「记住这个用户 hook 也可能 hook 错，所以要看 hook 之后是否有
可读取数据出来，以及 d3d 之类的能不能出来」。

### 7.1 三条件验证（缺一不可，且要**持续 3 秒**）

| 条件 | 判据 | 行号 |
| --- | --- | --- |
| ① Present 真的挂上（不是只注入成功） | `lt.hookFlags & NP_HOOK_PRESENT` | 1042 |
| ② 认出了 D3D | `lt.gfxApi != NP_API_UNKNOWN` | 1043 |
| ③ 帧数据真的在流动 | `lt.frameTotal > sLearnFt`（3 秒前记的基线） | 1052 |

```
if (monitoring && lt.pid != 0 && hooked && hasApi) {       // 1045
    if (sLearnPid != lt.pid) { 重置基线 sLearnSince/sLearnFt }  // 1046-1050
    if (sLearnSince && ltk - sLearnSince >= 3000) {        // 1051
        bool flowing = lt.frameTotal > sLearnFt;           // 1052
        sLearnSince = 0;        // 同一个 pid 只判一次       // 1053
        if (flowing) { ...取名字、入库... }                 // 1054-1107
    }
} else { sLearnPid = 0; sLearnSince = 0; }                 // 1110-1113
```

* 条件和 pid 断开时**整体重置**（`1110-1113`），所以「切走再切回来」会重新计时。
* 时间基准用 `GetTickCount()`（32 位毫秒，`1024`）；`ltk ? ltk : 1` 同样规避 0。

### 7.2 取进程名：主路径 + `EnumProcesses` 兜底

```
主路径（1057-1069）：OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)
                     -> QueryFullProcessImageNameW -> 取最后一段 -> 转小写
兜底（1076-1092）：  CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS) 遍历找同名 pid
                     -> pe.szExeFile -> 转小写
```

**为什么主路径选 `PROCESS_QUERY_LIMITED_INFORMATION`**（`main.cpp:1055-1056`）：
**商店/UWP 进程也能查**，不像 `VM_READ` 那样打不开。

**为什么必须有兜底**（`main.cpp:1070-1075` 原话）：对商店/UWP 或保护更严的进程，
`QueryFullProcessImageNameW` 有可能失败；`CreateToolhelp32Snapshot` **不需要打开目标进程**，
所以一定有名字。拿不到路径无所谓，学习只需要名字（商店应用的路径本来也读不了）。
**不加这个兜底的话，失败会得到空名字 → 静默跳过学习，很隐蔽。**

随后去重与自保（`1093-1098`）：`known`（已有的 `g.name` 里出现过）、
`self`（`nm == L"nextperf.exe"`）—— 避免把主程序自己记进名单。

### 7.3 落库（`1099-1107`）

只填 `name` + `learned = true`（**path 留空**），`gApp.games.push_back(g)`，立刻 `SettingsSave()`，
并写日志：`learn: 注入后验证通过（Present 挂上 + 认出 D3D + 帧在流动）-> 把 X 记入可信名单`。
落盘格式见 §13（`games` 只存手动添加的路径、`learned` 只存 exe 名）。

### 7.4 学习条目什么时候才自动注入

`InjectorScanNow` 里：`if (g.learned && !gApp.cfg.learnedAutoHook) continue;`（`injector.cpp:584`）。
即 **`learnedAutoHook`（界面上叫「实验性自动注入」）默认关**，
**手动添加的条目不受此开关影响，一直自动注入**（`np_common.h:189-192`、`ui.cpp:349-355`）。

---

## 8. 遥测的打开 / 挑选 / 回收

### 8.1 打开与挑选

见 §3.4（`TryOpenTelemetry` / `PickTelemetry`）。两者都在 `AppPublish` 的 `AppLock` 里执行。

### 8.2 陈旧遥测回收：**帧数 2.5 秒不增长就判定钩子已死**（`main.cpp:1171-1191`）

```
if (telemetry.attached && telemetry.pid != 0 && sLastGrowMs &&
    (now - sLastGrowMs) > 2500) {
    AppLog("telemetry: pid=%lu 帧数停增 %.1f 秒 -> 判定钩子已死，清空遥测", ...);
    { AppLock lk;
      DWORD dead = telemetry.pid;
      NPClearTelemetry(&gApp.telemetry);
      从 gApp.injected 里 erase 掉 dead（倒序）; }
    sLastFt = 0; sLastGrowMs = 0;
}
```

**为什么需要它**（`main.cpp:1171-1175` 原话）：钩子卸载后（无论走哪条路），
主程序可能还持有那个共享内存句柄，于是对象不销毁、每 tick 继续读到**冻结的旧数据**
（实测日志 `tele=1 ft=5023 growAge=86469ms`）。判据用「帧数是否增长」，
这样状态栏、面板、图表都不会再念旧数据，**而且不依赖跨进程时钟**。

**与 `InjectorTick` 的分工**（这一层关系必须理解，否则会以为回收没生效）：
本段只清 `gApp.telemetry` 和 `gApp.injected`；**真正的 `UnmapViewOfFile` + `CloseHandle`
发生在 `InjectorTick`**（`injector.cpp:676-686`）—— 它按「`telSlots[i].pid` 还在不在
`gApp.injected` 里」决定是否收掉这个 slot。所以两处都要保留。

### 8.3 `InjectorTick`（`injector.cpp:667`）

主循环每个 tick 调用一次（`main.cpp:1152`），整段在 `AppLock` 里：

1. 轮询 `gApp.injected`，`ProcessAlive` 为假就 `erase`（倒序 + `erase(begin()+i)`）。
2. 轮询 `gApp.telSlots`，pid 不在 `gApp.injected` 里就 `UnmapViewOfFile` + `CloseHandle`，
   顺带把 `telemetryPid` 清 0，再 `erase` 该 slot。
   —— `s.view` / `s.map` 要先判空指针（`injector.cpp:682-683`）。

### 8.4 跨进程时钟为什么不能比（`main.cpp:550-560`、`1154-1163`）

这是本项目**代价最大的一类 bug**，两处注释写着同一件事：

* `UpdateStatus`（`main.cpp:551-553`）：原来是 `(GetTickCount64() - t.tickMs) < 2500` ——
  **跨进程比时钟**，和 overlay mode 那个 bug 同源。
* overlay 判活（`main.cpp:1154-1163`）：实测诊断日志 `age=4294967265ms`
  （32 位下的 -31，即 `tickMs` 落在「未来」）→ `age < 2500` 永远为假 →
  `attached` 永远为假 → **桌面 HUD 永不关闭**（用户报的 bug）。

原话结论：**时钟跨进程比较本来就不该作为依据**：旧钩子实例残留、时钟回绕、
共享内存里的历史值，任何一样都会让它变成垃圾。改成看**帧计数是否在增长**：
与时钟无关，而且是「钩子活着并且真的在计数」的直证。守护线程停摆、Present 没挂上、
监控已暂停（不再更新遥测），都会让它自然变假。

> ⚠ **残留**：`ui.cpp:631` 的游戏列表状态判断**仍在用旧写法**
> （`t.attached && mine && (GetTickCount64() - t.tickMs) < 2500`），
> 与 `main.cpp` 的新判据不一致。见 §17。

---

## 9. 前台窗口跟踪

### 9.1 为什么必须提前记住（`main.cpp:226-240`）

点「注入到前台进程」按钮时，前台窗口一定是 NextPerf 自己 —— 所以必须提前记住用户刚才
切走的那个游戏。两个坑都踩过：

* **坑 1：靠主循环轮询会漏。** 原来 120ms 轮询一次，快速 ALT+TAB 切过去又切回来就可能没采到，
  用户看到的就是「读不到进程」。改用 **WinEvent 钩子**：系统在**每一次**前台变化时主动回调，不会漏。
* **坑 2：UWP（微软商店）应用拿到的不是它自己。** 商店应用外面套了一层
  `ApplicationFrameHost` 的壳（窗口类名 `ApplicationFrameWindow`），
  `GetWindowThreadProcessId` 返回的是**那个壳的宿主进程**，往它注入毫无意义 ——
  真正的应用是它的子窗口（`Windows.UI.Core.CoreWindow`），所以要往下找一层。

### 9.2 三个相关函数

| 函数 | 行号 | 逻辑 |
| --- | --- | --- |
| `IsUwpFrameHost(HWND)` | `243` | 类名 `_wcsicmp == "ApplicationFrameWindow"`；**兜底**再按进程名找 `applicationframehost`（壳进程固定叫这个） |
| `ResolveForegroundPid(HWND)` | `257` | 普通窗口直接返回 `GetWindowThreadProcessId` 的 pid；壳窗口则 `EnumChildWindows` 取**第一个 pid 与壳不同**的子窗口；找不到就退回壳 pid |
| `AppRememberForeground(HWND)` | `283` | `ResolveForegroundPid` → 过滤掉自己 → pid 变化时更新 `forePid`/`foreName`（名字拿不到写 `(未知进程)`） |
| `ForegroundWinEvent`（回调） | `294` | 只认 `EVENT_SYSTEM_FOREGROUND` + `OBJID_WINDOW` + `CHILDID_SELF`，其余直接忽略 |
| `AppEnsureForegroundHook` | `304` | **懒安装**（第一次轮询时装上），`WINEVENT_OUTOFCONTEXT \| WINEVENT_SKIPOWNPROCESS`，避免还要去改启动流程 |

### 9.3 `AppTrackForeground`（`main.cpp:494`）

```
AppEnsureForegroundHook();
fg = GetForegroundWindow();
if (fg) { pid = ResolveForegroundPid(fg);
          if (pid && pid != 自己) { AppRememberForeground(fg); return; } }
if (forePid && !ProcessAlive(forePid)) { forePid = 0; foreName.clear(); }   // 前台进程退出就作废
```

即：**WinEvent 钩子是主路径，120ms 轮询只负责「把已经退出的目标作废」**。

`forePid` 的消费方是 UI 的 `A_INJECTFRONT`（`ui.cpp:521-548`）：
**固定用记住的那个进程**。源码注释解释了为什么不再临时看 `GetForegroundWindow`：
① 点按钮时前台通常就是 NextPerf 自己（这正是最初报的 bug）；
② 更糟的是，如果前台是别的程序（终端、资源管理器、浏览器），我们会真的往那个进程里注入 ——
测试里就撞上了：**前台是终端，于是钩子被塞进了终端而不是游戏**。

### 9.4 已停用的启发式自动注入 —— 现存 dead code 清单

`AppAutoInjectTick`（`main.cpp:488-492`）**是空的**，注释原因（`main.cpp:474-487`）：
「是不是游戏」没有可靠的启发式判据。窗口化游戏与普通窗口在窗口属性上没有任何区别；
主界面可能不做 3D 渲染；商店(UWP)应用的前台窗口是 `ApplicationFrameHost` 的壳；
而枚举目标进程的模块既打不开 UWP、也可能把游戏搞崩。
主流工具（RTSS / Steam / Game Bar / Playnite）清一色靠**游戏数据库**，没有靠启发式猜的。
而且本项目本来就有可靠答案：**「添加游戏 exe」**（见 §5.4）。
之前几版这里先后把用户的梯子、微信、甚至 NextPerf 自己当成游戏注入过，
也误判过商店版的生化危机8 —— 这条路不可靠，不做。
界面上对应的「自动注入 3D 窗口」开关也已移除（`ui.cpp:365-369`）。

**因此以下函数目前是「定义了但没有人调用」的死代码**（保留着调研结论，将来若要复活
启发式注入可以直接复用；但**不要以为它们还在生效**）：

| 符号 | 行号 | 原用途 |
| --- | --- | --- |
| `NameLooksNonGame` | `main.cpp:326` | 浏览器/聊天工具/IDE 等拒绝清单（`kDeny[]`） |
| `kAntiCheatKw` / `NameIsAntiCheat` | `355` / `366` | 反作弊关键词表与匹配 |
| `AnyAntiCheatRunning` | `376` | 进程名扫描 + **5 秒缓存**（不必每帧扫） |
| `WindowLooksLikeGame` | `404` | 窗口特征判据：`WS_EX_NOREDIRECTIONBITMAP`（最强信号）或窗口矩形与某个显示器完全一致 |
| `RealGameWindow` | `445` | 把 UWP 壳窗口换成真实子窗口（判游戏特征必须用真实窗口） |
| `gAutoCand` / `gAutoCandSince` / `gAutoTried` / `gAutoScanMs` | `469-472` | 自动注入的候选状态机残骸 |

**调研结论值得留着**（`main.cpp:396-403`）：RTSS / Special K / PowerToys 这类工具判断全屏游戏，
走的都是「窗口扩展样式 + 显示器几何 + DWM 状态」这条路，而不是去枚举目标进程的模块 ——
后者既不可靠（UWP/受保护进程打不开，实测把生化危机8 误判成「不是游戏」），
又有副作用（枚举受保护进程的模块可能把它搞崩，实测游戏待机闪退）。
另外 `WindowLooksLikeGame` 的注释说明：本来还想用 `DwmGetWindowAttribute(DWMWA_CLOAKED)`
再排除一次被 DWM 隐藏的窗口，但那要额外链接 `dwmapi`，而前两条判据已经够用，所以不加依赖
（`main.cpp:433-436`）。

---

## 10. 传感器采集与数据来源

### 10.1 `AppPollSensors` 的顺序（`main.cpp:93-223`）—— **顺序本身就是一个坑**

```
① gApp.hub.Poll(gApp.sensors)              // 94
② EnumDisplaySettingsW 填 screenW/H/refreshHz   // 97-105
③ 取 pid（锁内：telemetryPid，回退 injected.back()）  // 109-116
④ np::Etw().SetTargetPid(pid)              // 119
⑤ ETW 只覆盖 fpsLow1 / fpsLow01             // 131-140
⑥ frameCount = max(telemetry.frameTotal, ETW.presentN)  // 149-154
⑦ hub.PollGameGpu(pid, frameCount, sensors)  // 155-156
⑧ 每 5 秒写一次「数据来源」日志               // 157-219
```

* **★ 显示模式必须写在 `Poll` 之后**（`main.cpp:95-96` 原话）：`Poll` 开头会把整个
  `NPSensors` 清空（`src/sensors/np_sensors.cpp:1028-1031` 里 `NPClearSensors(&out); out.valid = 1;`）。
  **踩过：写在前面会被覆盖成 0。**
* **`pid` 的读取必须加锁**（`main.cpp:111-112`）：`gApp.injected` 由守护线程维护，
  正好撞上 `push_back` 扩容就会读到野指针。
* 没有目标进程时把 `sensors.gpuBusyMs = -1.0f`（`main.cpp:221`），
  别让上一轮的残留值被当成有效数据。

### 10.2 ETW：**只把 Low 帧换成 ETW 的读数，其它一律不碰**（`main.cpp:121-140`）

原话（`main.cpp:123-130`）：钩子量的是「真 Present 调用之间的间隔」，而这游戏每帧调**两次**
Present（TEST + 真），真 Present 在帧内的相位每帧都在变 —— 实测单样本在 9.48~23.83ms 之间抖。
**平均值对，但每个样本都是脏的**，排序取「最慢 1%」时取到的正是那批假的 23.8ms 样本，
于是流畅的 60fps 会显示 1%Low ≈ 38~42。ETW 的事件在真正 flip 处触发，没有这个抖动。
**帧率/帧时间/GPU 时间不覆盖** —— 那些本来就已经对了，多覆盖一次只会把对的数弄坏
（**上一版就是这么翻车的**）。

还有一道必须有的保护（`main.cpp:133-135`）：**必须带 pid 判断** —— 没有目标进程时
ETW 的数据是「全系统所有进程的 present 事件」混出来的（桌面、浏览器、播放器都算），
拿它去覆盖 Low 帧等于把别人的数字塞进面板。以及 `er.low1Fps > 0.1f` 才写
（避免把 0 当有效值写进去）。

### 10.3 帧计数取两个来源的**较大值**（`main.cpp:142-154`）

原话：GPU 帧时间 = 「GPU 累计时间 ÷ 帧数」，而原来帧数**只**取自钩子。一旦钩子没挂上
（比如 NextPerf 不是以管理员运行、注入被拒），帧数恒为 0 → GPU 帧时间永远算出 -1 →
面板退回显示钩子那个空值。实测日志里就是这个现象：`admin=no → attached=0 → 帧数=0 → GPU 帧时间 = -1`。
ETW 是外置的，不需要注入，所以它也能当帧数来源。

### 10.4 每 5 秒的「数据来源」日志（`main.cpp:157-219`）

把每一项的**来源**都写清楚（用户要求：来源写日志就行，UI 不用标注 —— 这样界面干净，
排查时又一眼能看出某项到底是系统给的还是我们推算的）。示例几行：
`显示模式 <- EnumDisplaySettings(系统)`、`渲染分辨率 <- RSSetViewports(钩子, 已按输出宽高比过滤)`、
`GPU 帧时间 <- PDH \GPU Engine(pid_*)\Running Time(系统/驱动, 单位100ns)`、
`帧率/帧时间 <- IDXGISwapChain::Present 间隔(钩子, 已滤 DXGI_PRESENT_TEST)`、
`Low 帧 <- 最近1200帧样本推算(钩子)`。
ETW 那几行还会打出**实际匹配到的事件名**（最多 6 个，
`er.evNames`）—— 诊断「ETW 报的帧率是真实值的两倍」时，直接就能看出是哪种事件被重复算了
（`np_etw.h:57-61` 记录了同样的教训：原来靠「Task 名含 Present 且 opcode==1」判定，
实测每帧匹配到两次：120fps，真相是 60fps）。

---

## 11. 界面（`ui.cpp`）—— 自绘控件系统

### 11.1 为什么自绘

`ui.cpp:1-4`：主界面是自绘深色 UI（黑白配色、无高亮描边、无 Logo）。
布局全部按 DPI 缩放后**一次性算好**，列表滚动通过 `VisibleRect` 折算，
不会再出现控件互相压叠的情况。绘制用 GDI（`FillR`/`StrokeR`/`Txt`，`ui.cpp:174-203`）
+ 双缓冲 `WM_PAINT`（`ui.cpp:769-771`：`CreateCompatibleDC` + `CreateCompatibleBitmap`），
只有**叠加预览**走 Direct2D（`npb::PanelBitmap`，与真实叠加用的是同一条渲染通路）。

### 11.2 控件模型

```cpp
enum WType { W_CHECK, W_GROUP, W_BUTTON, W_SLIDER_F, W_SLIDER_I, W_CYCLE, W_GAMEROW };  // 109
struct Widget { WType type; int x,y,w,h; std::wstring text; uint64_t bit;
                float* pFloat; float fmin,fmax; int* pInt; int imin,imax;
                int* pCycle; std::vector<std::wstring> opts; std::vector<int> vals;
                int action; int index; bool hover; };                                    // 116-131
static std::vector<Widget> gW;                                                              // 133
```

* 控件**直接指向配置字段**（`pFloat`/`pInt`/`pCycle` 指向 `gApp.cfg.*`），
  拖动/点击即改配置，没有中间状态需要同步。
* `W_CYCLE`（循环选择）同时存 `opts`（显示文本）与 `vals`（真实值），
  点击时按**值**找当前下标再取下一个（`ui.cpp:728-738`）：
  `*pCycle = vals.empty() ? nxt : vals[nxt];`
* `Add()`（`ui.cpp:206`）是通用注册入口（带重叠检测）；
  `W_GROUP`/`W_CHECK` 在 `Layout()` 里**直接 `gW.push_back`**（`ui.cpp:248-268`），
  所以它们**不参与重叠检测** —— 计数器列表的行是等距算出来的，不需要。
* `Layout()`（`ui.cpp:235`）每次 `gW.clear()` 重建；`WM_DPICHANGED` 会重跑它（`ui.cpp:683-692`）。

### 11.3 尺寸常量：**加一行控件必须同步改 `kWinH`**（`ui.cpp:29-38`）

```cpp
static const int kWinW = 1000;
static const int kWinH = 730;   // 730 = 692 + 38：为「实验性自动注入」那一行腾出的高度
static const int kListH = 400;  // 计数器列表高度（只在这里定义一次）
```

源码注释原话：**「730 = 原来的 690 + 38：「自动注入 3D 窗口」那一行是后加的，
当时忘了同步加高窗口，结果把「退出监视」等按钮挤到窗口外面去了（用户实测）。
⚠ 上次加行忘了同步加高，把按钮挤出了窗口 —— 加行必须同时改这里。」**

`kListH` 同理：**只在这里定义一次** —— 原来 `Layout()` 和 `WM_PAINT` 里各写了一遍
`S(452)`，改一处漏一处就会让列表和游戏区对不上（`ui.cpp:35-37`）。

当前布局的实测（用源码数字手算，未实机验证）：
左列计数器列表 `42 ~ 442`；游戏区 `458 ~ 554`（3 行 × 32）；
按钮第一行 `564 ~ 596`、第二行 `602 ~ 634`；状态栏固定在 `S(658)`（`ui.cpp:844`）。
右列滑杆 7 条从 `288` 起每 30 一条 → 到 498；行为区 534/572/610/648 四行；
按钮行 694~726。**右列按钮底边 726 距离窗口高 730 只剩 4 像素** ——
再加任何一行都必须先改 `kWinH`。

### 11.4 控件重叠检测（`ui.cpp:212-231`）

```cpp
for (const auto& o : gW) {
    if (o.type != wd.type) continue;                  // ★ 只对同类型控件报警
    ... 求矩形交集，ax > ix && ay > iy 即相交 ...
    AppLog("UI 重叠警告：新控件「%ls」(%d,%d %dx%d) 与已有控件 (%d,%d %dx%d) 相交 "
           "-> 点击会被先注册的那个吃掉", ...);
}
```

* **为什么需要**（原话）：两个可交互控件画在同一个位置时，点击只会命中最先注册的那个，
  表现就是「某个开关点了没反应」—— 用户实测撞过一次（「自动注入 3D 窗口」被放在
  「传感器轮询」的同一个矩形上）。**这里主动查出来并记日志，别让它再靠用户发现。**
* **为什么只查同类型**：游戏列表行里，行容器与它内部的按钮天然是包含关系
  （父子的 y 相差十几像素、高度还不同），那不是 bug。真正会吃掉点击的是
  「两个同类型的开关叠在同一块矩形上」—— 用户实测撞到的就是这个。

### 11.5 命中测试、滚动与预览

* `VisibleRect`（`ui.cpp:402`）：`W_CHECK`/`W_GROUP` 用**逻辑行号 − gScroll** 折算成屏幕 y，
  超出可见范围返回 false；其余控件直接用自身矩形。
* `Hit`（`ui.cpp:420`）：**按注册顺序线性扫描，返回第一个命中的**
  （`W_GROUP` 跳过，组标题不可点）—— 这就是「先注册的吃掉点击」的机制来源。
* 滚动（`ui.cpp:755-762`）：`gScroll -= wheelDelta / (60 * gS + 1)` 再 clamp 到
  `[0, gTotalRows - gVisibleRows]`；迷你滚动条在 `DrawCounterList` 末尾画（`ui.cpp:597-604`）。
* 预览（`RenderPreview`，`ui.cpp:389`）：`BuildPanelData` → `Measure` → `npb::PanelBitmap::Render`，
  由 `UiRefresh` 节流 200ms 触发一次；`DrawPreview`（`648`）用 `AlphaBlend`
  **只缩小不放大**（`fit > 1.0f → 1.0f`，「避免糊」，`ui.cpp:658`）。

### 11.6 动作枚举与分发（`ui.cpp:110-114` / `431-553`）

```cpp
enum Act { A_NONE = 0, A_START, A_STOP, A_SAVE, A_DEFAULT, A_ADDGAME, A_REMGAME,
           A_INJECTFRONT, A_FORGET, A_ELEVATE, A_LOGDIR, A_QUIT };
```

| 动作 | 行为 | 行号 |
| --- | --- | --- |
| `A_START` | `monitoring = true` + `OverlaySetVisible(true)` + `InjectorScanNow()` | 433-438 |
| `A_STOP` | `monitoring = false` + `OverlaySetVisible(false)` | 439-443 |
| `A_SAVE` | `SettingsSave()` | 444-447 |
| `A_DEFAULT` | `NPDefaultConfig(&gApp.cfg)`（**不会重置 `autoInject`/`learnedAutoHook`，见 §17**） | 448-451 |
| `A_FORGET` | `ForgetLearned()` + `SetNotice("已清空学习名单（N 条，已卸载对应钩子）")` | 452-459 |
| `A_ADDGAME` | `GetOpenFileNameW` → 填 `path` + **同步填小写 `name`** → `AppLock` 内 `push_back` → 提示 + 日志 + `SettingsSave` | 460-494 |
| `A_REMGAME` | `ForgetGameAt(gSelGame)` 并清空选择 | 495-501 |
| `A_INJECTFRONT` | 用 `gApp.forePid`（见 §9.3），不存在/已退出都给明确提示 | 521-548 |
| `A_ELEVATE` | `AdminRelaunch()` 成功后给自己发 `NP_TRAY_EXIT` 退出 | 506-511 |
| `A_LOGDIR` | `ShellExecuteW("open", 日志目录)` + 提示两个日志文件名 | 512-520 |
| `A_QUIT` | `PostMessageW(gMain, WM_COMMAND, NP_TRAY_EXIT, 0)` | 502-505 |

* `A_ADDGAME` 里**必须同步填 `name`**（`ui.cpp:473-482` 注释）：`InjectorScanNow` 现在按
  `name` 匹配，不填的话手动添加的条目只能靠 path 回退，容易和自动学习的条目产生不一致。
* 所有分支结束后统一 `InvalidateRect(gMain, nullptr, FALSE)`（`ui.cpp:552`）。
* `A_ELEVATE` / `A_QUIT` 都绕道 `NP_TRAY_EXIT` 命令，**复用同一套退出流程**（`main.cpp:675-714`）。

### 11.7 游戏列表的状态显示（`DrawGameList`，`ui.cpp:607`）

* **`path` 为空时回退到 `name`**（`ui.cpp:615-618`）：学习来的条目**只有 name、没有 path**
  （商店应用的 exe 在 `WindowsApps` 下，普通用户读不了），不回退的话列表里显示空白
  （用户实测的 bug：自动学习的程序在列表里没名字）。
* 状态文案（`ui.cpp:626-641`）：`待注入` / `● 读取中` / `● Vulkan · 无帧数据` /
  `● OpenGL · 无帧数据` / `● 已注入 · 未接管` / `● 运行中 · 注入失败`。
  **目的（原话）：让用户一眼看出「为什么没数据」，而不是只看到「待注入」。**
  Vulkan/OpenGL 是**设计边界**（钩子只做 D3D），所以要专门说明，不能显示成故障。
* ⚠ 存活判断用的是 `(GetTickCount64() - t.tickMs) < 2500`（`ui.cpp:631`），
  **与主循环的帧计数判据不一致**，见 §8.4 与 §17。

### 11.8 窗口过程 `MainProc`（`ui.cpp:669`）

| 消息 | 处理 |
| --- | --- |
| `WM_CREATE` | `MakeFonts()` + `SetTimer(h, 1, 250, nullptr)`（`671-674`） |
| `WM_TIMER` | `UiRefresh()`（`676-678`） |
| `WM_ERASEBKGND` | 返回 1（自绘，禁止系统擦背景，防闪烁，`680-681`） |
| `WM_DPICHANGED` | 更新 `gS` → `MakeFonts()` → **`Layout()`** → 重绘（`683-692`） |
| `WM_MOUSEMOVE` | 更新所有控件 `hover`；若正在拖滑杆则按鼠标位置写配置值（`694-719`） |
| `WM_LBUTTONDOWN` | `Hit` → 按类型分发（`721-749`） |
| `WM_LBUTTONUP` | `gDragging = -1`（`751-753`） |
| `WM_MOUSEWHEEL` | 滚动计数器列表（`755-762`） |
| `WM_PAINT` | 双缓冲整窗重绘（`764-852`） |
| `WM_CLOSE` | **托盘不可用时关闭 = 真退出**（见 §12.2） |
| `NP_WM_TRAY` | `AppTrayNotify(LOWORD(l))`（`864-866`） |
| `WM_COMMAND` | **`lParam == 0` 才处理**（托盘菜单派发，见 §12.4） |
| `WM_DESTROY` | `PostQuitMessage(0)` |

字体（`MakeFonts`，`ui.cpp:159`）：`Microsoft YaHei UI`，
标题 19 / 小节 14 / 正文 13 / 小字 11（都过 `S()`），
`ANTIALIASED_QUALITY` 灰阶抗锯齿 —— **深底上不会出现 ClearType 的彩色边缘**（`ui.cpp:161`）。

**滑杆坐标的坑**（`ui.cpp:706-709` 原话）：`WM_MOUSEMOVE` 里的 `120/130` 必须和
`WM_PAINT` 里画轨道用的 `sx = wd.x + S(120)`、`sw = wd.w - S(130)` **完全一致**。
原来拖拽用的是 130/140，于是**滑块永远比鼠标偏 10 像素**（拖到最右边也到不了 100%）。

---

## 12. 托盘与退出

### 12.1 托盘图标（`AddTray`，`main.cpp:603`）

* `NOTIFYICONDATAW`：`uID=1`、`uCallbackMessage = NP_WM_TRAY`（`WM_USER+100`）、
  图标 `IDI_APPLICATION`、提示文字手写截断到 127 字符（`main.cpp:612-613`）。
* `gTrayAdded` / `gTrayOk` 两个标志：后者是**对外可见的「托盘是否真的注册成功」**
  （`TrayAvailable()`，`main.cpp:653`）。
* **为什么失败也要留证据**（`main.cpp:617-622`）：托盘图标是「唯一退出入口」时的最后一根稻草 ——
  加不上必须写日志（`tray: NIM_ADD -> FAILED`），并且要 `SetNotice(...)` 告诉用户
  「关闭主窗口将直接退出程序（不再缩到托盘）」。

### 12.2 关闭按钮的两种语义（`ui.cpp:854-862`）

```cpp
if (!TrayAvailable()) { PostMessageW(h, WM_COMMAND, NP_TRAY_EXIT, 0); return 0; }  // 真退出
ShowWindow(h, SW_HIDE);                                                            // 缩到托盘
```

原话：托盘图标没注册成功时，「缩到托盘」就等于让用户再也找不到程序
（用户反馈：退出只能进任务管理器）。**那种情况下关闭 = 真退出。**

### 12.3 托盘菜单与命令

* `ShowTrayMenu`（`main.cpp:625`）：`显示/隐藏主窗口`、`叠加面板`（勾选项）、
  `监控中/已暂停`、`以管理员身份重启`（已是管理员时 `MF_GRAYED`）、`打开日志文件夹`、`完全退出`。
  两个细节：
  * **窗口隐藏时不能抢前台**（会失败并导致菜单首击只关闭菜单），只在可见时
    `SetForegroundWindow`（`main.cpp:641-642`）。
  * 经典修复：菜单收回后补一个 `WM_NULL`，否则部分系统上菜单项**第一次点击不生效**（`644-645`）。
  * 模态循环可能把 `WM_QUIT` 吃掉：若点了「完全退出」，菜单关闭后补发一次
    （`gWantQuit` → `PostQuitMessage(0)`，`647-648`）。
* `AppTrayNotify`（`main.cpp:655`）：右键/`WM_CONTEXTMENU` → 菜单；
  **左键单击 = 快速开关叠加**（`gOverlayOff` 取反，关掉时立即 `OverlaySetVisible(false)`）；
  左键双击 = 显示/隐藏主窗口。
  > 注意：左键把 `gOverlayOff` 从 true 翻回 false 时**不会立即显示**，
  > 要等下一个 120ms tick 由 `wantDesktop` 重新算出来（`main.cpp:1194-1213`）才出现。
* `AppTrayCommand`（`main.cpp:675`）：`NP_TRAY_SHOW` / `NP_TRAY_OVERLAY` /
  `NP_TRAY_MONITOR`（开启时顺带 `InjectorScanNow()`，关闭时隐藏叠加）/
  `NP_TRAY_ELEVATE`（`AdminRelaunch` 成功后 `CleanupAndExit` + `EndMenu` + `PostQuitMessage`）/
  `NP_TRAY_LOGDIR` / `NP_TRAY_EXIT`（同上，`EndMenu` 是**强制结束 `TrackPopupMenu` 的模态循环**
  让 `ShowTrayMenu` 走收尾，`main.cpp:708`）。

### 12.4 为什么托盘消息要在窗口过程里处理（`main.cpp:1008-1010`）

原话：`TrackPopupMenu` 的模态循环会把 `WM_COMMAND` **直接派发给窗口过程**，
在消息循环里拦截反而收不到菜单点击 —— **这就是之前「右键菜单点了没反应」的根因**。
对应 `ui.cpp:868-874`：只有 `l == 0`（`TrackPopupMenu` 传 0）才走 `AppTrayCommand`。

### 12.5 退出清理 `CleanupAndExit`（`main.cpp:718`）—— 顺序不能改

```
gCleanedUp 幂等保护                       // 720-721
① np::Etw().Stop()                       // 724（不显式停会留下同名会话，系统里积垃圾）
② cfg.quit = 1; AppPublish(); Sleep(900) // 727-729（**源码注释**写的是「钩子 worker 每 500ms
                                          //   轮询一次 quit 标志；900ms 等它完成自卸载，
                                          //   含 100ms 排空中窗」。⚠ 与钩子当前实现对不上：
                                          //   守卫线程实际是 `Sleep(200)`（`np_hook.cpp:2671`），
                                          //   自卸载是 `Sleep(150)` → `RestoreAllHooks()`
                                          //   → `Sleep(250)` 排空（`np_hook.cpp:2535-2538`）。
                                          //   900ms 对「200ms 轮询 + 400ms 排空」仍然够用，
                                          //   但**改钩子侧周期/排空时长时，必须回头核这个 900**）
③ 删托盘图标 Shell_NotifyIconW(NIM_DELETE) // 732
④ InjectorShutdown() -> OverlayShutdown() -> UiDestroy()   // 735-737
⑤ TelemetryCloseAll()                    // 740
⑥ UnmapViewOfFile + CloseHandle(Config/Sensors 两对)        // 741-744
⑦ npb::GfxShutdown(); gApp.hub.Shutdown() // 746-747
```

* **为什么必须先广播 `quit` 再 `Sleep(900)`**：钩子是别人进程里的线程，
  我们有句柄它就不会死；只有让它自己走 `SelfUnloadNow`（还原 vtable 补丁 + 注销 VEH）
  才是干净退出。**这就是「反复注入不闪退」的前提**（`src/hook/np_hook.cpp:2673`）。
* 共享内存对象的本体随**最后一个句柄**关闭而销毁（`main.cpp:739`）——
  所以主程序退出后钩子还能读到旧配置（这也是为什么 `quit` 必须先广播）。
* `WinMain` 退出路径：`CleanupAndExit(); SettingsSave(); return 0;`（`main.cpp:1223-1224`）。

---

## 13. 配置存取（`settings.cpp`）

* 路径：`%APPDATA%\NextPerf\config.json`（`SettingsPath`，`settings.cpp:12`），
  目录不存在时 `CreateDirectoryW` 现建；`SHGetFolderPathW` 失败则退化为当前目录下的
  `nextperf_config.json`（`19-21`）。
* 格式：极简 JSON（`src/common/np_json.h`），**没有第三方依赖**。
* `SettingsLoad`（`45`）：**先 `NPDefaultConfig(&gApp.cfg)`**（把默认值铺好），
  再逐键覆盖；文件不存在 / 解析失败都**静默保留默认值**（`48`、`51`）。
* `SettingsSave`（`138`）：每次**整体重写**，包含 `cfgVersion = 5`。
  调用点：启动时（`main.cpp:950`）、退出时（`1224`）、`A_SAVE`、
  添加/移除游戏、学习成功、`ForgetGameAt` 末尾。

### 13.1 持久化的字段（`SettingsSave`，`settings.cpp:141-179`）

`cfgVersion`、`counters`、`autoInject`、`bgColor`、`textColor`、`accentColor`、`warnColor`、
`scale`、`bgOpacity`、`textOpacity`、`offsetX`、`offsetY`、`fontHeight`、`graphHeight`、
`overlayMode`、`fpsCap`、`pollMs`、`updateHz`、`deepEngineHook`、`vtableProbe`、`simulate`、
`learnedAutoHook`，以及两个列表：

| 键 | 内容 | 说明 |
| --- | --- | --- |
| `games` | 手动添加的**完整路径**数组 | 保持老格式，零迁移风险（`settings.cpp:165`） |
| `learned` | 学习来的 **exe 名**数组 | 商店应用的路径读不到，只能按名字认（`172`） |

### 13.2 **不该持久化**的字段（改代码时的红线）

| 字段 | 为什么不能存 |
| --- | --- |
| `cfg.pauseHook` | **运行态**：由 `monitoring` 每 tick 推导（`main.cpp:515`）。存了会导致下次启动带着「暂停」状态 |
| `cfg.detachPid` | **一次性请求**：只在主程序运行期间有效，且 2.5 秒后自动清（§6.4） |
| `cfg.quit` | **一次性退出广播**：存了会让钩子下次注入立刻自卸载 |
| `cfg.magic/version/size` | 由 `NPDefaultConfig`/`NPClear*` 填，不属于用户配置 |
| `cfg.opacity` | 遗留字段，现固定 1.0（`np_common.h:164`） |
| `cfg.reserved[]` | 预留槽位 |
| `gApp.cfg.autoInject` 之外的会话状态 | `monitoring`、`notice`、`forePid`、`telSlots`、共享内存句柄、`games[].pid/injected/hooked` 全部是运行态 |

> 这里有一条**踩过的教训**（值得记住这类 bug 的形态）：`learnedAutoHook` 原来**读和写都没有**
> → 用户勾选后重启就丢（`settings.cpp:92-93`）；`bgColor` 原来只有写回、**没有读取** →
> 用户改了背景色重启就丢（`settings.cpp:73`，审计发现）。**加字段时读写两侧都要加。**

### 13.3 旧配置升级（`settings.cpp:57-70`，按 `cfgVersion` 一次性补位）

| 版本 | 动作 |
| --- | --- |
| `ver < 2` | 补勾 `NP_C_CHART_USAGE \| NP_C_CHART_FPS \| NP_C_CHART_LATENCY` |
| `ver < 3` | 补勾 `NP_C_GRAPH \| NP_C_CHART_FPS \| NP_C_CHART_LATENCY` |
| `ver < 4` | 补勾 `NP_C_CPU_CLOCK \| NP_C_CPU_POWER`（否则新开关对老用户是关的，会让人以为功能没做出来） |
| `ver < 5` | **清掉** `NP_C_CPU_BUSY \| NP_C_CPU_WAIT`（高级项默认不显示，用户要求；显式清位而不是把复选框删掉，用户仍可手动勾选；且只在 ver<5 时执行一次，不会把用户的勾选反复抹掉） |

另外两处强制行为：`gApp.cfg.bgColor = 0xFF000000u;`（背景固定纯黑，用户明确要求，
旧配置里的蓝黑色一律丢弃，`settings.cpp:71-72`）；`deepEngineHook` / `vtableProbe`
**缺键时按 1（开）**、`simulate` / `learnedAutoHook` / `autoInject` 缺键时按 0（关）。

---

## 14. 桌面叠加窗口（`overlay.cpp`）

窗口样式（`overlay.cpp:55-58`）：
`WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE` +
`WS_POPUP`，尺寸先给 8×8，随后 `UpdateLayeredWindow` 决定真实位置与大小。

| 样式 | 作用 |
| --- | --- |
| `WS_EX_LAYERED` | 允许 `UpdateLayeredWindow` 逐像素 alpha |
| `WS_EX_TRANSPARENT` | **鼠标穿透**（点得到下面的游戏） |
| `WS_EX_TOPMOST` | 压在普通窗口之上 |
| `WS_EX_TOOLWINDOW` | 不出现在 Alt+Tab / 任务栏 |
| `WS_EX_NOACTIVATE` | 不抢焦点 |

* `OverlayInit`（`45`）里**渲染器初始化失败必须 `DestroyWindow` 再 return false**
  （`60-68` 原话）：否则 `gHwnd` 一直非空，调用方（`OverlayInit` 返回 false 后）不会再调
  `OverlayShutdown`，这个分层窗口就**永远挂在桌面上**了，而且还会让 `OverlayUpdate`
  以为叠加层可用。
* `OverlayProc`（`34`）：`WM_CLOSE` / `WM_DESTROY` 直接吞掉（返回 0），
  保证这个窗口**只能由程序自己销毁**。
* `OverlayUpdate`（`85`）：
  1. 未创建或不可见 → 直接返回（不可见时**不做任何渲染**，省 CPU）。
  2. `npb::Factory() || npb::GfxInit()` 懒初始化。
  3. **按前台窗口所在显示器的 DPI 设置光栅倍率**（`89-95`：`SetRasterScale(dpi/96)`，
     `dpi < 96` 时按 96）—— 用物理像素光栅化，保证高分屏清晰。
  4. `BuildPanelData` → `Measure` → `npb::PanelBitmap::Render`（与游戏内叠加同一条通路）。
  5. 定位（`107-120`）：`MonitorFromWindow(前台窗口, MONITOR_DEFAULTTOPRIMARY)`，
     **右上角**：`x = rcMonitor.right - bw - offsetX*scale`，`y = rcMonitor.top + offsetY*scale`；
     `GetMonitorInfoW` 失败就退回 `SM_CXSCREEN/SM_CYSCREEN`。
  6. `UpdateLayeredWindow(..., ULW_ALPHA)` 提交。
* **透明度在哪做**（`overlay.cpp:125-130`）：背景/文字透明度已经做进**位图像素**里
  （`bgOpacity` / `textOpacity`），所以窗口整体 `SourceConstantAlpha = 255`，
  **不再整体降透明**。
* 适用边界（`overlay.cpp:1-5`）：无边框/窗口化游戏，或用户强制选择桌面叠加模式。
  **独占全屏下系统会绕过 DWM 合成，这类窗口会被盖住** —— 那种情况由注入钩子
  在游戏内部绘制。
* 显隐由主循环 `wantDesktop` 统一决定（§4.4），
  `OverlaySetVisible` 只做 `ShowWindow(SW_SHOWNOACTIVATE / SW_HIDE)`（`79-83`）。

---

## 15. 日志与用户提示

### 15.1 `AppLog`（`injector.cpp:72`）—— 主程序侧诊断日志

* 落盘位置：`%TEMP%\NextPerf.log`（`AppLogPath`，`65`）。
  游戏内的钩子日志是 `NextPerfHook.log`（另一个文件）。
* 每行格式：`[HH:MM:SS.mmm pid=NNNN] 正文`，同时 `OutputDebugStringA`。
* **为什么必须有它**（`injector.cpp:38-42` 原话）：注入失败的原因原来只写进 `gApp.statusText`，
  而主循环每 120ms 就把状态栏刷掉了 —— 用户只会看到「未注入」，永远不知道是权限不够、
  DLL 载入失败、还是目标根本不是 64 位。游戏进程里的钩子日志又只有在 DLL 真载入之后才有。
  **中间这一段（注入本身）原来完全没有记录。**
* **1MB 截断的坑**（`injector.cpp:90-105` 原话）：不能像原来那样
  `SetFilePointer(h, 0, FILE_BEGIN)` 就以为「从头覆盖」—— 这个句柄是用 `FILE_APPEND_DATA`
  （没有 `FILE_WRITE_DATA`）打开的，`WriteFile` 会**忽略文件指针**、永远写到文件末尾，
  于是日志无限增长，那句「超过 1MB 从头覆盖」从来没生效过。
  正确做法是**关掉重开、用 `GENERIC_WRITE` + `SetEndOfFile` 截断**，再继续按追加方式打开。
* 打开模式 `FILE_SHARE_READ | FILE_SHARE_WRITE`：多个进程（主程序 + 每个游戏里的钩子）
  可以同时写各自的日志文件而不互锁。

### 15.2 `SetNotice`（`injector.cpp:111`）—— 「上一次操作的结果」要能看见

```cpp
AppLock lk;                                    // ★ 必须加锁，见下
gApp.notice = text; gApp.noticeLevel = level;
gApp.noticeUntil = GetTickCount64() + noticeMs;   // 默认 12000ms（np_app.h:113）
AppLog("notice[%d]: %s", level, text.c_str());
```

* **为什么需要它**（`np_app.h:64-66`）：主循环每 120ms 刷一次状态栏，如果直接写 `statusText`，
  注入失败的提示会在 120ms 内被覆盖 —— **用户只会看到「未注入」，永远不知道原因（踩过）**。
* **为什么必须加锁**（`injector.cpp:112-113`）：守护线程（注入看护）也会调 `SetNotice`，
  而主线程每 120ms 读/清 `gApp.notice`。**两个线程同时碰一个 `std::string` 是 UB
  （SSO 缓冲与堆指针撕裂会崩）**，必须加锁。
* 消费方：`UpdateStatus`（`main.cpp:565-574`）—— notice 未过期就优先显示
  （错误级别加 `⚠ ` 前缀），过期才回落到常规状态文本，并顺手 `clear()`。

### 15.3 `UpdateStatus`（`main.cpp:548`）的四种状态

1. notice 优先（见上）。
2. `live`（帧数在增长）→ `状态 | 已接管 <进程名> (pid N) | <API> | xx FPS | CPU | GPU`。
3. 没接管但 `!injected.empty()` → **优先显示 `t.lastError`**（钩子即使挂不上 Present 也会
   回报「为什么」）；没有 lastError 就用 `HookHint(t.hookFlags)`：
   `flags == 0 → 钩子没起来`、`缺 NP_HOOK_PRESENT → Present 未挂上`、
   `缺 NP_HOOK_OVERLAY → 叠加资源未就绪`，否则 `等待首帧`（`main.cpp:541-546`）。
4. 都没注入 → `未注入 | 数据源：<hub.Describe()> | CPU | GPU`。

### 15.4 `AdminRelaunch`（`injector.cpp:121`）

`ShellExecuteW(nullptr, L"runas", exe, ...)`；返回值 `<= 32` 视为失败
（用户拒绝 UAC），此时 `SetNotice("已被拒绝提升权限（或 UAC 取消），程序继续以普通权限运行", 1)`
并返回 false —— **绝不强求**。

---

## 16. 重要的坑与教训（逐条）

> 这一节是本文档最值钱的部分。每条都来自源码注释，都是**真的踩过**的。

### 16.1 线程与内存

1. **`erase`/`push_back` 会让另一线程的迭代器失效 → 必须有 `AppLock`**。
   守护线程 800ms 一轮改 `gApp.injected`/`gApp.games`，主线程 120ms 一轮遍历它们。
   `vector::push_back` 扩容后另一边正在用的迭代器/引用就是野指针，**直接崩，而且崩的是主程序**
   （`np_app.h:129-133`）。所有跨线程访问点清单见 §4.3。CRITICAL_SECTION 可重入，
   同线程嵌套加锁安全。
2. **持锁不做慢操作**：`InjectInto` 里有最长 10 秒的 `WaitForSingleObject`，
   所以 `InjectorScanNow` 必须在锁外注入（`injector.cpp:566-571`）。
3. **两个线程碰同一个 `std::string` 是 UB**：`SetNotice` 必须加锁（`injector.cpp:112-113`）；
   ETW 的 `status()` 因此返回**拷贝**而不是引用（`np_etw.h:66-68`）。
4. **远端内存只在确认远端线程结束后才释放**，超时宁可不释放（泄漏一页）也不能让游戏读已释放页
   （`injector.cpp:419-428`）。
5. **32 位目标必须用它自己的 `LoadLibraryW`** —— 64 位地址在 32 位地址空间里不存在，
   远端线程起不来（`injector.cpp:161-165`）。所有 PEB 偏移按 32 位布局硬编码（§5.3）。
6. **`NPDefaultConfig` 的 `reserved` 循环越界**：`NPConfig::reserved` 声明为 `uint32_t reserved[4]`
   （`np_common.h:200`），但 `NPDefaultConfig` 里写的是 `for (int i = 0; i < 8; ++i) c->reserved[i] = 0;`
   （`np_common.h:438`）—— **多写 4 个 uint32，越过结构体尾部 16 字节**。
   调用方在本层（`SettingsLoad`、UI 的 `A_DEFAULT`、`RunSelfTest`），
   而 `AppState` 里 `cfg` 后面紧跟 `sensors`，所以受害的是 `NPSensors` 的头部字段。
   这是**静态可验证的越界写**（不是推测），见 §17。

### 16.2 时间与判活

7. **跨进程比较时钟不可靠**（本项目最贵的一类 bug）：`GetTickCount64() - t.tickMs` 曾经算出
   `age = 4294967265ms`（32 位下的 -31，`tickMs` 落在「未来」）→ `age < 2500` 永远为假 →
   桌面 HUD **永不关闭**。旧钩子实例残留、时钟回绕、共享内存里的历史值，
   任何一样都会让它变成垃圾。**判活一律用「帧计数是否在增长」**（`main.cpp:1154-1163`）。
   同样的修复也应用在 `UpdateStatus`（`551-560`）和陈旧遥测回收（`1171-1175`）。
8. **2.5 秒是统一的「钩子已死」阈值**（帧数不增长），出现在三处：`UpdateStatus`、
   overlay 判活、陈旧遥测回收。三处都改才叫改完。
9. **`detachPid` 必须带超时清理（2.5 秒）**，只在「pid 已退出」时清会导致
   「下次注入同一个游戏，钩子立刻又自卸载」（`injector.cpp:488-492`）。
10. **`GetTickCount()` 可能返回 0**：时间戳用 `GetTickCount() ? GetTickCount() : 1`
    （`injector.cpp:498`），0 是「没有请求」的保留值。

### 16.3 数据来源与显示

11. **`SensorHub::Poll` 会清空整个 `NPSensors`**（`np_sensors.cpp:1030`），
    所以 `AppPollSensors` 里任何「额外写进 sensors 的字段」都必须写在 `Poll` **之后**
    —— 显示模式写在前面会被覆盖成 0（`main.cpp:95-96`）。
12. **ETW 只覆盖 Low 帧，不覆盖帧率/帧时间/GPU 时间**：那些本来就已经对了，
    多覆盖一次只会把对的数弄坏（上一版就是这么翻车的，`main.cpp:129-130`）。
    而且 ETW 覆盖前**必须判 `pid != 0`**，否则会把全系统所有进程的 present 混出来的数字
    塞进面板（`133-135`）。
13. **帧数要取钩子与 ETW 的较大值**，否则非管理员时帧数恒 0 → GPU 帧时间永远 -1（`142-148`）。
14. **PDH 通配实例在 `AddCounter` 那一刻就冻结**：`\GPU Engine(*)` 用 `AddWildcard` 展开后，
    游戏后启动就读不到（先开游戏再开 NextPerf 有数据、反过来没有）。
    正确姿势是带 `*` 的计数器（每次取值都给动态实例列表）——
    `src/sensors/np_sensors.h:58-70` 有完整说明。
15. **顺序敏感**：`TryOpenTelemetry`/`PickTelemetry` 遍历 `gApp.injected`，
    必须和守护线程用同一把锁（`main.cpp:522-526`）。

### 16.4 UI

16. **加一行控件必须同步改 `kWinH`**：上一次加「自动注入 3D 窗口」那行忘了加高，
    把「退出监视」等按钮挤到窗口外面去了（用户实测，`ui.cpp:29-34`）。
    当前右列按钮底边 726 / 窗口高 730，**只剩 4 像素余量**（§11.3）。
17. **控件画在同一矩形上，点击会被先注册的吃掉**：`Hit` 按注册顺序返回第一个命中的。
    所以 `Add()` 里做了同类型重叠检测并 `AppLog`，**别让它再靠用户发现**（`ui.cpp:212-215`）。
18. **同一个尺寸只定义一次**：`kListH` 原来在 `Layout()` 和 `WM_PAINT` 里各写一遍 `S(452)`，
    改一处漏一处就会让列表和游戏区对不上（`ui.cpp:35-37`）。
19. **滑杆的拖拽坐标必须和绘制坐标完全一致**（120/130），否则滑块永远偏 10 像素、
    拖到最右边也到不了 100%（`ui.cpp:706-709`）。
20. **学习来的条目 `path` 为空**，列表显示必须回退到 `name`，否则列表里是空白
    （用户实测的 bug，`ui.cpp:615-618`）。
21. **托盘没注册成功时 `WM_CLOSE` 必须真退出**，否则用户找不到程序也没法退出，
    只能去任务管理器杀（`ui.cpp:854-861`、`np_app.h:34-36`）。
22. **`TrackPopupMenu` 的模态循环会吃掉 `WM_COMMAND`/`WM_QUIT`**：
    菜单命令必须在窗口过程里处理（这就是「右键菜单点了没反应」的根因），
    并且在菜单关闭后补 `WM_NULL`、必要时补发 `PostQuitMessage`（`main.cpp:644-648`、`1008-1010`）。

### 16.5 注入与钩子生命周期

23. **32 位进程加载不了 64 位 DLL（反之亦然）**，必须各用各的 → `NextPerfHook32.dll`（§5.3）。
24. **只有确认线程结束才能释放远程内存**（同 16.1 第 4 条，值得单列，因为后果是崩游戏）。
25. **清空学习名单必须先卸载钩子再删条目**，否则学习逻辑立刻把它学回来（`injector.cpp:502-504`）。
26. **`ForgetGameAt` 里 `pid == 0` 要现场查进程**：`InjectorScanNow` 在 `!monitoring` 时直接 return，
    pid 可能是 0，原来就「根本没请求卸载」（`injector.cpp:511-514`）。
27. **摘条目还要摘 `injected` 和 `telemetry`**，否则界面一直显示「已接管」（`injector.cpp:525-536`）。
28. **注入成功 ≠ 钩子接管**：`STILL_ACTIVE(259)` 不算失败；界面必须区分「已注入」与「读取中」，
    并把 `hookFlags`/`lastError` 摆出来（`main.cpp:541-546`、`ui.cpp:626-641`）。
29. **`--selftest`/`--uismoke` 必须 `ExitProcess` 硬退出**，否则残留进程锁住 `dist\NextPerf.exe`
    让后续构建失败（`main.cpp:847-851`）。
30. **日志「超 1MB 从头覆盖」用 `FILE_APPEND_DATA` 是无效的**，必须关掉重开 +
    `GENERIC_WRITE` + `SetEndOfFile`（`injector.cpp:90-105`）。

### 16.6 自动注入与反作弊

31. **不要靠窗口特征猜「是不是游戏」**：窗口化游戏与普通窗口属性无区别；UWP 前台是壳窗口；
    枚举目标模块打不开 UWP 甚至把游戏搞崩。实测曾把用户的**梯子、微信、NextPerf 自己**
    当成游戏注入，也误判过商店版生化危机8（`main.cpp:474-487`）。
    「添加游戏 exe」才是可靠答案（§9.4）。
32. **遇到反作弊自动放弃，绝不绕过**（`main.cpp:344-353`）：反作弊主动阻止外部注入是它们的
    设计目标，硬上既有封号风险、也不符合工具定位。两道判据：① 目标自身加载了反作弊模块
    （跨位数时枚举不到）；② **当前有反作弊进程在运行** —— 保守起见此时整轮都不自动注入。
    （注：该逻辑目前随 `AppAutoInjectTick` 一起停用，见 §9.4。）

### 16.7 配置

33. **加字段必须同时加读和写**：`learnedAutoHook` 原来读写都没有（勾选重启就丢）、
    `bgColor` 只有写没有读（改色重启就丢）（`settings.cpp:73`、`92-93`）。
34. **新增计数器位不要加进 `NP_ALL_COUNTERS`**，否则默认就显示（用户要求默认关）；
    `NP_C_CPU_BUSY/WAIT` 就是这么处理的（`np_common.h:93-96`）。

---

## 17. 已知缺口 / 待确认（**不要当成「已经没问题了」**）

| # | 位置 | 现状 | 影响 | 建议 |
| --- | --- | --- | --- | --- |
| G1 | `np_common.h:438` vs `:200` | `NPDefaultConfig` 的循环写 `reserved[0..7]`，但数组只有 4 个元素 —— **越界写 16 字节** | 调用点全在本层：`SettingsLoad`（每次启动）、UI `A_DEFAULT`、`RunSelfTest`。按字段宽度推算 `sizeof(NPConfig) = 136` 且尾部无填充，`AppState` 里 `sensors` 正好从偏移 136 开始 → 越界写的正是 `gApp.sensors` 的 `magic`(136-139)、`version`(140-143)、`tickMs`(144-151)。后果：在下一轮 `NPClearSensors` 之前，发布出去的 Sensors 是 `magic = 0`（读方钩子按 `magic == NP_MAGIC` 判断，会忽略该帧）。**静默、无崩溃、但语义被破坏** | 修数组大小或循环上界（**本次未改代码**）。属 common 层，改动需两侧回归；顺带核对 `sizeof(NPConfig)` 是否仍等于 136 |
| G2 | `main.cpp:1093-1107` | 学习成功的 `gApp.games.push_back` **没有 `AppLock`** | 与 `ui.cpp:483-488` 的规则自相矛盾；守护线程正在遍历 `games` 时会踩迭代器失效（崩主程序） | 补 `AppLock`（**本次未改**） |
| G3 | `ui.cpp:631` | 游戏列表状态仍用 `(GetTickCount64() - t.tickMs) < 2500` **跨进程时钟比较** | 回到 §16.2 第 7 条那个 bug：状态可能永远显示不正确（32 位钩子 tickMs 落在「未来」） | 改用与 `main.cpp` 一致的帧计数判据（**本次未改**） |
| G4 | `main.cpp:66` | 打开遥测块时**只校验 `magic`，没校验 `version`** | 结构体布局不一致时（旧 DLL 残留）会读到错位字节，且不会被发现 —— 这正是 `version = sizeof` 想防的事 | 加 `v->version == (uint32_t)sizeof(NPTelemetry)` 判断（**本次未改**） |
| G5 | `NPDefaultConfig`（`np_common.h:413-439`） | **没有设置** `autoInject` / `pauseHook` / `learnedAutoHook` / `detachPid` | 点「恢复默认」**不会**重置「实验性自动注入」等开关（保留了用户当前值） | 确认是「有意」还是漏了；若有意请在注释里写明（**未确认**） |
| G6 | `GameEntry::hooked`（`np_app.h:46`）、`AppState::overlayVisible/uiVisible/lastPoll/lastOverlay`（`np_app.h:60-61`、`101-102`） | **全代码库无任何读写**（死字段） | 无功能影响，但会误导后来者以为有状态机 | 清理或注明 |
| G7 | `cfg.autoInject`（`np_common.h:185`） | 只在 settings 里读写，**没有任何行为**（`AppAutoInjectTick` 是空的） | 界面已无对应开关，配置里留着一个永远无效的键 | 未来做启发式自动注入时可复用；现在请当作无效字段 |
| G8 | `NP_MUTEX_SENSORS`（`np_common.h:24`） | 定义了但**从未使用** | 写传感器无锁，靠「整块拷贝」约定 | 若将来要原子发布传感器，这是现成的名字 |
| G9 | `docs/ARCHITECTURE.md:195` | 记录的 overlay 判据还是旧的 `(now - tickMs) < 2500` | 会把读者带到 G3 那个坑 | 该文件不在本次改动范围，未修改；改它时以 §4.4 为准 |
| G10 | `main.cpp:313` 与 `385` | `ProcessIsWow64(pid)` 被调用**两次**（选 DLL 一次、解析 `LoadLibraryW` 前又一次） | 微小冗余，无正确性问题（期间目标位数不会变） | 可选优化 |
| G11 | `--inject` 的参数解析（`main.cpp:910-916`） | 只做「跳过非数字」，`--inject abc123` 也会注入 123 | 自动化脚本传错参数不会报错 | 需要严格解析时再改 |

---

## 18. 对未来的自己 / 其他迭代者的叮嘱

1. **改完代码必须同步更新 `docs/APP.md` 与 `docs/CATALOG.md`。**
   本文档的每个「为什么」都是从源码注释里抠出来的，源码改了注释没改、或文档没跟，
   下一个接手的人就会按错的说明改代码 —— 这个项目已经因为「文档/构建时间对不上」
   白排查过一整轮（`np_app.h:14-18`）。
   > `docs/CATALOG.md` **已经存在**，而且它的「功能文档索引」里已经挂了
   > [`APP.md`](APP.md) 与 [`APP-FUNCTIONS.md`](APP-FUNCTIONS.md) 两条 ——
   > 也就是说：**本层再改任何东西，都必须在 [`CATALOG.md`](CATALOG.md) 里同步**
   > （它的 §6 也是这么规定的）。要同步的是：新增文件（§3.1 文件清单）、
   > 新增功能（§2.1 / §2.5 功能定位表）、以及血泪教训（§5）。
   > ⚠ 另外：**[`CATALOG.md`](CATALOG.md) §3.1 里 `src/app` 各文件的行数是旧估值**
   > （写成 ~1000 / ~813 / ~587 / ~158 / ~111 / ~125，本文档撰写时的真实值是
   > 1226 / 939 / 728 / 184 / 135 / 177）。本次任务只被允许写 `APP.md` 与
   > `APP-FUNCTIONS.md`，所以没有动它 —— **请后续维护者顺手把那张表更新掉**，
   > 行数不对会直接毁掉「用行号定位」这件事。
2. **改任何跨线程共享的数据结构，先问「另一边在哪个线程、有没有拿 `AppLock`」**，
   然后按 §4.3 的清单逐点核对。这类 bug 的表现是**随机崩溃**，最难查。
3. **判活/判新鲜度一律不要用跨进程时间戳**，用帧计数增长（§8.4）。
4. **改 UI 布局：先算一遍底部边界，再动 `Layout()`**；加行必改 `kWinH`，
   然后在 `%TEMP%\NextPerf.log` 里搜 `UI 重叠警告`（重叠检测会替你抓同类型重叠）。
5. **改注入流程：任何失败路径都要 `AppLog` + `SetNotice`**（用户看不到原因就等于没有提示），
   并且**任何新加的等待都不要放进 `AppLock` 里**。
6. **改共享内存结构体：只在末尾加字段，或复用 `reserved[]`**；
   动了布局就要同步 `sizeof` 自校验的语义（§3.5）与读方的校验代码
   （顺带把 G4 补上）。
7. **新增配置字段：读、写、UI、升级逻辑四处都要动**（§13.2、§16.7 第 33 条）。
   运行态字段（暂停/退出/一次性请求）**绝对不要持久化**。
8. **不要复活启发式自动注入**（§9.4），除非你有一套不依赖「猜」的判据。
   要做自动钩子，请走「添加游戏 exe」那条路。
9. **每 5 秒的 `overlay mode:` 与「数据来源」日志是排查的第一手证据**，
   重构时不要顺手删；要删就先在本文档里记下替代方案。
10. **改了 `InjectorScanNow`/钩子的任何轮询周期**，回头核对 §6.4 的 2.5 秒契约
    （`detachPid` 超时、陈旧遥测阈值、overlay 判活都建立在「2.5 秒足够一轮」之上）。
11. **保留「宁可功能退化也不能崩游戏」的原则**：远端内存泄漏一页、
    10 秒超时不释放、反作弊直接放弃 —— 这些都是**有意的取舍**，不是忘了写。

---

## 19. 验证手段（仓库内现成脚本，`tests/`）

| 脚本 | 验的是什么（据其文件头说明） |
| --- | --- |
| `tests/verify_inject.py` | 注入链路：**先建交换链再注入**（复现用户报的「游戏已经在跑时注入」场景） |
| `tests/verify_x86_inject.py` | 32 位注入：最直接的证据是 `%TEMP%\NextPerfHook.log` 有没有出现 |
| `tests/verify_reinject.py` | 「注入 → 优雅退出主程序 → 再注入同一个进程」是否还闪退（判定性测试） |
| `tests/try_inject.py` | 对任意运行中的进程做一次真实注入，并把每一步错误码摊开 |
| `tests/shot_ui.py` | 截主窗口，肉眼对照 UI 改动（输出 `tests/_ui.png`） |
| `tests/shot_overlay.py` | 连拍叠加画面（颜色闪烁是逐帧现象，单张看不出来） |
| `tests/struct_check.py`、`tests/vt_check.py`、`tests/row_metrics_check.py` 等 | 结构体布局 / vtable 下标 / 面板行度量等静态核对 |

主程序自带的两条自检通道：`NextPerf.exe --selftest`（§2.3）、`NextPerf.exe --uismoke`（§2.4）、
以及脚本化的 `NextPerf.exe --inject <pid>`（§2.5）。

> 这些脚本我没有逐个逐行读完，上面只转述了各自文件头的自我说明。
