# NextPerf 公共层（`src/common`）技术文档

> 面向：**下一次改这块代码的人**（很可能就是几个月后的你自己）。
> 逐字段的数据契约在 [`DATA-STRUCTS.md`](DATA-STRUCTS.md)；钩子内部实现见 [`HOOK.md`](HOOK.md)；
> 日志格式见 [`LOGGING.md`](LOGGING.md)；文件与功能总目录见 [`CATALOG.md`](CATALOG.md)。
>
> 阅读顺序建议：§1（这一层是什么）→ §2（数据契约）→ §3（Low 帧算法，**最容易改错的地方**）
> → §4/§5（面板与行组装）→ §6（JSON）→ §7（位图）→ **§8（坑与教训）** → §9（叮嘱）。

---

## 1. 这一层是干什么的

`src/common/` 是**主程序与注入钩子双方共用的那一份代码**。它不是"工具库"，
而是**两个进程之间的契约的实现**：

```
        NextPerf.exe（主程序）                        NextPerfHook.dll（注入到游戏里）
   ┌──────────────────────────┐                  ┌──────────────────────────────┐
   │ SensorHub ─► NPSensors ──┼─►[Config/Sensors]─┼─► 只读：Cfg() / Sens()        │
   │ NPConfig ────────────────┼─►  共享内存       │                              │
   │                          │                  │  Present 钩子 ─► NPTelemetry ─┼─┐
   │ np::BuildPanelData ◄─────┼── NPHistory(各一份)│  np::BuildPanelData ◄────────┼─┘
   │ np::PanelRenderer        │                  │  np::PanelRenderer            │
   │ npb::PanelBitmap         │                  │  npb::PanelBitmap             │
   └──────────────────────────┘                  └──────────────────────────────┘
        ▲                                                                   ▲
        └──────────── 同一份 np_common.h / np_stats.h / np_panel / np_build / np_bitmap ───────┘
```

**为什么要共用**：游戏内叠加（钩子画）与桌面叠加（主程序画）必须**长得一模一样**。
如果各写一套，"同一个面板在两种模式下不一样"这种问题会永远修不完。
共用 `np_build`（组装行）+ `np_panel`（排版绘制）+ `np_bitmap`（光栅化）之后，
两种模式只差最后一步"把位图交给谁"。

| 文件 | 行数 | 职责 | 谁用 |
| --- | --- | --- | --- |
| `np_common.h` | 497 | 三块共享内存的结构体、计数器位、枚举、宏、`NPCopyStr` / `NPDefaultConfig` / `NPClearSensors` / `NPClearTelemetry` | 双方 |
| `np_stats.h` | 269 | `np::FrameStats`（帧时间环形缓冲 + 各种百分位/Low 帧口径）、`np::Ema`、`np::RunningMax` | 双方（**实际只有钩子在 push**，主程序侧图表走 `NPHistory`） |
| `np_panel.h/.cpp` | 130 / 545 | Direct2D + DirectWrite 面板渲染器：三列网格、行配色、方括号区间、行内折线图、宽度锁定 | 双方 |
| `np_build.h/.cpp` | 13 / 473 | 把 `NPConfig + NPSensors + NPTelemetry + NPHistory` 组装成 `PanelData`（一行行文本 + 颜色 + 曲线指针） | 双方 |
| `np_bitmap.h/.cpp` | 52 / 102 | 把面板光栅化进一张 BGRA DIB（自上而下、**预乘 alpha**）；单例 D2D/DWrite 工厂 + 光栅倍率 | 双方（三处复用） |
| `np_json.h` | 218 | 极简 JSON 读/写，**只服务配置持久化** | 只有主程序 |

`np_bitmap` 是这套设计的关键：**一份绘制代码，三条输出通路**
（桌面叠加 `UpdateLayeredWindow` / D3D11 `UpdateSubresource` / D3D12 上传堆 + `CopyTextureRegion`）。

---

## 2. 数据契约（摘要）

三块共享内存：

| 名字 | 结构体 | sizeof | 写 → 读 |
| --- | --- | --- | --- |
| `Local\NextPerf_Config_v1` | `NPConfig` | 136 | 主程序 → 钩子（+ 主程序自用） |
| `Local\NextPerf_Sensors_v1` | `NPSensors` | 448 | 主程序 → 钩子（+ 主程序自用） |
| `Local\NextPerf_Telemetry_v1_<pid>` | `NPTelemetry` | 55736 | **该 pid 的钩子** → 主程序 |

**逐字段说明、默认值、读写方、`reserved[]` 的来龙去脉、哪些字段不该持久化，
全部在 [`DATA-STRUCTS.md`](DATA-STRUCTS.md)。** 这里只强调三条规矩：

1. **`NPConfig` 加字段只能从 `reserved[]` 里挪**（现在只剩 4 个槽位 = 16 字节），
   保持 `sizeof` 不变 —— 否则「新 exe + 旧 DLL」这一组合会读到错位字节，且**不报错**。
2. **`NPTelemetry` / `NPSensors` 加字段只能加在末尾**（这两个没有预留槽位）。
3. **自校验是给测试和读者用的，不是运行时防线**：C++ 侧没有任何读方比对
   `version`/`size`（只比对 `magic`）。唯一真正比对的是 `tests/verify_inject.py`。

---

## 3. 统计与 Low 帧算法（`np_stats.h`）

这是公共层里**被讨论最多、改过最多次、也最容易改错**的部分。请完整读完本节再动手。

### 3.1 `FrameStats`：帧时间环形缓冲

```cpp
class FrameStats {
    static constexpr uint32_t kCap = 4096;   // ≈ 60 秒 @60FPS（与 NP_FRAME_CAP 同值但各自独立）
    void     push(float ms);                 // buf_[write_] = ms; write_ = (write_+1)%kCap; count_ 封顶
    uint32_t count() const;                  // 已填样本数（满容量后**不再增长**）
    uint32_t recent(float* out, uint32_t n) const;  // 最近 n 帧，按时间顺序（最旧在前），返回实际数量
    float    last() const;
    // 统计口径 ↓
};
```

* `kCap` 与 `NP_FRAME_CAP`（`np_common.h`）是**两个独立的常量**，值恰好都是 4096。
  改一个记得另一个 —— 它们没有任何编译期关联。
* `count() == kCap` 之后**永远等于 kCap**。任何"单位时间内的样本数/采样率"都必须用
  **累计帧数 `frameTotal`** 去算：

  ```cpp
  // np_hook.cpp 诊断日志，注释明确写着这件事：
  // ★ 采样率必须用**累计帧数** frameTotal 算 —— count() 满容量后不再增长
  double rate = (nowFrames - sLastFrames) / dt;   // nowFrames = gTel->frameTotal
  ```

  用 `count()` 算出来的采样率在跑满 68 秒后会**掉到 0**，看起来像"钩子卡死了"。
* `recent()` 是读取窗口数据的**唯一入口**（另有只取一个值的 `last()`），
  实现是"从 `(write_ + kCap - take) % kCap` 开始顺推"，
  所以**FIFO 语义正确**（取的是最新 N 帧，不是最旧的）——
  `tests/low_recover_check.py` 专门验证过这件事。

### 3.2 ⚠ 临时缓冲必须是 `thread_local`

`avgFps` / `lowIntegral` / `lowPct` / `overRatio` / `lowWindowed` / `meanMs` / `medianMs` /
`percentileMs` **每一个**都用同一块临时缓冲：

```cpp
thread_local float tmp[kCap];   // 16KB，每个线程一份
take = recent(tmp, take);
```

**原来是 `static float tmp[kCap]`** —— 那是所有线程共享的一块 16KB。
而 `FrameStats` 的唯一调用方 `PresentCommon` 在**游戏多线程呈现时会被并发进入**：
两个线程同时往这块缓冲里复制并排序，Low 帧 / 百分位就会算出垃圾值，
而且**完全不报错、看起来"只是数不对"**（用户反复反馈过「Low 帧不准」，
`CODE-REVIEW-2026-10-09.md` 里也登记了这一条）。

