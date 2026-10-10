# simvk —— NextPerf 的 Vulkan 模拟游戏进程

一个**真实的 Vulkan 程序**，用来给 NextPerf 将来的 Vulkan 支持当靶子。

## 它解决什么问题

NextPerf 现在只支持 D3D11 / D3D12，靠挂钩 DXGI 的 `Present` 和 D3D 的命令队列采集帧数据。
Vulkan 是另一条完全独立的路径，而且**不能照搬 vtable 钩子那一套** —— Vulkan 的 loader
是开源的，函数指针由 `vkGetInstanceProcAddr` / `vkGetDeviceProcAddr` 动态派发，业内
（RTSS、Steam Overlay、OBS）公认的正确做法是写一个 **Vulkan 隐式层**：

```
HKLM\SOFTWARE\Khronos\Vulkan\ImplicitLayers
    "C:\...\VK_LAYER_NEXTPERF.json" = dword:00000000
```

`simvk` 就是在做那条路之前必须先有的**测试目标**，用来回答三个问题：

1. 隐式层能不能被 loader 自动挂到我的进程上？
2. 层能不能拿到 `vkQueuePresentKHR`，并且每帧都被调到？
3. 层从我这里采到的帧数据（帧序号、CPU/GPU 帧时间、Present 阻塞时长）到底对不对？

它刻意做成"一个正常得不能再正常的 Vulkan 游戏"：走标准分派路径、标准同步、
每帧 Present 一次、支持运行时 resize / 切窗口模式 / 切 vsync / 锁帧。

## 文件

| 文件 | 说明 |
|---|---|
| `vk_min.h` | 手写的 Vulkan 最小声明 + 三级动态加载封装（44 个 `static_assert` 锁死结构体布局） |
| `simvk_main.cpp` | 主程序：窗口 / 交换链 / 渲染 / 计时 / 统计 / stdin 命令 |
| `build_simvk.bat` | 构建脚本（zig），产物 `simvk.exe` |
| `simvk.exe` | 构建产物 |
| `README.md` | 本文件 |

依赖：**只需要 zig**。不需要 Vulkan SDK、不需要 MSVC、不需要 Windows SDK。

```bat
tests\simvk\build_simvk.bat
```

---

## 环境探测结果（本机实测）

| 项目 | 结果 |
|---|---|
| `C:\Windows\System32\vulkan-1.dll` | **存在**，版本 1.4.341.0（Vulkan Loader） |
| loader 支持的实例版本 | 1.4.341 |
| 物理设备 | **2 个**：NVIDIA GeForce RTX 5080 Laptop GPU（独显, api 1.4.351, 驱动 617.42）、Intel(R) Graphics（核显, api 1.4.303） |
| Vulkan SDK / 头文件 | **没有**（全盘无 `vulkan.h`）→ 所以声明全部手写 |
| `glslangValidator` / `spirv-as` / `dxc` | **没有** → 所以 SPIR-V 是手工汇编的 |
| 可用层 | 6 个：隐式 4 + 显式 2 |

> 注意：`HKLM\SOFTWARE\Khronos\Vulkan\Drivers` 这个 ICD 注册表键**在本机不存在**
> （`reg query` 确认），但 Vulkan 依然能枚举出 2 个设备。所以不要把"ICD 注册表键存在"
> 当成"Vulkan 可用"的判据 —— 唯一的判据是 `vkEnumeratePhysicalDevices` 的返回。

---

