# NextPerf 代码审查与缺陷清单（2026-10-09）

> 本文档是「完整代码审查」这一轮工作的交付清单，按**已修 / 未修 / 无法确定**三栏记录。
> 每条都写清楚**原来的问题是什么**，以及**为什么这样改**，便于后续维护者判断。

## 审查范围与方式

| 范围 | 负责 | 状态 |
|---|---|---|
| `src/hook/*`（注入进游戏的那一半） | 独立审查者 A | 已完成，10 处修复，**已独立编译验证** |
| `src/app/*`、`src/sensors/*`、`src/etw/*`、`src/common/np_common.h` | 独立审查者 B | 已完成，16 处修复 |
| `src/common/np_build.cpp` / `np_bitmap.cpp` / `np_panel.cpp` / `np_stats.h` | 主代理 | 已完成 |
| `tests/*`（验证套件本身） | 主代理 | 已加固 |

**为什么派独立审查者**：主代理在同一个指标上返工了六轮，积累了大量错误假设
（曾两次误判为"权限问题"，实际是读错日志）。从零读代码的审查者不受这些假设影响。

---

## 一、已修复

### 钩子层（`src/hook/`）

| 位置 | 原来的问题 | 修法 |
|---|---|---|
| `np_hook.cpp` PresentCommon 叠加段 | 两处 `bb->Release(); return CallOriginal();` 从函数**中部**提前返回，跳过了 `gInPresent=false` / `gFrameStarted=false` / `gLastPresentQpc` / `gLastInPresentMs` 的收尾 → **`gInPresent` 永久为 true**，此后每帧都在开头"已在 Present 中"分支直通原函数 → **叠加和 GPU 时间戳永久失效、帧时间停更**（用户反复报"玩一会儿面板就不刷新"就是它） | 改用 `stateOk` 标志跳过本帧叠加，函数尾部收尾照常执行；`bb->Release()` 保留在原处 |
| `np_hook.cpp` `NpDispatchRays` | SEH 分支里 `setjmp` 返回非 0 使原条件为假 → 控制流掉到兜底转发 → **同一次光追被提交两遍**，且只插了半对时间戳（画面错乱 / 设备 removed） | 独立 `faulted` 标记；出错只转发一次，并永久关闭 D3D12 插桩 |
| `np_hook.cpp` `NpDispatch` | 同上（重复提交），且未配对的 `TS_AI_START` 会让 Tensor 占用报出荒唐值 | 同上处理，并复位 `gAiSpanOpen` |
| `np_hook.cpp` 自卸载路径 | 先解引用 `gTel` 再判 `gUnloading` → `UnmapViewOfFile(gTel)` 之后存在**访问违例窗口** | `gUnloading` 判断提前，并改为 `std::atomic<bool>` |
| `np_hook.cpp` 三个工厂钩子 | 自卸载期间仍会 `PatchSwapChainVtable`，把 `NpPresent` 重新写进宿主 vtable，随后 `FreeLibrary` 留下指向**已卸载内存的野指针** | 加 `!gUnloading` 判断 |
| `np_hook.cpp` `gDraws` 等 4 个计数 | 普通 `uint32_t`，却被游戏多个提交线程与 Present 线程**并发读改写** | 改 `std::atomic<uint32_t>`（relaxed）并同步全部读写点 |
| `np_hook.cpp` `EnsureD3D12Hooks` | `attempts`/`lastTryMs` 是普通静态量 → 两个 Present 线程可同时通过检查、**各建一套查询堆/fence/叠加资源**（泄漏 + 状态错乱） | 三个原子量 + `installing` CAS 闸门（谁先抢到谁装）；`InstallGuard` 保证所有返回路径都放闸 |
| `np_hook.cpp` `NpDispatchRays` 槽位分配 | `gRtDispatches.load()` 判断 + 单独 `fetch_add` → 两个提交线程可能拿到**同一个槽号**、往同一时间戳槽写（RT 占用垃圾值 + 计数丢失） | 改用 `fetch_add` 的返回值占号；越界号不插桩 |
| `np_hook.cpp` 叠加 draw 统计 | 叠加自己的全屏三角形走进我们钩的 `DrawInstanced`，**混进"游戏 draw call"统计** | 新增 `gNpCountingOverlayDraws` 抑制标志（`thread_local`，只抑制我们自己这一路） |

### 绘制层（`src/hook/np_draw.cpp`）

