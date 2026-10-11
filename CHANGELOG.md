# 变更记录（CHANGELOG）

> **这个文件回答一个问题：`main` 上现在有什么还没发布，每个发布版本又包含什么。**
>
> 维护规矩（性质和 `docs/CATALOG.md` 的第一条一样，是硬性的）：
>
> * 打 tag 发布后，把「未发布」段整段挪成该版本的段落，并补上 tag / commit / 发布时间。
> * **未发布的段落必须显式标注「未经用户完整测试」。** 本项目的发布版要求是
>   「用户在自己机器上完整测试过」；`main` 上的提交**不满足**这个要求，
>   不要把它们当成与发布版同级的东西，也不要顺手给它编个版本号发出去。
> * 这里只记「发生了什么、影响到什么」。逐字段 / 逐函数的技术细节写在 `docs/` 里
>   （尤其 `docs/DATA-STRUCTS.md`），**不要往这里搬**。
> * **二进制不在本仓库里**：`dist/` 被 `.gitignore` 排除，发布资产通过 GitHub Release 分发。

---

## 未发布（main，2026-10-11 深夜）

> ⚠️ **未经用户完整测试**，未打 tag、未发布。**当前发布版仍是 v1.6.4。**

### 修掉一个回归：纯 DX12 游戏被判成 Vulkan → 完全没数据

**用户实测现象**（《漫威蜘蛛侠》，纯 DX12 游戏）：「以前读出来是 DX12，现在显示 Vulkan，而且完全没有数据。」

**根因**（`src/hook/np_hook.cpp`，两处 API 判定口径不一致 + 自我导入排除的副作用）：

1. 我们的钩子 DLL 静态导入 `d3d12.dll` / `d3d11.dll` / `dxgi.dll`，而 `SelfImports()`
   会把「我们自己带进来的」模块**全部跳过** → 最强的两个 D3D 信号**永远被跳过**；
2. 剩下 `vulkan-1.dll`（我们没导入）就命中了 → 纯 DX12 被判成 Vulkan → `hostIsD3D=false`
   → `probe disabled: host api=20 (non-D3D)` → **交换链一次都没挂钩**（`RestoreAllHooks: swapVt=0`）
   → 面板显示 Vulkan、`attached=0 帧数=0`；
3. 而这时**只有探测能救**：加了 10 秒自动注入延迟之后属于「晚注入」，游戏的交换链早就建好了，
   工厂钩子抓不到创建过程。所以这个 latent bug 被延迟暴露了出来。
   另外 `GuessApiFromModules()`（显示用）用的是**另一套顺序**（vulkan 排最前）——
   正是 `HostApiGuess()` 注释里点名警告过的写法，两处结论还互相矛盾。

**修法**：把 `D3D12Core.dll` 放在判定最前面。它是 `d3d12.dll` **真正创建 D3D12 设备时**才拉起的
运行时，我们的导入表里没有它（实测：纯 DX12 进程里有、我们自己的进程里没有）——既是最强信号，
又不会被自己污染。`GuessApiFromModules()` 改成先问 `HostApiGuess()`，口径统一；
`HostGraphicsUp()` 也一并加上这个信号。

**验证（在工作区内确定性复现，不用开游戏）**：新用例 `tests/verify_dx12_decoyvk.py`
用 `host_run.exe --decoy-vulkan`（照常跑 DX12 + 额外 `LoadLibrary("vulkan-1.dll")`）复现该组合。

| | API 判定 | 探测 | 结果 |
| --- | --- | --- | --- |
| 修复前 | `hostApi=20` | `probe disabled … (non-D3D)` | `attached=0 frameTotal=0 gfxApi=20` ❌ |
| 修复后 | `hostApi=12` | `probe attempt 1/3` | `attached=1 frameTotal=18 gfxApi=12` ✅ |

`selftest` 通过；该用例已纳入 `tests/run_all.py`。

---

## v1.6.4（tag `v1.6.4`，2026-10-11 发布）—— 修掉 D3D12 游戏内叠加的驱动崩溃

