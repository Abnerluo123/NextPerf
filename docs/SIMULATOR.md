# NextPerf 模拟游戏测试宿主（SIMULATOR）

> ## ⚠️ 改这些文件必须回来更新本文档
>
> 和 `CATALOG.md` 的约定一样：**改了模拟器的参数、输出字段或控制命令，
> 必须同步更新本文档**。模拟器是「不依赖真人也能验证产品」的唯一手段，
> 文档过期会让下一个迭代者（或压缩上下文后的我）根本不知道它能干什么。

---

## 0. 为什么需要它

NextPerf 是注入到游戏里工作的，它的正确性**只能在真实游戏里验证** ——
但真人验证代价极高：

* 每个结论都要用户开游戏、复现、截图、贴日志（本项目已经这样消耗了用户大量时间）；
* 有些场景（改分辨率、切窗口、开关垂直同步）**会让用户的游戏闪退**，
  在用户机器上反复试是不负责任的；
* 用户不在时（例如夜里）就没法推进。

所以做一套**工作区内的模拟游戏**：它自己能渲染、能自报每一帧的真实数据、
能被控制做各种「危险操作」，从而让 AI **自己**就能：

| 能做什么 | 怎么做 |
|---|---|
| 校验 hook 采到的帧数据**准不准** | 模拟器自报的 `cpu_frame_ms` / `gpu_frame_ms` / `present_ms` / 帧间隔，与钩子写进共享内存的值逐一对照 |
| 测稳定性 | 长时间跑 + 反复触发交换链重建，看崩不崩、`SELFCHECK` 是否 FAIL |
| 复现用户报的闪退 | 发 `resize` / `vsync` / `window` 命令，模拟「改分辨率 / 开关垂直同步 / 切窗口」 |
| 验证 Vulkan 层 | `simvk` 能报告「`vkQueuePresentKHR` 的分派落在哪个 DLL」 |

---

## 1. 两个模拟器

| 目录 | 产物 | 后端 | 说明 |
|---|---|---|---|
| `tests/sim/` | `sim.exe` | **DX11 + DX12** | 主力；构建脚本 `build_sim.bat`；另有 `README.md` |
| `tests/simvk/` | `simvk.exe` | **Vulkan** | 无 Vulkan SDK 环境下**手写声明 + 动态加载**；构建脚本 `build_simvk.bat`；另有 `README.md` |

两者**都自报逐帧数据 + 汇总统计**，并且都能被注入（它们是普通 64 位进程）。

---

## 2. `sim.exe`（DX11 / DX12）

### 2.1 构建

```powershell
cd NextPerf
cmd /c tests\sim\build_sim.bat      # 产出 tests\sim\sim.exe
```

### 2.2 命令行

| 参数 | 默认 | 说明 |
|---|---|---|
| `--api=dx11\|dx12` | dx11 | 图形后端 |
| `--seconds=N` | 不限 | 跑 N 秒后正常退出 |
| `--frames=N` | — | 跑 N 帧后退出（与 `--seconds` 互斥） |
| `--warmup=N` | 10 | 前 N 帧不计入统计（剔除首帧编译抖动） |
| `--no-stdin` | 关 | 不读 stdin 控制命令 |
| `--fps-cap=N` | 0 | **锁帧**，0=不锁 |
| `--vsync=on\|off` | on | **垂直同步** |
| `--width=W --height=H` | 1280x720 | 初始分辨率 |
| `--buffers=N` | DX11:2 / DX12:3 | 交换链 buffer 数（2..4） |
| `--window-mode=MODE` | windowed | windowed / borderless / fullscreen |
| `--hidden` | 关 | 不显示窗口（无头 / CI） |
| `--hold-ms=N` | 0 | 每帧在 CPU 侧额外占用 N 毫秒（**模拟逻辑开销**） |
| `--gpu-load-ms=X` | 0 | 每帧追加约 X 毫秒 GPU 工作量（**模拟 GPU 压力**） |
| `--no-tint` | 关 | 背景色不再逐帧变化（对照实验用） |
| `--json` | 关 | 每帧一行 JSON 到 **stdout**（诊断信息一律走 **stderr**） |
| `--json-every=N` | 1 | 每 N 帧输出一行 |

### 2.3 运行时控制（stdin 逐行）

```
resize W H              重建交换链到 WxH
window windowed|borderless|fullscreen
vsync on|off
fpscap N
stats / status
quit
```

### 2.4 自报字段（每帧 JSON）

`frame` · `api` · `t_ms` · `frame_delta_ms` · **`cpu_frame_ms`** · `cpu_render_ms` ·
**`gpu_frame_ms`**（`gpu_valid=false` 表示这一帧测不到）· **`present_ms`** ·
`vsync` · `fps_cap` · `width` · `height` · `window_mode` · `hidden` ·
**`swapchain_generation`** · **`resize_event`** · **`present_failed`** · `present_hr`

汇总（退出时）：平均帧率 · 帧时间 avg/P50/P99/P99.9 · **1% Low / 0.1% Low** ·
CPU 帧时间 · Present 阻塞 · GPU 时间（timestamp-disjoint）·
**交换链重建次数 · Present 失败次数 · 最终代数**

> `swapchain_generation` / `resize_event` / `present_failed` 这三个字段是
> **为复现用户所报闪退专门加的** —— 它们让「是不是我们让游戏那次调用失败了」
> 变成可观测的事实，而不是猜。

---

## 3. `simvk.exe`（Vulkan）