| 位置 | 原来的问题 | 修法 |
|---|---|---|
| `Overlay11` 纹理创建 | `CreateTexture2D` 成功后**先写** `texW_/texH_` 再建 SRV → SRV 失败后尺寸判据永远为假 → **永久拿 NULL SRV 绘制、面板再也画不出来** | 两张都建好才写尺寸；失败则释放纹理并置空 |
| `Overlay12::Release` | 漏放 `vsBlob_`/`psBlob_`，且不重置 `psoFormat_` → 重 Init 后 `EnsurePso` 会误用**已释放的旧 PSO** | 补上释放与重置 |

### 统计层（`src/common/np_stats.h`）

| 位置 | 原来的问题 | 修法 |
|---|---|---|
| `avgFps` / `lowPct` / `percentileMs` | 三个函数各用一个**函数级 `static float tmp[4096]`**（16KB）当临时缓冲 —— 那是**所有线程共享**的。`PresentCommon` 在游戏多线程呈现时并发进入，两个线程同时往同一块缓冲复制并排序 → **Low 帧 / p99 静默算错** | 改 `thread_local` |

### 面板层（`src/common/np_build.cpp`）

| 位置 | 原来的问题 | 修法 |
|---|---|---|
| `BuildPanelData` 开头 | `if (h == nullptr) return;` 会让 CPU / GPU / 内存这些**根本不需要历史数据**的行一起消失 —— 把"画不了曲线"错当成"什么都别显示" | 删掉早退；曲线有无由每处的 `showCharts` 挡 |

### 指标链路（`src/sensors/np_sensors.cpp`）

| 位置 | 原来的问题 | 修法 |
|---|---|---|
| `\GPU Engine(*)` 读取方式 | 用 `AddWildcard` 把通配符**在 AddCounter 那一刻展开成 N 个独立计数器**，实例列表从此冻结 → **游戏在 NextPerf 之后启动就永远读不到**该进程的引擎实例（用户实测：先开游戏再开 NextPerf 有数据，反过来整场没数据） | 改用**带 `*` 的计数器** + `PdhGetFormattedCounterArrayW`：实例列表每次取值时动态给出 |
| `PollGameGpu` 基线 | 函数结尾**无条件**把采样基线推到最新 → 界面轮询比阈值频繁时，间隔永远到不了阈值，**永远算不出值** | 间隔不足时保持基线不动，等它累积 |
| `PollGameGpu` 返回值 | `Poll()` 每次都把 `NPSensors` 清成 -1，而新样本要 100ms 才有 → **绝大多数轮询拿到 -1**（用户看到的"时有时无、偶尔闪一下"） | 用成员 `lastBusyMs_/lastCompute_/lastOfa_` 保留上次的值，每次轮询都填回去 |
| `PollGameGpu` 引擎占用段 | `compute`/`ofa` 两段代码写在 `if (dms < 100) return;` **之后** → 永远执行不到（"AI 引擎"行一直不显示） | 挪到提前返回之前，并同样加保留 |
| `Running Time` 单位 | 原始单位是 **100 纳秒**不是秒，PDH 对累积计数器不做换算 | 除以 `1e7` |

### 验证套件（`tests/`）

| 变更 | 原因 |
|---|---|
| 新增 `tests/struct_check.py` | 把**咬过两次的坑**（C++ 与 Python ctypes 镜像排列错位，两边字段总数与总大小相同所以大小自检抓不到）变成自动检查。**已用"故意注入历史缺陷"验证过它真的会失败** |
| 新增 `tests/verify_metrics.py` | 原来整套"回归"**只覆盖 vtable 下标与四条注入链路**，传感器与指标计算一概没验证 —— 也就是说"回归全绿"从来没验证过面板上那些数。本用例断言 PDH 指标链路在"先开 NextPerf、后开游戏"这个曾经坏掉的顺序下能出值 |
| 两者均接入 `tests/run_all.py` | 否则没人会跑 |

### ETW（`src/etw/`）

| 位置 | 原来的问题 | 修法 |
|---|---|---|
| `np_etw.cpp` `ClassifyEvent` | 取到的事件名指针指向**一个已被析构的局部 vector**，返回后仍被 `wcscpy`/`wcscmp` 使用 → **use-after-free** | 把名字拷贝进调用方提供的缓冲 |
| `np_etw.cpp` `OnEvent` | `targetPid_ == 0` 时把**全系统所有进程**的 present 都算进来 —— 没注入游戏时污染 Low 帧 | 无目标一律不统计；`main.cpp` 覆盖 Low 帧前加 pid 判断 |
| `np_etw.cpp` 事件匹配 | 名字里含 `Present` 就算一帧，`PresentHistory` / `PresentQueuePacket` 这类**记账事件**（每帧两次）混入 → 帧率翻倍成 120fps | 改为 `Present` / `Present_*` 的**边界匹配**（仍未在真机验证，见第三节） |
| `np_etw.h/.cpp` 临界区 | `CRITICAL_SECTION` 只在 `Start()` 里懒初始化 → 非管理员 / `--uismoke` 路径会在**全零的 CS** 上 `EnterCriticalSection` | 移到构造函数；`status_` 跨线程读写改加锁 + 返回拷贝 |
| `np_etw.cpp` `Snapshot` | 每次持锁对 2048 个样本**全排序**（主线程 120ms 内调 3 次，会拖住 ETW 消费者线程） | 用时间戳限流到 5Hz |

