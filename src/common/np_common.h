// NextPerf - 次世代性能计数器
// 公共层：进程间共享的数据结构、常量、计数器位定义。
//
// 三块共享内存构成了「主程序 <-> 注入到游戏内的钩子 DLL」之间的全部通信：
//   NP_SHM_CONFIG     主程序写，钩子读：要显示哪些计数器、皮肤、位置、缩放
//   NP_SHM_SENSORS    主程序写，钩子读：CPU/GPU/显存/内存等传感器快照
//   NP_SHM_TELEMETRY  钩子写，主程序读：帧时间、帧延迟、引擎推断（RT/Tensor）等
//
// 这样设计的好处是钩子 DLL 不需要自己再去打开 NVML/NVAPI/WMI，
// 主程序一个进程负责采集，钩子只负责「画」。

#pragma once

#include <cstdint>

// ---------------------------------------------------------------- 共享内存名
//
// Config / Sensors 是「一份」，主程序写、所有钩子读。
// Telemetry 必须**每个进程一份**：同时注入两个游戏时，如果共用一个名字，
// 两个钩子会往同一块内存里互相覆盖，主程序读到的就是一锅粥。
// 所以钩子按自己的 PID 建名字，主程序按 PID 去开（见 NPTelemetryShmName）。
#define NP_SHM_CONFIG    TEXT("Local\\NextPerf_Config_v1")
#define NP_SHM_SENSORS   TEXT("Local\\NextPerf_Sensors_v1")
#define NP_MUTEX_SENSORS TEXT("Local\\NextPerf_SensorsMutex_v1")

// 某个钩子进程（游戏）的遥测块名字。pid 由调用方给出：
//   钩子侧：GetCurrentProcessId()
//   主程序侧：遍历自己注入过的那些 pid
// 自带十进制转换，不依赖 swprintf（这个头被很多 TU 包含）。
inline void NPTelemetryShmName(uint32_t pid, wchar_t* out, size_t n) {
    if (!out || n == 0) return;
    if (n < 40) { out[0] = 0; return; }
    const wchar_t* base = L"Local\\NextPerf_Telemetry_v1_";
    size_t i = 0;
    for (; base[i] && i + 12 < n; ++i) out[i] = base[i];
    wchar_t digits[12];
    int d = 0;
    if (pid == 0) digits[d++] = L'0';
    while (pid && d < 11) {
        digits[d++] = (wchar_t)(L'0' + (int)(pid % 10u));
        pid /= 10u;
    }
    while (d > 0 && i + 1 < n) out[i++] = digits[--d];
    out[i] = 0;
}

#define NP_MAGIC 0x4E505231u  // "NPR1"

// 帧时间环形缓冲容量：约 60 秒 @60FPS
#define NP_FRAME_CAP 4096u
// 图表最近采样点数
#define NP_GRAPH_CAP 512u
#define NP_NAME_LEN  128u
#define NP_MOD_LEN    24u
#define NP_MAX_MODULES 8u