```powershell
cmd /c tests\simvk\build_simvk.bat
tests\simvk\simvk.exe --seconds=3 --json
tests\simvk\simvk.exe --list-layers      # 列出隐式/显式层（不创建实例，无副作用）
```

参数与 `sim.exe` 基本一致（`--seconds/--frames/--vsync/--fps-cap/--width/--height/`
`--window-mode/--json/--no-stdin/--list-layers`），控制命令为
`resize` / `mode` / `vsync` / `fpscap` / `quit`。

### 它对 Vulkan 支持最有价值的地方

**「层到底挂上了没」有进程内硬证据**：用 `GetModuleHandleExW(FROM_ADDRESS)`
反查函数指针落在哪个模块。实测：

```
vkCreateSwapchainKHR  -> GPP_VKLayer64.dll    ← 第三方隐式层确实自动挂上了（无需 opt-in）
vkQueuePresentKHR     -> nvoglv64.dll         ← 没有任何层拦 Present，直连 NVIDIA ICD
```

汇总 JSON 里给了机器可读字段 `present_dispatch_module` /
`present_wrapped_by_layer` / `attached_layer_dlls[]` / `wrapped_functions`
**供 CI 断言** —— NextPerf 将来做 Vulkan 层时，就是靠这些字段确认
`vkQueuePresentKHR` 的分派从 `nvoglv64.dll` 变成我们的层 DLL。

> ⚠️ **陷阱（子代理实测踩过）**：现代 loader 的 `vkGetDeviceProcAddr` 会把
> **未被层拦截**的 device 级函数**直接返回 ICD 实现**，所以
> 「指针不是 `vulkan-1.dll`」≠「被层拦截」。必须靠层清单的 `library_path`
> 名单区分 ICD 与层。

> ⚠️ **另一个陷阱**：垂直同步的等待在 **`vkWaitForFences`**，不在 Present
> （实测 wait 16.451 ms vs present 0.084 ms）。所以 **CPU 帧时间绝不能从帧首量起**，
> 否则恒等于 16.67 ms —— 这正是 `np_common.h` 里警告过的坑。

---

## 4. 压力探针 `tests/sim_stress.py`

```powershell
python tests\sim_stress.py                 # 注入钩子，跑 12 步重建场景
python tests\sim_stress.py --api=dx12
python tests\sim_stress.py --no-inject     # 对照组：不注入，看模拟器自己稳不稳
```

它做的事：启动模拟器 → 注入钩子 → 依次发 12 条会触发**交换链重建**的命令
（resize / vsync / window / fpscap）→ 收集模拟器逐帧 JSON 与钩子日志里的
`SEH` 条数，做客观对照。

**对照组很重要**：只有先确认「不注入时模拟器自己稳」，注入组的崩溃才能归因给钩子。

### 4.1 ⚠️ 写这类探针时必须避开的三个坑（我全踩过）

1. **不要把正在被写入的日志改名/删除**。钩子持有旧文件句柄，改名后它会继续写进
   被改名的文件，于是新建的 `NextPerfHook.log` 只剩零星几行 ——
   会被误判成「钩子没挂上」。
2. **不要用 `stdout=PIPE` 却只在末尾读**。模拟器每帧往 stdout 写 JSON，
   管道缓冲区写满后它会**阻塞在 `write` 上**，表现为「stdin 命令不再被处理」，
   极容易被误判成「钩子干扰了模拟器」。**stdout 请直接落文件**（没有容量上限）。
3. **探针时限必须大于场景耗时**。否则模拟器到时自动退出（退出码 `-1`），
   会被误判成崩溃。**退出码 -1 不等于崩溃**，要看是不是到时正常退出。

**通用教训**：判断「某现象是不是钩子造成的」之前，**先确认现象本身是真的**。
本项目的探针误判过三次，两次是测试骨架自己的问题。

---

## 5. 已知问题 / 待办

| 项 | 状态 |
|---|---|
| `sim.exe` 在**切换垂直同步**时会崩（对照组、无注入也崩） | ❌ **待修** —— 修好后压力测试的基准才可靠 |
| `sim.exe` 的 `--window-mode=fullscreen` | 未充分验证 |
| `simvk` 的独占全屏真正切换 | ❌ 本机 `EnumDisplaySettingsW` 枚举出 0 个模式，成功分支走不到（失败时优雅回退为无边框 ✓） |
| `simvk` 的 MAILBOX / FIFO_RELAXED 分支 | ❌ 未跑到（本机 IMMEDIATE 可用，总选它） |
| `simvk` 不是性能负载 | ⚠️ 只有 1 渲染通道 / 1 管线 / 1 次 `vkCmdDraw` / 3 顶点，GPU 帧时间约 0.005 ms。它测**帧管线与层注入**，不测 GPU 压力 |
| 数据对照（钩子 vs 模拟器自报） | ❌ **尚未做** —— 这是模拟器最大的用途，见下 |

### 5.1 下一步（最高价值）

**用模拟器的自报数据反过来校验钩子的采集准确性**：
同一段时间内，模拟器说 `cpu_frame_ms = X`、`gpu_frame_ms = Y`、帧间隔 `Z`，
钩子写进共享内存的 `cpuBusyMs` / `gpuFrameMs` / `frameMs` 应该与之吻合
（允许口径差异，但趋势与量级必须一致）。**这是本项目第一次能做「有 ground truth
的数值校验」**，比过去靠用户肉眼看面板可靠得多。