## 参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `--seconds=N` | 0 | 运行 N 秒后退出（支持小数，如 `--seconds=3.5`） |
| `--frames=N` | 0 | 渲染 N 帧后退出 |
| `--vsync=on\|off` | `on` | on → `VK_PRESENT_MODE_FIFO_KHR`；off → 优先 `IMMEDIATE`，其次 `MAILBOX`，都没有则回退 `FIFO` 并打印警告 |
| `--fps-cap=N` | 0 | 锁帧，0 = 不锁。用 QPC 计时 + `Sleep(1)`/自旋混合（`timeBeginPeriod(1)`） |
| `--width=W` `--height=H` | 1280x720 | 窗口 / 交换链尺寸 |
| `--window-mode=M` | `windowed` | `windowed` / `borderless` / `fullscreen`（见下方"未验证"） |
| `--json` | 关 | 每帧一行 JSON 到 **stdout**；人类可读信息改走 **stderr**（互不污染） |
| `--no-stdin` | 关 | 不启动 stdin 命令线程（纯自动化跑时用） |
| `--list-layers` | — | 列出系统上所有 Vulkan 层（隐式/显式）后退出，**不创建实例** |
| `--help` | — | 用法 |

退出码：`0` 正常 / `1` 参数错误 / `2` Vulkan 初始化失败 / `3` 加载不了 `vulkan-1.dll`。

### 运行时 stdin 命令

逐行下发，一行一条：

| 命令 | 效果 |
|---|---|
| `resize W H` | 重建交换链。窗口模式下改窗口客户区；无边框/全屏模式下只改交换链图像尺寸（会被拉伸到窗口） |
| `mode windowed\|borderless\|fullscreen` | 切窗口模式并重建交换链（恢复显示模式由程序负责） |
| `vsync on\|off` | 切 present mode 并重建交换链 |
| `fpscap N` | 改锁帧值，0 = 不锁。立即生效，无需重建 |
| `quit` | 退出（`exit` 同义） |

`stdin` 读到 EOF 时是否退出，取决于 stdin 类型（实测过的行为）：
**管道**（Python 的 `stdin.close()`）→ 退出；**重定向文件** → 不退出；**控制台** → 阻塞等待输入。

---

## 自报字段

### 每帧一行 JSON（`--json`）

```json
{"type":"frame","frame":60,"lag_frames":2,"warmup":0,
 "dt_ms":16.874,"fps":59.26,
 "wait_ms":16.292,"acquire_ms":0.007,"cpu_ms":0.139,"submit_ms":0.099,"present_ms":0.100,
 "gpu_ms":0.0045,"image":0,"w":1280,"h":720,
 "present_mode":"fifo","fps_cap":0,"window":"windowed","swap_recreated":0}
```

| 字段 | 单位 | 含义 |
|---|---|---|
| `frame` | — | 帧序号，从 1 开始，单调递增不重复也不跳号 |
| `lag_frames` | — | 本行相对渲染进度的滞后帧数（=2，见下方"JSON 滞后"） |
| `warmup` | 0/1 | 1 = 落在预热期（前 30 帧），下游可自行取舍 |
| `dt_ms` | ms | **帧周期** = 上一帧起点 → 本帧起点 |
| `fps` | — | `1000 / dt_ms`；`dt_ms < 0.01` 时报 0（避免算出 10000000 这种假值） |
| `wait_ms` | ms | 卡在 `vkWaitForFences` 里的时长 |
| `acquire_ms` | ms | `vkAcquireNextImageKHR` 时长 |
| `cpu_ms` | ms | **纯 CPU 干活时间**（上传顶点 + 录制命令 + 提交），**不含任何等待** |
| `submit_ms` | ms | 其中 `vkQueueSubmit` 那一段 |
| `present_ms` | ms | `vkQueuePresentKHR` 进入 → 返回 |
| `gpu_ms` | ms | 本帧 GPU 执行时间（`vkCmdWriteTimestamp` 首尾差值 × `timestampPeriod`）。**`-1` = 测不到**（时间戳查询不可用或读数作废），不是 0 |
| `image` | — | 交换链图像索引 |
| `w` `h` | px | 交换链图像尺寸（**不是**窗口尺寸） |
| `present_mode` | — | `fifo` / `immediate` / `mailbox` / `fifo_relaxed` |
| `fps_cap` | — | 本帧生效的锁帧值，0 = 不锁 |
| `window` | — | `windowed` / `borderless` / `fullscreen` |
| `swap_recreated` | 0/1 | 1 = 本帧渲染前重建过交换链（该帧数据受重建影响，建议丢弃） |