// ---------------------------------------------------------------- 计数器位
// UI 里每一个可勾选的项目对应一个 bit。
enum NP_COUNTER : uint64_t {
    NP_C_FPS          = 1ull << 0,   // 实时 FPS
    NP_C_FRAMETIME    = 1ull << 1,   // 帧生成时间 ms
    NP_C_LOW1         = 1ull << 2,   // 1% Low
    NP_C_LOW01        = 1ull << 3,   // 0.1% Low
    NP_C_CPU_FRAME    = 1ull << 4,   // CPU 帧延迟
    NP_C_GPU_FRAME    = 1ull << 5,   // GPU 帧延迟
    NP_C_CPU_USAGE    = 1ull << 6,   // CPU 占用率
    NP_C_CPU_TEMP     = 1ull << 7,   // CPU 温度
    NP_C_GPU_USAGE    = 1ull << 8,   // GPU 占用率
    NP_C_GPU_TEMP     = 1ull << 9,   // GPU 温度
    NP_C_GPU_HOTSPOT  = 1ull << 10,  // GPU 热点 / 显存结温
    NP_C_GPU_POWER    = 1ull << 11,  // GPU 功耗
    NP_C_GPU_CLOCK    = 1ull << 12,  // GPU / 显存频率
    NP_C_VRAM         = 1ull << 13,  // 显存占用
    NP_C_RAM          = 1ull << 14,  // 内存占用
    NP_C_GPU_FB       = 1ull << 15,  // 显存控制器（带宽）占用
    NP_C_GPU_VID      = 1ull << 16,  // 视频引擎占用
    NP_C_GPU_BUS      = 1ull << 17,  // PCIe 总线占用
    NP_C_RT           = 1ull << 18,  // RT Core 负载
    NP_C_TENSOR       = 1ull << 19,  // Tensor / AI 单元负载
    NP_C_RESOLUTION   = 1ull << 20,  // 渲染 / 输出分辨率与缩放
    NP_C_API          = 1ull << 21,  // 图形 API 与运行状态
    NP_C_DRAWS        = 1ull << 22,  // Draw / Dispatch 次数
    NP_C_AI_MODULES   = 1ull << 23,  // 已识别的 AI 超分 / 帧生成模块
    NP_C_FAN          = 1ull << 24,  // 风扇转速
    NP_C_GRAPH        = 1ull << 25,  // 帧生成时间实时曲线
    NP_C_SENSOR_SRC   = 1ull << 26,  // 当前生效的传感器数据源
    NP_C_GPU_NAME     = 1ull << 27,  // 显卡型号
    NP_C_CHART_USAGE  = 1ull << 28,  // 图表：CPU/GPU 占用率曲线
    NP_C_CHART_FPS    = 1ull << 29,  // 图表：FPS / 平均 / 1% Low 曲线
    NP_C_CHART_LATENCY= 1ull << 30,  // 图表：帧生成 / CPU / GPU 延迟曲线
    NP_C_CPU_CLOCK    = 1ull << 31,  // CPU 当前频率（CallNtPowerInformation）
    NP_C_CPU_POWER    = 1ull << 32,  // CPU 包功耗（EMI / HWiNFO）
    // CPU Busy / CPU Wait 两个半边。**故意不加进 NP_ALL_COUNTERS**，
    // 所以默认不显示（用户要求：保留但默认关）。想看得去界面上勾。
    NP_C_CPU_BUSY     = 1ull << 33,
    NP_C_CPU_WAIT     = 1ull << 34,
};

#define NP_ALL_COUNTERS                                                        \
    (NP_C_FPS | NP_C_FRAMETIME | NP_C_LOW1 | NP_C_LOW01 | NP_C_CPU_FRAME |     \
     NP_C_GPU_FRAME | NP_C_CPU_USAGE | NP_C_CPU_TEMP | NP_C_GPU_USAGE |        \
     NP_C_GPU_TEMP | NP_C_GPU_POWER | NP_C_GPU_CLOCK | NP_C_VRAM | NP_C_RAM |  \
     NP_C_RT | NP_C_TENSOR | NP_C_RESOLUTION | NP_C_API | NP_C_GRAPH |         \
     NP_C_CHART_USAGE | NP_C_CHART_FPS | NP_C_CHART_LATENCY |               \
     NP_C_CPU_CLOCK | NP_C_CPU_POWER)

// ---------------------------------------------------------------- 图形 API
enum NP_GFXAPI : uint32_t {
    NP_API_UNKNOWN = 0,
    NP_API_D3D9    = 9,
    NP_API_D3D11   = 11,
    NP_API_D3D12   = 12,
    NP_API_VULKAN  = 20,
    NP_API_OPENGL  = 30,
};

// 全屏模式
enum NP_PRESENT_MODE : uint32_t {
    NP_PM_WINDOWED  = 0,
    NP_PM_BORDERLESS = 1,
    NP_PM_EXCLUSIVE = 2,
};

// AI / 超分 / 帧生成模块（钩子在目标进程里识别）
enum NP_AI_MODULE : uint32_t {
    NP_AI_NONE      = 0,
    NP_AI_DLSS_SR   = 1u << 0,  // NVIDIA DLSS 超分（Tensor Core）
    NP_AI_DLSS_RR   = 1u << 1,  // NVIDIA DLSS 光线重建
    NP_AI_DLSS_FG   = 1u << 2,  // NVIDIA DLSS 帧生成（光流加速 OFA）
    NP_AI_FSR       = 1u << 3,  // AMD FidelityFX 超分 / 帧生成
    NP_AI_XESS      = 1u << 4,  // Intel XeSS（XMX / DP4a）
    NP_AI_DIRECTML  = 1u << 5,  // DirectML（Windows AI / 通用推理）
    NP_AI_ORT       = 1u << 6,  // ONNX Runtime
    NP_AI_OTHER     = 1u << 7,
};

