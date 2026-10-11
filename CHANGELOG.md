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