**规矩：这个类里的任何临时数组都必须是 `thread_local`，不许为了"省内存"改回 `static`。**

### 3.3 百分位：`percentileMs()` —— 线性插值 `(n-1)*p`

```cpp
float percentileMs(float p /* 0..100 */, uint32_t window = kCap) const {
    uint32_t take = min(count_, min(window, kCap));
    if (take < 20) return 0.0f;                 // 样本太少，不给数（不是给 0 帧的近似值）
    take = recent(tmp, take);
    float    idx = p / 100.0f * (take - 1);     // ★ 线性插值：index = (n-1) * p
    uint32_t lo  = (uint32_t)std::floor(idx);
    uint32_t hi  = std::min(lo + 1, take - 1);
    std::nth_element(tmp, tmp + lo, tmp + take);
    float a = tmp[lo];                          // lo 号顺序统计量（先存下来！）
    std::nth_element(tmp, tmp + hi, tmp + take);
    float b = tmp[hi];                          // hi 号顺序统计量
    return a + (b - a) * (idx - lo);            // 在两者之间线性插值
}
```

* `(n-1) * p` 这个写法**与 Intel PresentMon 一致**
  （`Core/source/pmon/StatisticsTracker.cpp:35`），不是随手取的 —— 它保证
  `p=0` 落在最小值、`p=100` 落在最大值、`p=50` 落在中位数位置上（n 为奇数时正好是中位数）。
* `nth_element` 的正确性靠它的**后置条件**（位置 `lo` 上放的就是排序后该在 `lo` 的元素），
  所以两次调用可以基于同一块被打乱过的数组：第二次调用不依赖第一次的内部排列。
  必须先存下 `a` 再调第二次（代码就是这么写的）。
* 平均复杂度 O(n)，**不做全排序** —— 每帧都要调它好几次（`fpsLow1` / `fpsLow01` 各一次，
  诊断日志更多），全排序会明显吃 CPU。
  （注：口径①/②/④ 与 `medianMs` 用的是真排序，它们是"对照口径"，不在每帧路径上。）
* **`take < 20` 一律返回 0**。调用方必须把这个 0 当成"暂时没有数据"（面板显示 `—`），
  不要把它当成"0 FPS"。
* `lowPercentileFps(p, window)` 就是它的倒数包装：`ms > 0.0001 ? 1000/ms : 0`。

### 3.4 四种 Low 帧口径 —— 为什么最终用 PresentMon 的那一种

`FrameStats` 里同时保留了四种口径，**它们是为同一个问题给出的四个不同答案**，
数值差异可以很大 —— 源码注释里记录的实测（RE8 锁 60，同一段素材）：

| 看到过的数 | 它是谁的读数 |
| --- | --- |
| `38~42` | 口径①（最差 x% 取平均）在**1200 帧窗口**下的读数 —— 就是用户投诉「流畅 60 却显示 38~42」 |
| `53.8` | 口径①换成"严格（最差 1% 单帧）"后的读数 |
| `30` | 口径②（integral）的读数（刀刃效应，见下） |
| `57.1` | 口径④（窗口平均 `win500_1`）的读数 |
| `59` | **NVIDIA 驱动面板显示的**（≈ 口径③ 按帧数取百分位） |

理解它们的差别是这份文档最重要的内容：

| 口径 | 函数 | 定义 | 业界出处 | 现状 |
| --- | --- | --- | --- | --- |
| ① 最差 x% 取平均 | `lowPct(pct, window)` | 取最慢 `ceil(take*pct/100)` 帧，求**帧时间**平均 → 换算 FPS | CapFrameX / OCAT 老口径 | **保留供对照**，当前无调用点 |
| ② integral（按**时间**累加） | `lowIntegral(pct, window)` | 帧时间**降序**排，依次累加直到累计时间 ≥ 总时长的 `pct%`，取**刚越过边界的那一帧** → 换算 FPS | MSI Afterburner；CapFrameX ≥ 1.5.3 | 保留供对照，当前无调用点 |
| ③ 百分位（按**帧数**） | `lowPercentileFps(pct, window)` | 帧时间的第 pct 百分位（线性插值）→ 换算 FPS | **NVIDIA 驱动面板 / FrameView**；Intel PresentMon（倒数指标排名反转） | ✅ **最终采用** |
| ④ 窗口平均 | `lowWindowed(pct, windowMs, lookback)` | 先把帧时间按 `windowMs` 分组求**窗口平均**，再取最差 `pct%` 的窗口平均求平均 → 换算 FPS | 游戏内 overlay / 驱动面板显示的本来就是窗口平均 FPS | 保留供对照，当前无调用点 |

**为什么会走这么多弯路（每一句都有实测依据）：**

* **口径①在锁帧场景下天然偏低。** 实测 1200 帧、稳定 60fps 时，最差 1% = 12 帧；
  只要其中约一半是掉垂直同步的 33.3ms、另一半是 16.7ms，平均就是 25ms → **40 FPS**，
  而玩家实际感受是满帧流畅。这正是用户反馈「流畅 60 却显示 38~42」的来源 ——
  **口径本身的问题，不是算错。**
* **口径②（integral）有"刀刃效应"。** 它按**时间**消耗预算，而 33.3ms 的 spike
  消耗预算的速度是正常帧的两倍：稳定 60fps、掉垂直同步的帧占 0.5% 时，
  6 帧 33.3ms 正好用满 1% 预算 → 恰好停在 spike 上 → 算出 **30 FPS**；
  而驱动面板显示 **59**。实测「驱动 59 / 本程序 30」就是这个差异。
  源码注释原话：*"刀刃效应，极不稳定"*。
* **口径③（百分位）与驱动面板一致**，因为百分位是**按帧数**算的：
  掉垂直同步的帧若不足 1%，P99 落在正常帧上 → 60 FPS。
* **口径④最贴近体感**（实测 RE8 锁 60：严格 53.8 vs 驱动 59 vs 窗口平均 57.1），
  但用户最终明确要求：**只用 Intel PresentMon 的权威口径，不再提供可调开关。**

**最终口径 = 口径③ + PresentMon 的窗口与实现细节**（`np_hook.cpp` `UpdateTelemetryCommon`）：

```cpp
// 窗口：对齐 Intel PresentMon 的 --window-size 默认值 1000ms
//   （IntelPresentMon/SampleClient/CliOptions.h:49）
//   （实现 PresentMonMiddleware/DynamicQuery.cpp:221: oldest = newest - windowSize）
uint32_t lowWin = (uint32_t)(t.fpsAvg > 1.0f ? t.fpsAvg : 60.0f);  // 1 秒 → 帧数
if (lowWin < 30)  lowWin = 30;      // 下限：低于 30fps 时也别把窗口压到几帧
if (lowWin > 480) lowWin = 480;     // 上限：高刷屏别用几千帧的窗口
t.fpsLow1  = gStats.lowPercentileFps(99.0f,  lowWin);
t.fpsLow01 = gStats.lowPercentileFps(99.9f, lowWin);
t.p99Ms    = gStats.percentileMs(99.0f);     // 全缓冲（不设窗口）—— 只是留档，无人读
t.p999Ms   = gStats.percentileMs(99.9f);
```

三个必须知道的细节：

1. **窗口是"1 秒"，不是"20 秒"。** 原来用 1200 帧（60fps 下 20 秒），
   比 PresentMon 默认大 20 倍 —— 窗口越长，历史里的偶发 spike 越容易被算进"最差 1%"，
   数值被压低；而且「进游戏那几秒的着色器编译卡顿」会在统计里**赖着不走一分钟**。
   1 秒窗口还有个附带好处：**卡顿约 1 秒后就被遗忘**，不会"钉住"很久。
2. **窗口用 `fpsAvg` 换算成帧数**，因为 `window` 参数的单位是**帧数**，不是毫秒。
   高刷（240fps）时 1 秒 = 240 帧，低帧（30fps）时 = 30 帧。夹在 `[30, 480]`。