// 传感器数据源（用于「自动选择最优数据源」与 UI 展示）
enum NP_SENSOR_SRC : uint32_t {
    NP_SRC_NONE    = 0,
    NP_SRC_NVML    = 1u << 0,  // NVIDIA Management Library
    NP_SRC_NVAPI   = 1u << 1,  // NVIDIA NVAPI（含利用率域）
    NP_SRC_ADL     = 1u << 2,  // AMD Display Library
    NP_SRC_HWINFO  = 1u << 3,  // HWiNFO 共享内存
    NP_SRC_LHM     = 1u << 4,  // LibreHardwareMonitor WMI
    NP_SRC_PDH     = 1u << 5,  // Windows 性能计数器
    NP_SRC_WMI     = 1u << 6,  // WMI（ACPI 温度等）
    NP_SRC_INTEL   = 1u << 7,  // Intel 显卡驱动接口
};

// ---------------------------------------------------------------- 配置块
struct NPConfig {
    uint32_t magic;
    uint32_t version;
    uint32_t size;

    uint64_t counters;      // NP_COUNTER 位组合

    // 外观
    uint32_t bgColor;       // 0xAARRGGBB
    uint32_t textColor;
    uint32_t accentColor;   // 数值高亮
    uint32_t warnColor;     // 告警
    float    scale;         // 0.75 ~ 2.5
    float    opacity;       // 整体不透明度（旧字段，现固定 1.0）
    float    bgOpacity;     // 背景不透明度，与文字独立调节
    float    textOpacity;   // 文字不透明度，与背景独立调节
    int32_t  offsetX;       // 相对右上角的偏移
    int32_t  offsetY;
    uint32_t fontHeight;    // 逻辑像素

    // 行为
    uint32_t graphHeight;
    uint32_t overlayMode;   // 0=自动(优先游戏内) 1=强制游戏内 2=强制桌面叠加
    uint32_t fpsCap;        // 参考帧率上限（图表 Y 轴），0=自动
    uint32_t pollMs;        // 传感器轮询间隔
    uint32_t updateHz;      // 叠加刷新率限制
    uint32_t deepEngineHook;// 1=启用深度引擎钩子（命令列表级，实验性）
    uint32_t vtableProbe;   // 1=注入后自己造一条临时交换链去拿 Present 的 vtable
                            //   （游戏已经在跑的时候，工厂钩子不会再被调用，只能靠这个）
    uint32_t quit;          // 1=主程序正在退出，钩子收到后自行卸载
    uint32_t simulate;      // 1=模拟数据（开发预览用，正式版删除）
    // ---- 自动注入开关（用户要求：默认关闭，为了安全）
    // 复用原来的 reserved[0] 槽位 —— NPConfig 的**大小完全不变**，
    // 共享内存的 version(=sizeof) 自检不受影响，旧钩子 DLL 也不会失配。
    uint32_t autoInject;     // 0 = 关（默认）  1 = 检测到 3D 窗口时自动注入
    // 「暂停钩子」：主程序停止监视时置位。钩子据此跳过叠加绘制与遥测更新
    // （= 停止读取游戏数据），只保留最小心跳。
    uint32_t pauseHook;
    // 「学习来的条目要不要自动注入」。默认 0（关）—— 这是实验性功能：
    // 学习过程有验证（Present 挂上 + 认出 D3D + 帧在流动），但宁可让用户
    // 主动打开。**手动添加的条目不受此开关影响，一直自动注入。**
    uint32_t learnedAutoHook;
    // 「请求某个 pid 的钩子自卸载」。非 0 且等于钩子自己的 pid 时，
    // 钩子会走 SelfUnloadNow() 干净卸载（还原 vtable 补丁 + 注销 VEH）。
    //
    // 为什么不用 CreateRemoteThread 调 NpHookDetach：那需要解析远端导出地址
    // （ASLR 下要自己算偏移），而且跨位数（32 位游戏）还得另做一套。
    // 钩子本来每帧就在读 NPConfig，用这个字段既简单又天然支持跨位数。
    uint32_t detachPid;
    uint32_t reserved[4];
};