时间口径：`dt_ms ≈ wait_ms + acquire_ms + cpu_ms + present_ms + 锁帧睡眠 + 循环开销`。
约等式成立，残差是消息泵和记账开销（实测几微秒到几十微秒）。

### 汇总（最后一行）

```json
{"type":"summary","stop_reason":"seconds","frames_total":182,"warmup_excluded":30,"frames_counted":152,
 "seconds":3.011,"avg_fps":60.00,"avg_frame_ms":16.667,
 "ft_p50_ms":16.662,"ft_p99_ms":16.996,"ft_p999_ms":17.635,
 "low1_fps":58.84,"low01_fps":56.72,"low1_avg_fps":57.49,"low01_avg_fps":56.37,
 "avg_wait_ms":16.451,"avg_cpu_ms":0.097,"avg_gpu_ms":0.0046,"avg_present_ms":0.084,
 "gpu_timing":true,"device":"NVIDIA GeForce RTX 5080 Laptop GPU","api_version":"1.4.351",
 "w":1280,"h":720,"present_mode":"fifo","fps_cap":0,"window":"windowed","vsync":true,
 "layers_available":6,"implicit_layers":["VK_LAYER_GAMEPP", "..."],
 "present_dispatch_module":"nvoglv64.dll","present_wrapped_by_layer":false,
 "wrapped_functions":1,"attached_layer_dlls":["GPP_VKLayer64.dll"]}
```

| 字段 | 说明 |
|---|---|
| `stop_reason` | `seconds` / `frames` / `quit` |
| `frames_total` | 本次提交的总帧数 |
| `warmup_excluded` | **被剔除的预热帧数**（= `min(总帧数/5, 30)`） |
| `frames_counted` | 真正参与统计的帧数 |
| `avg_fps` `avg_frame_ms` | 平均帧率 / 平均帧时间（只算统计样本） |
| `ft_p50_ms` `ft_p99_ms` `ft_p999_ms` | 帧时间的 P50 / P99 / P99.9 百分位 |
| `low1_fps` `low01_fps` | **对 FPS 取 P1 / P0.1 百分位**（严格按需求定义） |
| `low1_avg_fps` `low01_avg_fps` | PresentMon 口径：最慢 1% / 0.1% 帧的**平均帧时间**换算成 FPS |
| `avg_wait_ms` | 平均 `vkWaitForFences` 时长 |
| `avg_cpu_ms` | 平均纯 CPU 帧时间（不含等待） |
| `avg_gpu_ms` | 平均 GPU 帧时间；`-1` = 不可用 |
| `avg_present_ms` | 平均 Present 阻塞时长 |
| `gpu_timing` | 时间戳查询是否可用 |
| `present_dispatch_module` | `vkQueuePresentKHR` 这个函数指针**实际落在哪个模块**里 |
| `present_wrapped_by_layer` | 上者是否是层 DLL（= 有没有层拦截 Present） |
| `wrapped_functions` | 被层拦截的函数个数（共探测 6 个） |
| `attached_layer_dlls` | 出现在分派表里的层 DLL 列表 |

百分位算法：线性插值，`idx = p × (n−1)`，在 `floor(idx)` 与 `ceil(idx)` 之间插值。
两套 low 都报，是为了和别的工具对数据时能区分「差异来自算法」还是「差异来自采集」。

初始化失败时 stdout 会输出一行 `{"type":"error","stage":"...","message":"..."}`。

---

## 怎么用它测 NextPerf 的 Vulkan 层

这是本工具的核心用途。

### 1. 确认层注册上了

```bat
simvk.exe --list-layers
```

它会打印每个层的**隐式/显式分类**（依据是注册表 `ImplicitLayers` / `ExplicitLayers`
的实际内容 —— Vulkan **没有任何 API** 能告诉你一个层是隐式还是显式，只能读注册表）、
清单路径、**层 DLL 是否存在**、以及该层的 `disable_environment` / `enable_environment`
变量名（想临时禁用某个层时直接照抄）。

