# 本机环境改动台账与清理清单

> **这份文档只有一个目的：等项目做完，用户能照着它把电脑清干净。**
>
> ## 规矩（和「改代码必须同步文档」同一条性质）
>
> 1. **只要在会话工作区之外装了、改了、删了任何东西，就立刻在 §2 记一行** ——
>    包括 pip/uv 装的包、下载的 SDK、建过的目录、写过的文件、改过的环境变量/注册表/系统功能。
> 2. **删掉之后不要删掉记录**，把状态改成「已删」并注明核实方式 ——
>    用户要的是「这台机器上到底被动过什么」，不是「现在还剩什么」。
> 3. **工作区内的东西（`dist\`、构建产物、测试宿主）不算环境改动**，只记在 §6 备查。
> 4. 需要管理员权限/装系统功能的操作，**先问用户**，装了必须记在这里（§4）。
>
> 最后一次更新：2026-10-11（本次只补文档，未改动任何环境）。

---

## 1. 这台机器上的关键路径（写清楚，免得以后找不到）

| 名称 | 路径 |
| --- | --- |
| 会话工作区 | `C:\Users\Abner\Documents\deepseek-harness\default-workspace` |
| 项目根 | `...\default-workspace\NextPerf` |
| **用户真实 TEMP** | `C:\Users\Abner\AppData\Local\Temp`（**不是** `$env:TEMP`，见 `docs/LOGGING.md` 的警告） |
| 主程序配置 | `C:\Users\Abner\AppData\Roaming\NextPerf\config.json`（**程序自己的设置，不是垃圾**） |
| 日志 | `%TEMP%\NextPerf.log`（主程序）、`%TEMP%\NextPerfHook.log`（钩子，上限 4 MB 覆盖式回绕） |
| 用户自己的 zig（**从未改动**） | `C:\Users\Abner\.workbuddy\binaries\python\envs\default\Lib\site-packages\ziglang\zig.exe`（0.16.0） |
| 会话用 Python | `C:\Users\Abner\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe` |

---

## 2. 环境改动台账（**清理就看这张表**）

| # | 时间 | 做了什么 | 位置 | 大小 | 当前状态 |
| --- | --- | --- | --- | --- | --- |
| E1 | 2026-10-11 白天 | 用 `uv pip install --no-config --cache-dir tools\uv-cache` 装了 **ziglang 0.16.0**（当时为了在没有 MSVC/MinGW 的机器上交叉编译） | `NextPerf\tools\zig-venv\` | ≈ **347 MB** | ✅ **已删除**（2026-10-11，已核实目录不存在、全仓无引用；现在用用户自己 `.workbuddy` 里那个 zig） |
| E2 | 同上 | uv 的下载缓存 | `NextPerf\tools\uv-cache\` | 数十 MB | ✅ **已删除**（核实不存在） |
| E3 | 2026-10-11 21:39 | 清空日志前**备份**了两个日志 | `%TEMP%\NextPerf.log.bak-20261011-213927` | 339,000 B | ⏳ **还在磁盘上**（留着是为了查历史问题；不用了可以直接删） |
| E4 | 同上 | 同上 | `%TEMP%\NextPerfHook.log.bak-20261011-213927` | 570,875 B | ⏳ **还在磁盘上**（同上） |
| E5 | 持续 | 程序自己写的活日志 | `%TEMP%\NextPerf.log` | 约 29 KB（会增长） | ⏳ 由程序维护；不想要就删，下次运行会重建 |
| E6 | 持续 | 钩子的活日志 | `%TEMP%\NextPerfHook.log` | ≤ 4 MB（覆盖式） | ⏳ 同上 |
| E7 | 持续 | **DSH 自己的沙箱目录**（不是本项目建的，是 Harness 会话产生的） | `%TEMP%\dsh-*\`（约 25 个目录） | 合计 **≈ 80 MB**，其中 6 个较大（38 MB / 11 MB / 8.3 MB / 8.3 MB / 7.6 MB / 3.6 MB） | ⏳ 是否自动回收未确认；项目收尾时可以整个 `dsh-*` 一起删（**注意别删 `dsh-acl-locks` 之类正在用的**） |
| E8 | 2026-10-11 | 临时写的辅助文件（发布说明、发布说明补丁脚本、回归输出、回下载的发布资产做哈希校验） | 沙箱 TEMP（如 `...\Temp\dsh-XXXX\`） | 几 MB | ✅ 随沙箱目录回收，**真实 TEMP 里已确认没有残留** |
| E9 | 每条命令 | 临时设 `HTTP_PROXY` / `HTTPS_PROXY`（走本地 7890 代理） | 进程级环境变量 | — | ✅ **没有持久化**，只是命令级 |
| E10 | 若干次 | `git -c http.proxy=` 内联覆盖代理（**没有写进 `.gitconfig`**） | 仅该条命令 | — | ✅ 无残留 |
| E11 | 2026-10-11 | 用任务管理器/`terminate()` 结束过测试进程（`NextPerf.exe` / `host_run.exe` / 渲染宿主） | — | — | ✅ 只结束自己启动的进程；**从未按映像名全杀用户的进程** |
| E12 | 2026-10-11 23:0x | ⚠️ **误判并恢复了用户的配置**：发现 `%APPDATA%\NextPerf\config.json` 的 `games` / `learned` 变成空数组（mtime 22:42:39），我当成"被清空的 bug"，照早先读到的内容把《漫威蜘蛛侠》+ `re8.exe` 写了回去 | `C:\Users\Abner\AppData\Roaming\NextPerf\config.json` | 502 B | ✅ **已还原成用户自己的空状态**（用户说明"我自己清理过一次"）。**教训：动用户的配置/数据之前先问，别把"用户主动做的"当成 bug 去修** —— 这条同时说明为什么 `tests/verify_quit_noinject.py` 必须自己备份/还原 `config.json`（见 §7 C1/C2） |

### 2.1 没有装、没有改的（特意确认过）

* ❌ **没有**安装 Windows「图形工具」可选功能（D3D12 验证层）—— 需要管理员，见 §4。
* ❌ **没有**安装 MSVC / MinGW / Vulkan SDK / 任何第三方库（本项目零第三方依赖）。
* ❌ **没有**改动用户自己的 zig、没有改 `.gitconfig`、没有改系统 PATH/注册表。
* ✅ 用户配置 `%APPDATA%\NextPerf\config.json` **未被测试污染**（2026-10-11 核对：`learned` 里只有 `re8.exe`，
  `games` 里只有《漫威蜘蛛侠》，没有测试宿主 `host_run.exe`）。

---

## 3. 项目需要的工具链（现有环境，不是本项目装的）

| 用途 | 用什么 | 谁提供的 |
| --- | --- | --- |
| 交叉编译 C++20 → Windows（x64 / x86） | **zig 0.16.0** `zig c++ -target x86_64-windows-gnu` | 用户自己的 `.workbuddy` 里的 ziglang 包 |
| 构建脚本 | `NextPerf\build.bat`（GBK + CRLF，别改编码） | 本项目 |
| 跑测试/探针 | 会话自带的 Python | DSH runtime |
| 上层工具 | `git` / `gh`（端口 7890 代理） | 工作区 `tools\env.ps1` 里带的便携版 |

---

## 4. 将来可能要装的东西（装了必须补记到 §2）

| 项 | 为什么 | 怎么装 | 备注 |
| --- | --- | --- | --- |
| **Windows「图形工具」（D3D12 验证层）** | 万一 D3D12 叠加再出驱动崩溃，验证层能直接报「哪一次调用用错了」 | `DISM /Online /Add-Capability /CapabilityName:Tools.Graphics.DirectX~~~~0.0.1.0`（**需管理员**） | ⚠ 验证层必须在**游戏创建设备之前**启用才对该设备有效 → 诊断要尽早注入；装完建议记一下大小/是否可 `Remove-Capability` 卸载 |

---

## 5. 测试会临时创建/删除的东西（不必手工清理，但要知道）

| 项 | 说明 |
| --- | --- |
| `%APPDATA%\NextPerf\settings.json` | `tests/verify_quit_noinject.py` 会写这个文件再删掉。⚠️ **文件名是错的**（程序读的是 `config.json`）—— 见 §7 的缺陷记录 |
| `host_run.exe` / `sim.exe` / `simvk.exe` / `NextPerf.exe` 子进程 | 测试自己启动、自己结束；不动用户已开的进程 |
| 测试宿主的窗口 `NpHost` / `NextPerfMainCls` | 进程结束即消失 |

---

## 6. 工作区内的可清理项（不影响仓库，随时可删）

| 项 | 大小 | 说明 |
| --- | --- | --- |
| `NextPerf\dist\` | ≈ 2.8 MB | 构建产物（`NextPerf.exe` + 三个钩子 DLL），已在 `.gitignore` 里 |
| `NextPerf\tests\host_run.exe` | ≈ 196 KB | 测试宿主，由 `tests\build_hosts.bat` 生成 |
| `NextPerf\tests\shot_host_d3d11.png` / `shot_host_d3d12.png` | ≈ 529 KB | 回归截图（用来数亮像素判断叠加有没有画上） |
| `NextPerf\tests\sim\*.exe`、`tests\simvk\*.exe` | 几 MB | 模拟器构建产物 |

---

## 7. 本台账暴露出的待修项

| # | 项 | 状态 |
| --- | --- | --- |
| C1 | **`tests/verify_quit_noinject.py` 预置了错误的文件名**：它写 `settings.json`，而程序读的是 `%APPDATA%\NextPerf\config.json` → 看护线程**永远拿不到维护目标**，用例的前置条件恒不成立（在真机上会一直打印"读不到学习记录"后继续跑，主判据可能空跑） | 🚧 **待修**（改文件名 + 顺手备份 `config.json`，因为主程序退出时会重写它） |
| C2 | 该用例目前**没有备份 `config.json`**：主程序经「退出」路径结束时会 `AppSaveSettings()` 重写它。2026-10-11 核对过**这次没被污染**，但测试该自己保证 | 🚧 同 C1 一起修 |
| C3 | 沙箱每次命令给一个独立 TEMP 并在结束后回收 → 测试**不能依赖单一日志路径** | ⚠️ 已在 `docs/TODO.md` D13 记录；新用例已按 mtime 在多候选里挑 |