// 历史采样环形缓冲：图表曲线的数据源。
// 主程序（桌面叠加）与钩子（游戏内）各自维护一份，容量约半分钟到一分钟。
#define NP_HIST_CAP 256u
struct NPHistory {
    uint32_t write = 0;
    uint32_t count = 0;
    uint32_t sampleMs = 125;  // 采样间隔（用于 X 轴时间换算）
    float fps[NP_HIST_CAP];       // 瞬时 FPS
    float avg[NP_HIST_CAP];       // 平均 FPS
    float low1[NP_HIST_CAP];      // 1% Low（FPS）
    float low01[NP_HIST_CAP];     // 0.1% Low（FPS）
    float usageCpu[NP_HIST_CAP];  // CPU 占用 %
    float usageGpu[NP_HIST_CAP];  // GPU 占用 %
    float latFrame[NP_HIST_CAP];  // 帧生成时间 ms
    float latCpu[NP_HIST_CAP];    // CPU 帧延迟 ms
    float latGpu[NP_HIST_CAP];    // GPU 帧延迟 ms
    float latBusy[NP_HIST_CAP];   // CPU Busy ms（PresentMon 口径）
    float latWait[NP_HIST_CAP];   // CPU Wait ms（在 Present 内部等待）
};

inline void NPHistoryPush(NPHistory* h, float fps, float avg, float low1, float low01,
                          float ucpu, float ugpu, float lf, float lc, float lg,
                          float lbusy, float lwait) {
    uint32_t i = h->write;
    h->fps[i] = fps; h->avg[i] = avg; h->low1[i] = low1; h->low01[i] = low01;
    h->usageCpu[i] = ucpu; h->usageGpu[i] = ugpu;
    h->latFrame[i] = lf; h->latCpu[i] = lc; h->latGpu[i] = lg;
    h->latBusy[i] = lbusy; h->latWait[i] = lwait;
    h->write = (i + 1) % NP_HIST_CAP;
    if (h->count < NP_HIST_CAP) ++h->count;
}

// ---------------------------------------------------------------- 传感器块
struct NPSensors {
    uint32_t magic;
    uint32_t version;
    uint64_t tickMs;        // GetTickCount64
    uint32_t valid;         // 1 = 已填充

    // ---- CPU
    float    cpuUsage;      // %
    float    cpuTemp;       // ℃
    float    cpuPower;      // W（0 = 不可用）
    float    cpuClock;      // MHz
    uint32_t cpuCores;
    uint32_t cpuThreads;

    // ---- 内存
    float    ramUsedGB;
    float    ramTotalGB;
    float    ramPct;

    // ---- GPU
    char     gpuName[NP_NAME_LEN];
    uint32_t gpuVendor;     // 0 未知 1 NVIDIA 2 AMD 3 Intel
    float    gpuUsage;      // %
    float    gpuTemp;       // ℃
    float    gpuHotspot;    // ℃
    float    gpuMemTemp;    // ℃
    float    gpuPower;      // W
    float    gpuPowerLimit; // W
    float    gpuClock;      // MHz
    float    memClock;      // MHz
    float    gpuFanPct;     // %
    float    gpuFanRpm;

    // ---- 显存
    float    vramUsedGB;
    float    vramTotalGB;
    float    vramPct;

    // ---- NVAPI 利用率域（NVIDIA 独占，其他厂商为 -1）
    float    domGpu;        // 图形引擎
    float    domFb;         // 显存控制器（近似带宽占用）
    float    domVid;        // 视频引擎
    float    domBus;        // PCIe 总线
    float    domExt[4];     // 未公开的 4~7 号域：本程序会持续探测，若有值即视为厂商新增域
    uint32_t domExtPresent; // bit0..3 对应 domExt[0..3] 是否有值

    // ---- 硬件级 RT / Tensor 计数器（若厂商接口提供）
    float    hwRtPct;       // -1 = 硬件接口未提供
    float    hwTensorPct;   // -1 = 硬件接口未提供
    uint32_t hwRtTensorSrc; // NP_SENSOR_SRC 位

