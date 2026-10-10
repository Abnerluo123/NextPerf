// ============================================================================
//  NextPerf 模拟游戏进程 —— 公共接口与数据结构
// ============================================================================
//
//  为什么需要这个文件？
//  NextPerf 是**注入式**的性能叠加层：它挂钩 IDXGISwapChain::Present /
//  ResizeBuffers / DXGI 工厂的 CreateSwapChain* 来采集帧数据。要验证它采到的
//  数字对不对，就必须有一个「自己知道每一帧真实情况」的游戏进程来对照。
//  旧的 tests/host_run.cpp 只有固定 ~32fps 的 Present + Sleep(16)，
//  既复现不了 60fps，也没法在运行中触发交换链重建（改分辨率 / 切窗口状态 /
//  开关垂直同步），所以很多结论只能靠真人开真游戏验证。
//
//  本目录（tests/sim/）就是这个「可编程的假游戏」：
//    * 两个后端：DX11（CreateSwapChain / Present）与 DX12（flip 模型）
//    * 锁帧（QPC 精确睡眠）、垂直同步开关、分辨率 / 窗口状态热切换
//    * 帧级自报数据：CPU 帧时间、GPU 时间戳帧时间、Present 阻塞时长、累计统计
//    * JSON 行输出（Python 好解析）+ 退出汇总 JSON
//    * stdin 逐行控制通道（比命名管道好驱动，跨语言零依赖）
//
//  设计原则：**引擎自己知道的才算数**。凡是引擎测不出来的（例如「显示器实际
//  刷新率」），要么如实标注不可用，要么给一个明确的来源，绝不编数字。
// ============================================================================

#pragma once

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------- 常量

// QPC 频率只在进程启动时取一次：GetFrequency 不会变，QPC 调用本身很便宜。
inline int64_t SimQpcFreq() {
    static int64_t f = 0;
    if (f == 0) {
        LARGE_INTEGER li{};
        QueryPerformanceFrequency(&li);
        f = li.QuadPart ? li.QuadPart : 1;
    }
    return f;
}