### 2. 确认层真的挂上了（关键）

直接跑，看 `[分派归属]` 那一段：

```
[分派归属]   vkQueuePresentKHR          -> nvoglv64.dll                 驱动 ICD（直接分派，无层介入）
[分派归属]   vkCreateSwapchainKHR       -> GPP_VKLayer64.dll            层 DLL（已被层拦截）
```

原理：`vkGetDeviceProcAddr` 返回的函数指针落在哪个模块里，是**进程内的硬证据**，
不依赖任何日志。没有被层拦截的 device 级函数会被 loader 直接分派到 ICD
（所以会出现 `nvoglv64.dll`），一旦某个层拦截了它，指针就落进层 DLL。

**注意**：这里只能证明"被某个层拦截了"，不能指出是哪个层 —— 不过模块名通常就写着。
另外，某个函数显示"无层介入"**不等于**"没有层被加载"（层可能加载了但选择不拦截它）。

实测结论（本机）：GamePP 的隐式层会自动挂上并拦截 `vkCreateSwapchainKHR`，
但 **`vkQueuePresentKHR` 谁都没拦** —— 这正好是 NextPerf 的层要补的位置。

### 3. 自动化断言

`--json` 的 summary 里有 `present_wrapped_by_layer` 和 `attached_layer_dlls`，
可以直接在 CI / 测试脚本里断言。

### 4. 用 loader 自己的日志交叉验证

```bat
set VK_LOADER_DEBUG=layer
simvk.exe --seconds=2
```

loader 会把每个层的加载/协商过程打到 stderr。

### 5. 临时启用 / 禁用层

```bat
set VK_INSTANCE_LAYERS=VK_LAYER_GAMEPP      &  rem 追加启用
set DISABLE_VK_LAYER_MEDIASDK_HOOK_1=1      &  rem 禁用（变量名从 --list-layers 抄）
```

> ⚠ 实测坑：在 loader 1.4.341 上，`VK_INSTANCE_LAYERS` 里写一个**不存在**的层名，
> `vkCreateInstance` **不会**失败，loader 只是忽略它。所以"实例创建成功"
> **不能**用来证明 `VK_INSTANCE_LAYERS` 生效了。

---

## 实测输出

```
> simvk.exe --seconds=3 --json
（stdout，共 183 行 = 182 帧 + 1 行 summary）
{"type":"frame","frame":60,"lag_frames":2,"warmup":0,"dt_ms":16.874,"fps":59.26,
 "wait_ms":16.292,"acquire_ms":0.007,"cpu_ms":0.139,"submit_ms":0.099,"present_ms":0.100,
 "gpu_ms":0.0045,"image":0,"w":1280,"h":720,"present_mode":"fifo","fps_cap":0,
 "window":"windowed","swap_recreated":0}
...
{"type":"summary","stop_reason":"seconds","frames_total":182,"warmup_excluded":30,
 "frames_counted":152,"seconds":3.011,"avg_fps":60.00,"avg_frame_ms":16.667,
 "ft_p50_ms":16.662,"ft_p99_ms":16.996,"ft_p999_ms":17.635,
 "low1_fps":58.84,"low01_fps":56.72,"low1_avg_fps":57.49,"low01_avg_fps":56.37,
 "avg_wait_ms":16.451,"avg_cpu_ms":0.097,"avg_gpu_ms":0.0046,"avg_present_ms":0.084,
 "gpu_timing":true,"device":"NVIDIA GeForce RTX 5080 Laptop GPU","api_version":"1.4.351",
 ...}
```

其他已验证的选项实测值：