    // ---- 按游戏进程读的 GPU 引擎数据（PDH，非管理员，驱动报的数）
    // gpuBusyMs = 每帧真正的 GPU 执行时间，与锁帧/Reflex/多线程提交无关
    float    gpuBusyMs;     // -1 = 读不到
    float    engCompute;    // 该进程 compute 引擎占用 %（AI/超分的代理指标），-1 = 无
    float    engOfa;        // 该进程光流加速器占用 %（DLSS 帧生成专用），-1 = 无

    // ---- 显示模式（系统给：EnumDisplaySettings）
    // refreshHz 很有用：玩家说「锁 60」时，它就是那个 60 的来源，
    // 也能用来判断帧率到底是被垂直同步限住还是真的跑满了。
    uint32_t screenW, screenH, refreshHz;

    uint32_t sources;       // 实际生效的数据源（NP_SENSOR_SRC 位组合）
    char     sourceText[NP_NAME_LEN];  // 例如 "NVML+NVAPI | CPU:PDH | 温度:HWiNFO"
};

// ---------------------------------------------------------------- 遥测块
struct NPTelemetry {
    uint32_t magic;
    // 写的是 **sizeof(NPTelemetry)**，不是版本号。
    // 读方（主程序、测试脚本）拿它和自己算出来的大小对一下，就能立刻发现
    // 结构体布局对不上 —— 曾经往中间插了一个字段而 Python 那边的 ctypes
    // 结构体没同步，结果读到的全是错位字节（钩子明明工作正常却报「未挂上」）。
    uint32_t version;
    uint32_t pid;
    uint32_t attached;      // 1 = 钩子已接管渲染

    uint64_t tickMs;        // 最近一次更新
    uint32_t gfxApi;        // NP_GFXAPI
    uint32_t presentMode;   // NP_PRESENT_MODE
    uint32_t renderW, renderH;   // 渲染分辨率（后台缓冲）
    uint32_t windowW, windowH;   // 输出 / 窗口分辨率
    char     processName[NP_NAME_LEN];

    // ---- 帧统计
    uint32_t frameTotal;      // 累计帧数
    uint32_t frameWrite;      // 环形缓冲写指针
    float    frames[NP_FRAME_CAP];    // 帧生成时间 ms（Present 间隔）
    float    cpuFrames[NP_FRAME_CAP]; // CPU 帧时间 ms
    float    gpuFrames[NP_FRAME_CAP]; // GPU 帧时间 ms

    float    fps;           // 瞬时 FPS（已做指数平滑，见 UpdateTelemetryCommon）
    float    fpsAvg;        // 平均 FPS
    float    fpsLow1;       // 1% Low
    float    fpsLow01;      // 0.1% Low
    float    frameMs;       // 最近一帧（原始值，参与统计与图表）
    float    frameMsAvg;    // 平滑后的帧时间，**面板显示用这个**
    // CPU 帧时间 = 帧周期 − 卡在 Present 里等垂直同步的时间。
    // 必须扣掉等待，否则锁 60 时它恒等于 16.66ms —— 那是帧周期，不是 CPU 的活。
    float    cpuFrameMs;
    float    cpuFrameMsAvg;   // 平滑后的 CPU 帧时间（图表用它，避免锯齿看着像剧烈波动）
    // 把 CPU 帧时间拆成两段 —— 这是 Reflex 的口径，用户要的是**模拟阶段**：
    //   模拟阶段   : 上一帧 Present 返回 → 本帧渲染线程第一次提交命令
    //                （游戏自己的逻辑、物理、动画、剔除）
    //   渲染提交   : 第一次提交 → 调 Present
    //                （录制命令列表 + 提交，**这一段是排队时间**，
    //                  用户明确说不该算进 CPU 帧）
    float    simMs;
    float    submitMs;
    // GPU 帧时间 = 本帧每一次 ExecuteCommandLists 的 GPU 实测耗时之和。
    // 用两段时间戳把每一批夹住，GPU 空转不算进去 —— 所以它和锁帧无关，
    // 60fps 下 GPU 有余量时就是 10ms 甚至更低。
    float    gpuFrameMs;
    float    msInPresent;   // 本帧卡在 Present 调用里的时长（主要就是等垂直同步）