### 传感器与厂商层（`src/sensors/`）

| 位置 | 原来的问题 | 修法 |
|---|---|---|
| `np_sensors.cpp` `PdhQuery::Close` | 不清 `stars_` → `Close`/`Open` 之后 `AddStarCounter` 以为已经加过，**继续用野 `HCOUNTER`** | 一并清空 |
| `np_sensors.cpp` GPU 占用兜底 | 对所有 `\GPU Engine(*)` 实例**求和** → 必然 >100 被 clamp 成**恒 100%**；且用的是启动时冻结的实例表 | 新增 `MaxStarCounter`，取动态实例里最忙的那个引擎 |
| `np_sensors.cpp` 帧计数 | 帧计数回退（钩子重注入 / pid 复用）致 `uint32` **下溢** → GPU 帧时间恒 0 | 回退时重建基线；去掉 pid 切换时重复的 `Collect`；`compute`/`ofa` 移到**所有提前 return 之前** |
| `np_vendor.cpp` HWiNFO | 读数元素下界判 128 字节，**实际要读 292 字节** → 越界读 | 改 292；补 `offReading`/`offSensor` 的越界与无符号下溢判断 |

### 应用层（`src/app/`）

| 位置 | 原来的问题 | 修法 |
|---|---|---|
| `injector.cpp` 远端加载 | 远端 `LoadLibrary` 10 秒未返回仍 `VirtualFreeEx` —— **游戏可能还在读那段 DLL 路径内存** | 只在确认线程结束后释放；超时**故意泄漏一页** |
| `injector.cpp` 数据竞争 | 守护线程与主线程无同步共用 `injected`/`games`/`notice`（`push_back` 扩容会让另一边的迭代器失效） | 新增 `AppLock`；扫描/清理/发布/状态栏/games 增删全部入锁；10 秒等待留在锁外 |
| `injector.cpp` 日志轮转 | 日志句柄以 `FILE_APPEND_DATA` 打开，`SetFilePointer(h,0,FILE_BEGIN)` **无效** → 1MB 轮转从未生效、日志无限增长 | 改用 `GENERIC_WRITE` 截断后续写 |
| `overlay.cpp` 初始化失败 | `gPanel.Init` 失败时直接 `return false`，**分层窗口再没人销毁** | 失败时 `DestroyWindow` 后返回 |
| `ui.cpp` 滑杆 | 拖拽判定用 130/140，绘制用 120/130 → **滑块恒偏 10px** | 统一 |

### 共享结构（`src/common/np_common.h`）

| 位置 | 原来的问题 | 修法 |
|---|---|---|
| `NPClearSensors` | `s->version = 1;` **硬编码**，而 `NPClearTelemetry` 写的是 `sizeof(...)` —— 即 `NPSensors` 这一路（app 写 / hook 读）**完全没有布局自检**，而当时刚往里加了 5 个字段 | 改为 `(uint32_t)sizeof(NPSensors)` |

---

## 一·补、主代理本轮另修的（清单外）

| 位置 | 问题 | 修法 |
|---|---|---|
| `np_hook.cpp` `UpdateTelemetryCommon` | `gTel->gpuFrameMs` 的唯一写入者是**已拆除的逐批时间戳夹取** → 该字段恒为 0。面板数值走 `s.gpuBusyMs`（PDH）所以是对的，但**所有以它为源的曲线都是一条零线**：图表环形缓冲 `graphGpu`、逐帧缓冲 `gpuFrames`（用户实测："gpu 帧这个曲线全部为 0"） | **在源头**统一回填：`Sens().gpuBusyMs >= 0` 时写进 `t.gpuFrameMs`，所有消费者自动正确 |
| `np_hook.cpp` `EnsureD3D12Hooks` | `attempts`/`lastTryMs` 是普通静态量 → 两个 Present 线程可同时通过检查、各建一套查询堆/fence | 三原子 + `installing` CAS 闸门 + `InstallGuard` |
| `np_hook.cpp` `NpDispatchRays` 槽位分配 | `load()` 判断 + 单独 `fetch_add` → 两线程可能拿到同一槽号 | 改用 `fetch_add` 返回值占号 |
| `np_hook.cpp` / `np_draw.cpp` | `gNpCountingOverlayDraws` 是普通全局 `bool` → 叠加绘制期间游戏在别的线程 draw 会被误抑制 | 改 `thread_local` |
| `src/common/np_build.cpp` | 我编辑时被工具整体转成了 **CRLF**（本项目要求纯 LF） | 全仓排查并还原为 LF（`.bat` 保持 CRLF，批处理需要） |


