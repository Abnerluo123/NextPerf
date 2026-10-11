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

## 未发布（main，2026-10-11 晚）

> ⚠️ **未经用户完整测试**，未打 tag、未发布。**当前发布版仍是 v1.6.3。**

这一轮是**清账**：把通读文档时核出的 6 处「文档与代码不一致」一次修掉，外加同类问题 3 行日志文案。

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

| 文件 | SHA-256 | 与 v1.6.3 相比 |
|---|---|---|
| `NextPerf.exe` | `943DAF37051A9DD82EB06BA6C4254A68697682CD1DE06A9D120B1AB4C5E3F1E3` | **变了**（本轮改了 `src/app/main.cpp`） |
| `NextPerfHook.dll` | `208C5A73C1E32CCD8DB43F6007A77595D0E02DF0316EA687BA5CC4586251A6DF` | 完全相同 |
| `NextPerfHook32.dll` | `037869F3DCAE0E375B4A7E22934EF85C1AD94A018D64AF6F22DEE2B049C19F2C` | 完全相同 |
| `NextPerfHook64.dll` | `208C5A73C1E32CCD8DB43F6007A77595D0E02DF0316EA687BA5CC4586251A6DF` | 完全相同 |

> 三个钩子与 v1.6.3 **字节相同**（这一轮没动 `src/hook`），只有主程序变了 ——
> 这也再次印证 v1.6.3 段落里的第 3 条：**哈希只能同机同路径比**。

### 验证

| 验证 | 结果 |
|---|---|
| `dist\NextPerf.exe --selftest` | 全部通过（exit 0） |
| `python tests\run_all.py --skip-build` | 全部通过（7 项，exit 0） |
| 新日志文案确实编进 exe | ✅ 在 `dist\NextPerf.exe` 里能搜到 `钩子 1000ms 窗口平均`、`CPUBusy + CPUWait(PresentMon 口径`、`engtype_Compute / engtype_OFA`；旧文案 `最近1200帧样本推算` 已搜不到 |
| 真实游戏实测 | ❌ 未做（改动只涉及日志文案，但按规矩仍算未测） |

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