| 命令 | 实测结果 |
|---|---|
| `--seconds=2`（默认 vsync on） | 122 帧 / **60.00 FPS** / `fifo` |
| `--seconds=2 --vsync=off` | 15899 帧 / **7982 FPS** / `immediate` |
| `--seconds=2 --vsync=off --fps-cap=120` | 241 帧 / **119.87 FPS** |
| `--seconds=2 --fps-cap=30` | 61 帧 / **29.97 FPS** |
| `--frames=100` | 恰好 100 帧，`stop_reason=frames` |
| `--width=800 --height=600` | 800x600 |
| `--window-mode=borderless` | 1707x1067（铺满显示器） |
| stdin `resize` / `vsync` / `mode` / `fpscap` / `quit` | 全部生效，交换链按需重建 |

### 一个值得注意的实测发现

开垂直同步时，**等待发生在 `vkWaitForFences` 里，不在 `vkQueuePresentKHR` 里**：

```
平均 等待(WaitForFences): 16.451 ms   ← 垂直同步的等待主要在这里
平均 取图(AcquireNextImg): 0.007 ms
平均 CPU 帧时间         : 0.097 ms   （只算真正干活，不含等待）
平均 Present 阻塞       : 0.084 ms
```

现代 WDDM 的翻转模型下 Present 很快返回，节流体现为"这一槽位的 fence 迟迟不 signal"。
所以**如果 CPU 帧时间从帧首开始量，它会恒等于 16.67ms —— 那是帧周期，不是 CPU 的活**。
（`src/common/np_common.h` 里对这个坑有明确警告，本工具第一版正好踩了进去，已修正。）

### 关于预热帧

开头约 5~10 帧是瞬态：驱动要编译管线、交换链刚建好时 3 张图像都空闲（垂直同步还没开始节流），
`dt_ms` 会低到 0.2ms。直接算进统计会把 P99.9 和 1% Low 拉垮（实测 1% Low 从 58 掉到 30）。
所以汇总统计会剔除 `min(总帧数/5, 30)` 帧，并在 `warmup_excluded` 里**明确报出丢了多少**。
单帧 JSON 里前 30 帧带 `"warmup":1` 标记，下游可以自行取舍。

---

## 未验证 / 未实现（重要，别当成已经能用）

### 未验证（代码写了，但本环境跑不到那条分支）

1. **独占全屏的真正切换**。`--window-mode=fullscreen` 的实现是：用
   `EnumDisplaySettingsExW` 找匹配 `W×H` 的显示模式 → `ChangeDisplaySettingsExW(CDS_FULLSCREEN)`
   → 成功后记住待还原。**但本机 `EnumDisplaySettingsW` 一个模式都枚举不出来**
   （返回 0 个），`ChangeDisplaySettingsExW` 也返回 `DISP_CHANGE_FAILED(-1)`，
   所以在**本环境里这条分支永远走不到**。
   - ✅ 已验证：找不到模式 / 切换失败时**优雅回退为无边框全屏**，并打印原因，**不改动显示模式**。
   - ❌ **未验证**：切换成功后独占全屏的实际表现，以及退出时的还原
     （还原路径有三条：切回窗口模式时、`atexit`、Ctrl+C 处理函数）。
   - ❌ **未实现**：真正的 Vulkan 独占全屏需要 `VK_EXT_full_screen_exclusive`
     （`VkSurfaceFullScreenExclusiveWin32InfoEXT` + `vkAcquireFullScreenExclusiveModeEXT`），
     本工具**没有**用它，只用了 Win32 的 `CDS_FULLSCREEN`。
2. **`MAILBOX` / `FIFO_RELAXED` present mode 没被实际跑到**。本机 `IMMEDIATE` 可用，
   `--vsync=off` 总是选中 `IMMEDIATE`，所以 `MAILBOX` 分支的代码路径只做了可用性探测，
   没被真正使用过。
3. **Intel 核显路径没测**。设备选择总是挑独显（discrete > integrated 打分）。
   没有 `--gpu=N` 参数，无法指定用核显跑。Intel 驱动上的 GPU 时间戳行为未验证。
4. **窗口最小化时的 idle 路径没测**。代码里有 `IsIconic()` 判断（最小化时不渲染、不计帧），
   但自动化测试没法可靠地最小化窗口。