inline int64_t SimQpcNow() {
    LARGE_INTEGER li{};
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

// QPC 计数 -> 毫秒。用 double 而不是整数除法：帧间隔精度要到微秒级，
// 整数除法会把 60fps 的 16.667ms 截成 16ms，统计出来的 P50 就系统性偏低。
inline double SimQpcToMs(int64_t ticks) {
    return (double)ticks * 1000.0 / (double)SimQpcFreq();
}

// ---------------------------------------------------------------- 配置

// 窗口状态。注意 borderless（无边框全屏窗口）和 fullscreen（DXGI 独占全屏）
// 是两条完全不同的代码路径，对叠加层的影响也不同：
//   * borderless 走 DWM 合成，Present 通常不阻塞
//   * fullscreen 走独占全屏，DWM 被绕过，Present 会阻塞到垂直消隐
enum class SimWindowMode { Windowed = 0, Borderless = 1, Fullscreen = 2 };

const char* SimWindowModeName(SimWindowMode m);
bool SimWindowModeParse(const char* s, SimWindowMode* out);

struct SimConfig {
    bool use_dx12 = false;              // --api=dx11|dx12
    int seconds = 0;                    // --seconds=N（0 = 不按时间退出）
    int64_t frames = 0;                 // --frames=N（0 = 不按帧数退出）
    bool frames_set = false;            // 是否显式给了 --frames（用来报冲突）
    int fps_cap = 0;                    // --fps-cap=N，0 = 不锁
    bool vsync = true;                  // --vsync=on|off
    int width = 1280;                   // --width=W
    int height = 720;                   // --height=H
    int buffers = 0;                    // --buffers=N，0 = 用后端默认（DX11:2 / DX12:3）
    SimWindowMode window_mode = SimWindowMode::Windowed;  // --window-mode=...
    bool hidden = false;                // --hidden（不显示窗口，无头跑）
    bool json = false;                  // --json
    int json_every = 1;                 // --json-every=N，每 N 帧打一行
    int warmup_frames = 10;             // --warmup=N，前 N 帧不计入统计
    bool no_stdin = false;              // --no-stdin（不读控制命令，避免占用 stdin）
    bool tint = true;                   // --no-tint 时关掉逐帧变色（对照用）
    int hold_ms = 0;                    // --hold-ms=N，模拟 CPU 侧的固定开销
    double gpu_load_ms = 0.0;           // --gpu-load-ms=X，用空提交量模拟 GPU 侧压力
    bool list_outputs = false;          // --list-outputs
    std::string error;                  // 解析失败的原因（非空即失败）
};

// 解析命令行。支持 --k=v 与 --k v 两种写法（后者对 Python 拼参数更友好）。
SimConfig SimParseArgs(int argc, char** argv);
void SimPrintUsage();

// ---------------------------------------------------------------- 单帧自报数据

// 一帧的「真值」。字段分成三组：
//   1) 时间：这一帧到底花了多久，CPU / GPU / Present 各占多少
//   2) 状态：这一帧是在什么设置下跑的（vsync / 锁帧 / 分辨率 / 窗口状态）
//   3) 事件：这一帧前后有没有发生交换链重建（叠加层最容易在这里翻车）
struct SimFrameReport {
    int64_t frame_index = 0;      // 帧序号，从 0 开始

    double t_start_ms = 0.0;      // 本帧开始时刻（相对进程 start），QPC 换算
    double frame_delta_ms = 0.0;  // 与上一帧开始的间隔，ms（第一帧为 0）

    double cpu_frame_ms = 0.0;    // CPU 帧时间：帧开始 -> 命令提交完成
    double cpu_render_ms = 0.0;   // 其中录制/提交命令列表那一段
    bool gpu_time_valid = false;  // GPU 时间戳这一帧是否可用（不可用就别当真）
    double gpu_frame_ms = 0.0;    // GPU 帧时间：本帧首尾时间戳之差
    double present_ms = 0.0;      // Present 阻塞时长：进 Present 到返回

    bool vsync = true;
    int fps_cap = 0;
    int width = 0;
    int height = 0;
    SimWindowMode window_mode = SimWindowMode::Windowed;
    bool hidden = false;

    bool resize_event = false;         // 本帧发生了 ResizeBuffers / 重建交换链
    int swapchain_generation = 0;      // 交换链代数（每次重建 +1，叠加层可据此判断换对象了）
    bool present_failed = false;       // 本帧 Present 返回失败
    long present_hr = 0;               // Present 的 HRESULT
};

// ---------------------------------------------------------------- 累计统计

// 帧时间序列的分位数。用的是「最近秩」定义（nearest-rank），和项目其它地方
// 统计口径保持一致：idx = ceil(p/100 * N) - 1，夹到 [0, N-1]。
struct SimStats {
    int64_t frames = 0;               // 计入统计的帧数（不含 warmup）
    double elapsed_ms = 0.0;          // 计入统计的时间跨度

    double fps_avg = 0.0;             // 总帧数 / 总时间（整体平均帧率）
    double frame_ms_avg = 0.0;
    double frame_ms_min = 0.0;
    double frame_ms_max = 0.0;

    double frame_ms_stddev = 0.0;

    double p50_ms = 0.0;
    double p95_ms = 0.0;
    double p99_ms = 0.0;
    double p999_ms = 0.0;

    // 1% Low / 0.1% Low：与项目一致的口径 ——
    //   把帧时间序列升序排，取最差的 1% / 0.1% 那一段的**算术平均帧时间**，
    //   再换算成 FPS（1000 / 平均帧时间）。这比「取 P99 单帧」稳定，
    //   也是 PresentMon / CapFrameX 一类工具的习惯口径。
    double fps_1pct_low = 0.0;
    double fps_01pct_low = 0.0;
    int64_t low1_count = 0;   // 参与 1% low 的帧数
    int64_t low01_count = 0;  // 参与 0.1% low 的帧数

    double cpu_ms_avg = 0.0;
    double gpu_ms_avg = 0.0;          // 只对 gpu_time_valid 的帧求平均
    int64_t gpu_valid_frames = 0;
    bool gpu_available = false;       // 整个运行期间 GPU 时间戳到底能不能用
    double gpu_freq_hz = 0.0;         // GPU 时间戳计数频率
    const char* gpu_freq_source = "none";  // "disjoint"(DX11) / "clock-calibration"(DX12)
    double present_ms_avg = 0.0;

    double fps_best_1s = 0.0;         // 最好的 1 秒窗口帧率（抖动诊断用）
    double fps_worst_1s = 0.0;        // 最差的 1 秒窗口帧率
};

// 统计一个帧时间序列（ms）。
//   frame_ms   : 每帧的帧间隔（帧开始 -> 下一帧开始）。第一帧没有间隔，由调用方剔掉。
//   cpu_ms / present_ms : 与 frame_ms 一一对应。
//   gpu_ms     : 只放 gpu_time_valid 的帧，可以与 frame_ms 长度不同。
// 内部会复制并排序，调用方传进来的 vector 不会被改。
SimStats SimComputeStats(const std::vector<double>& frame_ms,
                         const std::vector<double>& cpu_ms,
                         const std::vector<double>& gpu_ms,
                         const std::vector<double>& present_ms,
                         bool gpu_available, double gpu_freq_hz,
                         const char* gpu_freq_source, double first_t_ms);

// ---------------------------------------------------------------- JSON

// 极简 JSON 写出（不引第三方库，也不用 src/common/np_json.h —— 本目录要求
// 自包含、只依赖 windows.h）。只做「转义 + 拼字符串」，够用就好。
void SimJsonEscapeTo(std::string& out, const char* s);
std::string SimFrameJson(const SimFrameReport& r, const SimConfig& cfg);
std::string SimSummaryJson(const SimConfig& cfg, const SimStats& st, int64_t total_frames,
                           int swapchain_generation, int64_t resize_events,
                           const char* exit_reason, const char* backend_note);
std::string SimStatusJson(const SimConfig& cfg, int64_t frame_index, int w, int h,
                          SimWindowMode mode, bool vsync, int fps_cap, double fps_recent);

// ---------------------------------------------------------------- 后端接口

// 交换链时间戳环。DX11 与 DX12 的查询回读机制不同（DX11 用 GetData 的
// DO_NOT_FLUSH 轮询，DX12 用 readback buffer + fence），但「哪一帧对应哪一对
// 时间戳、换算成 ms 是多少」这件事两边一样，所以放在公共头里由后端自己填写。
struct SimTimestampRing {
    static const int kSlots = 8;
    int64_t frame[kSlots] = {0};
    double begin_ms[kSlots] = {0.0};
    double end_ms[kSlots] = {0.0};
    bool valid[kSlots] = {false};
    int next = 0;

    void Store(int64_t frame_index, double b_ms, double e_ms, bool ok) {
        frame[next] = frame_index;
        begin_ms[next] = b_ms;
        end_ms[next] = e_ms;
        valid[next] = ok;
        next = (next + 1) % kSlots;
    }
};

// 图形后端抽象。所有 Present / ResizeBuffers 都发生在后端的 EndFrame 里 ——
// 这正是 NextPerf 挂钩子的地方，模拟进程必须老老实实走真实的 swapchain API。
class SimEngine {
public:
    virtual ~SimEngine() {}

    virtual bool Init(HWND hwnd, const SimConfig& cfg, std::string* err) = 0;

    // 一帧的流程：BeginFrame -> 录制/执行 -> EndFrame（内部 Present）。
    // EndFrame 收的是**非 const 引用**：后端在 Present 之后会把真实的
    // present_ms / present_hr / present_failed 回填进去 —— 这三个量只有
    // 真正调用 Present 的那一层才知道，公共代码猜不出来。
    virtual void BeginFrame(int64_t frame_index) = 0;
    virtual void EndFrame(SimFrameReport& r) = 0;

    // 改分辨率：DX11/DX12 都调 ResizeBuffers，后端负责先释放 RTV 等引用。
    virtual bool Resize(int w, int h, std::string* err) = 0;

    // 开关垂直同步（运行中可切换）。
    virtual void SetVsync(bool on) = 0;

    // 切窗口状态：公共代码负责改窗口样式 / 调 SetFullscreenState，
    // 后端只做「交换链自己需要跟着做的事」（例如 ResizeBuffers 到新尺寸）。
    virtual void OnWindowModeChanged(SimWindowMode mode) = 0;

    // 是否使用 DXGI 独占全屏。只有 DX11 后端需要（它的 flip 模型在切换前后
    // 必须显式 ResizeBuffers；而 DX12 常用现代 flip 模型 + 无边框窗口来模拟全屏）。
    // 返回 false 表示「本后端不做 SetFullscreenState，切成无边框即可」。
    virtual bool WantsExclusiveFullscreen() const { return true; }

    // 模拟 GPU 侧压力：DX12 空提交 / DX11 空 draw。
    virtual void SetGpuLoadMs(double ms) = 0;

    virtual int SwapchainGeneration() const = 0;
    virtual int BufferCount() const = 0;
    virtual const char* BackendName() const = 0;

    // 取本帧对应的 GPU 帧时间（ms）。返回 false 表示「这一帧还没有可用的
    // GPU 时间戳样本」——两个后端的时间戳都是**异步回读**的（延迟几帧才拿得到
    // 结果），所以早几帧会是 false。绝不返回编造的 0。
    virtual bool TakeLastGpuMs(double* out_ms) = 0;

    // GPU 时间戳频率及其来源。0 表示不可用（换算不了毫秒）。
    // 来源字符串会写进汇总 JSON，脚本可以据此判断这个数字可不可信。
    virtual double GpuTimestampFreqHz() const = 0;
    virtual const char* GpuTimestampFreqSource() const = 0;
    // 到目前这一刻为止，是否真的成功取到过至少一个 GPU 时间戳样本。
    virtual bool GpuTimeAvailable() const = 0;

    // 后端能力的如实声明（例如「本环境不支持时间戳」），会写进汇总 JSON。
    virtual const char* Note() const { return ""; }
};

// 后端工厂（分别实现在 sim_dx11.cpp / sim_dx12.cpp）。
SimEngine* SimCreateEngineDx11();
SimEngine* SimCreateEngineDx12();

// ---------------------------------------------------------------- 窗口

// 创建模拟游戏窗口。hidden=true 时不 ShowWindow（无头跑）。
HWND SimCreateWindow(HINSTANCE inst, const SimConfig& cfg, int w, int h, SimWindowMode mode);

// 按模式调整窗口样式 / 位置。fullscreen 由调用方另外调 SetFullscreenState。
void SimApplyWindowMode(HWND hwnd, SimWindowMode mode, int w, int h);

// 桌面（主显示器）分辨率 —— borderless / fullscreen 时要铺满它。
void SimPrimaryMonitorSize(int* w, int* h);

// 把窗口拉回前台（fullscreen 切换后 Windows 可能把焦点丢掉）。
void SimFocusWindow(HWND hwnd);

// 公共消息泵：处理 WM_QUIT。返回 false 表示收到了退出请求。
bool SimPumpMessages();

// ---------------------------------------------------------------- 帧率节流

// QPC 精确锁帧。这是本模拟器最关键的一小块代码：
//   朴素写法 Sleep(16) 的实际间隔是 15.6~31ms（Windows 默认时钟粒度 15.6ms），
//   这就是旧宿主「固定约 32fps」的根因。这里用
//   「粗睡到还差 ~1.5ms -> 让出时间片 -> 自旋到点」
//   三段式，才能在 60/120/144fps 这些档位上给出稳定的帧间隔。
class SimPacer {
public:
    void SetCap(int fps_cap);
    int Cap() const { return cap_; }
    // 帧首调用：返回本帧的目标开始时刻（QPC）。会按需睡 / 自旋。
    void WaitForFrameStart();
    // 该不该按锁帧节流？（锁帧 >= 显示器刷新率时其实没意义，但由用户决定）
    void Reset();

private:
    int cap_ = 0;
    int64_t next_deadline_ = 0;   // 下一帧的目标开始时刻
    int64_t period_ = 0;          // 目标帧间隔（QPC）
};

// 忙等一小段（自旋 + SwitchToThread），比 Sleep 精度高。
void SimSpinUntil(int64_t qpc_deadline);

// ---------------------------------------------------------------- stdin 控制

// 命令的含义（解析后）。
struct SimCommand {
    enum Kind {
        None,
        Resize,        // resize W H
        WindowMode,    // window windowed|borderless|fullscreen
        Vsync,         // vsync on|off
        FpsCap,        // fpscap N
        HoldMs,        // holdms N
        GpuLoad,       // gpuload X
        Quit,          // quit
        Stats,         // stats / dump
        Status,        // status
        Mark,          // mark NAME（在输出里插一条注释，方便脚本对时间轴）
        Help,
    };
    Kind kind = None;
    int w = 0, h = 0;
    bool on = false;
    int n = 0;
    double x = 0.0;
    SimWindowMode mode = SimWindowMode::Windowed;
    std::string arg;
    std::string raw;
};

// 解析一行控制命令。kind == None 时 raw 里是解析失败的原因。
SimCommand SimParseCommand(const std::string& line);

// ---------------------------------------------------------------- 数据采集

// 帧序列采集器：把每帧的自报数据攒下来，退出时算统计。
struct SimStatsCollector {
    std::vector<double> frame_ms;    // 帧间隔（第一帧没有间隔，不入列）
    std::vector<double> cpu_ms;
    std::vector<double> gpu_ms;      // 只放 gpu_time_valid 的帧
    std::vector<double> present_ms;
    double first_t_ms = 0.0;
    int64_t total_frames = 0;        // 含 warmup 的总帧数
    int64_t resize_events = 0;
    int64_t present_failures = 0;
    int64_t gpu_valid = 0;
    bool gpu_available = false;
    double gpu_freq_hz = 0.0;
    const char* gpu_freq_source = "none";

    void Add(const SimFrameReport& r);
    SimStats Compute() const;
    void Reset();
};

// 把控制动作作用到引擎 + 窗口上。返回 false 表示请求退出。
// need_resize_out 为 true 时调用方需要重建交换链尺寸（后端内部已处理则忽略）。
struct SimControlContext {
    HWND hwnd = nullptr;
    SimConfig* cfg = nullptr;
    SimEngine* engine = nullptr;
    SimPacer* pacer = nullptr;
    int* width = nullptr;
    int* height = nullptr;
    SimWindowMode* mode = nullptr;
    bool* quit = nullptr;
    bool* stats_requested = nullptr;
    int64_t frame_index = 0;
    int64_t* resize_events = nullptr;
};
void SimApplyCommand(const SimCommand& cmd, SimControlContext& ctx);

// 后台读 stdin 的线程。用线程而不是轮询，是因为管道 / 重定向 stdin 上
// PeekNamedPipe 对「文件结尾」和「暂时没数据」区分不可靠，阻塞读最简单可靠。
// 线程只负责「读出整行 + 解析」，真正作用于引擎的 SimApplyCommand 一律在
// **渲染线程**里执行 —— 交换链 / 窗口都不是线程安全的，跨线程直接改会翻车。
struct SimStdinThread {
    HANDLE thread = nullptr;
    CRITICAL_SECTION cs{};
    std::vector<SimCommand> queue;
    volatile LONG quit = 0;
    bool started = false;

    bool Start();
    void Stop();
    // 渲染线程每帧调一次：把排队的命令取走（并清空队列）。
    void Drain(std::vector<SimCommand>* out);
    static DWORD WINAPI Proc(LPVOID param);
};

// ---------------------------------------------------------------- 小工具

void SimLog(const char* fmt, ...);          // 带时间戳打到 stdout，自动 flush
void SimLogRaw(const char* fmt, ...);       // 不打时间戳，用于 JSON 行
const char* SimHrName(long hr);             // 常见 DXGI HRESULT 的可读名