> ✅ **已由用户实测确认**（2026-10-11，商店版《生化危机8》DX12）：
> 连续 **2755 帧 / 60fps ≈ 53 秒**，期间还发生过**一次交换链重建**（`heapSwaps=1`），
> **零 `SEH`、零闪烁**。对比上一版同一场景：**466 帧（约 8 秒）就崩**在 `nvwgf2umx.dll+0x330124`。
>
> ⚠️ 观测窗口是**一次约 1 分钟的正常游玩**（不是"连续几小时"）—— 按本项目的老规矩，这算
> "用户已实测"，但**不等于已被长期证伪**；如果以后还在别处复现，请回到下面「候选根因修复」那一节。
>
> 本版除崩溃修复外，还包含：6 处文档口径修正、状态栏跳变修复、退出时反复注入的根因修复、
> 自动注入延迟，以及一批诊断（看门狗 / DETACH / `bbIdx` 直方图 / 日志上限 4MB）。

这一轮本来是**清账**：把通读文档时核出的 6 处「文档与代码不一致」一次修掉，外加同类问题 3 行日志文案。

### 代码改动（1 个文件，只动日志文案）

`src/app/main.cpp` 的「数据来源」日志块（每 5 秒一条）里有 3 处**会把排查带偏**的标注：

| 原来写的 | 问题 | 现在写的 |
|---|---|---|
| `AI 引擎 … (engtype_compute / engtype_ofa)` | 那是**修复前**的关键字（见 `53037bd`），照它排查会得出错误结论 | `(engtype_Compute / engtype_OFA)` |
| `CPU 帧时间 <- 帧周期 - Present 阻塞` | `t.cpuFrameMs` 实际是 **CPUBusy + CPUWait**（`np_hook.cpp:1816` 同一次减法），原标注只描述了「忙」那一半 | `<- CPUBusy + CPUWait(PresentMon 口径, 钩子); 本帧在 Present 内 %.2f ms` |
| `Low 帧 <- 最近1200帧样本推算(钩子)` | 口径早已换成 **1000ms 窗口平均（回看约 10s）**，而且有 ETW 时会被 ETW 覆盖 | `<- 钩子 1000ms 窗口平均(回看约 10s); ETW 可用时由 ETW 覆盖` |

### 文档同步（6 处，全部先核到代码再改）

| 文档 | 原来错的 | 改成 |
|---|---|---|
| `docs/COMMON.md` §3.4 / §8.2-12 / §9.2 | 写「最终采用口径③（严格百分位，1 秒窗口）」，并称口径④「当前无调用点」 | **当前实际是口径④（窗口平均，1000ms 窗口 / 回看约 10 秒）**；口径③ 只在窗口数不足 4 个时兜底。并把「这里曾写错、差点把后来者带沟里」记进文档 |
| `docs/CATALOG.md` §2.3 / §5-10 | 同一处错误还有两份副本：功能索引写「Low 帧（Intel 权威口径）· `lowPercentileFps`」、血泪教训 10 写「最终采用…对 FPS 取 P1」 | 都同步成窗口平均口径，并写明「**本文件曾把口径写错，改口径前先读代码**」 |
| `docs/SENSORS.md` §3.4 / §6.1、`docs/SENSORS-FUNCTIONS.md` | 把 AI 引擎 `engtype` 大小写 bug 列为**现存缺陷** | 改为 **✅ 已修复（`53037bd`）**，保留实测证据，并明确 **真机复验 `engOfa > 0` 仍未做** |
| `docs/APP.md` §17 | G1 / G2 / G3 标着「本次未改」，G9 说 ARCHITECTURE 没改 | G1 / G2 / G3 / G9 标 **✅ 已修**（附修复位置）；G4 标 **❌ 仍未改**；并提醒「现状」列是**发现问题当时**的描述 |
| `docs/ARCHITECTURE.md` §2.4 | 判活片段还是 `(now - telemetry.tickMs) < 2500` —— **跨进程时钟**，正是 CATALOG 血泪教训 1 | 改成 `gApp.telemetryLive`（帧计数增长），并补「2.5 秒三处共用」的说明 |
| `README.md` 已知问题 | 「静默机制挂在**永远不成立**的条件上…等于从未生效」 | **触发条件是「RTV 槽位攒满 8 个」，而槽位是按后台缓冲对象累加的、每次重建换掉一整批 → 它会的，只是要等到第二~三次重建：第一次重建完全没有保护。准确说法是「来得太晚」。** 要根治应补 `ResizeBuffers` 钩子 |