## 二、未修复（含原因）

| 位置 | 问题 | 为什么不改 |
|---|---|---|
| `np_hook.cpp` `BeginList` | 32 个分配器 + 每帧多次 `EndList`，帧上界只在连续跳帧超 600 次后熔断，极端情况下叠加可能先停画几十秒 | 原设计是"宁可丢数据也绝不在 Present 栈里等"，改动风险大于收益 |
| `np_hook.cpp` `gTsFreq` | 取自临时队列，该队列随即 `Release`；换队列后换算可能错 | 实践中同一 device 的队列频率一致，暂不动 |
| `np_draw.cpp` `Overlay11::Draw` | 绘制后不还原 RTV / VS / PS / 拓扑 / blend，依赖游戏每帧重绑 —— 会干扰同帧后续的第三方叠加（RTSS 等） | 改造成本与风险都高，需要单独一轮 |
| `np_build.cpp` 渲染分辨率 | `gVpW/gVpH` 取"见过的最**大**视口"，UI/后处理 pass 用全分辨率视口时会盖掉 DLSS 的输入尺寸，比例显示成 100% 而不是 67% | 更可靠的做法是钩 `OMSetRenderTargets` 取渲染目标尺寸，属于新功能而非修 bug |
| `ui.cpp` `WM_DPICHANGED` | 只重算布局，不按 `lParam` 建议矩形 `SetWindowPos` → 高 DPI / 跨显示器会**裁切** | 需要决定窗口尺寸策略；本轮未动，建议下轮做 |
| `ui.cpp` `A_INJECTFRONT` | 在 UI 线程直接调 `InjectInto`（最长等 10 秒）→ **界面冻结** | 应丢给守护线程并显示"注入中"；属于交互改造 |
| `np_sensors.cpp` 显存占用 | 把 `Dedicated + Shared Usage` 一起求和（含共享内存），可能超过 DXGI 报的专用显存使百分比被 clamp | **是否有意为之不确定**，等用户确认口径 |

> `np_common.h` 的 `NPClearSensors` 布局自检缺失**已由审查者 B 修掉**（改为
> `sizeof(NPSensors)`），故不在未修列表中。

## 三、无法确定

| 项 | 说明 |
|---|---|
| ETW 帧计时的**事件匹配** | 会话能建（需管理员），但曾抓到的是 `PresentQueuePacket` / `PresentHistory` 这类**记账事件**（每帧两次 → 帧率翻倍成 120fps）。真正的 `Present_Start` 应在 `Base`（0x1）关键字下，已同时启用，但**运行时要靠用户的管理员实测确认** |
| AI 区间跨线程共用 `TS_AI_START/END` 槽 | 多线程提交超分时会互相覆盖，是否按线程分槽需要先定口径 |
| 指标数值**是否准确** | 自动化只能验证"有值"，准确性需要真实游戏 + 另一套可信来源对照 |

---

## 四、验证结果（本清单对应的代码状态）

| 项 | 结果 |
|---|---|
| 完整编译 cmd /c build.bat | **通过**（exit 0，两个目标都产出） |
| 回归 python tests\run_all.py --skip-build | **全部通过 ✓**（7 项） |
| 结构体镜像一致性 | ✅ NPTelemetry 49 字段，名字/顺序/宽度/总大小全部一致 |
| PDH 指标链路端到端 | ✅ 命中 16 个引擎实例，GPU 帧时间有值（"先开 NextPerf、后开游戏"的顺序） |
| 全仓换行符 | ✅ 源码纯 LF（.bat 保持 CRLF） |
| dist 产物 | 已重新生成 |

**未能验证的部分（如实标注）**：
* ETW 真实事件名 —— 建会话需要管理员，开发环境没有，只能靠用户管理员实测；
* 各指标数值**是否准确** —— 需要真实游戏与另一套可信来源对照；
* 游戏内长时间运行的稳定性（叠加是否持续刷新、是否崩）—— 需要用户实测。