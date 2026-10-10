# NextPerf 日志规范（LOGGING）

> ## 🚨 第一条，也是最重要的一条：日志文件在哪
>
> **AI 助手（包括未来的我）必须先读这一节，否则你会读错文件、得出错误结论。**
>
> 本项目有**两个**日志系统、**两个**位置：
>
> | 日志 | 谁写的 | **真实位置（用户的机器）** | 编码 |
> |---|---|---|---|
> | `NextPerf.log` | 主程序 `NextPerf.exe` | `C:\Users\Abner\AppData\Local\Temp\NextPerf.log` | **GBK** |
> | `NextPerfHook.log` | 注入到游戏里的 `NextPerfHook.dll` | `C:\Users\Abner\AppData\Local\Temp\NextPerfHook.log` | **GBK** |
>
> ### ⚠️ 为什么这里要专门警告
>
> **AI 自己的工作区里，`$env:TEMP` 是被沙箱重定向过的**（形如
> `...\Temp\dsh-XXXXXX\`）。也就是说：
>
> * **AI 自己的测试脚本**启动的进程 → 日志落在**沙箱 TEMP**里；
> * **用户自己**跑 NextPerf 产生的日志 → 落在**真实 TEMP**
>   （`C:\Users\Abner\AppData\Local\Temp\`）里。
>
> **这两者不是同一个目录。** 已经因此出过事故：
> 我去读"用户刚才那次测试的日志"，实际读到的是**我自己的测试脚本**留下的
> 旧日志，于是把一个不存在的问题当成用户反馈去查，浪费了大量时间。
>
> ### 铁律
>
> 1. **要看用户的实际运行情况，就去读工作区外的真实路径**（上面的表），
>    不要去读 `$env:TEMP`。
> 2. **要看自己测试脚本的结果，才读 `$env:TEMP`**（那是沙箱里的）。
> 3. 用 PowerShell 读时**必须指定 GBK**：
>    ```powershell
>    $real = "C:\Users\Abner\AppData\Local\Temp"
>    $b = [System.IO.File]::ReadAllBytes("$real\NextPerfHook.log")
>    [System.Text.Encoding]::GetEncoding('GBK').GetString($b) -split "`r?`n"
>    ```
> 4. **不要问用户"把日志贴给我"** —— 直接去读。用户明确说过这件事。
> 5. 日志被**截断**：钩子日志超过 512 KB 会从文件头开始覆盖，
>    所以**钩子日志的最新内容在文件开头**（不是末尾！）。
>    主程序日志则是常规追加，**最新在末尾**。
>    定位某次崩溃时，两端的读法不一样。
> 6. 主程序还有**诊断文件**，位置用界面上的「打开日志文件夹」按钮最稳。

---

## 1. 日志的两种角色

| | 主程序日志 | 钩子日志 |
|---|---|---|
| 文件 | `NextPerf.log` | `NextPerfHook.log` |
| 写入者 | `src/app/*` 里的 `AppLog()` | `src/hook/np_hook.cpp` 里的 `Log()` |
| 追加方式 | 追加，最新在**末尾** | 超过 512 KB 从头覆盖，最新在**开头** |
| 用途 | 注入决策、配置、界面动作、学习/忘记、退出 | 挂钩过程、帧数据、SEH、卸载 |
| 典型内容 | `auto-inject:` `detach:` `learn:` `forget:` `overlay mode:` `telemetry:` | `SEH:` `PatchSwapChainVtable:` `RestoreAllHooks:` `low compare:` `overlay 5s:` |

**同一个现象往往要两边一起看**：
例如"清空名单后钩子没卸载"，主程序侧看 `detach: 请求…`，
钩子侧看 `detachPid matched -> SelfUnloadNow`，缺哪一边就知道断在哪。

---

## 2. 当前日志格式

### 主程序 `NextPerf.log`

```
[HH:MM:SS.mmm pid=<自身pid>] <正文>
```
例：
```
[02:30:52.109 pid=3536] overlay mode: mode=0 attached=0 (tele=1 ft=5023 growAge=86469ms pid=54720) monitoring=1 off=0 -> wantDesktop=1
[02:29:11.004 pid=3536] detach: 请求 pid=54720 的钩子自卸载
```

### 钩子 `NextPerfHook.log`

```
[HH:MM:SS.mmm pid=<游戏pid> <exe名> tid=<线程>] <正文>
```
例：
```
[23:12:42.430 pid=51568 re8.exe tid=35848] SEH: code=0xc0000005 at nvwgf2umx.dll+0x330124
[02:29:26.040 pid=54720 re8.exe tid=50908] RestoreAllHooks: swapVt=0 factory=0 factory2=0 queue=0 cmdlist=0 seh=0
```
> **墙钟时间 + exe 名是后加的**：以前只有"开机后毫秒数"和 pid，
> 排查时既对不上用户说的时间点、也不知道是哪个游戏。改动见
> `docs/HOOK.md`。SEH 记录用 `GetModuleHandleExA(FROM_ADDRESS)` 反查模块，
> 解析不出来时打 `?+%p`。

---

## 3. 关键诊断行速查（照着查问题）

### 注入相关（主程序）
| 关键字 | 含义 |
|---|---|
| `auto-inject:` | （**已移除**靠特征猜的自动注入，见 CATALOG 教训 9） |
| `watch: <exe> is running (pid=…)` / `exited` | 已添加的游戏进程出现/退出 |
| `inject <who>: …` | 注入结果 |
| `detach: 请求 pid=… 的钩子自卸载` | 已发出卸载请求 |
| `detach: 清除 detachPid（…）` | 卸载请求已回收（超时或进程退出） |
| `forget: 已移除 <exe>（pid=…）` | 清空/移除条目 |
| `learn: 注入后验证通过… -> 把 <exe> 记入可信名单` | 自动学习成功 |
| `telemetry: pid=… 帧数停增 … 判定钩子已死，清空遥测` | 陈旧遥测回收 |

### 状态相关（主程序）
| 关键字 | 含义 |
|---|---|
| `overlay mode: …` | 每 5 秒一条，桌面 HUD 的显示判定依据 |
| ` 钩子: attached=… 帧数=… api=… flags=…` | 诊断 dump 里的钩子概览 |
| `UI 重叠警告：…` | 两个同类型控件矩形相交（点击会被先注册的吃掉） |

### 钩子侧
| 关键字 | 含义 |
|---|---|
| `SEH: code=… at <模块>+<偏移>` | 结构化异常。**同一偏移反复出现 = 那个模块里一个确定的崩溃点** |
| `ProbeSwapChainVtable: …` | 临时交换链探测（`no vtable obtained` / `SEH during probe, abandoned`） |
| `PatchSwapChainVtable: GetBuffer/GetDesc not in dxgi` | vtable 归属检查 |
| `RestoreAllHooks: swapVt=… factory=… seh=…` | 卸载时还原了几个补丁（**全 0 = 干净卸载**） |
| `low compare: merged1=… raw1=…` | Low 帧对照（合并后 vs 未合并） |
| `overlay 5s: drawn=… skipped=… heapSwaps=…` | 叠加层绘制统计（`heapSwaps` 增长说明交换链被重建过） |
| `cpu split avg: busy=… wait=… sum=…` | CPU 帧时间拆分自检（**sum 必须等于 busy+wait**） |
| `detachPid matched -> SelfUnloadNow (asked by host)` | 钩子收到卸载请求 |

---

## 4. 自检（self-check）设计原则

**这是本项目的核心工程习惯**：凡是"三个数应该相等""这个值应该在范围内"
的关系，**都要在日志里打出来自检**，而不是靠人眼看。

已落地的自检例子：

| 自检 | 在哪 | 说明 |
|---|---|---|
| `cpu split avg: busy=… wait=… sum=…` | 钩子 | `sum` 必须等于 `busy + wait`（同一次减法导出，由构造保证） |
| `low compare: merged… raw…` | 钩子 | 合并前后对照，判断帧内合并是否生效/误合并 |
| `hookFlags` / `gfxApi` / `frameTotal` | 主程序 | 判断"钩子是否真的接管"，**不用时钟** |
| `RestoreAllHooks: …=0` | 钩子 | 确认卸载干净 |
| `overlay 5s: drawn/skipped/heapSwaps` | 钩子 | 确认叠加层在画、交换链重建了几次 |
| `UI 重叠警告` | 主程序 | 布局错误自动暴露 |
| PDH 未出数的原因 | 主程序 | 写明命中几个实例、累计多少秒 |

### 已落地的机器可断言自检：SELFCHECK

格式约定：

    SELFCHECK <名字> = OK|FAIL [细节]

目前有 SELFCHECK unloadClean（卸载时必须把 swapVt / factory / factory2 /
queue / cmdlist / VEH 句柄**全部**清空 —— 任何一项残留都意味着进程里留下了
指向即将被卸载代码的补丁，是「重复注入闪退」的一号根因）。

**测试脚本可以直接 grep 这一行做断言**，不需要人眼判断。

### ⚠️ 已知的测试缺口（2026-10-11 发现）

	ests/verify_reinject.py 名义上测「重复注入」，但钩子日志显示它**从未走到
卸载路径** —— 它用的是进程被强杀（TerminateProcess 不执行 DllMain），
所以**干净卸载这条链其实没有被这条测试覆盖**。
真正的干净卸载要用 NPConfig.detachPid 触发（钩子守卫线程会走
SelfUnloadNow）。等 	ests/sim/ 的模拟游戏可用后，应当补一条
「注入 -> 改分辨率/切窗口 -> detachPid 卸载 -> 断言 SELFCHECK unloadClean = OK」
的测试。

**新增功能时请一并想**：这个改动**怎么在日志里证明它是对的**？
想不出来就说明还没设计好自检。

---

## 5. 待改进（roadmap）

* [ ] **统一日志等级**：现在只有一条流，没有 level。计划加 `[E]`/`[W]`/`[I]`/`[D]`
      前缀，并提供"详细模式"开关，避免正常运行时日志过吵。
* [ ] **结构化日志**：关键事件（注入、卸载、学习、崩溃）打成 `key=value` 单行，
      便于脚本 grep 与自动断言。
* [ ] **崩溃上下文**：SEH 记录时附带最近一次 Present 的状态
      （`gInPresent`、当前 swapchain、重建计数），便于判断崩溃发生在哪个阶段。
* [ ] **自检汇总**：退出时输出一份"本次会话自检报告"（各项自检是否通过）。
* [ ] **日志轮转**：钩子日志现在是"超过 512 KB 从头覆盖"，
      会丢历史。计划改成 `NextPerfHook.log.1` 轮转。
* [ ] **时间戳一致性**：主程序与钩子都打墙钟时间，已经能对齐；
      后续可加单调递增的序号便于跨文件排序。

---

## 6. 给未来的 AI：改日志时要注意什么

1. **改了格式，必须回来改这份文档**（`docs/LOGGING.md`）和 `docs/CATALOG.md`。
2. **不要把日志写在会被沙箱重定向的位置**，也不要指望 `$env:TEMP` 就是用户的 TEMP。
3. **加日志时想清楚"这条日志将来谁会看、用来判断什么"**，
   而不是"打印一下方便调试"。日志是本项目的**主要证据来源** ——
   很多 bug 是靠日志定位的（跨进程时钟、驱动崩溃偏移、Low 帧钉住…）。
4. **日志里不要打敏感信息**（路径可以，进程全路径尽量只打 basename）。
5. **性能**：钩子日志在游戏进程里，**不要每帧都打**。现有做法是每 5 秒一条
   （`low compare` / `overlay 5s`），请沿用这个节奏。