### 二进制（本地 `dist/` 已重建）

| 文件 | 字节 | SHA-256 |
|---|---|---|
| `NextPerf.exe` | 787,968 | `5DF96CB621EBF8E466B75B66FC42DF32583894A365DC1CF1DDDEC6160B1D397E` |
| `NextPerfHook.dll` | 644,096 | `B547C1E8E808208CD34BAC50B132FFB11A6C925DFD166BB83A1AC6B9C2BEA9B7` |
| `NextPerfHook32.dll` | 689,152 | `E81D391E0B53D7BCAB200766332C678B1716F2AE739369444327F99C9EF40A0B` |
| `NextPerfHook64.dll` | 644,096 | `B547C1E8E808208CD34BAC50B132FFB11A6C925DFD166BB83A1AC6B9C2BEA9B7` |

> `src/app` 与 `src/hook` 本版**都动过**，所以四个产物都与 v1.6.3 不同。
> 哈希只能**同机同路径**比（见 v1.6.3 段落里第 3 条事实）。

### 验证

| 验证 | 结果 |
|---|---|
| `dist\NextPerf.exe --selftest` | 全部通过（exit 0） |
| `python tests\run_all.py --skip-build` | 全部通过（7 项，exit 0） |
| 新日志文案确实编进 exe | ✅ 在 `dist\NextPerf.exe` 里能搜到 `钩子 1000ms 窗口平均`、`CPUBusy + CPUWait(PresentMon 口径`、`engtype_Compute / engtype_OFA`；旧文案 `最近1200帧样本推算` 已搜不到 |
| 真实游戏实测 | ✅ **已补做**：用户用**常驻上传缓冲环那一版**实测 —— 2755 帧 / 60fps ≈ 53 秒、含一次交换链重建，零 SEH、零闪烁（详见本段「用户实测确认」） |

### 追加：真机反馈的两个 bug（2026-10-11 晚，用户实测 · 商店版《生化危机8》DX12）

用户报了三件事，性质完全不同：

| Bug | 现象 | 状态 |
|---|---|---|
| **状态栏每 2.5 秒跳变** | 关窗后 NextPerf 在「钩上 / 没钩上」之间反复翻 | ✅ **根因已定位并修复** —— 是主程序自己的逻辑 bug：判定"钩子已死"后把 `sLastFt` 清成 0，于是下一次读到的**冻结帧数**（888）与 0 不相等 → 被当成"帧数又长了" → 复活 → 2.5 秒后再判死。现在**只清计时器、保留冻结帧数** |
| **关窗后游戏进程不退** | 窗口消失，后台进程留着 | 🔶 **一半已根因修复、一半待定性**，见下 |
| **自动注入让游戏闪退** | 自动盯 re8 → 启动即闪退；进游戏后手动注入不闪退 | ✅ **已缓解**：探测到进程后**等 10 秒**再自动注入（`kAutoInjectDelayMs`，手动注入不受影响）。⚠️ **根因未解** —— 早期注入撞上游戏自己的 D3D 初始化，而我们还要自建 D3D11 设备去探 vtable |

#### 「关窗后进程不退」：用户复现后的日志给出了确凿答案

**已根因修复的一半 —— 退出时的注入/卸载死循环。** 用户点「退出」之后，日志里仍有注入：

```
21:07:30.150  quit from UI button
21:07:30.794  inject pid 20124 (re8.exe): remote LoadLibrary returned 0xE2F50000
21:07:31.618  inject pid 20124 (re8.exe): remote LoadLibrary returned 0xE2F50000
```