    // ---- CPU 侧的两个半边（PresentMon 口径，见 MetricsCalculator.cpp:299）
    //   CPUBusy = 本帧 Present 开始 − 上一帧 Present 返回
    //   CPUWait = 本帧在 Present 内部停留（= msInPresent）
    //   CPUBusy + CPUWait = 帧周期
    // 注意：CPUBusy 本质是「Present 之间的残差」。流水线渲染的游戏里它本来就接近 0
    // （渲染线程在上一帧还阻塞于 Present 时已准备下一帧）；开 Reflex 后等待被移到
    // Present 之前，那段睡眠落进间隙，这个值会明显变大 —— 正是「低延迟」提示的依据。
    float    cpuBusyMs;
    float    cpuBusyAvg;    // 平滑后（图表用）
    float    cpuWaitMs;
    float    cpuWaitAvg;
    float    p99Ms, p999Ms; // 帧时间百分位

    // ---- 引擎级推断
    uint32_t drawCalls;     // 上一帧 Draw* 次数
    uint32_t dispatches;    // 上一帧 Dispatch 次数
    uint32_t rtDispatches;  // 上一帧 DispatchRays 次数
    uint32_t asBuilds;      // 上一帧 BVH 构建次数
    float    rtGpuMs;       // 上一帧 RT pass 实测 GPU 时间
    float    aiGpuMs;       // 上一帧 AI/后处理 compute pass 实测 GPU 时间
    float    rtLoad;        // RT 单元负载 %（rtGpuMs / gpuFrameMs）
    float    tensorLoad;    // Tensor/AI 负载 %
    uint32_t aiModules;     // NP_AI_MODULE 位组合
    uint32_t rtMeasured;    // 1 = RT 数值为实测；0 = 推断/未启用
    uint32_t tensorMeasured;

    // ---- 图表曲线（降采样后的最近数据，便于直接绘制）
    uint32_t graphWrite;
    uint32_t graphCount;
    float    graphFrame[NP_GRAPH_CAP];
    float    graphCpu[NP_GRAPH_CAP];
    float    graphGpu[NP_GRAPH_CAP];

    // ---- 诊断
    uint32_t hookFlags;     // 见 NP_HOOK_*
    char     lastError[NP_NAME_LEN];
};

enum NP_HOOK_FLAG : uint32_t {
    NP_HOOK_PRESENT    = 1u << 0,
    NP_HOOK_QUEUE      = 1u << 1,
    NP_HOOK_CMDLIST    = 1u << 2,
    NP_HOOK_TIMESTAMP  = 1u << 3,
    NP_HOOK_OVERLAY    = 1u << 4,
    // 游戏自己在上报 Reflex 延迟标记（说明低延迟技术已启用）。
    // 这条比「用 CPUBusy 猜」可靠得多 —— 那是旁证，这是直证。
    NP_HOOK_REFLEX     = 1u << 5,
    // 「我们成功问过驱动了」—— 只有它置位时，NP_HOOK_REFLEX 才是**权威结论**；
    // 否则那一位只是缺省值，调用方要回退到自己的判据（如 CPU Wait < 1.6ms）。
    NP_HOOK_REFLEX_KNOWN = 1u << 6,
};

// ---------------------------------------------------------------- 便捷函数
inline void NPCopyStr(char* dst, size_t n, const char* src) {
    if (!dst || n == 0) return;
    size_t i = 0;
    if (src) {
        for (; i + 1 < n && src[i]; ++i) dst[i] = src[i];
    }
    dst[i] = '\0';
}

inline void NPDefaultConfig(NPConfig* c) {
    c->magic = NP_MAGIC;
    c->version = 1;
    c->size = (uint32_t)sizeof(NPConfig);
    c->counters = NP_ALL_COUNTERS;
    c->bgColor = 0xFF000000u;   // 纯黑背景，透明度走 bgOpacity
    c->textColor = 0xFFFFFFFFu;
    c->accentColor = 0xFF5AC8FAu;
    c->warnColor = 0xFFFF6B5Bu;
    c->scale = 1.0f;
    c->opacity = 1.0f;
    c->bgOpacity = 0.72f;
    c->textOpacity = 1.0f;
    c->offsetX = 16;
    c->offsetY = 16;
    c->fontHeight = 15;
    c->graphHeight = 64;
    c->overlayMode = 0;
    c->fpsCap = 0;
    c->pollMs = 500;
    c->updateHz = 20;
    c->deepEngineHook = 1;
    c->vtableProbe = 1;
    c->quit = 0;
    c->simulate = 0;
    for (int i = 0; i < 8; ++i) c->reserved[i] = 0;
}