3. **"对 FPS 取 P1 ≡ 1000 / P99(帧时间)"** 这个等价关系来自 PresentMon 的
   「倒数指标排名反转」（`PresentMonMiddleware/DynamicStat.cpp:323`）：
   帧时间是"越小越好"的指标、FPS 是"越大越好"的指标，对 FPS 取第 1 百分位
   就等于对帧时间取第 99 百分位再取倒数。**`lowPercentileFps(99.0f)` 做的事就是这个**，
   所以不要另外去写一个 "P1 of fps[]" —— 那是另一个（错误的）统计量。

**改口径的唯一正确姿势**：先在 `tests/` 里造一段可复现的帧时间序列，把四种口径的
输出都打出来对比，再决定。源码注释里写得很清楚：*"有源码出处，不要凭感觉改"*。

### 3.5 均值 vs 中位数：`meanMs()` 与 `medianMs()` 的争论

这两个函数看着像重复品，其实注释里藏着一场"翻车"：

* `meanMs(window)`：**最近 window 个样本的均值**。用于**帧内合并的基准**。
* `medianMs(window)`：中位数。**保留给其它用途；注释明确警告：不要拿它做帧内合并的基准。**

理由（`np_stats.h` 里写得很细，值得逐字读）：

* 帧内合并要处理的正是「8.4 / 24.9 各占一半」这种 **50/50 双峰**分布。
  **中位数在双峰上会随样本数奇偶而振荡**：偶数长度取中间两值平均 = 16.65；
  奇数长度取中间那一个 = 8.4 或 24.9 —— 于是阈值在 9.99 / 5.04 之间跳
  （数值模拟实测到过），表现就是"该合并的时而不合并" = 用户看到的**帧时间忽高忽低**。
* **均值在双峰上恒定**（`(8.4+24.9)/2 = 16.65`），基准稳定。均值对真卡顿敏感，
  靠**长窗口**（`meanMs(240)` ≈ 4 秒）摊薄，并在调用处把阈值夹到合理区间
  （`np_hook.cpp`：`meanMs * 0.6`，夹在 `[2.0, 30.0]`）。

更早的一版还踩过**正反馈**：基准用的是 `fpsAvg`，而 `fpsAvg` 是从**合并后**的序列算出来的
→ *合并 → 间隔被抬高 → fpsAvg 变小 → 阈值变大 → 合并更多*。
所以基准**必须取自从未被合并过的原始序列**（钩子里是 `gRawStats`，另一个 `FrameStats` 实例）。

> 这一节是 `np_stats.h` 与钩子层耦合最紧的地方：**改 `meanMs`/`medianMs` 的默认窗口
> 或语义之前，先读 `np_hook.cpp` 的帧合并段**（[`HOOK.md`](HOOK.md) 里也有专门一节）。

### 3.6 其它小工具

| 工具 | 语义 | 备注 |
| --- | --- | --- |
| `overRatio(ms, window)` | 窗口内**超过阈值**的帧数占比 | 纯诊断（钩子每 5 秒打一条分布日志：`over20`/`over30`） |
| `Ema(alpha=0.25)` | 指数移动平均。**首次 `update` 直接取 `x`**（不产生一段从 0 爬升的假曲线） | 钩子里用的是 `Ema(0.10f)`（更平滑） |
| `RunningMax` | 滑动窗口最大值，用于图表 Y 轴自适应：`120` 次没有刷新就 `cur_ *= 0.85f` | **当前无调用点**（图表的 Y 轴自适应改在 `np_panel.cpp` 里按窗口均值 ± span 现算） |

---

## 4. 面板渲染器（`np_panel.h` / `np_panel.cpp`）

视觉目标是 **Apple Metal HUD**。源码开头的注释把要素列成了 6 条（三列网格 / 数值等宽 /
按指标分色 / 深底 / 极紧行距 / 折线图无刻度），**改视觉前先读那一段**。

### 4.1 字号与缩放

* `fontHeight`（逻辑像素）+ `scale` → 实际字号 `fontHeight * scale`，
  由 `Render()` 传给 `EnsureFormats(fs)`。
* `EnsureFormats` 只在 `fabs(fs - fontSize_) >= 0.01f` 时重建全部 `IDWriteTextFormat`，
  并在重建时**把 `latchedW_ = 0`**（字号变了宽度锁要重来）。
* 字体链（**第一个能创建成功的就用**）：
  * 标签/标题（`fmt_` / `fmtHead_`）：`NSimSun` → `SimSun` → `MS Gothic` → `Microsoft YaHei UI`
  * 数值/括号（`fmtR_` / `fmtSmall_` / `fmtSmallL_`）：`Cascadia Mono` → `Consolas` →
    `Lucida Console` → `Courier New`
* **中文标签也必须是等宽的**（`NSimSun` 是 Windows 自带的等宽中文字体）——
  否则中英混排时左侧栅格是歪的（用户反馈过"左侧字体也没改"）。
* 分组标题用**与正文同号**的字号（曾给 0.92× 被用户反馈"明显偏小"），只靠**加粗**区分。
* `lineHeight_` 由 DWrite 实测得到：用样本文本 `L"Ag0.帧°%"` 建一个 `CreateTextLayout`
  再 `GetLineMetrics`；失败兜底 `fs * 1.30f`。
  **行高不能小于字体自身行距**，否则 `D2D1_DRAW_TEXT_OPTIONS_CLIP` 会把字形上下裁掉，
  各行视觉高低也会不齐。
* `Init()` 失败（没有 D2D/DWrite）时 `Measure()` 返回 `240 x 110` 兜底，不会崩。

### 4.2 布局常量（`Layout()`，**测量与绘制共用同一套数字**）

| 量 | 公式 | 出处/理由 |
| --- | --- | --- |
| `pad` | `4.0f * scale + 3.0f` | 那 `3.0f` 是用户指定的"四周留一圈黑"（原来是 `行高/6 ≈ 2.85`） |
| `lh` | `max(fontHeight * 1.14f * scale, lineHeight_)` | 金属 HUD 行距约 1.18 倍字号；普通界面 1.34 会显得松散 |
| `headH` | `= lh` | 标题与正文同号 |
| `chartH` | `max(11.0f, graphHeight * 0.26f) * scale` | 曲线区比文字区矮得多（原来 0.42 显得又高又空） |
| `rowGap` | `1.0f * scale` | |
| `chartGap` | `2.0f * scale` | |
| `colGap` | `2.0f * scale` | |

> 布局常量集中在一个匿名命名空间里，**就是为了避免"测量和绘制各算各的"** ——
> 源码注释写着：*"以前就因此错位过"*。

### 4.3 三列网格与列宽（`ComputeGrid()`）

```
[标签 + 异色提示] ······ [ (范围列) ][ 数值列 ]
 labelX              rngX  rngRight  valueRightEdge(=面板右缘 - pad)
```

列的取宽规则：

| 列 | 宽度 | 说明 |
| --- | --- | --- |
| 标签列 | `cols(maxLabel)`，`cols(px) = ceil(px / chW) * chW` | **量化到整字符格**；`maxLabel` 按 `r.label + r.hint` 量（提示也要占位，否则会压到右边的列） |
| 数值列 | **固定 `14 * chW`** | 数据层 `np_build.cpp::FitCols(..., 14)` 已把过长的值截断，所以既不会裁掉显示，也不会因某个值变长而撑宽面板 |
| 范围列 | `wantRange ? 12 * chW : 0` | `wantRange` 现在是**硬编码 `false`**（区间显示被用户要求关闭） |

**`fmtSmall` 的 `chW`（一个等宽字符宽）怎么量**：

```cpp
float chW = MeasureW(f, fmtSmall, L"0");       // 用单字符量
if (chW <= 0.0f) chW = fmtSmall->GetFontSize() * 0.6f;   // 兜底
```