机制：广播 `quit` → 钩子看到 `quit` 立刻自卸载 → 注入标记互斥量消失 → 看护线程 800ms 后
认为"它没被注入" → **又注入** → 新钩子一启动又自卸载……**0.8 秒一轮，停不下来**。
后果是**往一个正在关停的游戏进程里反复 `LoadLibrary`**：撞上 loader lock 就是永久挂死。

修法（三处，都是根因级而不是缓解）：
1. `CleanupAndExit`：**先 `InjectorShutdown()`（停看护线程），再 `cfg.quit = 1`**；
2. `InjectorScanNow`：`cfg.quit` 置位后**直接 return**，退出流程中绝不再注入；
3. `InjectorScanNow`：**正在请求卸载的 pid 不注入**（这是同一个死循环的另一半：
   注入 → 钩子看到 `detachPid == 自己` → 立刻卸载 → 下一轮又注入）。

**仍未定性的一半 —— 关窗那一刻渲染线程就静了。** 从 `21:07:01.5` 起钩子**连续 29 秒零日志**，
而该进程一直活着、loader 也正常（我们后来两次注入都成功了）。两种解释改法完全不同：
"游戏自己停住（不再调 Present）" vs "渲染线程卡在我们的 Present 路径里"。
按本项目「先加诊断再改行为」的规矩**没有猜着改**，而是加了看门狗：

* `PresentCommon` 进出时维护 `gPresentEnterMs` / `gLastPresentMs` / `gPresentStage`；
* worker 每 200ms 检查一次，超时**只在状态变化时**打一条：
  * `watchdog: PresentCommon 进入后 X.Xs 未返回 (stage=…)` → 卡在我们这条路径，并按 stage
    定位（`0=帧统计段 / 1=叠加绘制段 / 2=原始 Present(驱动) / 3=收尾段`）；
  * `watchdog: 已有 X.Xs 没有 Present 调用` → **游戏侧停住了**，别动钩子的 Present 代码。

#### 追加（2026-10-11 晚，用户再次实测 + A/B 结论）

**1）「关窗后进程不退」结案：不是我们的问题。** 用户 A/B 实测：**完全不启动 NextPerf**
玩一次 re8 再关窗，进程**同样退不干净**。钩子侧证据一致（看门狗报"游戏侧停住了"、
没有"卡在我们 Present 路径"的行、日志里没有 `DETACH: enter` —— 进程是被任务管理器结束的）。
本轮真正抓到的**属于我们自己的** bug 只有"退出时反复注入"那一条，已根因修复。

**2）D3D12 闪退不再只发生在"反复切换设置"时。** 21:32 复现：延迟注入**生效**、
正常游玩 `frames=466`、`heapSwaps=0` 时崩在 `nvwgf2umx.dll+0x330124`，`inPresent=1`。
同一会话的 `swapchain desc: fmt=28 buffers=3 swapEffect=4` 与
`bbIdx=[101,100,101,0|0]` 证明我们**画进了每一张会被呈现的缓冲** ⇒ **"画错缓冲"排除**。
含义：README 里那句"正常玩不受影响"**作废** —— D3D12 游戏内叠加在查清之前不可靠。

**3）候选根因修复：上传缓冲环。** 历史二分里"去掉 `CopyTextureRegion` **仍然崩**"，
于是上传那一半里剩下的唯一每帧动作就是**每帧新建约 300KB 的 UPLOAD 资源**。
现改为 **4 个常驻上传缓冲 + fence 复用**（`np_draw.h/.cpp` 的 `uploadRing_`），
把这个动作从每帧路径上删掉，顺带省掉约 **18MB/s** 的分配抖动。
回归仍全绿（D3D12 宿主截图亮像素 140712，叠加绘制正常；D3D11 宿主 `bbIdx=[209]` 符合 blt 模型）。
⏳ **待用户实测**是否还崩；若仍崩，下一步装「图形工具」用 D3D12 验证层定位
（需管理员；且验证层必须在游戏建设备**之前**启用 → 要尽早注入，与 10s 延迟冲突，需单独诊断构建）。