inline void NPClearSensors(NPSensors* s) {
    s->magic = NP_MAGIC;
    // ★ 和 NPClearTelemetry 一样写**结构体大小**，不要写死的 1。
    //   原来这里是 `s->version = 1`，于是「主程序写 / 钩子读」这一路
    //   （NPSensors）完全没有任何布局自检：往中间插一个字段，两边照样能跑，
    //   但读出来的是错位字节（这个坑在 NPTelemetry 上已经咬过一次了）。
    //   写 sizeof 之后，读方只要对一下就能立刻发现布局不一致。
    s->version = (uint32_t)sizeof(NPSensors);
    s->valid = 0;
    s->gpuName[0] = 0;
    s->sourceText[0] = 0;
    s->cpuUsage = s->gpuUsage = -1.0f;
    s->cpuTemp = s->gpuTemp = s->gpuHotspot = s->gpuMemTemp = -273.0f;
    s->cpuPower = s->gpuPower = s->gpuPowerLimit = -1.0f;
    s->cpuClock = s->gpuClock = s->memClock = -1.0f;
    s->gpuFanPct = s->gpuFanRpm = -1.0f;
    s->vramUsedGB = s->vramTotalGB = s->vramPct = -1.0f;
    s->ramUsedGB = s->ramTotalGB = s->ramPct = -1.0f;
    s->cpuCores = s->cpuThreads = 0;
    s->gpuVendor = 0;
    s->domGpu = s->domFb = s->domVid = s->domBus = -1.0f;
    for (int i = 0; i < 4; ++i) s->domExt[i] = -1.0f;
    s->domExtPresent = 0;
    s->hwRtPct = s->hwTensorPct = -1.0f;
    s->hwRtTensorSrc = 0;
    s->gpuBusyMs = s->engCompute = s->engOfa = -1.0f;
    s->screenW = s->screenH = s->refreshHz = 0;
    s->sources = 0;
    s->tickMs = 0;
}

inline void NPClearTelemetry(NPTelemetry* t) {
    t->magic = NP_MAGIC;
    t->version = (uint32_t)sizeof(NPTelemetry);   // 见结构体里 version 的说明
    t->pid = 0;
    t->attached = 0;
    t->tickMs = 0;
    t->gfxApi = NP_API_UNKNOWN;
    t->presentMode = NP_PM_WINDOWED;
    t->renderW = t->renderH = t->windowW = t->windowH = 0;
    t->processName[0] = 0;
    t->lastError[0] = 0;
    t->frameTotal = t->frameWrite = 0;
    t->fps = t->fpsAvg = t->fpsLow1 = t->fpsLow01 = 0;
    t->simMs = t->submitMs = 0;
    t->frameMs = t->frameMsAvg = t->cpuFrameMs = t->cpuFrameMsAvg = t->gpuFrameMs =
        t->msInPresent = t->p99Ms = t->p999Ms = 0;
    t->cpuBusyMs = t->cpuBusyAvg = t->cpuWaitMs = t->cpuWaitAvg = 0;
    t->drawCalls = t->dispatches = t->rtDispatches = t->asBuilds = 0;
    t->rtGpuMs = t->aiGpuMs = t->rtLoad = t->tensorLoad = 0;
    t->aiModules = 0;
    t->rtMeasured = t->tensorMeasured = 0;
    t->graphWrite = t->graphCount = 0;
    t->hookFlags = 0;
    for (uint32_t i = 0; i < NP_FRAME_CAP; ++i) { t->frames[i] = 0; t->cpuFrames[i] = 0; t->gpuFrames[i] = 0; }
    for (uint32_t i = 0; i < NP_GRAPH_CAP; ++i) { t->graphFrame[i] = 0; t->graphCpu[i] = 0; t->graphGpu[i] = 0; }
}