⚠ **不要用 `MeasureW(L"  ")` 去量空格**：DWrite 会把**行尾空格裁掉**，量出来接近 0，
结果是括号里的最小值与最大值直接黏在一起（实测显示成 `0.4426.23`）。
等宽字体里每个字符（含空格）宽度相同，用"一个字符宽"推算即可。

### 4.4 面板宽度锁定：`latchedW_` **只增不减**

```cpp
float want = std::max(170.0f * c.scale, contentW + M.pad * 2.0f);
if (want > latchedW_) latchedW_ = want;   // 只增不减
*w = latchedW_;
```

* 行是**随数据陆续出现**的（有 GPU 才有显卡型号行、有 NVAPI 才有带宽行）。
  允许回落的话，面板会随行的有无**反复伸缩**。
* 锁定值在**字号变化时重置**（`EnsureFormats` 里 `latchedW_ = 0`）。
* **总宽不再额外量化**（原来对总宽做 4 格量化，一档 35px，只会把 290 撑成 318，
  白白宽一圈）。各列已经落在字符栅格上了。

历史上宽度抖动有**三个独立来源**，全部修掉了 —— 改这块时**别把它们放回来**：

1. **范围列按"有行带范围才预留"** → 启动时无数据（无范围）面板 266 宽，
   一拿到数据范围列出现 → 立刻 421 宽，数据有无之间来回切。
   结论：**宁可留白，也不能抖**；
2. **范围列按"实际最长括号"取宽** → 行随数据陆续出现会让它一路长
   （诊断日志实测 `87.7 → 96.5 → 105.3`）。结论：**宽度必须纯由配置决定，绝不看当前数据**；
3. **数值列按实际最长值取宽** → "9% ↔ 100%" 这类 1~3 格的变化会改变面板宽度。
   结论：**固定格数**。

`PanelRenderer::diag`（`labelW/valueW/rngW/headW/charW/contentW/rowH/sepH`）就是为定位
这类问题留的：钩子把它打进日志，配合 `tests/panel_width_check.py`
（量连拍图里"面板左边缘是否稳定"，右对齐面板宽度一变左边缘就会动）。

### 4.5 量宽的两个硬规矩

```cpp
static float MeasureW(IDWriteFactory* f, IDWriteTextFormat* fmt, const std::wstring& s);
```

1. **量宽必须用"与绘制相同的字体"，而且要用左对齐的格式。**
   * 用**右对齐**（`DWRITE_TEXT_ALIGNMENT_TRAILING`）的 layout 去量：给一个很宽的盒子时
     `GetMetrics().width` 可能返回**整个盒子宽度**而不是文本自身宽度 →
     `valueW` 变成几千像素，面板被撑得极宽、字像被横向拉开。
     （重写那一版把量宽格式从 `fmt_` 换成 `fmtR_`，属于自己引入的回归。）
   * 数值已改为等宽字体绘制，若仍用标签字体（`NSimSun`）去量，量出来偏小，
     长值会被裁掉 —— 实测 `1125/9001 MHz` 被裁成 `1125/9001 MH`。
2. **`MeasureW` 内部失败时兜底 `size * fontSize * 0.6f`**，保证任何情况下都有个数可用。

### 4.6 值颜色规则