**4）日志基线已重置**：按用户提议，`%TEMP%\NextPerf.log` / `NextPerfHook.log` 先备份为
`.bak-<时间戳>` 再清零，方便下一次复现时干净读日志。

#### 用户实测确认（本次发布的判据）

修正上一版后，用户用清新日志重测（`attach 21:50:55`，商店版《生化危机8》DX12）：

| 项 | 数据 |
|---|---|
| 会话 | ≈ **53 秒** / **2755 帧** @60fps（`frame dist: rate=60.0/s fps=60.0 p50=16.66`，`SELFCHECK fpsLowLeAvg = OK`） |
| 崩溃 | **`SEH` 0 条**（上一版同场景：`frames=466` 崩在 `nvwgf2umx.dll+0x330124`） |
| 闪烁 | 用户报告**消失**；日志侧 `drawn=300 skipped=0`、`bbIdx=[100,100,100]`（flip 模型三缓冲**均匀分布**） |
| 交换链重建 | 期间发生 **1 次**（`heapSwaps=1`）且**未崩** —— 这正是上一版最敏感的路径 |
| 游戏退出 | 仍不干净，但看门狗给出的是 `连提交都停了`（游戏自己停住）→ **已确认是游戏自身问题**（不启动本工具时同样如此） |

#### 这一轮的验证

| 验证 | 结果 |
|---|---|
| `selftest` | 通过（exit 0） |
| 回归套件 | **7/7 全绿**（exit 0） |
| 看门狗不误报 | ✅ 跑了一次真实会话（`tests\_exitprobe.py`，宿主正常跑 5 秒后关窗），日志里**没有任何 `watchdog:` 行**；`DETACH: enter → worker wait -> exited` 链条完整 |
| 看门狗确实会报（正例） | ✅ **21:16 的真实复现里它报了**：`watchdog: 已有 5.2s 没有 Present 调用 —— 游戏侧停住了，不是卡在我们的 Present 路径里`，且**没有**任何"PresentCommon 进入后未返回"的行 → 我们的 Present 路径被排除。那次进程最终是**用户用任务管理器结束**的（所以日志里没有 `DETACH: enter`，`TerminateProcess` 不走 `DllMain`） |
| 退出问题的下一步 | ❓ 只剩"**我们在场本身**（device/queue 引用、vtable 补丁、命令列表钩子）是否拖住游戏自己的关停"。需要一次 A/B：**不启动 NextPerf** 玩一次 re8 关窗，看进程多久消失；若是几秒，再按 `overlayMode=2` / `deepEngineHook=0` / `vtableProbe=0` 逐项二分 |
| 面板闪烁 | 🔶 用户新报「游戏内面板闪烁」。日志事实：`drawn=300/skipped=0/heapSwaps=0`（每帧都画、没跳过、没换堆），上传缓冲是**每帧新建 + fence 延迟回收**（无复用竞态）。按 HOOK.md 判据这指向**成因B（画错缓冲）**，故新增两条判据：`swapchain desc`（BufferCount/SwapEffect）与 `overlay 5s … bbIdx=[…]` 直方图 —— flip 模型下应均匀分布，只落在 1 个下标即为成因B |

---

## v1.6.3（tag `v1.6.3`，2026-10-11 发布）

> ✅ **已由用户实测通过**：商店版《生化危机8》（DX12），2026-10-11，真机运行正常、未报告异常。
>
> 本条目发布时最初写作「未经用户完整测试」，用户实测后已更新
> （那句措辞仍留在 tag `v1.6.3` 内部那一版 CHANGELOG 里，属正常的历史差异）。
> 自动化验证的结果见下面「验证证据」。

**基线**：`v1.6.2`（tag `56b11e5`，2026-10-11 13:30 发布）。

### 这一段有哪些提交

