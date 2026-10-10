# NextPerf 数据结构逐字段说明（共享内存数据契约）

> **这份文档是契约本身。** `主程序（NextPerf.exe）` 与 `注入钩子（NextPerfHook.dll）`
> 是两个进程、两份二进制、可以**分别升级**。它们之间唯一的接口就是
> `src/common/np_common.h` 里的四个结构体和一堆宏。改结构体 = 改协议。
>
> 动任何字段之前，请先读 [§9 改字段的完整清单](#9-改字段的完整清单必读)。
> 公共层整体说明见 [`COMMON.md`](COMMON.md)，文件/功能总目录见 [`CATALOG.md`](CATALOG.md)。

**本文校准的源码版本**（SHA256 前 16 位，按本文写作时的 `src/common/` 实际内容）：

| 文件 | 行数（LF） | sha256[:16] |
| --- | --- | --- |
| `np_common.h` | 506 | `BBB2B37B53A0287D` |
| `np_stats.h` | 269 | `3D35E2CC4A23F2A3` |
| `np_panel.h` / `np_panel.cpp` | 130 / 545 | `BC18551444D18B75` / `78B50DEDF7467A5E` |
| `np_build.h` / `np_build.cpp` | 13 / 473 | `F9A86AAF9FE2D8D0` / `4575FE184FEAF4E6` |
| `np_json.h` | 218 | `FEA2F85281501ADA` |
| `np_bitmap.h` / `np_bitmap.cpp` | 52 / 102 | `EF1EFD398865C713` / `F9B16F379B7A94F3` |

> ⚠ **`np_common.h` 正在被并行修改。** 本文写作期间它被修复过一次
> （`NPDefaultConfig()` 的 `reserved[]` 越界写，见 §2.5）：行数 497 → 506，
> 哈希 `AF8B9CC5…` → `BBB2B37B…`；同时给容量宏补了 `u` 后缀
> （`4096u` / `512u` / `128u` / `24u` / `8u` / `256u`，**值不变**）。
> 上表是**修复后**的版本。若你读到本文时哈希不符，请以源码为准并回来更新本文。

> 行数注：部分 Windows 工具（`Get-Content | Measure-Object -Line`）会把 **LF-only 文件
> 的空行吃掉**，报出 344 之类的偏小数字。本文用的是 LF 换行符计数，与 `read`/`grep`
> 的行号一致。`CATALOG.md` 里 `np_common.h ~344` 是那个偏小口径，**以本文为准**。

---

## 1. 四块共享内存（三块数据 + 一块辅助）

| 共享内存名 | 结构体 | sizeof | 谁创建 | 谁写 | 谁读 | 内容 |
| --- | --- | --- | --- | --- | --- | --- |
| `Local\NextPerf_Config_v1` | `NPConfig` | **136** | 主程序 `CreateShm()` | 主程序（`AppPublish()` 每 tick memcpy 整个结构体） | 钩子 `Cfg()`；主程序自身（UI/叠加/`np_build`） | 勾选了哪些计数器、颜色、缩放、位置、刷新率、行为开关 |
| `Local\NextPerf_Sensors_v1` | `NPSensors` | **448** | 主程序 `CreateShm()` | 主程序 `SensorHub::Poll()` → `AppPublish()` | 钩子 `Sens()`；主程序自身 | CPU/GPU/显存/内存/NVAPI 域/硬件 RT·Tensor 快照 |
| `Local\NextPerf_Telemetry_v1_<pid>` | `NPTelemetry` | **55736** | **钩子进程自己** | 该进程的钩子 | 主程序（`AppState::telSlots`，按 pid 分别打开） | 帧时间环形缓冲、帧延迟、RT/Tensor、分辨率、AI 模块、诊断 |
| `Local\NextPerf_Injected_<pid>` | （无数据） | — | 钩子：`CreateMutexW(nullptr, TRUE, name)`，**持有到进程结束** | 钩子 | 主程序 `IsInjected()`（`OpenMutexW(SYNCHRONIZE, ...)`）、`tests/diag.py`、`tests/verify_inject.py`（钩子自卸载后这个互斥量会消失，用它当卸载证据） | 「这个进程真的载入了钩子」的证据（**不是**"注入成功"，注入成功但 DLL 早期 return 就不会有这个互斥量） |
| `Local\NextPerf_SensorsMutex_v1` | （无数据） | — | — | — | — | ⚠ `NP_MUTEX_SENSORS` **全项目没有任何使用点**（只有 `np_common.h` 里的定义），是历史遗留 |

设计要点：

* **Config / Sensors 是「一份」**：只有主程序采集，所有钩子只读。传感器（NVML / NVAPI /
  WMI / PDH）只在主程序初始化一次，多开游戏不会互相打架。
* **Telemetry 必须每个进程一份**。共用一个名字时，同时注入两个游戏的两个钩子会往同一块
  内存里互相覆盖，主程序读到的就是一锅粥 —— 所以名字里带 pid。
* 命名用 `Local\` 前缀（会话命名空间）：普通权限即可创建，不会被 UAC 完整性级别拦住；
  也天然隔离「不同登录会话」的多个实例。

### 1.1 遥测块命名规则

```c
// src/common/np_common.h
inline void NPTelemetryShmName(uint32_t pid, wchar_t* out, size_t n);
// 结果形如：Local\NextPerf_Telemetry_v1_12345
```

实现细节（都是踩过的坑，别"优化"掉）：

* **`n < 40` 直接输出空串**（静默失败，不写半个名字）。名字 = 前缀 28 字符 + 最多 11 位
  十进制 + `\0` = 40，正好卡在边界上。所有调用点都传 64（钩子 `OpenIpc()`、
  主程序 `TryOpenTelemetry()`、`tests/diag.py`）。
* **自带十进制转换，不用 `swprintf`**。这个头被很多 TU 包含，`swprintf` 会拖进格式化
  代码与 locale 依赖；钩子 DLL 越薄越好。
* `pid == 0` 输出 `..._0`（不会输出空后缀）。
* 数字是**逆序取余再回填**，`digits[12]` 够用（uint32 最多 10 位，`d < 11` 已封顶）。
* pid 的顺序按 `uint32_t` 十进制打印 —— **主程序侧必须用同一个 pid 值**去 `OpenFileMappingW`，
  两者不存在任何解析歧义。

### 1.2 布局自校验（`version == sizeof(...)`）—— 现状与真相

`NPTelemetry` 里的注释写得很清楚：*曾经往中间插了一个字段而 Python 的 ctypes 镜像没同步，
结果读到的全是错位字节（钩子明明工作正常却报「未挂上」）*。于是有了这套自校验：

| 结构体 | `version` 写入的值 | 写入位置 | 现在真正校验它的地方 |
| --- | --- | --- | --- |
| `NPTelemetry` | `sizeof(NPTelemetry)` = 55736 | `NPClearTelemetry()` | **只有** `tests/verify_inject.py:167`（`t.version != TEL_SIZE` 就 assert 失败） |
| `NPSensors` | `sizeof(NPSensors)` = 448 | `NPClearSensors()` | **没有任何地方**（见下） |
| `NPConfig` | **硬编码 `1`**（不是 sizeof） | `NPDefaultConfig()` | **没有任何地方**；`NPConfig` 的自校验字段其实是 `size`（= `sizeof(NPConfig)` = 136） |

⚠ **必须知道的三件事：**

1. **C++ 侧没有任何读方校验 `version` / `size`。** `钩子 Cfg()` 只看 `magic`；
   `钩子 Sens()` 只看 `magic` + `valid`；主程序 `TryOpenTelemetry()` / `PickTelemetry()`
   也只看 `magic`。所以「自校验」目前是**给测试脚本和未来的读者准备的**，
   不是运行时防线。**不要以为写了 `sizeof` 就安全了。**
2. **`NPSensors` 这一路（主程序写 / 钩子读）曾经完全没有自检**：原来写的是
   `s->version = 1`，两边布局错位也照样跑（见 `CODE-REVIEW-2026-10-09.md`）。
   已改为 `sizeof`，但**读方仍然不比对** —— 想让它真正生效，得在 `Sens()`/`Cfg()` 里加一句。
3. **`NPConfig` 的自校验只能用 `size`**，因为 `version` 恒为 1（历史如此，没改成 sizeof）。
   如果将来要统一，注意 `version` / `size` 还出现在另外两份镜像里
   （`tests/diag.py` 的 `CFG_FIELDS`、`DATA-STRUCTS.md` 本文），别只改一头。

**为什么「保持 sizeof 不变」是硬要求**：主程序与钩子 DLL 是**两份可以任意组合的二进制**。
用户机器上完全可能出现「新版 NextPerf.exe + 旧版 NextPerfHook.dll」（DLL 被游戏进程占着
没更新、或用户手工替换了 `dist\` 里的一个文件）。此时：

* 结构体**大小不变、字段只从 `reserved[]` 里挪** → 旧 DLL 读到的仍是合法字节，
  只是不认识新字段（拿到 0 / 旧语义），不会错位、不会崩。
* 结构体**中间插字段 / 改大小** → 两边对同一段内存的解释不同，读出来的是错位字节：
  帧率可能是 0，可能是天文数字，而且**不报任何错**。这类问题历史上查了很久。

---

## 2. `NPConfig` —— 配置块（主程序写，钩子读）

```c
// src/common/np_common.h  struct NPConfig
// sizeof = 136（x64 / MSVC 与 mingw 一致的默认对齐规则）
```

### 2.1 内存布局（实测：用 ctypes 按平台 ABI 复现同一字段序列，`sizeof == 136`）

| offset | 字段 | 类型 |
| --- | --- | --- |
| 0 | `magic` | uint32 |
| 4 | `version` | uint32 |
| 8 | `size` | uint32 |
| 12 | *(4 字节填充)* | — |
| 16 | `counters` | uint64 |
| 24 | `bgColor` | uint32 |
| 28 | `textColor` | uint32 |
| 32 | `accentColor` | uint32 |
| 36 | `warnColor` | uint32 |
| 40 | `scale` | float |
| 44 | `opacity` | float |
| 48 | `bgOpacity` | float |
| 52 | `textOpacity` | float |
| 56 | `offsetX` | int32 |
| 60 | `offsetY` | int32 |
| 64 | `fontHeight` | uint32 |
| 68 | `graphHeight` | uint32 |
| 72 | `overlayMode` | uint32 |
| 76 | `fpsCap` | uint32 |
| 80 | `pollMs` | uint32 |
| 84 | `updateHz` | uint32 |
| 88 | `deepEngineHook` | uint32 |
| 92 | `vtableProbe` | uint32 |
| 96 | `quit` | uint32 |
| 100 | `simulate` | uint32 |
| 104 | `autoInject` | uint32 ← 原 `reserved[0]` |
| 108 | `pauseHook` | uint32 ← 原 `reserved[1]` |
| 112 | `learnedAutoHook` | uint32 ← 原 `reserved[2]` |
| 116 | `detachPid` | uint32 ← 原 `reserved[3]` |
| 120 | `reserved[4]` | uint32 × 4（**只剩 4 个槽位**） |

> `counters` 是 `uint64_t`，所以 offset 12 有 4 字节填充 —— 这也解释了为什么
> `tests/diag.py` 里手算偏移的代码必须做 `align = min(sz, 8)` 对齐。

### 2.2 逐字段表

「进 JSON?」= 是否由 `settings.cpp` 持久化到 `%APPDATA%\NextPerf\config.json`。

| 字段 | 类型 | 默认值 | 含义 | 谁写 | 谁读 | 进 JSON? |
| --- | --- | --- | --- | --- | --- | --- |
| `magic` | uint32 | `NP_MAGIC` = `0x4E505231`（"NPR1"） | 结构体标识，读方第一道校验 | 主程序 `NPDefaultConfig()` + 每次发布 | 钩子 `Cfg()`；主程序 | ❌ **不应持久化** |
| `version` | uint32 | `1` | 版本号；**注意不是 `sizeof`** | 同上 | 目前**无人读** | ❌ 不应持久化 |
| `size` | uint32 | `sizeof(NPConfig)` = 136 | 布局自校验（`NPConfig` 走这里） | 同上 | 目前**无人读**（测试也没查） | ❌ 不应持久化 |
| `counters` | uint64 | `NP_ALL_COUNTERS` | 勾选的计数器位组合，见 [§7.2](#72-np_counter--计数器位) | `ui.cpp` 复选框点击；`settings.cpp` 载入 | `np_build.cpp` 每一行的 `if`；钩子侧经 `Cfg()` 快照 | ✅ `counters` |
| `bgColor` | uint32 | `0xFF000000`（纯黑） | 面板底色 0xAARRGGBB | `settings.cpp` **强制**写成纯黑（用户要求）后允许 JSON 覆盖 | `np_panel.cpp` `Render()`（唯一真正使用的颜色） | ✅ `bgColor` |
| `textColor` | uint32 | `0xFFFFFFFF` | 正文字色 | `settings.cpp` 载入 | **无人读**（渲染器用 Metal HUD 固定配色，见 §8.3） | ✅ `textColor` |
| `accentColor` | uint32 | `0xFF5AC8FA` | 数值高亮色 | `settings.cpp` 载入 | **无人读** | ✅ `accentColor` |
| `warnColor` | uint32 | `0xFFFF6B5B` | 告警色 | `settings.cpp` 载入 | **无人读** | ✅ `warnColor` |
| `scale` | float | `1.0f` | 整体缩放，UI 允许 0.75~2.0 | `ui.cpp` 滑杆「缩放」 | `np_panel.cpp` `Layout()` / `Measure()` / `Render()`；`overlay.cpp`、钩子算偏移 | ✅ `scale` |
| `opacity` | float | `1.0f` | **旧字段，现固定 1.0，全项目无人读** | 仅 `NPDefaultConfig()` | 无人 | ❌（也不在 JSON 里，等于死字段） |
| `bgOpacity` | float | `0.72f` | 背景不透明度（与文字独立） | `ui.cpp` 滑杆 | `np_panel.cpp` `Render()`：`bg.a * c.bgOpacity` | ✅ `bgOpacity` |
| `textOpacity` | float | `1.0f` | 文字/内容不透明度 | `ui.cpp` 滑杆 | `np_panel.cpp` `Render()`：`alpha` 乘进每个画刷 | ✅ `textOpacity` |
| `offsetX` | int32 | `16` | 相对**右上角**的水平偏移（逻辑像素；桌面叠加会再乘光栅倍率） | `ui.cpp` 滑杆（0~400） | `overlay.cpp` 定位；钩子内部定位 | ✅ `offsetX` |
| `offsetY` | int32 | `16` | 相对显示器顶边的垂直偏移 | 同上 | 同上 | ✅ `offsetY` |
| `fontHeight` | uint32 | `15` | **逻辑像素字号**；实际字号 = `fontHeight * scale` | `ui.cpp` 滑杆（10~24） | `np_panel.cpp` `EnsureFormats((float)c.fontHeight * c.scale)` | ✅ `fontHeight` |
| `graphHeight` | uint32 | `64` | 图表区高度基准；实际 `chartH = max(11, graphHeight * 0.26) * scale` | `ui.cpp` 滑杆（30~140） | `np_panel.cpp` `Layout()` | ✅ `graphHeight` |
| `overlayMode` | uint32 | `0` | `0`=自动（优先游戏内） `1`=强制游戏内 `2`=强制桌面叠加 | `ui.cpp` 循环控件 | **钩子**：`wantOverlay = ... && cfg.overlayMode != 2` **且** 主程序：`overlayMode == 2 \|\| (!attached && overlayMode != 1)` 决定是否显示桌面叠加 | ✅ `overlayMode` |
| `fpsCap` | uint32 | `0` | 参考帧率上限（本意是图表 Y 轴），**当前无人读** | 仅 `settings.cpp` | 无人 | ✅ `fpsCap` |
| `pollMs` | uint32 | `500` | 传感器轮询间隔 ms（UI 里可选 200/350/500/1000）。**当前无人读**：主程序是每 120ms 一个 tick、无条件调 `AppPollSensors()`（`AppState::lastPoll` 声明了也没人用） | 仅 `settings.cpp` | **无人读** | ✅ `pollMs` |
| `updateHz` | uint32 | `20` | 叠加刷新率上限（UI 可选 60/30/20/10） | `ui.cpp` 循环控件 | **钩子**：`hz = cfg.updateHz ? cfg.updateHz : 20`（游戏内叠加的重绘间隔）。⚠ 主程序**不读**它（桌面叠加有自己的节流） | ✅ `updateHz` |
| `deepEngineHook` | uint32 | `1` | 启用命令列表级钩子（Draw/Dispatch/RT 计数） | `ui.cpp` 循环控件 | 钩子安装命令列表钩子前判断 | ✅ `deepEngineHook` |
| `vtableProbe` | uint32 | `1` | 注入后自己造一条 8×8 隐藏交换链拿 `Present` 的 vtable（游戏已经在跑时工厂钩子不会再被调用） | `ui.cpp` 循环控件 | 钩子探测线程 | ✅ `vtableProbe` |
| `quit` | uint32 | `0` | `1` = 主程序正在退出，钩子收到后 `SelfUnloadNow()` | `main.cpp` 退出路径 | 钩子守卫线程 | ❌ **绝不持久化** |
| `simulate` | uint32 | `0` | `1` = 模拟数据（开发预览） | `ui.cpp` 循环控件 | `np_build.cpp`（`active = hooked \|\| simulate`） | ✅ `simulate` |
| `autoInject` | uint32 | **`NPDefaultConfig()` 不设置**（实际靠 `gApp` 全局零初始化 = 0） | 原意「检测到 3D 窗口时自动注入」。**功能已废弃**：`AppAutoInjectTick()` 现在是**故意留空的空函数**（曾把梯子/微信/自己误判成游戏） | 仅 `settings.cpp` | **无人读** | ✅ `autoInject`（但已无意义） |
| `pauseHook` | uint32 | **不设置**（=0） | `1` = 主程序停止监视，钩子跳过叠加绘制与遥测更新（= 停止读游戏数据），只留最小心跳 | `main.cpp` `AppPublish()` **每 tick** 同步 `monitoring` 状态 | 钩子 `PresentCommon` | ❌ **绝不能持久化** |
| `learnedAutoHook` | uint32 | **不设置**（=0） | 「学习来的条目」要不要自动注入。**手动添加的条目不受此开关影响，一直自动注入** | `ui.cpp` 循环控件「实验性自动注入」 | `injector.cpp` 扫描时 `if (g.learned && !cfg.learnedAutoHook) continue;` | ✅ `learnedAutoHook` |
| `detachPid` | uint32 | **不设置**（=0） | 非 0 且 **等于钩子自己的 pid** → 钩子干净自卸载（还原 vtable 补丁 + 注销 VEH） | `injector.cpp` `RequestHookDetach()`；用完由注入器清零（约 2.5 秒后无条件清零） | 钩子守卫线程 | ❌ **绝不能持久化** |
| `reserved[4]` | uint32×4 | 0 | 预留槽位。**加字段必须从这里挪**（保持 sizeof 不变） | `NPDefaultConfig()`（用 `sizeof(reserved)/sizeof(reserved[0])` 推导，见 §2.5） | 无人 | ❌ |

### 2.3 从 `reserved[]` 挪出来的四个字段（历史与规则）

`NPConfig` 原本以 `uint32_t reserved[8]`（32 字节）结尾 —— 这从 `NPDefaultConfig()` 里
残留的 `for (i = 0; i < 8; ...)` 循环可以确认。为了在不改变 `sizeof` 的前提下
新增运行时开关，**依次**把 `reserved[0..3]` 改名成了四个真字段（`reserved` 缩小到 `[4]`，
但那个循环忘了跟着改，见 §2.5）：

| 原槽位 | 新字段 | 为什么必须复用槽位 |
| --- | --- | --- |
| `reserved[0]` | `autoInject` | 见下 |
| `reserved[1]` | `pauseHook` | 同上 |
| `reserved[2]` | `learnedAutoHook` | 同上 |
| `reserved[3]` | `detachPid` | 同上 |

源码注释写得很直白：「**复用原来的 `reserved[0]` 槽位 —— `NPConfig` 的大小完全不变，
共享内存的 `version(=sizeof)` 自检不受影响，旧钩子 DLL 也不会失配。**」

**规则（写给未来的自己）：**

1. 新增 `NPConfig` 字段 → **只能**从 `reserved[]` 里挪，并且**同步把 `reserved` 的维度改小**。
2. `reserved` 现在**只剩 4 个槽位（16 字节）**。用完之后，下一次加字段就**必须**真的
   改变 `sizeof` —— 那时：
   * 主程序与钩子 DLL **必须同时升级**（`dist\` 里两个文件一起换）；
   * 才第一次真正需要读方比对 `size`/`version`（现在是靠人肉保证，见 §1.2）；
   * 建议改成写一个**递增的布局版本号**而不是 `sizeof`，这样「同样大小、不同排列」
     这种最阴险的情况也能被抓到（`struct_check.py` 的注释专门讲过这种情形）。
3. 字段**语义**可以不变地扩展（例如 `pauseHook` 从「暂停绘制」扩到「连遥测也停」），
   但不能把 `0`/`1` 的含义反过来 —— 旧 DLL 还在读。

### 2.4 哪些字段**不该**持久化到 config.json（以及为什么）

`SettingsSave()` 只写出它显式列出的键。下面这些**必须留在共享内存 / 运行时里**：

| 字段 | 为什么不能进 JSON |
| --- | --- |
| `magic` / `version` / `size` | 每次启动由 `NPDefaultConfig()` 重设。持久化毫无意义，而且一旦 JSON 被手工改成别的 `magic`，钩子会**拒绝整块配置**（`Cfg()` 回退到默认值）—— 表现为「所有设置失效」，极难查。 |
| `quit` | 一次性退出信号。若被持久化成 1，**下次启动钩子会立刻自卸载**，游戏里永远看不到面板。 |
| `pauseHook` | 它是「主程序当前是否在监视」的**运行时镜像**（`AppPublish()` 每 tick 覆盖）。持久化成 0 还好，持久化成 1 就等于「重启后钩子躺平不画」；而且它会与 UI 的监视开关**互相打架**（到底听谁的？）。 |
| `detachPid` | 一次性卸载请求。源码注释专门讲过这个坑：*不能只在「pid 已退出」时清 —— 游戏还活着的话 `detachPid` 会一直留着，下次再注入这个游戏，钩子一看 `detachPid == 自己` 就立刻又自卸载*。持久化会让这个坑变成永久性故障。 |
| `reserved[]` | 预留位的值没有语义，写进 JSON 只会在未来某天被误当成真字段。 |
| `opacity` | 已经不读出（渲染器只用 `bgOpacity`/`textOpacity`）。 |

**另外两个「该持久化但曾经漏了」的历史事故**（`CATALOG.md` 血泪教训 3）：
`learnedAutoHook` 曾经读和写都没有 → 用户勾选后重启就丢；
`bgColor` 曾经只有写没有读 → 用户改了背景色重启就丢。**加配置项必须同时加「存」和「取」。**

### 2.5 ✅ 曾经的缺陷：`NPDefaultConfig()` 越界写 16 字节（**已修复**）

**修复前的代码**（历史，别再写回去）：

```c
for (int i = 0; i < 8; ++i) c->reserved[i] = 0;   // ← reserved 已经只剩 [4] 了！
```

`reserved` 从 `[8]` 被逐步缩到 `[4]`（槽位被 `autoInject` / `pauseHook` /
`learnedAutoHook` / `detachPid` 用掉），但这个循环**没跟着改**，
于是每次调用都会**往 `NPConfig` 之后多写 16 个字节的 0**。影响面（都是真实调用点）：

| 调用点 | 对象 | 后果 |
| --- | --- | --- |
| 钩子 `Cfg()` | `NPConfig c{}` **栈上局部变量**（每帧都会调一次） | 每次都在栈上越界写 16 字节零 —— 真实的栈越界写，只是"紧随其后的栈槽恰好还没被用到" |
| `settings.cpp` `SettingsLoad()` | `&gApp.cfg`（`AppState` 的第一个成员） | 越界 16 字节正好落进 `AppState::sensors` 的头部（`magic` + `version` + `tickMs`，共 16 字节）→ 主程序发布的 `Sensors` 块 `magic = 0`，**被钩子直接忽略**（不崩，但传感器全丢） |
| `ui.cpp`「恢复默认设置」 | `&gApp.cfg` | 同上 |
| `main.cpp` 自检 | `&gApp.cfg` | 同上 |

**现在的写法**（`np_common.h`，已修复）：

```c
// ★ 必须**跟着数组实际大小**清，不能写死数字。
//   这里曾经是 `for (int i = 0; i < 8; ++i)`，而 reserved 已被逐步缩小到 [4] …
//   由审计（子代理通读 np_common.h）发现。写成 sizeof 推导，以后改大小不会再犯。
for (size_t i = 0; i < sizeof(c->reserved) / sizeof(c->reserved[0]); ++i) {
    c->reserved[i] = 0;
}
```

**教训（比 bug 本身重要）**：凡是"清空/初始化某个数组"的循环，
**一律用 `sizeof(arr) / sizeof(arr[0])` 推导，不要写死数字** ——
结构体的字段会被后来的迭代者搬来搬去（这个 `reserved` 就被搬过 5 次），
写死的上限不会跟着变，而且这类越界**不一定崩**，只是悄悄把邻居清零。

**另一个相关的小行为（仍在）**：`ui.cpp` 的「恢复默认设置」调 `NPDefaultConfig(&gApp.cfg)`，
但该函数不碰 `autoInject` / `pauseHook` / `learnedAutoHook` / `detachPid`，
所以**「恢复默认」不会把 `learnedAutoHook` 复位成 0**。这算 bug 还是 feature 未确认
（用户没有明确要求），但要知道。

**注释里的预告**：修复后的注释提到槽位曾被
`autoInject / lowStrict / pauseHook / learnedAutoHook / detachPid` 使用 ——
其中 **`lowStrict` 这个字段当前并不存在**（`NPConfig` 里没有它）。
这多半是"Low 帧（严格）"开关的预留名（钩子注释里提到过这个开关）。
**真要加它**：从 `reserved[]` 挪一个槽位（现在只剩 4 个）→ 同步 `NPClearConfig` →
`ui.cpp` 加控件 → `settings.cpp` 加存/取 → 同步 `tests/verify_inject.py`（若涉及遥测）、
`tests/diag.py` 的 `CFG_FIELDS` → 更新本文与 `COMMON.md` → 跑 `tests/struct_check.py`。

---

## 3. `NPSensors` —— 传感器快照（主程序写，钩子读）

```c
// src/common/np_common.h  struct NPSensors
// 字段宽度合计 444 字节，sizeof = 448（尾部 4 字节填充，因为 tickMs 是 uint64 → 对齐 8）
```

### 3.1 哨兵值约定（**全项目统一，别自己发明**）

| 哨兵 | 含义 | 谁负责写 |
| --- | --- | --- |
| `-1.0f` | 「读不到 / 该数据源不可用」 | `NPClearSensors()`，以及各 Poll 分支的失败路径 |
| `-273.0f` | 「温度读不到」（物理上不可能的温度；比 `-1` 更"刺眼"，一眼能看出是哨兵） | `NPClearSensors()`：`cpuTemp` / `gpuTemp` / `gpuHotspot` / `gpuMemTemp` |
| `0` | 「未知 / 不适用」：`cpuCores` / `cpuThreads` / `gpuVendor` / `screenW` / `screenH` / `refreshHz` / `sources` / `domExtPresent` / `hwRtTensorSrc` | `NPClearSensors()`、`NPDefaultConfig` 语义 |
| 空串 | 字符串字段未填：`gpuName[0] = 0` / `sourceText[0] = 0` | `NPClearSensors()` |

`np_build.cpp` 的格式化函数就靠这些哨兵决定显示 `—`：
`Num()` 判 `< -1000`、`Cv()` 判 `< -200`、`Wv()`/`MHzv()`/`GBv()` 判 `< 0`。

### 3.2 内存布局（关键偏移）

| offset | 字段 | 说明 |
| --- | --- | --- |
| 0 / 4 / 8 / 16 | `magic` / `version` / `tickMs` / `valid` | `tickMs` 是 `uint64`，所以 `version` 之后有 4 字节填充 |
| 56 | `gpuName[128]` | UTF-8，`NP_NAME_LEN` = 128 |
| 316 | `sourceText[128]` | UTF-8，人读的源描述 |
| 444 → 448 | （尾部填充） | `sizeof(NPSensors)` = **448** |

### 3.3 逐字段表

| 字段 | 类型 | 清空值 | 含义 | 谁写 |
| --- | --- | --- | --- | --- |
| `magic` | uint32 | `NP_MAGIC` | 结构体标识 | `NPClearSensors()`（写方 `SensorHub::Poll()` 开头） |
| `version` | uint32 | `sizeof(NPSensors)` = 448 | 布局自校验（写进去了，但**读方目前不比对**，见 §1.2） | 同上 |
| `tickMs` | uint64 | 0 | 最近一次采集时间（`GetTickCount64`）。⚠ **跨进程只能判"是否变化"，不要拿主程序的钟去减它**（不同进程的 tick 基准可能差很远） | 同上 |
| `valid` | uint32 | 0 → `Poll()` 置 **1** | `1` = 本块已填充。钩子 `Sens()` 要求 `magic` 正确**且** `valid != 0` 才采用共享内存里的值，否则回退到 `NPClearSensors()` 的空快照 | `SensorHub::Poll()` |
| **CPU** | | | | |
| `cpuUsage` | float | -1 | CPU 总占用率 %（`GetSystemTimes` 差分） | `PollCpu()` |
| `cpuTemp` | float | -273 | CPU 温度 ℃（HWiNFO 共享内存 / WMI ACPI 兜底） | `PollCpu()` / `PollHwinfoExtras()` |
| `cpuPower` | float | -1 | CPU 包功耗 W（EMI / HWiNFO），0 = 不可用 | `PollHwinfoExtras()` 等 |
| `cpuClock` | float | -1 | CPU 当前频率 MHz。**优先 `CallNtPowerInformation`**（每核实测取最高核心）；回退 PDH「标称 × 性能百分比」会**系统性偏低** | `PollCpu()`；内部标志 `cpuFreqFromNt_` 记录走的是哪条路 |
| `cpuCores` / `cpuThreads` | uint32 | 0 | 物理核 / 逻辑线程数 | `PollCpu()` |
| **内存** | | | | |
| `ramUsedGB` / `ramTotalGB` | float | -1 | 内存用量 / 总量 GB（`GlobalMemoryStatusEx`） | `PollRam()` |
| `ramPct` | float | -1 | 内存占用 % | `PollRam()` |
| **GPU** | | | | |
| `gpuName` | char[128] | 空 | 显卡型号（UTF-8）。来源优先级：NVML 全名 → ADL → DXGI（跨厂商兜底） | `PollGpuNvidia()` / `PollGpuAmd()` / `PollGpuGeneric()` |
| `gpuVendor` | uint32 | 0 | `0` 未知 `1` NVIDIA `2` AMD `3` Intel | 各 Poll 分支 |
| `gpuUsage` | float | -1 | GPU 占用 %。**按"最忙引擎"取，不跨引擎求和**（求和会恒等于 100%） | `PollGpuNvidia()` / `PollGpuGeneric()` |
| `gpuTemp` | float | -273 | GPU 核心温度 ℃ | 各分支 |
| `gpuHotspot` | float | -273 | GPU 热点温度 ℃ | NVML / HWiNFO |
| `gpuMemTemp` | float | -273 | 显存结温 ℃ | NVML / HWiNFO |
| `gpuPower` | float | -1 | GPU 当前功耗 W | NVML / ADL |
| `gpuPowerLimit` | float | -1 | GPU 功耗上限 W。**面板不显示**（用户明确说"不需要读取最大功耗"），字段保留 | NVML |
| `gpuClock` | float | -1 | 核心频率 MHz | NVML → ADL → PDH 依次兜底 |
| `memClock` | float | -1 | 显存频率 MHz | 同上 |
| `gpuFanPct` | float | -1 | 风扇转速 % | NVML / ADL |
| `gpuFanRpm` | float | -1 | 风扇转速 RPM（`gpuFanPct` 不可用时用它） | NVML `GetTachReading` |
| **显存** | | | | |
| `vramUsedGB` / `vramTotalGB` / `vramPct` | float | -1 | 显存用量 / 总量 / 占比 | NVML / DXGI（总量）+ PDH（用量） |
| **NVAPI 利用率域**（NVIDIA 独占，其他厂商恒 -1） | | | | |
| `domGpu` | float | -1 | 图形引擎占用 % | `PollGpuNvidia()`（NVAPI `GetDynamicPstatesInfoEx`） |
| `domFb` | float | -1 | 显存控制器占用 %（≈ 带宽占用） | 同上；面板显示为「显存带宽」 |
| `domVid` | float | -1 | 视频引擎占用 % | 同上；面板显示为「视频引擎」 |
| `domBus` | float | -1 | PCIe 总线占用 % | 同上；面板显示为「PCIe 总线」 |
| `domExt[4]` | float[4] | -1 | **未公开的 4~7 号域**：程序持续探测，有值即视为厂商新增域 | `PollGpuNvidia()`（仅当驱动真的分发出来） |
| `domExtPresent` | uint32 | 0 | bit0..3 对应 `domExt[0..3]` 是否有值 | 同上。**面板/UI 不读，仅诊断** |
| **硬件级 RT / Tensor** | | | | |
| `hwRtPct` | float | -1 | RT 单元负载 %（HWiNFO 共享内存 / LibreHardwareMonitor WMI 提供时才有） | `ProbeRtTensorHardware()` |
| `hwTensorPct` | float | -1 | Tensor/AI 单元负载 % | 同上 |
| `hwRtTensorSrc` | uint32 | 0 | 上面两个值的来源（`NP_SENSOR_SRC` 位组合：`HWINFO` / `LHM`） | 同上。**当前无人读**（面板只判断 `>= 0`） |
| **按游戏进程读的 GPU 引擎数据**（PDH，不需要管理员，驱动报的数） | | | | |
| `gpuBusyMs` | float | -1 | **每帧真正的 GPU 执行时间** ms（`\GPU Engine(...)\Running Time` 差分 ÷ 帧数）。与锁帧 / Reflex / 多线程提交全都无关 —— 这是「GPU 帧时间」的首选来源 | `SensorHub::PollGameGpu()`（由 `AppPollSensors()` 针对当前游戏 pid 调用） |
| `engCompute` | float | -1 | 该进程 compute 引擎占用 %（AI / 超分的代理指标） | 同上 |
| `engOfa` | float | -1 | 该进程光流加速器（OFA）占用 % —— **DLSS 帧生成专用**，只要它在动，帧生成就在工作 | 同上 |
| **显示模式**（`EnumDisplaySettings`，主程序在 `Poll()` **之后**写，否则会被清空） | | | | |
| `screenW` / `screenH` | uint32 | 0 | 桌面分辨率 | `main.cpp AppPollSensors()` |
| `refreshHz` | uint32 | 0 | 刷新率 Hz。玩家说「锁 60」时那个 60 的来源；也能判断帧率是被垂直同步限住还是真跑满。**当前只有日志在读** | 同上 |
| **数据源描述** | | | | |
| `sources` | uint32 | 0 | 实际生效的数据源位组合（`NP_SENSOR_SRC`）。注意 `Poll()` 末尾会写成 `available_ \| out.sources` —— 即「探测到可用」与「本轮真的用到」的并集 | `SensorHub::Poll()` |
| `sourceText` | char[128] | 空 | 人读文案，例如 `NVML+NVAPI \| CPU:PDH \| 温度:HWiNFO`。面板「数据源」行显示它（`FitCols(..., 14)` 截断） | `SensorHub::Poll()` 末尾 |

**写入顺序的硬要求**：`SensorHub::Poll()` **开头就 `NPClearSensors(&out)`**，
所以任何「想补进 `NPSensors` 的外部字段」都必须在 `Poll()` **之后**写
（`screenW/screenH/refreshHz` 就是这么处理的，源码里专门留了注释：
*"写在前面会被覆盖成 0"*）。

**`NPClearSensors()` 的字段覆盖面**：它逐个字段写清空值（不是 `memset`），
覆盖了**全部**字段 —— 加字段时**必须**在这里加一行，否则新字段会带着上一次的残值
（或未初始化的栈垃圾）流过进程边界。

---

## 4. `NPTelemetry` —— 遥测块（钩子写，主程序读）

```c
// src/common/np_common.h  struct NPTelemetry
// 53 个字段，sizeof = 55736（= 字段宽度之和，无尾部填充）
// 由 tests/struct_check.py 逐字段核对 C++ 与 Python ctypes 镜像
```

`version` 里写的是 **`sizeof(NPTelemetry)`**，不是版本号。理由见 §1.2。

### 4.1 内存布局（大数组的位置很关键）

| offset | 字段 | 大小 |
| --- | --- | --- |
| 0 | `magic` | 4 |
| 4 | `version` | 4 |
| 8 | `pid` | 4 |
| 12 | `attached` | 4 |
| 16 | `tickMs` | 8 |
| 24 | `gfxApi` | 4 |
| 28 | `presentMode` | 4 |
| 32 / 36 | `renderW` / `renderH` | 4 + 4 |
| 40 / 44 | `windowW` / `windowH` | 4 + 4 |
| 48 | `processName[128]` | 128 |
| 176 / 180 | `frameTotal` / `frameWrite` | 4 + 4 |
| 184 | `frames[4096]` | 16384 |
| 16568 | `cpuFrames[4096]` | 16384 |
| 32952 | `gpuFrames[4096]` | 16384 |
| 49336 | `fps` | 4 |
| 49340 | `fpsAvg` | 4 |
| 49344 / 49348 | `fpsLow1` / `fpsLow01` | 4 + 4 |
| 49352 / 49356 | `frameMs` / `frameMsAvg` | 4 + 4 |
| 49360 / 49364 | `cpuFrameMs` / `cpuFrameMsAvg` | 4 + 4 |
| 49368 / 49372 | `simMs` / `submitMs` | 4 + 4 |
| 49376 / 49380 | `gpuFrameMs` / `msInPresent` | 4 + 4 |
| 49384 / 49388 | `cpuBusyMs` / `cpuBusyAvg` | 4 + 4 |
| 49392 / 49396 | `cpuWaitMs` / `cpuWaitAvg` | 4 + 4 |
| 49400 / 49404 | `p99Ms` / `p999Ms` | 4 + 4 |
| 49408 / 49412 | `drawCalls` / `dispatches` | 4 + 4 |
| 49416 / 49420 | `rtDispatches` / `asBuilds` | 4 + 4 |
| 49424 / 49428 | `rtGpuMs` / `aiGpuMs` | 4 + 4 |
| 49432 / 49436 | `rtLoad` / `tensorLoad` | 4 + 4 |
| 49440 | `aiModules` | 4 |
| 49444 / 49448 | `rtMeasured` / `tensorMeasured` | 4 + 4 |
| 49452 / 49456 | `graphWrite` / `graphCount` | 4 + 4 |
| 49460 | `graphFrame[512]` | 2048 |
| 51508 | `graphCpu[512]` | 2048 |
| 53556 | `graphGpu[512]` | 2048 |
| 55604 | `hookFlags` | 4 |
| 55608 | `lastError[128]` | 128 |
| **55736** | — | **`sizeof(NPTelemetry)` = 55736**（= 上表最后一个字段的结束偏移，**无尾部填充**，所以 `struct_check.py` 的「宽度之和 == sizeof」判据成立） |

> 上表由 ctypes 按平台 ABI 复现同一字段序列实测得到（`sizeof = 55736`，53 个字段），
> 与 C++ 侧 `NPClearTelemetry()` 写入 `version` 的值必须一致。

> 一个坑：钩子写 / 主程序读之间**没有锁**。主程序 `PickTelemetry()` 的做法是把整块
> **拷贝**成 `gApp.telemetry` 再用（`gApp.telemetry = *best->view;`）——
> 这个拷贝本身不是原子的，理论上可能撕裂。实践上能接受，是因为
> **面板只读标量字段**（fps / 帧时间 / 分辨率…），而**曲线完全走主程序自己的 `NPHistory`**；
> 即便某一帧的某个 float 被撕裂，也只是瞬间一个小抖动，不会累积。
> ⚠ **不要把这个块当成"无锁的逐帧数组"来用** —— 那三个 4096 的数组在读取时
> 没有任何"一定完整"的保证。

### 4.2 逐字段表

「状态」列的含义：
**活跃** = 有人写也有人读（面板/UI/日志/测试）；**写-无读** = 有人在写但当前没有任何读方
（保留字段）；**废弃/恒定** = 写入者已经拆掉或只在特定条件下才写。

| 字段 | 类型 | 含义 | 写方（钩子） | 读方 | 状态 |
| --- | --- | --- | --- | --- | --- |
| `magic` | uint32 | `NP_MAGIC` | `NPClearTelemetry()` | 主程序 `TryOpenTelemetry()` / `PickTelemetry()`、所有测试脚本 | 活跃（唯一被校验的字段） |
| `version` | uint32 | `sizeof(NPTelemetry)` = 55736 | `NPClearTelemetry()` | **只有** `tests/verify_inject.py` | 校验用 |
| `pid` | uint32 | 钩子自己的进程 id | `OpenIpc()` | 主程序 | 活跃 |
| `attached` | uint32 | `1` = 钩子已接管渲染（`UpdateTelemetryCommon` 每次进入都置 1） | `UpdateTelemetryCommon()` | 主程序（面板 `active`、叠加模式自动切换、监控状态文案） | 活跃 |
| `tickMs` | uint64 | 最近一次更新（`GetTickCount64`） | 同上 | 主程序 `PickTelemetry()`（挑最新鲜的块）、`tests` | 活跃；⚠ **跨进程别做减法** |
| `gfxApi` | uint32 | `NP_GFXAPI` 枚举 | `UpdateTelemetryCommon()` | `np_build.cpp ApiName()`（「API」行）；注入学习判据「识别出 D3D」 | 活跃 |
| `presentMode` | uint32 | `NP_PRESENT_MODE` 枚举 | `PresentCommon`（读交换链 `GetDesc` / 全屏状态） | `np_build.cpp ApiName()` 后缀（窗口/无边框/独占全屏） | 活跃 |
| `renderW` / `renderH` | uint32 | 渲染分辨率（后台缓冲）。默认取交换链 `BufferDesc`，有视口时改用视口 `gVpW/gVpH` | `PresentCommon` / `UpdateTelemetryCommon` | `np_build.cpp`「分辨率」行（`hooked && t.renderW` 才显示）；`tests/verify_inject.py`（`renderW == 0` 判失败） | 活跃 |
| `windowW` / `windowH` | uint32 | 输出/窗口分辨率 | `PresentCommon` | `np_build.cpp`（与 `renderW/H` 不同则显示 `→ 输出 (缩放%)`） | 活跃 |
| `processName` | char[128] | 进程 exe 名（UTF-8） | `PresentCommon` / 守卫线程（`NPCopyStr`） | 主程序状态栏、`tests/verify_inject.py` 打印 | 活跃 |
| `frameTotal` | uint32 | **累计帧数**（只增，溢出回绕） | `PresentCommon`（合并判定通过后 `++`） | 主程序（防"时间戳跨进程比较"的**唯一可靠活性判据**）、`tests`（算稳态帧率） | 活跃；⚠ **不要用 `count()` 替代它**（环形缓冲满容量后 `count()` 不再增长 —— 源码注释专门标了这个坑） |
| `frameWrite` | uint32 | `frames/cpuFrames/gpuFrames` 的环形写指针，`(wi + 1) % NP_FRAME_CAP` | `PresentCommon` | **无人读**（Python ctypes 镜像里为布局保留） | 写-无读 |
| `frames[4096]` | float[4096] | 每帧帧生成时间 ms（Present 间隔，已做「帧内重复 Present 合并」） | `PresentCommon` | **无人读**（主程序图表走 `NPHistory`，不是这个数组） | 写-无读（保留：给未来的离线分析/导出用） |
| `cpuFrames[4096]` | float[4096] | 每帧 CPU 帧时间 ms | 同上 | 无人读 | 写-无读 |
| `gpuFrames[4096]` | float[4096] | 每帧 GPU 帧时间 ms（= 当时的 `gpuFrameMs`） | 同上 | 无人读 | 写-无读（历史上是"GPU 曲线全为 0"投诉的现场） |
| `fps` | float | 瞬时 FPS。**按 500ms 时间窗计数**得到（不是逐帧 `1000/frameMs`）：锁 60 时窗口内 30 帧/0.5s = 恰好 60.0，不会 60↔100 乱跳 | `UpdateTelemetryCommon()` | 面板「FPS」行、主程序 `NPHistoryPush` | 活跃 |
| `fpsAvg` | float | 平均 FPS = 最近 600 帧的平均帧时间换算（`gStats.avgFps(600)`） | 同上 | 仪表/日志/`NPHistory`；**也用于把 1 秒窗口换算成帧数** | 活跃 |
| `fpsLow1` | float | **1% Low（FPS）**，最终口径：`lowPercentileFps(99.0, lowWin)` | 同上 | 面板「1% Low」行；主程序可能被 ETW 读数覆盖（见下） | 活跃 |
| `fpsLow01` | float | **0.1% Low（FPS）**，`lowPercentileFps(99.9, lowWin)` | 同上 | 面板「0.1% Low」行 | 活跃 |
| `frameMs` | float | **最近一帧的原始帧生成时间** ms（未平滑）。参与统计、合并判定、图表 | `PresentCommon` | `UpdateTelemetryCommon()`（喂 `gStats`、算 EMA、算 `rtLoad` 分母）、`np_build` 兜底显示 | 活跃 |
| `frameMsAvg` | float | 平滑后的帧时间（`np::Ema(0.10f)`）。**面板显示用这个** | `UpdateTelemetryCommon()` | `np_build.cpp`「帧时间」行（`frameMsAvg > 0.0001 ? frameMsAvg : frameMs`） | 活跃 |
| `cpuFrameMs` | float | CPU 帧时间 = **`CPUBusy + CPUWait`**（PresentMon 口径）= 帧周期。⚠ 必须扣掉卡在 `Present` 里的等待，否则锁 60 时恒等于 16.66ms —— 那是帧周期，不是 CPU 的活 | `PresentCommon` 尾部（`busyMs + waitMs`）；首帧兜底 `= frameMs` | 主程序 `NPHistoryPush`（桌面曲线的 `latCpu`）；`np_build` 在平滑值还没建立时兜底 | 活跃（但**面板优先用 `cpuBusyAvg + cpuWaitAvg`**，见 `cpuBusyAvg`） |
| `cpuFrameMsAvg` | float | 平滑后的 CPU 帧时间（EMA） | `PresentCommon` | 钩子侧 `NPHistoryPush`（游戏内曲线的 `latCpu`，用平滑值避免锯齿看着像剧烈波动） | 活跃 |
| `simMs` | float | **模拟阶段** ms：上一帧 `Present` 返回 → 本帧渲染线程第一次提交命令（游戏自己的逻辑/物理/动画/剔除）。有 Reflex 时会被游戏自报的权威值覆盖 | `PresentCommon`（启发式）/ Reflex 分支 | **无人读**（面板不再用它算 CPU 帧时间） | 写-无读（保留供对照） |
| `submitMs` | float | **渲染提交** ms：第一次提交 → 调 `Present`（录制命令列表 + 提交，**本质上是在排队**，用户明确说不该算进 CPU 帧） | 同上 | 无人读 | 写-无读（保留供对照） |
| `gpuFrameMs` | float | GPU 帧时间 ms | **不再是钩子自己推算的**：逐批时间戳夹取方案已整个拆除（它给每次 `ExecuteCommandLists` 插时间戳，真实游戏一帧十几到几十批 → allocator starved + 叠加永久停画）。现在在 `UpdateTelemetryCommon()` 里**从 `Sens().gpuBusyMs`（系统 PDH）回填** | `np_build.cpp`「GPU 帧时间」的兜底（要求 `> 0.01f` 才显示）；`NPHistoryPush`；图表环形缓冲 | 活跃，但**依赖 PDH**：读不到 PDH 时**恒为 0**（面板会显示 `—`，历史事故：所有以它为源的曲线是一条零线） |
| `msInPresent` | float | 本帧卡在 `Present` 调用里的时长 ms（主要就是等垂直同步） | `PresentCommon` | 用于算 `cpuWaitMs`；主程序诊断 | 活跃 |
| `cpuBusyMs` | float | **CPUBusy** = 本帧 `Present` 开始 − **上一帧 `Present` 返回**。本质是「Present 之间的残差」：流水线渲染的游戏里本来就接近 0；开 Reflex 后等待被移到 `Present` 之前，这段睡眠落进间隙 → 数值明显变大 | `PresentCommon` 尾部 | 面板「CPU Busy（高级）」行（默认不显示）；`CPU 帧时间 = busyAvg + waitAvg` | 活跃 |
| `cpuBusyAvg` | float | 上面那个的 EMA（`np::Ema(0.10f)`） | 同上 | **面板「CPU 帧时间」的主来源**（`cpuBusyAvg + cpuWaitAvg`） | 活跃 |
| `cpuWaitMs` | float | **CPUWait** = 本帧在 `Present` 内部停留（= `msInPresent`） | 同上 | 「低延迟」判定的回退判据（`< 1.6ms`）；「CPU Wait」行 | 活跃 |
| `cpuWaitAvg` | float | 上面那个的 EMA | 同上 | 低延迟判定优先用平滑值 | 活跃 |
| `p99Ms` / `p999Ms` | float | 帧时间百分位 ms（整个环形缓冲，不设窗口） | `UpdateTelemetryCommon()` | **无人读**（面板不显示百分位；诊断日志自己调 `gStats.percentileMs`） | 写-无读（保留） |
| `drawCalls` | uint32 | 上一帧 `Draw*` 次数 | `UpdateTelemetryCommon()`（从 `gDraws` 原子量） | `np_build.cpp`「Draw/Disp」行（`hooked` 才显示） | 活跃 |
| `dispatches` | uint32 | 上一帧 `Dispatch` 次数 | 同上 | 同上 | 活跃 |
| `rtDispatches` | uint32 | 上一帧 `DispatchRays` 次数 | `UpdateTelemetryCommon()`（从命令列表钩子统计） | `np_build.cpp`「RT Core」行（没实测数据时显示「启用 N 次」） | 活跃 |
| `asBuilds` | uint32 | 上一帧 BVH 构建（`BuildRaytracingAccelerationStructure`）次数 | 同上 | **无人读** | 写-无读 |
| `rtGpuMs` | float | 上一帧 RT pass 实测 GPU 时间 ms | 命令队列时间戳（`Signal`/查询堆） | `np_build.cpp`「RT Core」行后缀 | 活跃 |
| `aiGpuMs` | float | 上一帧 AI/后处理 compute pass 实测 GPU 时间 ms | 同上 | `np_build.cpp`「Tensor」行后缀 | 活跃 |
| `rtLoad` | float | RT 单元负载 % = **`rtGpuMs / frameMs`**。⚠ 分母是**帧周期**，不是 `gpuFrameMs`：后者是「第一个 `ECL` → `Present`」的 GPU 时间轴跨度，GPU 跑完就空转等下一帧，所以它约等于帧周期、是个**上界**，拿它当分母会严重低估光追占比（用户实测：开了光追却像没开） | `UpdateTelemetryCommon()` | `np_build.cpp`「RT Core」行（`hooked && t.rtMeasured`） | 活跃 |
| `tensorLoad` | float | Tensor/AI 负载 % = `aiGpuMs / frameMs`（同上，分母是帧周期） | 同上 | 「Tensor」行 | 活跃 |
| `aiModules` | uint32 | `NP_AI_MODULE` 位组合（DLSS-SR/RR/FG、FSR、XeSS、DirectML、ORT、其他） | 守卫线程扫描进程模块 → `gAiModules` → `UpdateTelemetryCommon()` | `np_build.cpp`「AI 模块」行、Tensor 行的「AI 已启用 · 估算中」 | 活跃 |
| `rtMeasured` | uint32 | `1` = RT 数值为**实测**；`0` = 推断/未启用 | 命令列表钩子（`p.rtCount != 0`） | `np_build.cpp` 决定显示「(实测)」还是回退 | 活跃 |
| `tensorMeasured` | uint32 | 同上，针对 AI/Tensor | 同上（`aiUsed && aiMs > 0`） | 同上 | 活跃 |
| `graphWrite` | uint32 | `graph*` 环形写指针 | `UpdateTelemetryCommon()` | **无人读** | 写-无读 |
| `graphCount` | uint32 | `graph*` 已填样本数（封顶 `NP_GRAPH_CAP`） | 同上 | **无人读** | 写-无读 |
| `graphFrame[512]` | float[512] | 降采样后的帧时间曲线（每帧一点，**未做时间轴降采样**，容量 512 点） | 同上 | **无人读** | 写-无读 |
| `graphCpu[512]` | float[512] | 同上，CPU 帧时间 | 同上 | 无人读 | 写-无读 |
| `graphGpu[512]` | float[512] | 同上，GPU 帧时间 | 同上 | 无人读 | 写-无读 |
| `hookFlags` | uint32 | `NP_HOOK_FLAG` 位组合（诊断） | 钩子守卫线程 / `PresentCommon` / 叠加初始化 | 主程序状态栏（`NP_HOOK_PRESENT` / `NP_HOOK_OVERLAY` → 文案）、**`np_build.cpp` 的低延迟判定（`REFLEX` + `REFLEX_KNOWN`）**、`tests` | 活跃 |
| `lastError` | char[128] | 人读错误文案（UTF-8），例如「没检测到图形 API：游戏可能还没开始渲染」 | 守卫线程 | `main.cpp` 状态栏（`if (t.lastError[0])`） | 活跃 |

**关于 `fpsLow1` / `fpsLow01` 的第二个写入者**：主程序 `AppPollSensors()` 会用
**ETW**（外置，不注入游戏，读内核 `DxgKrnl` 的 Present 事件）算出的 Low 帧
**覆盖**这两个字段（`main.cpp` 里 `gApp.telemetry.fpsLow1 = er.low1Fps;`）。
只覆盖这两个 —— 帧率/帧时间/GPU 时间**不覆盖**（"那些本来就已经对了，
多覆盖一次只会把对的数弄坏"）。而且**必须带 pid 判断**：没有目标进程时 ETW 的数据是
全系统所有进程混出来的，拿它覆盖等于把别人的数字塞进面板。

### 4.3 「已废弃 / 保留 / 写-无读」一览（给未来的自己省时间）

| 分类 | 字段 | 现况 / 想用起来该看哪里 |
| --- | --- | --- |
| 方案被拆除 | `gpuFrameMs` 的**自主推算** | 逐批 GPU 时间戳夹取已删；现在只是 PDH `gpuBusyMs` 的镜像。**要恢复自主测量，别重做逐批插时间戳**（会把 allocator 拖死） |
| 写-无读（在结构体里占 55296 字节 ≈ 54KB） | `frames[4096]`、`cpuFrames[4096]`、`gpuFrames[4096]`、`graphFrame/Cpu/Gpu[512]`、`graphWrite`、`graphCount` | 图表实际走 `NPHistory`（每 125ms 一个采样点、256 点）。这三个大数组是**每帧**精度，只适合离线导出/事后分析 |
| 写-无读（小字段） | `p99Ms`、`p999Ms`、`simMs`、`submitMs`、`asBuilds`、`frameWrite` | 面板不显示；诊断日志自己调 `FrameStats` |
| 只在特定条件下写 | `cpuFrames`/`gpuFrames`/`frameMs`（仅当合并判定通过、且间隔在 0.02~1000ms 之间）、`simMs`/`submitMs`（仅当渲染线程 + 时间戳有效）、`hookFlags` 的 `TIMESTAMP`（仅当查询堆就绪） | 读方必须**先判有效性**（`> 0.01f`、`!= 0`），不要直接当有效值显示 —— 「注入没成功时面板出现假的 0.00 ms」就是这么来的 |

---

## 5. `NPHistory` —— 图表历史环形缓冲（**不是共享内存**）

```c
// src/common/np_common.h
#define NP_HIST_CAP 256u
struct NPHistory {
    uint32_t write = 0;
    uint32_t count = 0;
    uint32_t sampleMs = 125;   // 采样间隔（X 轴时间换算用）
    float fps[256], avg[256], low1[256], low01[256];
    float usageCpu[256], usageGpu[256];
    float latFrame[256], latCpu[256], latGpu[256];
    float latBusy[256], latWait[256];
};
```

* **谁维护**：**主程序和钩子各维护一份**，互不相干 —— 所以它**不在** Config/Sensors/Telemetry
  任何一块共享内存里，只是个纯本地结构体（`np_common.h` 里定义是因为两边都要用同一份）。
  * 主程序：`gApp.hist`（`AppState::hist`），在主循环里（`monitoring` 为真时，
    紧跟 `AppPollSensors()` / `AppPublish()` 之后）`NPHistoryPush(&gApp.hist, ...)`，
    `sampleMs` 固定 125。`simulate` 打开时推的是模拟出来的正弦数据。
  * 钩子：文件级 `gHist`（`np_hook.cpp`），**每 8 帧推一个点**
    （`if ((gTel->frameTotal & 7u) == 0)`），并把 `sampleMs` 设成
    `max(8.0, 8000.0 / fpsAvg)` —— 也就是"大约 8 秒 256 点"的自适应采样。
* **`sampleMs` 的语义**：一个采样点代表多少毫秒。当前它**只被写进 `PanelRow::sampleMs`**，
  而 `np_panel.cpp` 的折线图是「把已有样本铺满整个宽度」的（不按固定时间轴），
  所以 `sampleMs` 现在对绘制**没有影响**。保留它是为了将来做真正的固定时间轴。
* **为什么是 256**：约半分钟到一分钟的点数，够画一屏；也是 `np_build.cpp::HistRange()`
  的上限（该函数目前**没有被调用** —— 区间显示被关掉了，见 `COMMON.md`）。
* **`NPHistoryPush()` 的定位**：`inline` 函数，一次写 11 个数组的同一槽位再推进
  `write`，**不做任何边界/有效性检查**。读方（`PanelRenderer` 的折线图 / `HistRange`）
  必须自己跳过 `<= 0` 的槽位（源码里就是这么做的：*"0 / 负数 = 还没填过的槽，跳过"*）。
* **读取约定**：`write` 是**下一个要写**的下标，`count` 是已填数量（封顶 `NP_HIST_CAP`）。
  「最新的一点」= `(write + CAP - 1) % CAP`；按时间顺序遍历要
  `start = (write + CAP - count) % CAP` 再顺推。**任何新读方都照这个来，别自己发明。**

---

## 6. `GameEntry` —— 游戏列表项（**只在主程序内，与钩子无关**）

```c
// src/app/np_app.h  （不在 np_common.h，不进共享内存）
struct GameEntry {
    std::wstring path;      // 完整 exe 路径；学习来的条目为空
    std::wstring name;      // exe 名（小写）
    DWORD        pid = 0;   // 当前匹配到的进程，0 = 没在运行
    bool         injected = false;
    bool         hooked = false;   // 钩子已接管画面（有新鲜遥测）
    bool         learned = false;  // true = 自动学习加的；false = 用户手动添加
};
```

| 字段 | 含义与坑 |
| --- | --- |
| `path` | 手动添加的完整路径。**学习来的条目 `path` 为空**：商店（UWP）应用的 exe 在 `WindowsApps` 下，普通用户读不了，只能按名字匹配 |
| `name` | exe 名小写。载入时 `settings.cpp` 就地取 basename 再 `towlower`（不调用 `injector.cpp` 里的 `static ExeNameOf`） |
| `pid` | 由注入器每轮扫描填。**跨线程访问必须持 `AppLock`**（守护线程 800ms 一轮会 `push_back` 扩容，主线程 120ms 一轮在读 → 野指针直接崩主程序） |
| `injected` / `hooked` | `injected` = 我们注入过；`hooked` = **钩子真的接管了画面**（有新鲜遥测）。学习逻辑要求三条同时成立（`NP_HOOK_PRESENT` + `gfxApi != UNKNOWN` + `frameTotal` 在增长）才把条目记进名单 |
| `learned` | 持久化分流：`SettingsSave()` 把 `learned == false` 的写进 `games[]`（完整路径），`learned == true` 的写进 `learned[]`（只有 exe 名）。**手动添加的条目不受 `cfg.learnedAutoHook` 影响，一直自动注入** |

---

## 7. 宏与枚举速查

### 7.1 常量与标识

| 宏 | 值 | 含义 / 坑 |
| --- | --- | --- |
| `NP_MAGIC` | `0x4E505231u`（"NPR1"） | 三个结构体共用的标识。**唯一被读方校验的东西**（C++ 侧） |
| `NP_FRAME_CAP` | `4096` | `NPTelemetry` 三个逐帧环形缓冲的容量；约 60 秒 @60FPS。也是 `np::FrameStats::kCap` 的值（两处**各写各的常量**，改一个要记得另一个） |
| `NP_GRAPH_CAP` | `512` | `NPTelemetry` 三条图表曲线的容量 |
| `NP_NAME_LEN` | `128` | 所有跨进程字符串缓冲区长度（`gpuName` / `sourceText` / `processName` / `lastError`）。写入一律走 `NPCopyStr()`（保证 `\0` 结尾、不越界） |
| `NP_MOD_LEN` | `24` | ⚠ **全项目无人使用**（AI 模块名检测在钩子里有自己的数组） |
| `NP_MAX_MODULES` | `8` | ⚠ **全项目无人使用** |
| `NP_HIST_CAP` | `256` | `NPHistory` 各数组容量 |
| `NP_SHM_CONFIG` / `NP_SHM_SENSORS` / `NP_MUTEX_SENSORS` | 见 §1 | 后两个字符串是 `TEXT()` 宽字符；`NP_MUTEX_SENSORS` 无人使用 |
| `NP_ALL_COUNTERS` | 见 §7.2 | 「默认勾选」的位组合。**刻意不含**一些位（下面标了 ✗） |

### 7.2 `NP_COUNTER` —— 计数器位

`cfg.counters` 的每一位 = UI 里一个可勾选项 = `np_build.cpp` 里一段 `if`。

| 位 | 名称 | 面板行 | 在 `NP_ALL_COUNTERS`? | 备注 |
| --- | --- | --- | --- | --- |
| 0 | `NP_C_FPS` | FPS | ✅ | 白色 `PL_FPS`；可挂 `NP_C_GRAPH` 控制的曲线 |
| 1 | `NP_C_FRAMETIME` | 帧时间 | ✅ | 用 `frameMsAvg`；数值颜色**固定**，不随帧时间跳红 |
| 2 | `NP_C_LOW1` | 1% Low | ✅ | **明确不画曲线**（用户要求：显示数值就好） |
| 3 | `NP_C_LOW01` | 0.1% Low | ✅ | 同上 |
| 4 | `NP_C_CPU_FRAME` | CPU 帧时间 | ✅ | 显示值 = `cpuBusyAvg + cpuWaitAvg`；**同时控制「系统」组里的「低延迟 On/Off」行** |
| 5 | `NP_C_GPU_FRAME` | GPU 帧时间 | ✅ | 三级兜底：`s.gpuBusyMs` → `t.gpuFrameMs`（须 `> 0.01f`）→ `—` |
| 6 | `NP_C_CPU_USAGE` | 占用（CPU 组） | ✅ | 蓝 `PL_CPU` |
| 7 | `NP_C_CPU_TEMP` | 温度（CPU 组） | ✅ | |
| 8 | `NP_C_GPU_USAGE` | 占用（GPU 组） | ✅ | 绿 `PL_GPU` |
| 9 | `NP_C_GPU_TEMP` | 温度（GPU 组） | ✅ | |
| 10 | `NP_C_GPU_HOTSPOT` | 热点 / 显存结温 | ✗ **默认关** | 实现完整，UI 有复选框；`> 95℃` 转 `PL_WARN` |
| 11 | `NP_C_GPU_POWER` | 功耗 | ✅ | **只显示当前功耗**，不带上限（用户明确要求） |
| 12 | `NP_C_GPU_CLOCK` | 核心/显存 | ✅ | 写成 `2002/14001 MHz` 而非 `2002 MHz / 14001 MHz` —— 省 4 个字符格，面板才收得窄 |
| 13 | `NP_C_VRAM` | 显存 | ✅ | `vramPct > 92` 转 `PL_WARN` |
| 14 | `NP_C_RAM` | 内存（系统组） | ✅ | `ramPct > 92` 转 `PL_WARN` |
| 15 | `NP_C_GPU_FB` | 显存带宽 | ✗ 默认关 | 需要 `domFb >= 0`（NVIDIA NVAPI 域） |
| 16 | `NP_C_GPU_VID` | 视频引擎 | ✗ 默认关 | 需要 `domVid >= 0` |
| 17 | `NP_C_GPU_BUS` | PCIe 总线 | ✗ 默认关 | 需要 `domBus >= 0` |
| 18 | `NP_C_RT` | RT Core | ✅ | **五档**：硬件接口 → 实测 → 「启用 N 次」→「无 DXR」→「需注入」 |
| 19 | `NP_C_TENSOR` | Tensor | ✅ | **五档**：硬件接口 → 实测 → 「AI 已启用 · 估算中」→「无 AI」→「需注入」 |
| 20 | `NP_C_RESOLUTION` | 分辨率（系统组） | ✅ | 需要 `hooked && t.renderW`；与输出不同时显示 `→ 输出 (缩放%)` |
| 21 | `NP_C_API` | API | ✅ | `D3D11/D3D12/Vulkan/OpenGL/D3D9/未接入` + 窗口模式 |
| 22 | `NP_C_DRAWS` | Draw/Disp | ✗ 默认关 | 需要 `hooked` |
| 23 | `NP_C_AI_MODULES` | AI 模块 | ✗ 默认关 | 需要 `hooked` |
| 24 | `NP_C_FAN` | 风扇转速 | ✗ 默认关 | 优先 `gpuFanPct`，否则 `gpuFanRpm` |
| 25 | `NP_C_GRAPH` | FPS / 帧时间小图 | ✅ | **实际只控制「FPS」和「帧时间」两行的行内折线图** |
| 26 | `NP_C_SENSOR_SRC` | 数据源 | ✗ 默认关 | 显示 `s.sourceText`（截到 14 格） |
| 27 | `NP_C_GPU_NAME` | 显卡型号 | ✗ 默认关 | `PL_DIM`，型号截到 14 格 |
| 28 | `NP_C_CHART_USAGE` | （图表：占用率曲线） | ✅ | ⚠ **`np_build.cpp` 里没有任何地方读它** → 死位（历史遗留，settings 迁移还会把它置上） |
| 29 | `NP_C_CHART_FPS` | Low 帧小图 | ✅ | ⚠ **同样无人读** → 死位。Low 帧行已明确不画曲线 |
| 30 | `NP_C_CHART_LATENCY` | 帧延迟小图 | ✅ | 控制 CPU 帧时间 / CPU Busy / CPU Wait / GPU 帧时间 四行的折线图 |
| 31 | `NP_C_CPU_CLOCK` | CPU 频率 | ✅ | `> 0` 才显示（回退路径会偏低） |
| 32 | `NP_C_CPU_POWER` | CPU 功耗 | ✅ | `> 0` 才显示 |
| 33 | `NP_C_CPU_BUSY` | CPU Busy（高级） | ✗ **故意不加** | 用户要求「保留但默认关」。settings 迁移（`ver < 5`）会**显式清位**，只清一次，不覆盖用户后来的勾选 |
| 34 | `NP_C_CPU_WAIT` | CPU Wait（高级） | ✗ **故意不加** | 同上。它和 CPU Busy 之和已经作为「CPU 帧时间」显示 |

**加新计数器位的完整动作**：`NP_COUNTER` 加位 → 决定是否进 `NP_ALL_COUNTERS` →
`ui.cpp` 的 `kCounters[]` 加一行标签 → `np_build.cpp` 加对应的 `if (... & NP_C_XXX)` 段
→ `settings.cpp` 若需要迁移就加一条 `ver < N` 规则并**把 `cfgVersion` 加 1**。

### 7.3 其他枚举

| 枚举 | 值 | 含义 |
| --- | --- | --- |
| `NP_GFXAPI` | `NP_API_UNKNOWN=0`、`D3D9=9`、`D3D11=11`、`D3D12=12`、`VULKAN=20`、`OPENGL=30` | ⚠ 值就是 API 的"数字名"（9/11/12），不是 0..5 的序号 —— **不要按下标处理**。`np_build.cpp` 用 `switch` 映射成文字，未知一律显示「未接入」 |
| `NP_PRESENT_MODE` | `WINDOWED=0`、`BORDERLESS=1`、`EXCLUSIVE=2` | 由钩子从交换链 `GetDesc` + 全屏状态推断；`np_build.cpp` 拼在 API 名之后 |
| `NP_AI_MODULE` | `NONE=0`、`DLSS_SR=1<<0`、`DLSS_RR=1<<1`、`DLSS_FG=1<<2`、`FSR=1<<3`、`XESS=1<<4`、`DIRECTML=1<<5`、`ORT=1<<6`、`OTHER=1<<7` | 位组合，钩子扫进程模块得到，写进 `t.aiModules` |
| `NP_SENSOR_SRC` | `NONE=0`、`NVML=1<<0`、`NVAPI=1<<1`、`ADL=1<<2`、`HWINFO=1<<3`、`LHM=1<<4`、`PDH=1<<5`、`WMI=1<<6`、`INTEL=1<<7` | 数据源位。用在 `NPSensors::sources`、`hwRtTensorSrc`、以及 `SensorHub::SetOverride()`（0 = 自动） |
| `NP_HOOK_FLAG` | 见下 | `NPTelemetry::hookFlags`，诊断 + 低延迟判定 |

### 7.4 `NP_HOOK_FLAG` 位

| 位 | 名称 | 谁置位 | 谁读 / 注意 |
| --- | --- | --- | --- |
| `1<<0` | `NP_HOOK_PRESENT` | `PresentCommon`（真的挂上后）；守卫线程每轮纠正 | 主程序状态栏（"Present 未挂上"）、注入学习判据、`tests/verify_inject.py`（**必须置位**）、README 排障表 |
| `1<<1` | `NP_HOOK_QUEUE` | 守卫线程（`gOrigECL` 存在 = 命令队列钩子挂上） | 状态栏诊断 |
| `1<<2` | `NP_HOOK_CMDLIST` | 守卫线程（`gClHooked`） | 状态栏诊断 |
| `1<<3` | `NP_HOOK_TIMESTAMP` | 守卫线程（查询堆 `gHeap` 就绪 = GPU 时间戳可用） | 状态栏诊断 |
| `1<<4` | `NP_HOOK_OVERLAY` | 叠加资源初始化成功 | 状态栏（"叠加资源未就绪"） |
| `1<<5` | `NP_HOOK_REFLEX` | Reflex 分支（`NvAPI_D3D_GetSleepStatus` 说开着，或收到游戏自报的延迟标记） | ⚠ **只有 `REFLEX_KNOWN` 也置位时，这一位才是权威结论** |
| `1<<6` | `NP_HOOK_REFLEX_KNOWN` | Reflex 分支（"我们成功问过驱动了"） | `np_build.cpp` 低延迟判定的分水岭：置位 → 直接看 `REFLEX`；未置位 → 回退「`cpuWaitAvg < 1.6ms`」这个旁证 |

---

## 8. 附：与共享内存无关但同属「数据契约」的常量

### 8.1 面板尺寸/布局常量（`np_panel.cpp` `Layout()`，测量与绘制共用）

| 量 | 公式 | 备注 |
| --- | --- | --- |
| 内边距 `pad` | `4.0f * scale + 3.0f` | 那 3px 是用户指定的"四周留黑" |
| 行高 `lh` | `max(fontHeight * 1.14f * scale, lineHeight_)` | **不能小于字体自身行距**（DWrite 实测），否则 `DRAW_TEXT_OPTIONS_CLIP` 会裁掉字形上下 |
| 标题行高 `headH` | `= lh` | 标题字号已改为与正文同号，只靠**加粗**区分 |
| 图表高 `chartH` | `max(11.0f, graphHeight * 0.26f) * scale` | 金属 HUD 的曲线区比文字区矮得多 |
| 行间距 `rowGap` | `1.0f * scale` | |
| 图表间距 `chartGap` | `2.0f * scale` | |
| 列间距 `colGap` | `2.0f * scale` | |
| 面板最小宽 | `max(170.0f * scale, contentW + pad * 2)`，再经 `latchedW_` 锁定 | |
| 数值列宽 | **固定 14 个等宽字符格** | 数据层 `FitCols(..., 14)` 保证不会被截断 |
| 标签列宽 | 量化到**整字符格**（`ceil(px / charW) * charW`） | 标签集合固定 → 量化后完全稳定 |
| 分组间距 `gapBefore` | `np_build.cpp` 四个分组依次传 **`0.0f`（帧率与延迟）/ `9.0f`（CPU）/ `3.0f`（GPU）/ `9.0f`（系统）** | ⚠ 这几个值**没有乘 `scale`**，与 `Layout()` 里其他间距的处理不一致（见 `COMMON.md` 教训清单）；首行不计 `gapBefore` |
| 圆角半径 | `11.0f * scale` | 底板是**带轻微圆角**的矩形（与源码注释"方角"不一致，以代码为准） |

### 8.2 行内折线图的 Y 轴自适应（`np_panel.cpp` `Render()`）

```
avg  = 窗口内均值
span = max(|avg| * 0.35, (mx - mn) * 0.60)
span < 0.5 → span = max(|avg| * 0.20, 0.5)
yMin = max(0, avg - span);  yMax = avg + span
unit == 1（百分比）→ 再夹到 [0, 100]，且保证 yMax - yMin >= 1
```

图中画：6 条等分**竖向网格**（`brGrid`：白 × 0.10）、上下两条边线、指标色 1px 折线。
**没有 Y 轴刻度**（金属 HUD 的做法：区间数值给在方括号里）。采样点**铺满整个宽度**，
不按固定时间轴（否则数据不足时线只挤在右边缘，看起来像坏了）。

### 8.3 行配色（`np_panel.cpp` `LevelRGB()`）—— 与 `NPConfig` 颜色的关系

| `PanelLevel` | 值 | RGB | 用途 |
| --- | --- | --- | --- |
| `PL_NORMAL` | 0 | `0.93,0.93,0.93` | 普通行（近白） |
| `PL_ACCENT` | 1 | `1.00,0.78,0.35` | 琥珀（RT/Tensor/AI 模块） |
| `PL_WARN` | 2 | `1.00,0.42,0.42` | 红（热点/结温 > 95℃、显存/内存 > 92%） |
| `PL_DIM` | 3 | `0.62,0.62,0.62` | 灰（次要信息） |
| `PL_FPS` | 4 | `0.95,0.95,0.95` | 白（FPS） |
| `PL_CPU` | 5 | `0.39,0.65,1.00` | 蓝（CPU 侧全部指标） |
| `PL_GPU` | 6 | `0.49,0.85,0.49` | 绿（GPU 侧全部指标） |
| `PL_MEM` | 7 | `0.95,0.95,0.95` | 白（帧时间等"总量"指标） |

**这些颜色是硬编码的** —— 所以 `cfg.textColor` / `accentColor` / `warnColor`
**写了也没用**（渲染器不读）。`cfg.bgColor` 是唯一真正生效的颜色。
标签色固定 `0.95,0.95,0.95` × `textOpacity`；`hint`（如「低延迟」）固定亮黄
`1.00,0.85,0.15`。**要恢复可配置颜色，改这里 + 让 `mk()` 接收 `cfg` 的颜色。**

---

## 9. 改字段的完整清单（必读）

改任何一个结构体字段（增 / 删 / 改类型 / 改语义 / 调整顺序），**必须**做完下面全部动作：

1. **只从 `reserved[]` 挪**（`NPConfig`），并**同步改小 `reserved` 的维度**；
   `NPTelemetry` / `NPSensors` 加字段一定要加在**末尾**（中间插字段 = 全部错位）。
2. **同步更新 `NPClearTelemetry()` / `NPClearSensors()` / `NPDefaultConfig()`** ——
   把新字段写进清空/默认值逻辑。漏了会让字段带着栈垃圾过进程边界。
3. **同步更新 `tests/verify_inject.py` 里的 ctypes 镜像**（`class NPTelemetry`）。
   它的字段顺序/宽度必须与 C++ **逐字段**一致。
4. **跑 `python tests/struct_check.py`**，必须 PASS。
   * ⚠ 在 **GBK 控制台**下它会在打印 `✅` 时抛 `UnicodeEncodeError` 并 **exit 1**
     （实测：不加 `PYTHONIOENCODING` 就是 exit 1，加 `PYTHONIOENCODING=utf-8` 才是 exit 0）。
     别把编码错误当成结构体不一致。
   * ⚠ 它当前只检查 `NPTelemetry` 一个结构体（`main()` 里的列表只有一项）。
     要给 `NPSensors` 加检查：注意 **`NPSensors` 的字段宽度之和是 444，而
     `ctypes.sizeof` 是 448**（尾部对齐填充）—— `struct_check.py` 的
     `cppsize != pysize` 判据会**误报失败**，要先修脚本的比较方式。
5. **同步更新 `tests/diag.py` 里的第三份镜像**（`CFG_FIELDS`）。
   它用手算偏移读 `Local\NextPerf_Config_v1`，是最容易被忘掉的一份。
   给 `NPConfig` 中间插字段 → 这份镜像后面所有字段全读错。
6. **同步更新本文档 `docs/DATA-STRUCTS.md`**（字段表 + 布局表 + `sizeof`），
   以及 [`COMMON.md`](COMMON.md)（如果涉及算法/渲染）与 [`CATALOG.md`](CATALOG.md) 的登记项。
7. 如果动了 `sizeof`：**必须同时重新构建 `NextPerf.exe` 和 `NextPerfHook.dll`**
   （`build.bat` 两个都出），并且意识到旧 DLL / 新 exe 的组合**从这一刻起不再兼容** ——
   这时才需要真的把「读方校验 `version == sizeof`」加到 `Cfg()` / `Sens()` /
   `TryOpenTelemetry()` 里。
8. 如果加了 `NP_C_*` 位：见 [§7.2](#72-np_counter--计数器位) 结尾的完整动作。
9. 如果加了配置项：**同时加 `settings.cpp` 的读和写**（血泪教训 3），
   并想清楚它**该不该**持久化（见 §2.4）。

---

## 10. 未确认 / 需要进一步核实的事项

诚实标注，避免后人把猜测当事实：

1. **`sizeof` 数值未经本机编译验证**：本文的 136 / 448 / 55736 是按 x64 默认对齐规则
   推算并用 ctypes 复现同一字段序列得到的（136 与 55736 完全吻合）。
   `NPSensors` 的 448 含 4 字节尾部填充，**建议用 `static_assert(sizeof(NPSensors) == 448)`
   把它固定下来** —— 现在没有任何编译期断言保护这些尺寸。
2. **`domExt` / `domExtPresent` / `hwRtTensorSrc` / `gpuPowerLimit` / `screenW/H/refreshHz`
   的读取方**：当前只在主程序日志或传感器内部诊断里出现，**面板/UI 不展示**。
   是否有其他分支读取未逐一确认（grep 全仓库未见）。
3. **`autoInject` 的最终归宿**：`AppAutoInjectTick()` 已被清空（故意），
   但字段、JSON 键、`settings.cpp` 的读写全都还在（UI 里**没有**它的开关 ——
   界面上那个「实验性自动注入」对应的是 `learnedAutoHook`）。是保留占位还是彻底删掉，
   需要决策 —— 删它会影响 `settings.cpp`，并释放它占的 `reserved[0]` 槽位。
4. **`NP_MUTEX_SENSORS`**：确认全项目无使用点，但**为什么**当初设计它（Config/Sensors
   是单向单写，本来不需要锁）没有记录 —— 可能是为将来多写方预留。
5. **`opacity` / `textColor` / `accentColor` / `warnColor` / `fpsCap` / `pollMs`
   是否真的可以删**：它们只被 `NPDefaultConfig()` / `settings.cpp`（`pollMs` 还有 UI 控件）
   读写，**没有任何读取方**。
   删字段会改变 `sizeof(NPConfig)` → 按 §9 第 7 条处理；不删则要接受"配置里有几个假开关"。
6. **`np_hook.cpp` 的行号**：本文只引用函数名，不引用该文件行号 ——
   它正在被频繁修改，行号不稳定。`src/common/` 的行号引用取自本文校准版本（见开头哈希）。
7. **本文写作期间 `np_common.h` 被并行修改过**（497 → 506 行，修掉了 §2.5 的越界写；
   容量宏补了 `u` 后缀）。**任何时刻若开头那张哈希表与源码不符，先重新通读
   `np_common.h` 再相信本文的字段表** —— 尤其"字段列表/顺序/默认值"这三样。
8. **`lowStrict`**：修复后的注释里出现了这个字段名，但 `NPConfig` 里**当前没有它**
   （见 §2.5 末尾的加字段清单）。如果它被加进来，本文所有与 `reserved[]`、
   `sizeof(NPConfig)`、JSON 键、`tests/diag.py::CFG_FIELDS` 相关的段落都要同步更新。