* 行级颜色来自 `PanelRow::level`（`PanelLevel`），由 `np_build.cpp` 决定，
  渲染器按 `LevelRGB()` 映射（表在 [`DATA-STRUCTS.md` §8.3](DATA-STRUCTS.md#83-行配色np_panelcpp-levelrgb-与-npconfig-颜色的关系)）。
  * **CPU 侧指标一律蓝 `PL_CPU`；GPU 侧一律绿 `PL_GPU`；FPS/帧时间/内存类用白。**
* 标签色固定 `0.95,0.95,0.95`；`hint`（如「低延迟」）固定**亮黄** `1.00,0.85,0.15`，
  单独绘制一次（异色），所以标签列宽必须按 `label + hint` 量。
* 括号里的数字与**数值同色**，不做单独预警（用户要求："数值什么色就是什么色"）。
* `cfg.textOpacity` 乘进**每一个**画刷的 alpha；`cfg.bgOpacity` 乘进背景色的 alpha
  （`bg.a = clamp(bg.a * bgOpacity)`）。`cfg.textColor/accentColor/warnColor`
  **渲染器根本不读**（见 [`DATA-STRUCTS.md` §8.3](DATA-STRUCTS.md)）。
* 背景：`FillRoundedRectangle`，圆角 `11.0f * scale`。
  ⚠ 源码开头的风格注释写的是"方角纯深底"，**代码其实是轻微圆角** —— 以代码为准。

### 4.7 方括号 `[ min  max ]` 的对齐（现在处于"关闭但代码保留"状态）

代码路径完整保留（`HasRange` / `rngX` / `drawAt`），只是 `wantRange = false`
让范围列宽为 0、`np_build` 也不再填 `vmin/vmax`。**要恢复**：把
`np_build.cpp::RowAcc::rowRange()` 里注释掉的那几行放开，并把 `np_panel.cpp` 的
`wantRange` 置回 `true`。

绘制顺序与定位（从右往左）：

```
closeL = rEdge - closeW                // ]  固定贴住范围列右缘
maxL   = closeL - wmax
minR   = maxL - gapW
minL   = minR - wmin
openL  = minL - openW                  // [  紧贴最小值，前面**不留** gapW
drawAt(L"[", fmtSmall_,  bbrk, openL, minL);   // ← 用**右对齐**格式
drawAt(r.vmin, fmtSmall_, bmin, minL, minR);
drawAt(r.vmax, fmtSmall_, bmax, maxL, closeL);
drawAt(L"]", fmtSmallL_, bbrk, closeL, rEdge); // ← 用**左对齐**格式
```

两条踩出来的细节：

* **`[` 必须用右对齐格式绘制**，让字形紧贴最小值。用左对齐时，等宽字体里 `[` 的
  右侧留白（side bearing）会变成一个肉眼可见的空格 —— 实机看起来就是 `[ 0.40 22.57]`。
* **`[` 前面不要再留 `gapW`**（原来多留了一个，就是上面那个现象的直接原因）。

### 4.8 行内折线图

* 挂在**该行下方独占一行**：高度 `chartH`，宽度占满内容区（`labelX → rightX`）。
* Y 轴自适应（全部在 `Render()` 里现算）：

  ```
  avg  = 窗口均值；mn/mx = 窗口极值
  span = max(|avg| * 0.35, (mx - mn) * 0.60)
  span < 0.5 → span = max(|avg| * 0.20, 0.5)
  yMin = max(0, avg - span); yMax = avg + span
  unit == 1（%）→ 再夹到 [0,100]，并保证 yMax - yMin ≥ 1
  ```
* 画：6 条等分竖网格（白 ×0.10）+ 上下两条边线 + 指标色 1px 折线；**没有 Y 轴刻度**
  （金属 HUD 把区间数值放在方括号里）。
* **采样点铺满整个宽度**（`frac = i / (n-1)`），**不按固定时间轴**。
  原来按固定时间轴定位 —— 数据不足时线只挤在右边缘一小段，看起来像坏掉了
  （截图里 FPS 曲线几乎空白就是这个原因）。
* ⚠ 一个小坑：`n = min(chartCount ? chartCount : chartCap, chartCap)` ——
  当 `chartCount == 0` 时会退化成"整块缓冲"（512 个 0 值），于是画出一条贴底的平线。
  目前调用点都保证 `count > 0`，但**新调用点要自己注意**。
* `PanelRow::unit`：`0=ms` `1=%` `2=FPS`，只影响上面的 Y 轴夹取（`1` 才夹 [0,100]）。
  `sampleMs` 目前**不参与绘制**（见 [`DATA-STRUCTS.md` §5](DATA-STRUCTS.md)）。

### 4.9 三种输出通路（谁调用渲染器）

| 通路 | 调用者 | 说明 |
| --- | --- | --- |
| 桌面分层叠加 | `src/app/overlay.cpp` | `npb::SetRasterScale(dpi/96)` → `BuildPanelData` → `Measure` → `npb::PanelBitmap::Render` → `UpdateLayeredWindow`，定位到**前台窗口所在显示器**的右上角 |
| D3D11 游戏内 | 钩子（`np_draw.cpp` 的 `Overlay11`） | 面板先画进位图，再 `UpdateSubresource` 上传 |
| D3D12 游戏内 | 钩子（`np_draw.cpp` 的 `Overlay12`） | 上传堆 + `CopyTextureRegion` |

钩子侧的硬教训：**`npg::GfxInit()`、`npb::GfxInit()`、`gPanel.Init()` 必须一起调**
（`EnsureRt()` 里三个都在）。少调 `npb::GfxInit()` 的话 `PanelBitmap::Ensure()` 里那句
`if (!gD2D) return false;` 会一直失败 —— 表现是**"注入成功、数据也有，但游戏里看不到面板"**。
而且它们**必须在渲染线程上初始化**（单线程 D2D 工厂跨线程用会崩，多线程渲染的游戏尤其明显）。

---

## 5. 行组装（`np_build.h` / `np_build.cpp`）

`BuildPanelData(PanelData& out, const NPConfig& c, const NPSensors& s, const NPTelemetry& t, const NPHistory* h)`
把四份数据变成"一行行文本 + 颜色 + 曲线指针"。**两种叠加模式共用它，所以这里的任何改动
都会同时影响桌面和游戏内。**

### 5.1 三个总开关

```cpp
const bool hooked     = t.attached != 0;              // 钩子在线
const bool active     = hooked || (c.simulate != 0);  // 帧数据可用（模拟模式下也算可用）
const bool showCharts = h != nullptr;                 // 有历史数据才挂曲线
```

* **`h == nullptr` 绝不能提前 `return`。** 源码注释专门写了这件事：
  原来写的是"无历史数据时帧率行也没有图可画 → 干脆返回"，结果
  **CPU / GPU / 内存这些根本不需要历史数据的行也一起消失了** ——
  把"画不了曲线"错误地当成了"什么都别显示"。现在每处 `attach()` 都由 `showCharts` 挡住。
  （当前所有调用点都传了非空指针，所以它是一颗**埋着的雷**，已经拆掉。）
* `active == false` 时，帧率类的值显示 `—`（而不是 0），因为"没数据"和"0 FPS"是两件事。

### 5.2 `RowAcc`：分组与输出

```cpp
struct RowAcc {
    std::vector<PanelRow> pending;      // 当前组缓冲
    void row(label, value, level);      // 攒一行
    void rowRange(...);                 // 同 row（区间显示已关闭）
    void flush(group, hideTitle, gapBefore);   // 有内容才输出标题 + 全部行
};
```

四个分组（顺序即面板顺序）：

| 分组 | `hideTitle` | `gapBefore` | 说明 |
| --- | --- | --- | --- |
| `帧率与延迟` | **`true`** | `0.0f` | 首组：标题**不显示**（用户要求），也不留顶端间距 |
| `CPU` | `false` | `9.0f` | 显示标题 |
| `GPU` | `false` | `3.0f` | 显示标题 |
| `系统` | **`true`** | `9.0f` | 标题不显示，只留间距（用户要求 `帧率与延迟` / `系统` 这两个小标题不要出现） |

* `flush()` 里 `if (pending.empty()) return;` —— **空组不输出标题**（避免空标题行）。
* ⚠ 这三个 `gapBefore` 值（`0 / 9 / 3 / 9`）在 `np_build.cpp` 里是**字面量**，
  **没有乘 `cfg.scale`**；而 `np_panel.cpp` 的 `Layout()` 里其它间距都乘了 `scale`。
  高缩放（scale = 2.0）时分组留白会显得偏紧 —— 已知的不一致，改的时候一起处理。
* `hideTitle == true` 的分组在 `Measure/Render` 里会被当成分隔处理：
  **首行不计 `gapBefore`**（否则 HUD 顶端会多出一条空行）。

### 5.3 `rowRange()` —— 区间显示现在是关的

```cpp
void rowRange(label, value, level, active, series, h, dec, warnMinBelow, warnMaxAbove) {
    // ★ 用户要求：**不再显示 [min max] 区间**，只显示数值本身。
    //   保留函数签名（调用点不用改），想恢复只要把下面这段注释放开、
    //   并把 np_panel.cpp 里的 wantRange 置回 true 即可。
    (void)active; (void)series; (void)h; (void)dec; (void)warnMinBelow; (void)warnMaxAbove;
    row(label, std::move(value), level);
    // ... 注释掉的实现 ...
}
```

* 因此那些 `warnMaxAbove = 33.34f` / `36.0f` 的阈值**当前不生效**，
  行颜色完全由 `level` 决定（静态）。`HistRange()`（从 `NPHistory` 里统计极值，
  跳过 `<= 0` 的未填槽位）也成了**无调用点**的保留函数。
* 恢复时注意 `HistRange` 的窗口是**整个 `NPHistory`**（`h->count`，最多 256 点），
  设计意图是"括号里就是曲线上看得见的那段区间的小/最大值"——
  这样**括号和曲线永远自洽**（Apple 的做法）。

### 5.4 `FitCols()` —— 数据层主动截断

* 面板数值列固定 14 格，**超过就会被 CLIP 裁掉看不见**，所以在数据层先截断并加省略号。
* 计宽规则：**全角字符算 2 格**（用一段 Unicode 区间表判断：`0x1100..0x115F`、
  `0x2E80..0xA4CF`、`0xAC00..0xD7A3`、`0xF900..0xFAFF`、`0xFE30..0xFE6F`、
  `0xFF00..0xFF60`、`0xFFE0..0xFFE6` 等），末尾补 `…`（占 1 格）并保证不超宽。
* 使用者：显卡型号（14 格）、数据源文案（14 格）。
* 加任何"来自外部（驱动/系统）的字符串行"**都必须过一遍 `FitCols`**，
  否则一个长型号就能把数值列挤爆。

### 5.5 数值格式化与哨兵

| 函数 | 规则 |
| --- | --- |
| `WF(v, dec)` | `swprintf`，`dec <= 0` 用 `%.0f`，否则 `%.*f` |
| `Num(v, dec, unit)` | `v < -1000` → `—` |
| `PctV` | `Num(v, 0, "%")` |
| `Msv` | `Num(v, 2, " ms")` |
| `Cv` | `v < -200` → `—`，否则 `%.0f ℃`（配合 `-273` 哨兵） |
| `Wv` / `MHzv` / `GBv` | `v < 0` → `—` |

**哨兵值 → `—`** 的映射必须与 [`DATA-STRUCTS.md` §3.1](DATA-STRUCTS.md) 的约定一致。
新增指标时先问：**"读不到"用什么值表示？面板显示什么？** 不要让它显示 `0`。

### 5.6 逐个指标的取值口径（改之前先看这里）

| 面板行 | 值来自 | 关键规则 |
| --- | --- | --- |
| FPS | `t.fps` | 白色 `PL_FPS`；调用点还传了 `warnMinBelow = 28.0f`（"低于 28 标红"的意图），但**区间显示关闭后这个参数被忽略**，`HystWarn()`（颜色迟滞，`[[maybe_unused]]`）也没有调用点 —— 所以 FPS 行当前**不会变色** |
| 帧时间 | `t.frameMsAvg`（>0）否则 `t.frameMs` | **数值颜色固定**，不随帧时间跳红。原来用瞬时 `frameMs` 判阈值 → 逐帧在 9~24ms 抖动 → 一跨阈值就闪红；改成平滑值 + 迟滞后，30fps 场景又会"进去就出不来"（触发 33.3ms、恢复要 22ms → 整行长期红着）。金属 HUD 的做法是**数值保持指标色**，照此办理 |
| 1% Low / 0.1% Low | `t.fpsLow1` / `t.fpsLow01` | **不画曲线**（用户明确要求：显示出数值就好） |
| CPU 帧时间 | **`t.cpuBusyAvg + t.cpuWaitAvg`** | ⚠ 必须用**平滑值之和**：面板上「CPU Busy」「CPU Wait」两行显示的就是这两个 EMA，用瞬时值的话**三行永远加不上**（用户实测抓到过 0.37 vs 0.24+5.95）。两者都为 0 时才回退 `t.cpuFrameMs` |
| CPU Busy / Wait | `t.cpuBusyAvg` / `t.cpuWaitAvg` | 默认不显示（`NP_C_CPU_BUSY/WAIT` 不在 `NP_ALL_COUNTERS`），用户勾选才出现 |
| GPU 帧时间 | `s.gpuBusyMs`（PDH）→ `t.gpuFrameMs`（须 `> 0.01f`）→ `—` | 三级兜底。**兜底值必须判正数**：原来是 0 也照显示，于是"注入没成功"时面板上出现一个**假的 0.00 ms** |
| 热点 / 显存结温 | `s.gpuHotspot` / `s.gpuMemTemp` | `> -200` 才显示；`> 95℃` 转 `PL_WARN` |
| 核心/显存 | `s.gpuClock` / `s.memClock` | 写成 `2002/14001 MHz` 而非 `2002 MHz / 14001 MHz` —— 省 4 个字符格，面板才收得窄 |
| 风扇转速 | `gpuFanPct` → `gpuFanRpm` | 前者 `> -1` 用百分比，否则用 RPM |
| RT Core | 硬件接口 → 实测 → 「启用 N 次」→「无 DXR」/「需注入」 | **五档**，级别依次 `PL_ACCENT` / `PL_ACCENT` / `PL_ACCENT` / `PL_DIM` / `PL_DIM` |
| Tensor | 硬件接口 → 实测 → 「AI 已启用 · 估算中」→「无 AI」/「需注入」 | 三档 |
| 分辨率 | `t.renderW/H` + `t.windowW/H` | 只在 `hooked && t.renderW` 时显示；与输出不同则加 `→ 输出 (缩放%)`。**归到「系统」分组**（用户要求）：在 GPU 组里只把值算好，到系统组才入行 |
| 低延迟 | `hookFlags` 或 `cpuWaitAvg` | 判定优先级：`NP_HOOK_REFLEX_KNOWN` 置位 → **直接采信 `NP_HOOK_REFLEX`（驱动直证）**；否则回退「`cpuWaitAvg < 1.6ms`」（等待被移出 Present 的旁证）。阈值 1.6ms 是按用户实测指定的，想调直接改这个数 |
| 显存 / 内存 | `vramUsedGB/Total` | 有总量时写 `x.x/y.y GB`；`vramPct > 92` / `ramPct > 92` 转 `PL_WARN` |
| 功耗 | `s.gpuPower` | **只显示当前功耗，不带上限**（用户明确要求"不需要读取最大功耗"）—— 所以 `gpuPowerLimit` 是写了没人读的字段 |

---

## 6. JSON（`np_json.h`）

**唯一的用途**：把 `NPConfig` + 游戏名单持久化到 `%APPDATA%\NextPerf\config.json`
（`settings.cpp`）。不追求完整规范覆盖，但对 `\` 转义、UTF-8、数字、对象、数组都做了正确处理。

* 类型：`Null / Bool / Num / Str / Arr / Obj`（一个 `Value` 全包，`obj` 是 `std::map`、
  `arr` 是 `std::vector`）。便捷构造：`mkNum / mkStr / mkBool / mkArr / mkObj`；
  取值：`numOr / boolOr / strOr`（**类型不符时返回默认值**，这正是 `settings.cpp` 想要的容错）。
* `dump()`：
  * 数字：整数值且 `|v| < 1e15` 写 `%.0f`（**不带小数点**），否则 `%.6g`；
  * 缩进 2 空格，对象/数组每个成员一行；
  * **对象的键是按 `std::map` 的字典序输出**（不是插入序）—— 文件 diff 稳定，但
    和人写 JSON 的顺序不一致，别以为是 bug；
  * 字符串转义 `"` `\` `\n` `\r` `\t`，其它 `< 0x20` 写成 `\u00xx`；非 ASCII **原样透传**（UTF-8）。
* `parse()`：递归下降 + 一个 `bool ok` 标志。**容错性比看上去更"宽松"，必须知道**：
  * 只有三种情况会把 `ok` 置 `false`：输入为空、`{` 之后不是 `"` 键、键之后不是 `:`；
  * **缺 `,`、缺 `]`、缺 `}` 一律不报错** —— 循环会 `break` 并**静默丢掉后面的内容**
    （`ok` 仍是 `true`）。所以"解析成功"不等于"文件完整"：
    配置里**靠后的键可能整段消失**。想避开它，就每次都整份重写（`SettingsSave()` 就是这么做的）。
  * `\uXXXX` 支持 **BMP 单码位**（`appendUtf8` 最多写 3 字节）——
    **代理对（如 emoji `\uD83D\uDE00`）会写成两个 3 字节序列 = 非法 UTF-8**。
    当前配置里不会出现这类字符串，但**不要拿它去解析任意 JSON**。
* 调用方必须自己校验：`settings.cpp` 里 `if (!ok || v.type != Obj) return false;`
  （解析失败就保留 `NPDefaultConfig()` 的默认值）。
* **`cfgVersion` 迁移机制**（在 `settings.cpp`，但会影响公共契约）：

  | 版本 | 迁移动作 |
  | --- | --- |
  | `< 2` | 置上 `NP_C_CHART_USAGE \| NP_C_CHART_FPS \| NP_C_CHART_LATENCY` |
  | `< 3` | 置上 `NP_C_GRAPH \| NP_C_CHART_FPS \| NP_C_CHART_LATENCY` |
  | `< 4` | 置上 `NP_C_CPU_CLOCK \| NP_C_CPU_POWER`（新开关对老用户默认打开，否则"看起来功能没做出来"） |
  | `< 5` | **清掉** `NP_C_CPU_BUSY \| NP_C_CPU_WAIT`（高级项默认关；**只清一次**，不覆盖用户后来的勾选） |

  当前写出的 `cfgVersion = 5`。**加新计数器位并希望老用户默认勾上，就必须加一条迁移并升版本号。**

---

## 7. 面板位图（`np_bitmap.h` / `np_bitmap.cpp`）

把面板画进一张 **BGRA DIB**（`biHeight = -ph` → **自上而下**，方便直接上传；
`DXGI_FORMAT_B8G8R8A8_UNORM` + `D2D1_ALPHA_MODE_PREMULTIPLIED` → **预乘 alpha**），
然后三条通路各取所需（`pixels()/width()/height()/stride()` 或 `dc()`）。

* **光栅倍率 `RasterScale`**：`1.0 = 96DPI`。两条通路取值方式**不同**：
  * 桌面叠加：`GetDeviceCaps(LOGPIXELSX) / 96.0f`（**前台窗口所在显示器**的 DPI，`< 96` 时夹到 96）；
  * 游戏内钩子：**与 DPI 无关**，取后台缓冲高度 `clamp(bufferHeight / 1080.0f, 1.0f, 2.5f)` ——
    即"1080p 及以上按比例放大，最高 2.5×"，并且只有当新值与上次相差 **> 0.05** 时才
    `SetRasterScale` **并重画面板**（避免每帧重排）。
  `SetRasterScale` 只在 `(0.05, 8.0)` 之间生效（防止除零/爆内存）。
  D2D 是矢量绘制，所以放大后依然锐利 —— 高分屏不会显得过小。
* `Ensure(w, h)` 收的是**逻辑尺寸（DIP）**，内部乘倍率换成物理像素；
  `w/h` 与当前一致且 `rt_` 已存在时直接返回（**幂等**，可每帧调）。
* `Render()` 里 `BindDC` 用的 `RECT` 是**物理像素**（`{0,0,w_,h_}`），
  然后 `SetTransform(Scale(gRasterScale))` —— 于是调用方（`PanelRenderer::Render`）
  可以**一直按 DIP 画**。这两个坐标系别搞混。
* `Render()` 每次都 `Clear(0,0,0,0)`（没有残留）；`npb::GfxInit()` / `GfxShutdown()`
  是**进程级单例**（模块静态 `gD2D` / `gDW`），钩子里必须和 `np::PanelRenderer::Init()`
  一起调（见 §4.9）。`PanelBitmap` 自身只有 `Ensure` / `Release`（幂等重建）。
* 少调 `npb::GfxInit()` 的症状：`Ensure()` 里 `if (!gD2D) return false;` 直接失败 →
  "注入成功、数据齐全、游戏里没有面板"。

---

## 8. 重要的坑与教训（逐条，都有出处）

**这一节是从源码注释里提取的"血泪史"。改动公共层之前请至少扫一遍。**

### 8.1 数据契约类

1. **`NPTelemetry` 中间插字段 + ctypes 镜像没同步 = 读到错位字节。**
   两边**字段总数相同、总大小也相同**（只是排列不同），所以
   `version == sizeof(...)` 那道自检**抓不到** —— 表现为"钩子明明工作正常却报未挂上"，
   查了很久。（`np_common.h` 里 `version` 的注释 + `struct_check.py` 开头的说明）
2. **`NPClearSensors()` 原来写 `s->version = 1`** → 「主程序写 / 钩子读」这一路
   **完全没有布局自检**：往中间插字段，两边照样能跑，但读出来的是错位字节。
   改成 `sizeof(NPSensors)`。（`CODE-REVIEW-2026-10-09.md`）
3. ✅ **（曾经的坑，已修复）`NPDefaultConfig()` 的 `for (i < 8)` 越界写 16 字节**：
   `reserved` 已被逐步缩到 `[4]`，循环却没跟着改 —— 而 `AppState` 里紧邻 `cfg` 的就是
   `sensors`，越界清零会把 `sensors.magic/version/tickMs` 打掉（发布的 Sensors 块
   `magic = 0` → **被钩子直接忽略**，不崩但传感器全丢）。现写法是
   `sizeof(c->reserved) / sizeof(c->reserved[0])`。
   **教训：清数组一律用 `sizeof` 推导，不要写死数字。** 详情见
   [`DATA-STRUCTS.md` §2.5](DATA-STRUCTS.md)。
4. **`NPConfig` 加字段必须复用 `reserved[]`**（保持 `sizeof` 不变），
   否则"新 exe + 旧 DLL"会静默读到垃圾。`reserved` 现在**只剩 4 个槽位**。
5. **哪些字段绝不能持久化**：`magic`/`version`/`size`/`quit`/`pauseHook`/`detachPid`。
   尤其 `detachPid` —— 持久化会让"下次注入这个游戏时钩子立刻自卸载"。
6. **加了配置项就要同时加"存"和"取"**：`learnedAutoHook` 曾因漏了 `settings.cpp`
   的读写而"重启就丢"；`bgColor` 曾"只有写没有读"。（`CATALOG.md` 血泪教训 3）
7. **`NP_C_CHART_FPS` / `NP_C_CHART_USAGE` 是死位**：在 `NP_ALL_COUNTERS` 里、
   UI 有复选框、settings 迁移还会置上，但 `np_build.cpp` **没有任何地方读它们**。
   `NP_ALL_COUNTERS` 还**故意漏掉** 11 个位（热点/带宽/视频/PCIe/Draw/AI 模块/风扇/
   数据源/显卡型号/CPU Busy/CPU Wait）→ 这些实现完整但默认关。
8. **`NP_MOD_LEN` / `NP_MAX_MODULES` 无人使用**；`NP_MUTEX_SENSORS` 无人使用；
   `opacity` / `fpsCap` / `pollMs` / `textColor` / `accentColor` / `warnColor` 写了没人读
   （`pollMs` 甚至还有 UI 控件，但主程序的实际轮询节拍是主循环的 120ms，
   `AppState::lastPoll` 声明了也没人用）。
   看到它们别以为有功能。

### 8.2 统计与算法类

9. **`thread_local` 临时缓冲不能写成 `static`。** 原来是 `static float tmp[kCap]`（16KB
   全线程共享），而 `PresentCommon` 在**游戏多线程呈现时会被并发进入** ——
   两个线程同时往同一块缓冲里复制并排序，Low 帧 / 百分位算出垃圾值，
   而且**完全没有报错、看起来"只是数不对"**。（`np_stats.h` 三个函数上都有这个警告）
10. **饱和计数要用 `frameTotal`，不能用 `count()`。** `FrameStats::count()` 满容量
    （4096）后不再增长，用它算采样率会在跑满 68 秒后掉到 0。
11. **帧内合并的基准不能用 `fpsAvg`（正反馈），也不该用中位数（双峰振荡）。**
    `合并 → 间隔抬高 → fpsAvg 变小 → 阈值变大 → 合并更多`；
    中位数在 50/50 双峰上随奇偶振荡 → 阈值跳 → "该合并的时而不合并"。
    正确答案：**从未被合并过的原始序列的长窗口均值** `meanMs(240)`。
12. **不要凭感觉改 Low 帧口径。** 四种口径实测差异巨大（对照表见 §3.4：
    口径① `38~42`/`53.8`、口径② `30`、口径④ `57.1`，而**驱动面板是 `59`**），
    最终采用 Intel PresentMon 的权威口径（1 秒滑动窗口 + 对 FPS 取 P1 ≡ `1000/P99(帧时间)`），
    **有源码出处**。用户在这一点上明确要求"不再提供可调开关"。
13. **窗口越长，Low 帧越低。** 1200 帧（20 秒）会让"进游戏那几秒的着色器编译卡顿"
    在统计里赖着不走一分钟 → 用户看到"稳定 60fps 却显示 1% Low = 10"。
14. **超过 1 秒的帧间隔不是"一帧"**（是切出去/加载/挂起留下的空档），
    必须丢掉，否则 1% Low 会被直接拖到个位数。

### 8.3 渲染与排版类

15. **量宽必须用"与绘制相同的字体 + 左对齐格式"。** 用右对齐 layout 量宽可能返回
    **整个盒子宽度**（`valueW` 变几千像素，面板被撑爆）；用标签字体量等宽数值会偏小
    （`1125/9001 MHz` 被裁成 `1125/9001 MH`）。
16. **不能用 `MeasureW(L"  ")` 量空格** —— DWrite 裁掉行尾空格，量出来接近 0，
    括号里的两个数字会黏在一起（`0.4426.23`）。
17. **列宽绝不能看当前数据**：范围列"有行才预留"导致 266 ↔ 421 抖动；
    "按实际最长括号"导致面板开局几秒持续变宽（`87.7 → 96.5 → 105.3`）。
    只有"由配置决定的固定格数 + `latchedW_` 只增不减"才稳。
18. **行高不能小于字体自身行距**，否则 `DRAW_TEXT_OPTIONS_CLIP` 裁掉字形上下，
    各行视觉高低不齐。`lineHeight_` 要**实测**（`GetLineMetrics`），不能拍脑袋 ×1.18。
19. **`[` 要用右对齐格式画**（否则等宽字体的 side bearing 变成肉眼可见的空格），
    而且**前面不要再留 gap**（`[ 0.40 22.57]` 就是这么来的）。
20. **折线图要把已有样本铺满整个宽度**：按固定时间轴定位时，数据不足会让线挤在右边缘、
    看起来像坏了（截图里 FPS 曲线几乎空白）。
21. **测量与绘制必须共用同一套网格/常量**：各算各的 → 括号和数值列对不齐（截图一眼可见）。
22. **`cfg.textColor/accentColor/warnColor` 不生效**（行颜色是 `LevelRGB()` 硬编码的）；
    背景注释说"方角"其实有 `11*scale` 圆角；数值列注释说"量化到 4 格"其实**固定 14 格**。
    **遇到注释与代码不一致，以代码为准，并顺手把注释改对。**
23. **`gapBefore`（0/9/3/9）没乘 `scale`**，与 `Layout()` 里其它间距不一致。

### 8.4 组装与显示类

24. **`h == nullptr` 不能提前 return** —— 会把不需要历史数据的行一起干掉。
25. **"读不到"必须显示 `—`，不能显示 0。** GPU 帧时间的兜底值不判 `> 0.01f` 时，
    "注入没成功"会在面板上变成**假的 `0.00 ms`**。
26. **CPU 帧时间必须显示 `cpuBusyAvg + cpuWaitAvg`**，否则它和下面两行**永远加不上**
    （用户实测抓到 0.37 vs 0.24+5.95）。
27. **新加的字符串行必须过 `FitCols`**（14 格，全角算 2 格），否则会撑破固定列宽被裁掉。
28. **`tickMs` 不能跨进程做减法**：钩子写的 `GetTickCount64()` 与主程序自己的钟不可比，
    会算出负数或天文数字。判"钩子还活着吗"**一律用 `frameTotal` 是否增长**。
    （`CATALOG.md` 血泪教训 1）

### 8.5 流程与工具类

29. **`struct_check.py` 在 GBK 控制台下会崩**：打印 `✅` 时 `UnicodeEncodeError` → **exit 1**。
    这不是结构体不一致。加 `PYTHONIOENCODING=utf-8` 才是准的（实测：不加 exit 1，
    加了 PASS / `53 个字段，55736 字节`）。
30. **`struct_check.py` 目前只检查 `NPTelemetry`**；给 `NPSensors` 加检查前要注意
    **字段宽度之和 444 ≠ `sizeof` 448**（尾部对齐填充），脚本的
    `cppsize != pysize` 判据会**误报**。
31. **第三份镜像在 `tests/diag.py`**（`CFG_FIELDS`，手算偏移读 Config 共享内存）——
    最容易忘，且给 `NPConfig` 中间插字段会让它后面所有字段全读错。
32. **磁盘上同时存在两份 NextPerf（其中一份是旧构建）**：习惯性打开了旧的那份 →
    所有修复看起来都"没生效"，白排查一整轮。所以主程序标题栏与启动日志里都带
    **构建时间戳**（`BuildStamp()`，取自身文件时间戳而不用 `__DATE__`：
    clang 会因可复现构建报 `-Wdate-time` 错误）。

---

## 9. 对未来的自己 / 其他迭代者的叮嘱

### 9.1 改任何结构体字段（**必做**）

1. **只从 `reserved[]` 挪**（`NPConfig`）或**只加在末尾**（`NPTelemetry`/`NPSensors`），
   尽量保持 `sizeof` 不变；
2. 同步 `NPClearTelemetry()` / `NPClearSensors()` / `NPDefaultConfig()`；
3. **同步 [`docs/DATA-STRUCTS.md`](DATA-STRUCTS.md) 的逐字段表**（名字/类型/含义/默认值/读写方）；
4. **同步 [`docs/CATALOG.md`](CATALOG.md)** 的登记项；
5. **同步 `tests/verify_inject.py` 里的 ctypes 镜像**；
6. **跑 `python tests/struct_check.py`**（记得 `PYTHONIOENCODING=utf-8`，否则会因编码报错 exit 1）；
7. 顺手看一眼 `tests/diag.py` 的 `CFG_FIELDS`；
8. 动了 `sizeof` 就**两个二进制一起重编**，并把"读方校验 `version == sizeof`"补上
   （现在 C++ 侧没有任何读方校验，只靠人肉保证）。

### 9.2 动 Low 帧 / 统计口径

* 先在 `tests/` 里造一段**可复现**的帧时间序列，把四种口径的输出打出来对比；
* 记住最终口径的出处（Intel PresentMon：1 秒窗口 + 对 FPS 取 P1）；
* **不要**为了"看起来更准"把窗口调大、把口径改回"最差 x% 平均"；
* 别把 `thread_local` 改回 `static`。

### 9.3 动面板视觉 / 排版

* 先读 `np_panel.cpp` 开头那 6 条风格要素，再改；
* 新增可变量的列 → 必须量化到**字符格整数倍**，并且**不看当前数据**；
* 新增字符串行 → 过 `FitCols`；
* 改完用 `tests/shot_overlay.py` 连拍 + `tests/panel_width_check.py` 量**左边缘是否稳定**
  （面板右对齐，宽度一变左边缘就动）；
* 注释与代码不一致时**改注释**，不要留一句骗下一个人的话。

### 9.4 验证与自检（最短路径）

```bash
python tests\struct_check.py            # 结构体镜像（先设 PYTHONIOENCODING=utf-8）
python tests\vt_check.py                # COM vtable 下标（改钩子前必跑）
python tests\verify_inject.py           # 注入 + 遥测端到端（D3D12）
python tests\verify_inject.py --d3d11   # 同上，D3D11
dist\NextPerf.exe --selftest            # 传感器 + 面板渲染自检（结果写项目根 selftest.txt）
dist\NextPerf.exe --uismoke             # 真开窗口跑消息循环
python tests\diag.py                    # 现场诊断：谁真的载入了钩子
python tests\panel_width_check.py       # 面板宽度是否随数值抖动（先跑 shot_overlay.py 连拍）
python tests\low_recover_check.py       # Low 帧在卡顿后多久恢复（验证 recent() 的 FIFO 语义）
python tests\row_metrics_check.py       # 面板行数值口径
python tests\merge_algo_check.py        # 帧内重复 Present 的合并算法
```

### 9.5 文档维护

* 本文（`COMMON.md`）= 公共层的**算法与设计意图**；字段级事实一律去 `DATA-STRUCTS.md`，
  **不要在两处各写一份字段表**（会漂移）。
* 行号会漂移：正文里对 `src/common/` 的引用尽量用**函数名/结构体名**，
  只在必要时给行号，并且改代码时顺手核对。
* **不要引用 `src/hook/np_hook.cpp` 的行号**（该文件改动频繁，行号随时会变）；
  引用它时用函数名（如 `UpdateTelemetryCommon()` / `PresentCommon()`）。

---

## 10. 未确认 / 待办（诚实标注）

1. `sizeof` 数值未经本机编译验证（无编译器可用）：136 / 55736 与 ctypes 复现完全一致，
   448 含 4 字节尾部填充。**建议加 `static_assert` 固定这三个尺寸。**
2. `lowIntegral` / `lowPct` / `RunningMax` / `medianMs` / `HistRange` / `HystWarn`
   当前**无调用点**（保留供对照或恢复功能）。是否删除需要产品决策。
3. `NPSensors` 的 `domExt` / `domExtPresent` / `hwRtTensorSrc` / `gpuPowerLimit` /
   `screenW` / `screenH` / `refreshHz` 目前**面板不显示**（只在日志/诊断里）。
4. `autoInject` 功能已废（`AppAutoInjectTick()` 故意留空），字段与 JSON 键仍在。
5. 「恢复默认设置」**不会**复位 `autoInject` / `pauseHook` / `learnedAutoHook` / `detachPid`
   （`NPDefaultConfig()` 不碰它们）—— 这算 bug 还是 feature 未确认。
6. `NPConfig::version` 恒为 1（不是 `sizeof`），与另外两个结构体的约定不一致；
   统一它需要同时改 `tests/diag.py` 与 `DATA-STRUCTS.md`。
7. **源码在动**：本文写作期间 `np_common.h` 被并行修改过一次
   （越界写修复 + 容量宏补 `u` 后缀，497 → 506 行）。本文开头的哈希表只能证明
   "写作那一刻的事实"，**改代码的人请顺手更新哈希与受影响段落**
   （尤其 `DATA-STRUCTS.md` 的字段表）。