| 时间 | 提交 | 性质 | 改了什么 |
|---|---|---|---|
| 13:31 | `45b0d21` | 文档 | 更正 README 里「GPU 帧时间读到 0」的错误结论（用户已在真机确认它是对的）；补 CATALOG / SIMULATOR |
| 13:51 | `0cbc528` | 测试工具 | 新增 `tests/_bisect.py`（D3D12 闪退二分定位脚本） |
| 19:18 | `cbc3119` | 诊断 | 钩子接入 D3D12 验证层：把 `ID3D12InfoQueue` 的消息转存进 `NextPerfHook.log`（+30 行）；新增 `tests/_adddebuglayer.py`；`tests/sim/sim_dx12.cpp` +13 行 |
| 19:36 | `8120c56` | 钩子 | 默认**关闭** D3D12 游戏内叠加（+29 行，靠环境变量 `NEXTPERF_D3D12_OVERLAY=1` 打开） |
| 19:40 | `bc15bff` | 钩子 + 文档 | **撤销**上一条（−29 行）；README 改为如实记录该闪退「已尝试修复、暂时搁置」；新增 `tests/_readmecrash.py` |
| 本次 | `fix:` | 修复 | 上面那段诊断加**一次性标志**：没装验证层时不再**每帧**重试 `QueryInterface`（+5 行） |
| 本次 | `docs:` | 文档 | 新增本文件；`docs/CATALOG.md` 登记「仓库根」文件清单与维护约定 |

> `8120c56` → `bc15bff` 是 **4 分钟内的往返**：先把 D3D12 叠加默认关掉，又改回默认开着。
> **净结果 = D3D12 叠加行为与 v1.6.2 完全相同**，那个环境变量开关**没有**保留下来。

### 净影响（相对 v1.6.2）

| 文件 | 净变化 |
|---|---|
| `src/app/**` | **零改动** |
| `src/hook/np_hook.cpp` | **+35 / −0**（30 行验证层诊断 + 5 行一次性标志） |
| `README.md`、`docs/CATALOG.md`、`docs/SIMULATOR.md` | 文档 |
| `tests/` | 新增 3 个脚本；`sim/sim_dx12.cpp` +13 行 |
| `CHANGELOG.md` | 新增（本文件） |

### 交付的二进制（`dist/`，同时是 v1.6.3 的发布资产）

| 文件 | 字节 | SHA-256 |
|---|---|---|
| `NextPerf.exe` | 787,968 | `9A620CC76B800B70777222FD62B9AF2A589ACA0F138E4090CAA1A6CCCA5EC74E` |
| `NextPerfHook.dll` | 642,048 | `208C5A73C1E32CCD8DB43F6007A77595D0E02DF0316EA687BA5CC4586251A6DF` |
| `NextPerfHook32.dll` | 686,080 | `037869F3DCAE0E375B4A7E22934EF85C1AD94A018D64AF6F22DEE2B049C19F2C` |
| `NextPerfHook64.dll` | 642,048 | `208C5A73C1E32CCD8DB43F6007A77595D0E02DF0316EA687BA5CC4586251A6DF` |

三条必须知道的事实：

1. `NextPerf.exe` 与 **v1.6.2 发布版字节完全一致** —— 因为这一段 `src/app` 一行没改。
   这同时证明构建环境与当初发布时等价（同一个 zig 0.16.0）。
2. 三个钩子 DLL 与 v1.6.2 不同，差异就是上表那 **+35 行**。
3. ⚠️ **构建产物受「编译器所在路径」影响**：用另一份同为 0.16.0 的 zig（装在不同目录）
   构建同一个源码，`NextPerf.exe` 会得到**不同字节、相同大小、行为相同**的产物。
   **所以跨环境的哈希比对没有意义** —— 要核对就用同机同路径重建，
   或以上面第 1 条推论为准（`src/app` 零改动 ⇒ exe 与 v1.6.2 同字节）。

### 验证证据（本版本交付前实际跑过的）