5. **设备丢失（`VK_ERROR_DEVICE_LOST`）没测**。代码有分支会打印错误并退出，
   但没办法在不搞崩显卡驱动的前提下触发。
6. **多显示器 / 热插拔没测**。只用 `MonitorFromWindow(..., MONITOR_DEFAULTTOPRIMARY)`，
   永远在主显示器上。
7. **Steam 的两个层没验证到拦截行为**。`--list-layers` 能看到它们注册了、DLL 也在，
   但分派表里没出现 `SteamOverlayVulkanLayer64.dll` —— 没去查是"加载了但没拦截"
   还是"根本没加载"（Steam 覆盖层通常需要 Steam 客户端在跑，靠
   `ENABLE_VK_LAYER_VALVE_steam_overlay_1=1` 打开）。
8. **`VK_LAYER_MEDIASDK_HOOK` 是坏的**：它的清单指向
   `C:\ProgramData\mediasdkhook\game_detour_64.dll`，该文件**不存在**。
   loader 每次都会报 `Failed to open dynamic library ... error 126`，但不影响其他层。
   （这是第三方工具的问题，不是本程序的。）
9. **没有用校验层（validation layers）**。本机没装 `VK_LAYER_KHRONOS_validation`，
   所以程序里的 Vulkan API 误用不会被抓出来。手工汇编的 SPIR-V 是"驱动接受了"级别，
   不是"通过 spirv-val 校验"级别（机器上没有 `spirv-val`）。
10. **长时间稳定性**。最长只跑到 3 秒 / 16000 帧（vsync off）。没有做小时级压测。

### 未实现

1. **深度缓冲、纹理、描述符集、MSAA、多渲染通道、compute、多线程命令缓冲** —— 都没有。
   只有 1 个渲染通道、1 个图形管线、**1 次 `vkCmdDraw`、3 个顶点**。
   所以它**不能**用来验证层处理复杂状态的能力（比如层要正确遍历描述符、
   处理多 subpass、处理 secondary command buffer）—— 那些要另做靶子。
2. **不作为"性能负载"使用**。GPU 帧时间实测约 **0.0046 ms**（单个三角形 + 清屏，物理上合理：
   1280×720×4B ≈ 3.7MB 写入），完全跑不满 GPU。它测的是**帧管线与层注入**，不是 GPU 压力。
3. **不做呈现质量的校验**（没有回读像素比对，只能靠肉眼看旋转的三角形）。
4. **不支持 HDR / 非 sRGB 色彩空间**，只用 `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR`。
5. **不解析命令行里的 `--layer=...`**（用标准环境变量 `VK_INSTANCE_LAYERS` 即可）。
6. **不做多进程 / 多实例协同**。

### 已知的行为特性（不是 bug，但要知道）

1. **JSON 行滞后 2 帧**。`gpu_ms` 要等这一槽位的 fence 被 signal 才能读，
   所以每行在渲染后 2 帧（`kFramesInFlight`）才输出。**行是按帧号顺序、一帧不落的**，
   字段里的 `lag_frames` 明确写了这个滞后量。收尾时会 `vkDeviceWaitIdle` 把
   流水线里剩下的帧全部读完再输出，不会丢帧。
2. **`present_ms` 在现代 WDDM 下通常接近 0**，垂直同步的等待落在 `wait_ms`（见上文实测）。
3. **`--list-layers` 的注册表项数统计**会把 64 位视图和 `WOW6432Node`(32 位)
   分别计数；同名层在两个视图里各注册一次时会计两次（这是故意的，能看到 32/64 位差异）。
4. **`resize` 在无边框/全屏模式下只改交换链图像尺寸**，窗口仍铺满显示器，
   画面会被 present 引擎拉伸。这是有意为之（那种模式下没法改窗口大小）。
5. **`dt_ms` 和分项之和是近似相等**，残差是消息泵与记账开销。

---

## 设计说明（为什么这么写）

### 为什么手写 Vulkan 声明而不是 `#include <vulkan/vulkan.h>`

本机没有 Vulkan SDK。官方 `vulkan.h` 是个 8 万行的巨型头文件，还依赖一串兄弟头。
我们只用到极小一个子集，所以自己声明：

- Vulkan 的 ABI 是纯 C，没有 name mangling / 虚表 / 异常；
- 所有 handle 在 x86_64 上都是 8 字节，按值传递没有区别；
- 结构体永远是 `{ sType, pNext, ... }` 开头，字段顺序在 `vk.xml` 里固定。

**为了让"抄得对不对"可验证**，`vk_min.h` 末尾有 44 个 `static_assert`，把关键结构体的
`sizeof` 钉死在官方头文件在 LLP64 下的已知值上（如
`sizeof(VkPhysicalDeviceLimits) == 504`、`sizeof(VkSwapchainCreateInfoKHR) == 104`）。
对不上就编译不过。

> 这套断言当场就抓到了 11 处错误 —— 手算 padding 很不可靠，
> 靠断言比靠"跑起来好像没崩"可靠得多。

### 为什么全部走 `LoadLibrary` + `GetProcAddress`

1. 链接期根本找不到 `vulkan-1.lib`（没有 SDK 就没有 import library）；
2. 更重要的是**分派必须走 loader 的正规路径**：
   `vkGetInstanceProcAddr(NULL, ...)` → `vkGetInstanceProcAddr(instance, ...)`
   → `vkGetDeviceProcAddr(device, ...)`。真实游戏就是这么取 `vkQueuePresentKHR` 的，
   也只有这样拿到的才是"层链最外层"的函数指针 —— 而这正是我们要验证的东西。

### SPIR-V 为什么是手工汇编的

机器上没有任何 GLSL 编译器（`glslangValidator`）也没有 `spirv-as`。
所以 `kVertSpv` / `kFragSpv` 是逐条指令手写的，每条一行、带注释。

为了不把拼错的字交给驱动（驱动只会回一个没头没脑的
`VK_ERROR_INITIALIZATION_FAILED`），启动时会先跑 `SpirvSelfCheck()`：
逐条校验指令的 `wordCount` 能否严丝合缝地走到数组末尾。

> 这个自检也当场抓到了 bug：`OpCapability` 和 `OpMemoryModel` 我写成
> 了"操作数个数"而不是"总字数"，4 处全被拦下来了。

着色器本身很简单：一个绕原点旋转的彩色三角形，旋转角通过 **push constant**
（`vec4`：cos/sin + 宽高比修正）传给顶点着色器 —— 选 push constant 是因为它是
Vulkan 特有、D3D 没有对应物的东西，将来写层时是必须正确处理的一块。

### 其他值得一提的实现选择

- **`renderFinished` 信号量按交换链图像分配**（不是按 frames-in-flight）。
  这是 Vulkan 里出了名的坑：present 引擎等待信号量是绑定到具体图像的。
- **顶点缓冲每帧重写**（两个槽位各一个，常驻映射 `HOST_COHERENT`），
  模拟真实游戏每帧更新动态数据的行为，这样测出的 CPU 帧时间才包含上传开销。
- **`readGpuMs` 有 `pending[slot].valid` 闸门**：查询池刚创建时里面的查询从未执行过，
  此时带 `VK_QUERY_RESULT_WAIT_BIT` 调用 `vkGetQueryPoolResults` 可能永远等不到结果（挂死）。
- **时间戳 `timestampValidBits < 64` 时掩掉高位**（`1ull << 64` 是 UB，要特判）。
- **stdin 在独立线程上读**，命令经队列交给渲染线程执行 —— 所有 Vulkan 调用都留在主线程。
- **`.bat` 必须是 GBK + CRLF**：cmd.exe 按 OEM 代码页逐字节解析批处理，
  UTF-8 中文和裸 LF 换行都会让 `for (...)` 块和 `^` 续行解析崩掉。
  （`.cpp/.h/.md` 则是 UTF-8 + LF，与项目其余部分一致。）