| 验证 | 结果 |
|---|---|
| `dist\NextPerf.exe --selftest` | **全部通过**（exit 0）：D2D/DWrite 可用、面板位图渲染 OK、Config/Sensors 创建 OK、钩子 DLL 存在 |
| `python tests\run_all.py --skip-build` | **全部通过 ✓**（exit 0，7 项）：结构体镜像、vtable 下标、D3D12 注入、D3D11 注入、图形初始化前注入、前台注入、PDH 指标链路 |
| 注入过程日志自检 | `SELFCHECK unloadClean = OK`、`SELFCHECK fpsLowLeAvg = OK`，全程无 `SEH` 记录 |
| 真实游戏实测 | ✅ **用户实测通过**：商店版《生化危机8》（DX12），2026-10-11，未报告异常 |

### 遗留风险 / 待办

* **D3D12 改分辨率 / 开关垂直同步 / 切窗口全屏仍可能闪退**（`nvwgf2umx.dll+0x330124`），
  状态仍是「已尝试修复、暂时搁置」。**本版本没有改变这个结论**
  （`cbc3119` 加诊断正是为了下一次能直接问验证层，而不是继续靠排除法猜）。
* 那 30 行诊断在**装了 D3D12 验证层**的机器上会把它记录的消息写进 `NextPerfHook.log`（上限 40 条）；
  **没装验证层时只问驱动一次，之后零开销**（这就是本次修的 5 行）。
* 用户已于 2026-10-11 实测通过，**实测对象：商店版《生化危机8》（DX12）**；
  具体时长没有记录（本项目吃过"说不清测了什么"的亏，能记就记）。
* 上一条 D3D12 闪退的结论**不因这次实测而改变**：它只在"反复改分辨率 / 开关垂直同步 /
  切窗口全屏"这组操作下出现，用户本次是否走到那一步未记录，所以别把它当成"已验证不会崩"。

### 复核方法（可直接复制）

```powershell
. "C:\Users\Abner\Documents\deepseek-harness\default-workspace\tools\env.ps1"
cd NextPerf

git log --oneline v1.6.2..v1.6.3     # 这一段有哪些提交
git diff  --stat   v1.6.2..v1.6.3    # 净改动

# 本机下载到的二进制是否与发布时一致（同机同路径重建才有比对意义，见上文第 3 条）
Get-FileHash dist\NextPerf.exe, dist\NextPerfHook.dll, dist\NextPerfHook32.dll, dist\NextPerfHook64.dll -Algorithm SHA256

# 发布资产有没有被事后替换（updated_at 只应比 created_at 晚几秒）
gh api repos/Abnerluo123/NextPerf/releases/tags/v1.6.3 `
  --jq '.assets[] | [.name,.size,.created_at,.updated_at] | @tsv'
```

---

## v1.6.2（tag `56b11e5`，2026-10-11 13:30 发布）—— CPU 传感器修复

包含 `v1.6.1..v1.6.2` 的 6 个提交：

* CPU 频率修好了（根因：计数器注册进了另一张表；改用 `CallNtPowerInformation`，
  PDH 那条路降级为回退）。
* CPU 功耗加 EMA 平滑，`--selftest` 增加频率 / 功耗自检（`7a8f00d` 当时仍标着未修好，
  由 `8219517` 收尾）。
* 自检里温度哨兵值显示为 `—`，不再打出演成绝对零度的数字。
* README 口径同步 ×2。

发布说明原文：

> GPU 帧时间已由用户实测确认正确（此前标注的「待查」有误：模拟器读到 0 是因为它每帧
> GPU 工作量仅约 0.005ms，低于测量分辨率，读 0 属预期）。

发布资产：`NextPerf.exe`、`NextPerfHook.dll`、`NextPerfHook32.dll`、`NextPerfHook64.dll`。
四个资产的 `created_at` / `updated_at` 只差 1 秒 —— **发布后从未被替换过**。

---

## v1.0.0 … v1.6.1

不在本文里，见 GitHub Releases 各自的说明与 `git tag`。若要把它们回填进来，
按 tag 区间逐个生成即可（例如 `git log --oneline v1.6.1..v1.6.2`）。
