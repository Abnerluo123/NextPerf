// ============================================================================
//  NextPerf 模拟游戏进程 —— 公共实现
//    窗口创建 / 命令行解析 / 锁帧 / 控制命令 / 统计 / JSON 输出 / 主循环
// ============================================================================
//
//  这个文件的重点是「让外部脚本能像操作真游戏一样操作它」：
//    * 锁帧必须真的准（QPC + 三段式睡眠），否则测不了 60fps 这类档位
//    * 分辨率 / 窗口状态 / 垂直同步 / 锁帧 都要能在运行中改
//    * 每帧自报真值，退出时给汇总 JSON，供 Python 与叠加层的数字对照
// ============================================================================

#include "sim.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdlib>

// ============================================================================
// 小工具
// ============================================================================

static int64_t g_start_qpc = 0;

// 相对进程启动的毫秒数。所有时间戳都用它做基准，方便和脚本时间轴对齐。
static double SimNowMs() { return SimQpcToMs(SimQpcNow() - g_start_qpc); }

void SimLogRaw(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    // 必须每行 flush：Python 用管道读的时候，块缓冲会把输出憋到进程退出，
    // 那就没法做「实时对照」了。
    fflush(stdout);
}

void SimLog(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    // 非 JSON 诊断信息一律走 stderr：这样 stdout 可以保持「纯 JSON 行」，
    // Python 端 json.loads(line) 不会被噪声打断。
    fprintf(stderr, "[sim %8.2fms] %s\n", SimNowMs(), buf);
    fflush(stderr);
}

const char* SimHrName(long hr) {
    switch ((unsigned long)hr) {
        case 0x00000000UL: return "S_OK";
        case 0x087A0001UL: return "DXGI_STATUS_OCCLUDED";
        case 0x087A0002UL: return "DXGI_STATUS_CLIPPED";
        case 0x087A0004UL: return "DXGI_STATUS_MODE_CHANGED";
        case 0x887A0001UL: return "DXGI_ERROR_INVALID_CALL";
        case 0x887A0002UL: return "DXGI_ERROR_NOT_FOUND";
        case 0x887A0004UL: return "DXGI_ERROR_UNSUPPORTED";
        case 0x887A0005UL: return "DXGI_ERROR_DEVICE_REMOVED";
        case 0x887A0006UL: return "DXGI_ERROR_DEVICE_HUNG";
        case 0x887A0007UL: return "DXGI_ERROR_DEVICE_RESET";
        case 0x887A000AUL: return "DXGI_ERROR_WAS_STILL_DRAWING";
        case 0x887A0022UL: return "DXGI_ERROR_NOT_CURRENTLY_AVAILABLE";
        case 0x887A002BUL: return "DXGI_ERROR_ACCESS_LOST";
        case 0x8007000EUL: return "E_OUTOFMEMORY";
        case 0x80070057UL: return "E_INVALIDARG";
        case 0x80004005UL: return "E_FAIL";
        case 0x80004001UL: return "E_NOTIMPL";
        default: return "HRESULT?";
    }
}

// ============================================================================
// 命令行
// ============================================================================

const char* SimWindowModeName(SimWindowMode m) {
    switch (m) {
        case SimWindowMode::Windowed: return "windowed";
        case SimWindowMode::Borderless: return "borderless";
        case SimWindowMode::Fullscreen: return "fullscreen";
    }
    return "?";
}

bool SimWindowModeParse(const char* s, SimWindowMode* out) {
    if (!s || !out) return false;
    if (!_stricmp(s, "windowed") || !_stricmp(s, "window")) { *out = SimWindowMode::Windowed; return true; }
    if (!_stricmp(s, "borderless") || !_stricmp(s, "noborder")) { *out = SimWindowMode::Borderless; return true; }
    if (!_stricmp(s, "fullscreen") || !_stricmp(s, "exclusive") || !_stricmp(s, "fs")) {
        *out = SimWindowMode::Fullscreen; return true;
    }
    return false;
}

static bool ParseBool(const char* s, bool* out) {
    if (!s || !out) return false;
    if (!_stricmp(s, "on") || !_stricmp(s, "1") || !_stricmp(s, "true") || !_stricmp(s, "yes")) {
        *out = true; return true;
    }
    if (!_stricmp(s, "off") || !_stricmp(s, "0") || !_stricmp(s, "false") || !_stricmp(s, "no")) {
        *out = false; return true;
    }
    return false;
}

static bool Atoll(const char* s, long long* out) {
    if (!s || !*s) return false;
    char* end = nullptr;
    long long v = _strtoi64(s, &end, 10);
    if (end == s) return false;
    *out = v;
    return true;
}

static bool Atod(const char* s, double* out) {
    if (!s || !*s) return false;
    char* end = nullptr;
    double v = strtod(s, &end);
    if (end == s) return false;
    *out = v;
    return true;
}

// 命令行约定：--key=value 和 --key value 都支持。
// 后者对 Python 的 subprocess（列表参数）更顺手，前者手敲方便，所以两种都留。
SimConfig SimParseArgs(int argc, char** argv) {
    SimConfig c;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(argv[i]);

    // 取出第 i 个参数的值：--k=v 已内联，否则吃下一个 argv。
    auto value_at = [&](size_t i, const std::string& a, size_t eq, const char** val) -> bool {
        if (eq != std::string::npos) {
            // 注意：这里返回的是 a 里的临时子串，调用方必须马上用。
            *val = a.c_str() + eq + 1;
            return true;
        }
        if (i + 1 < args.size()) {
            *val = args[i + 1].c_str();
            return true;
        }
        return false;
    };

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a.size() < 3 || a[0] != '-' || a[1] != '-') {
            if (a == "-h") { c.error = "__help__"; return c; }
            c.error = "无法识别的参数：" + a;
            return c;
        }
        size_t eq = a.find('=');
        std::string key = a.substr(2, (eq == std::string::npos ? a.size() : eq) - 2);
        const char* val = nullptr;
        bool has_val = (eq != std::string::npos);
        if (!has_val) {
            // 无值参数：先看是不是布尔开关
            if (key == "hidden" || key == "json" || key == "help" || key == "list-outputs" ||
                key == "no-stdin" || key == "vsync") {
                // --vsync 后面可能跟 on/off，也可能不跟（默认 on）
                if (key == "vsync" && i + 1 < args.size() &&
                    (args[i + 1] == "on" || args[i + 1] == "off")) {
                    val = args[i + 1].c_str();
                    ++i;
                } else {
                    val = "1";
                }
                has_val = true;
            }
        }
        if (!has_val) {
            if (!value_at(i, a, eq, &val)) {
                c.error = "参数 --" + key + " 缺少取值";
                return c;
            }
            if (eq == std::string::npos) ++i;  // 吃掉了下一个 argv
        }
        // 注意：--k=v 时 val 指向 a.c_str()，a 在本轮结束后仍然有效（args 常驻），
        // 但循环变量 i 变化不会影响，所以这里直接往下用是安全的。

        long long n = 0;
        double d = 0.0;
        bool b = false;

        if (key == "api") {
            if (!_stricmp(val, "dx12") || !_stricmp(val, "d3d12") || !_stricmp(val, "12")) {
                c.use_dx12 = true;
            } else if (!_stricmp(val, "dx11") || !_stricmp(val, "d3d11") || !_stricmp(val, "11")) {
                c.use_dx12 = false;
            } else {
                c.error = "--api 只支持 dx11 / dx12";
                return c;
            }
        } else if (key == "seconds") {
            if (!Atoll(val, &n) || n < 0) { c.error = "--seconds 需要非负整数"; return c; }
            c.seconds = (int)n;
        } else if (key == "frames") {
            if (!Atoll(val, &n) || n <= 0) { c.error = "--frames 需要正整数"; return c; }
            c.frames = n;
            c.frames_set = true;
        } else if (key == "fps-cap" || key == "fpscap") {
            if (!Atoll(val, &n) || n < 0) { c.error = "--fps-cap 需要非负整数"; return c; }
            c.fps_cap = (int)n;
        } else if (key == "vsync") {
            if (!ParseBool(val, &b)) { c.error = "--vsync 只支持 on / off"; return c; }
            c.vsync = b;
        } else if (key == "width") {
            if (!Atoll(val, &n) || n <= 0) { c.error = "--width 需要正整数"; return c; }
            c.width = (int)n;
        } else if (key == "height") {
            if (!Atoll(val, &n) || n <= 0) { c.error = "--height 需要正整数"; return c; }
            c.height = (int)n;
        } else if (key == "buffers") {
            if (!Atoll(val, &n) || n < 2 || n > 4) { c.error = "--buffers 只支持 2..4"; return c; }
            c.buffers = (int)n;
        } else if (key == "window-mode" || key == "window") {
            if (!SimWindowModeParse(val, &c.window_mode)) {
                c.error = "--window-mode 只支持 windowed / borderless / fullscreen";
                return c;
            }
        } else if (key == "hidden") {
            c.hidden = true;
        } else if (key == "json") {
            c.json = true;
        } else if (key == "json-every") {
            if (!Atoll(val, &n) || n <= 0) { c.error = "--json-every 需要正整数"; return c; }
            c.json_every = (int)n;
        } else if (key == "warmup") {
            if (!Atoll(val, &n) || n < 0) { c.error = "--warmup 需要非负整数"; return c; }
            c.warmup_frames = (int)n;
        } else if (key == "no-stdin") {
            c.no_stdin = true;
        } else if (key == "no-tint") {
            c.tint = false;
        } else if (key == "hold-ms") {
            if (!Atoll(val, &n) || n < 0) { c.error = "--hold-ms 需要非负整数"; return c; }
            c.hold_ms = (int)n;
        } else if (key == "gpu-load-ms") {
            if (!Atod(val, &d) || d < 0.0) { c.error = "--gpu-load-ms 需要非负浮点"; return c; }
            c.gpu_load_ms = d;
        } else if (key == "list-outputs" || key == "list-outputs-once") {
            c.list_outputs = true;
        } else if (key == "help" || key == "h") {
            c.error = "__help__";
            return c;
        } else {
            c.error = "无法识别的参数：--" + key;
            return c;
        }
    }

    if (c.seconds > 0 && c.frames_set) {
        // 两个都给的话按「先到先退」处理更贴近直觉，但明确报出来更好调试。
        c.error = "--seconds 与 --frames 只能给一个（要「先到先退」请用控制命令 quit）";
        return c;
    }
    return c;
}

void SimPrintUsage() {
    fputs(
        "NextPerf 模拟游戏进程（DX11 / DX12 双后端）\n"
        "\n"
        "用法: sim.exe [--api=dx11|dx12] [--seconds=N | --frames=N] [选项...]\n"
        "\n"
        "流程控制\n"
        "  --api=dx11|dx12        图形后端（默认 dx11）\n"
        "  --seconds=N            跑 N 秒后正常退出（默认不限时）\n"
        "  --frames=N             跑 N 帧后正常退出（与 --seconds 互斥）\n"
        "  --warmup=N             前 N 帧不计入统计（默认 10，剔除首帧编译抖动）\n"
        "  --no-stdin             不读 stdin 控制命令\n"
        "\n"
        "帧率与显示\n"
        "  --fps-cap=N            锁帧到 N fps，0=不锁（默认 0）\n"
        "  --vsync=on|off         垂直同步（默认 on）\n"
        "  --width=W --height=H   初始分辨率（默认 1280x720）\n"
        "  --buffers=N            交换链 buffer 数 2..4（DX11 默认 2，DX12 默认 3）\n"
        "  --window-mode=MODE     windowed | borderless | fullscreen（默认 windowed）\n"
        "  --hidden               不显示窗口（无头 / CI 跑）\n"
        "  --hold-ms=N            每帧在 CPU 侧额外占用 N 毫秒（模拟逻辑开销）\n"
        "  --gpu-load-ms=X        每帧追加约 X 毫秒的 GPU 工作量（模拟 GPU 压力）\n"
        "  --no-tint              背景色不再逐帧变化（对照实验用）\n"
        "\n"
        "输出\n"
        "  --json                 每帧输出一行 JSON 到 stdout（诊断信息一律走 stderr）\n"
        "  --json-every=N         每 N 帧输出一行（默认 1）\n"
        "  --list-outputs         列出显示输出后就退出\n"
        "  --help                 显示本帮助\n"
        "\n"
        "运行时控制（stdin 逐行命令，详见 tests/sim/README.md）\n"
        "  resize W H             改分辨率（触发 ResizeBuffers）\n"
        "  window MODE            切窗口状态 windowed|borderless|fullscreen\n"
        "  vsync on|off           开关垂直同步\n"
        "  fpscap N               改锁帧值（0=不锁）\n"
        "  holdms N               改每帧 CPU 侧开销\n"
        "  gpuload X              改每帧 GPU 侧负载（毫秒）\n"
        "  stats                  立即输出一份汇总 JSON\n"
        "  status                 输出一行当前状态 JSON\n"
        "  mark NAME              在输出里插一条标记（对齐脚本时间轴）\n"
        "  quit                   正常退出（会输出汇总 JSON）\n",
        stdout);
    fflush(stdout);
}

// ============================================================================
// 窗口
// ============================================================================

static const wchar_t* kWndClass = L"NextPerfSimWindow";
static volatile LONG g_quit_requested = 0;

static LRESULT CALLBACK SimWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CLOSE:
            // 用户点叉 / Alt+F4：走正常退出路径（会输出汇总 JSON）。
            // 不用 DestroyWindow 立刻销毁，交给主循环统一收尾。
            g_quit_requested = 1;
            return 0;
        case WM_DESTROY:
            g_quit_requested = 1;
            PostQuitMessage(0);
            return 0;
        case WM_SYSCOMMAND:
            // 屏蔽 Alt+Enter 之外的屏保 / 显示器休眠，避免测试中途被系统打断。
            if ((w & 0xFFF0) == SC_SCREENSAVE || (w & 0xFFF0) == SC_MONITORPOWER) return 0;
            break;
        case WM_SIZE:
            // 尺寸变化只记录，不在这里重建交换链 —— 重建必须发生在渲染线程
            // 的确定时机，否则会出现「RTV 指向已释放的 backbuffer」这类崩溃。
            return 0;
        case WM_ERASEBKGND:
            return 1;  // 我们每帧自己 Clear，别让 GDI 再刷一遍
        default: break;
    }
    return DefWindowProcW(h, m, w, l);
}

void SimPrimaryMonitorSize(int* w, int* h) {
    int mw = GetSystemMetrics(SM_CXSCREEN);
    int mh = GetSystemMetrics(SM_CYSCREEN);
    if (mw <= 0) mw = 1280;
    if (mh <= 0) mh = 720;
    if (w) *w = mw;
    if (h) *h = mh;
}

HWND SimCreateWindow(HINSTANCE inst, const SimConfig& cfg, int w, int h, SimWindowMode mode) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = SimWndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWndClass;
    if (!RegisterClassExW(&wc)) {
        DWORD e = GetLastError();
        if (e != ERROR_CLASS_ALREADY_EXISTS) {
            SimLog("RegisterClassExW 失败 err=%lu", (unsigned long)e);
            return nullptr;
        }
    }

    DWORD style;
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    int cw = w, ch = h;
    if (mode == SimWindowMode::Windowed) {
        style = WS_OVERLAPPEDWINDOW;
    } else {
        // borderless 与 fullscreen 都用弹出式无边框窗口；区别在于
        // fullscreen 会再调一次 IDXGISwapChain::SetFullscreenState(TRUE)。
        style = WS_POPUP;
        int mw, mh;
        SimPrimaryMonitorSize(&mw, &mh);
        cw = mw; ch = mh;
        x = 0; y = 0;
    }

    // 窗口客户区必须正好等于交换链分辨率，否则 Present 会被 DWM 拉伸，
    // 采集到的 present 时机会失真。所以先算好带边框的外框尺寸再创建。
    RECT rc{0, 0, cw, ch};
    AdjustWindowRectEx(&rc, style, FALSE, 0);
    HWND hwnd = CreateWindowExW(0, kWndClass, L"NextPerf Sim", style, x, y,
                                rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, inst,
                                nullptr);
    if (!hwnd) {
        SimLog("CreateWindowExW 失败 err=%lu", (unsigned long)GetLastError());
        return nullptr;
    }
    // 注意：即使 --hidden，也要保持窗口「存在且尺寸有效」。flip 模型在
    // 零尺寸窗口上是不能 Present 的，所以 hidden 只是不 ShowWindow，不是不建窗口。
    if (!cfg.hidden) {
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }
    return hwnd;
}

void SimApplyWindowMode(HWND hwnd, SimWindowMode mode, int w, int h) {
    if (!hwnd) return;
    if (mode == SimWindowMode::Windowed) {
        // 从全屏回来后先恢复普通样式，再改尺寸 + 居中。
        SetWindowLongPtrW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW);
        RECT rc{0, 0, w, h};
        AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);
        int sw = rc.right - rc.left, sh = rc.bottom - rc.top;
        int sx = (GetSystemMetrics(SM_CXSCREEN) - sw) / 2;
        int sy = (GetSystemMetrics(SM_CYSCREEN) - sh) / 2;
        SetWindowPos(hwnd, HWND_TOP, sx < 0 ? 0 : sx, sy < 0 ? 0 : sy, sw, sh,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        int mw, mh;
        SimPrimaryMonitorSize(&mw, &mh);
        SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP);
        SetWindowPos(hwnd, HWND_TOP, 0, 0, mw, mh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
}

void SimFocusWindow(HWND hwnd) {
    if (!hwnd) return;
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    SetFocus(hwnd);
}

bool SimPumpMessages() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) g_quit_requested = 1;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return g_quit_requested == 0;
}

// ============================================================================
// 锁帧
// ============================================================================

void SimSpinUntil(int64_t qpc_deadline) {
    // 自旋 + SwitchToThread：SwitchToThread 会让出剩余时间片但很快回来（微秒级），
    // 比 Sleep(0)/Sleep(1)（后者实际可能睡 1~15ms）精度高得多。
    // 循环上限只是保险，防止 QPC 异常时死循环。
    while (SimQpcNow() < qpc_deadline) {
        if (!SwitchToThread()) YieldProcessor();
    }
}

void SimPacer::SetCap(int fps_cap) {
    cap_ = fps_cap < 0 ? 0 : fps_cap;
    period_ = cap_ > 0 ? SimQpcFreq() / cap_ : 0;
    next_deadline_ = 0;  // 下一帧重新锚定，避免改锁帧值时突然补一大串帧
}

void SimPacer::Reset() { next_deadline_ = 0; }

void SimPacer::WaitForFrameStart() {
    if (cap_ <= 0 || period_ <= 0) {
        // 不锁帧：什么都不等。此时帧率由 GPU（vsync）或 CPU 自己决定 ——
        // 这正是我们想观察的「无上限」场景。
        next_deadline_ = 0;
        return;
    }
    int64_t now = SimQpcNow();
    if (next_deadline_ == 0) {
        // 首帧（或刚改过锁帧值）：以「现在」为锚点，不补偿历史。
        next_deadline_ = now + period_;
        return;
    }
    // 目标锚定在固定的时间网格上（而不是「上一帧 + period」），这样
    // Sleep 的抖动不会累积成帧率漂移。
    while (next_deadline_ <= now) next_deadline_ += period_;

    int64_t remain = next_deadline_ - now;
    // 三段式：
    //   1) 还剩很多 -> Sleep 掉大头，但至少留 2ms 给后面的精细段
    //      （Sleep 的实际精度受系统时钟粒度影响，Win10+ 通常 1~15.6ms）
    //   2) 还剩 >0.5ms -> SwitchToThread 级别的让出
    //   3) 最后 0.5ms -> 纯自旋，精度到微秒
    const int64_t freq = SimQpcFreq();
    int64_t coarse = remain - freq / 500;       // 留 2ms
    if (coarse > 0) {
        DWORD ms = (DWORD)(coarse * 1000 / freq);
        if (ms > 0) Sleep(ms);
    }
    int64_t fine = next_deadline_ - freq / 2000;  // 剩 0.5ms 时转纯自旋
    SimSpinUntil(fine > SimQpcNow() ? fine : SimQpcNow());
    SimSpinUntil(next_deadline_);
}

// ============================================================================
// 控制命令
// ============================================================================

static void SplitWs(const std::string& s, std::vector<std::string>* out) {
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r')) ++i;
        size_t b = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t' && s[i] != '\r') ++i;
        if (i > b) out->push_back(s.substr(b, i - b));
    }
}

SimCommand SimParseCommand(const std::string& line) {
    SimCommand c;
    c.raw = line;
    std::vector<std::string> t;
    SplitWs(line, &t);
    if (t.empty()) return c;  // 空行：kind 保持 None，调用方忽略

    std::string k = t[0];
    for (auto& ch : k) ch = (char)tolower((unsigned char)ch);

    auto need = [&](size_t n, const char* usage) -> bool {
        if (t.size() < n + 1) { c.raw = std::string("参数不足，用法：") + usage; return false; }
        return true;
    };

    if (k == "resize" || k == "r") {
        if (!need(2, "resize W H")) return c;
        c.w = atoi(t[1].c_str());
        c.h = atoi(t[2].c_str());
        if (c.w <= 0 || c.h <= 0) { c.raw = "resize 的宽高必须是正整数"; return c; }
        c.kind = SimCommand::Resize;
    } else if (k == "window" || k == "window-mode" || k == "mode") {
        if (!need(1, "window windowed|borderless|fullscreen")) return c;
        if (!SimWindowModeParse(t[1].c_str(), &c.mode)) {
            c.raw = "window 只支持 windowed / borderless / fullscreen";
            return c;
        }
        c.kind = SimCommand::WindowMode;
    } else if (k == "vsync") {
        if (!need(1, "vsync on|off")) return c;
        bool b = false;
        if (!ParseBool(t[1].c_str(), &b)) { c.raw = "vsync 只支持 on / off"; return c; }
        c.on = b;
        c.kind = SimCommand::Vsync;
    } else if (k == "fpscap" || k == "fps-cap" || k == "cap") {
        if (!need(1, "fpscap N")) return c;
        c.n = atoi(t[1].c_str());
        if (c.n < 0) { c.raw = "fpscap 不能为负"; return c; }
        c.kind = SimCommand::FpsCap;
    } else if (k == "holdms" || k == "hold-ms") {
        if (!need(1, "holdms N")) return c;
        c.n = atoi(t[1].c_str());
        if (c.n < 0) { c.raw = "holdms 不能为负"; return c; }
        c.kind = SimCommand::HoldMs;
    } else if (k == "gpuload" || k == "gpu-load") {
        if (!need(1, "gpuload X")) return c;
        c.x = atof(t[1].c_str());
        if (c.x < 0) { c.raw = "gpuload 不能为负"; return c; }
        c.kind = SimCommand::GpuLoad;
    } else if (k == "quit" || k == "exit" || k == "q") {
        c.kind = SimCommand::Quit;
    } else if (k == "stats" || k == "dump" || k == "summary") {
        c.kind = SimCommand::Stats;
    } else if (k == "status") {
        c.kind = SimCommand::Status;
    } else if (k == "mark") {
        c.arg = t.size() >= 2 ? t[1] : std::string("mark");
        c.kind = SimCommand::Mark;
    } else if (k == "help" || k == "?") {
        c.kind = SimCommand::Help;
    } else {
        c.raw = "未知命令：" + t[0];
    }
    return c;
}

void SimApplyCommand(const SimCommand& cmd, SimControlContext& ctx) {
    switch (cmd.kind) {
        case SimCommand::Resize: {
            std::string err;
            SimLog("命令 resize %dx%d", cmd.w, cmd.h);
            if (ctx.engine->Resize(cmd.w, cmd.h, &err)) {
                *ctx.width = cmd.w;
                *ctx.height = cmd.h;
                if (ctx.resize_events) ++*ctx.resize_events;
            } else {
                SimLog("Resize 失败：%s", err.c_str());
            }
            break;
        }
        case SimCommand::WindowMode: {
            SimLog("命令 window %s", SimWindowModeName(cmd.mode));
            *ctx.mode = cmd.mode;
            int mw = 0, mh = 0;
            SimPrimaryMonitorSize(&mw, &mh);
            if (cmd.mode == SimWindowMode::Fullscreen && ctx.engine->WantsExclusiveFullscreen()) {
                // 独占全屏前必须回到 windowed：flip 模型不允许从 borderless
                // 直接跳 exclusive，中间必须经过窗口化状态。
                SimApplyWindowMode(ctx.hwnd, SimWindowMode::Windowed, *ctx.width, *ctx.height);
                SimPumpMessages();
            }
            SimApplyWindowMode(ctx.hwnd, cmd.mode, *ctx.width, *ctx.height);
            SimPumpMessages();
            if (cmd.mode != SimWindowMode::Windowed) {
                // 全屏 / 无边框都铺满桌面：分辨率跟着变，交换链也要重建尺寸。
                *ctx.width = mw;
                *ctx.height = mh;
                std::string err;
                if (!ctx.engine->Resize(mw, mh, &err)) SimLog("全屏 Resize 失败：%s", err.c_str());
            }
            ctx.engine->OnWindowModeChanged(cmd.mode);
            SimFocusWindow(ctx.hwnd);
            SimPumpMessages();
            SimLog("当前分辨率 %dx%d 窗口状态 %s", *ctx.width, *ctx.height,
                   SimWindowModeName(*ctx.mode));
            break;
        }
        case SimCommand::Vsync:
            SimLog("命令 vsync %s", cmd.on ? "on" : "off");
            ctx.cfg->vsync = cmd.on;
            ctx.engine->SetVsync(cmd.on);
            break;
        case SimCommand::FpsCap:
            SimLog("命令 fpscap %d", cmd.n);
            ctx.cfg->fps_cap = cmd.n;
            ctx.pacer->SetCap(cmd.n);
            break;
        case SimCommand::HoldMs:
            SimLog("命令 holdms %d", cmd.n);
            ctx.cfg->hold_ms = cmd.n;
            break;
        case SimCommand::GpuLoad:
            SimLog("命令 gpuload %.3f ms", cmd.x);
            ctx.cfg->gpu_load_ms = cmd.x;
            ctx.engine->SetGpuLoadMs(cmd.x);
            break;
        case SimCommand::Quit:
            SimLog("命令 quit");
            *ctx.quit = true;
            break;
        case SimCommand::Stats:
            if (ctx.stats_requested) *ctx.stats_requested = true;
            break;
        case SimCommand::Status: {
            // 状态行也走 stdout 的 JSON，脚本可以随时问「你现在什么设置」。
            SimLogRaw("%s", SimStatusJson(*ctx.cfg, ctx.frame_index, *ctx.width, *ctx.height,
                                          *ctx.mode, ctx.cfg->vsync, ctx.cfg->fps_cap, 0.0)
                                 .c_str());
            break;
        }
        case SimCommand::Mark:
            SimLog("标记 %s", cmd.arg.c_str());
            if (ctx.cfg->json) {
                // 标记也打成一行 JSON，好让脚本把事件和帧号对上。
                SimLogRaw("{\"type\":\"mark\",\"frame\":%lld,\"name\":\"%s\"}",
                          (long long)ctx.frame_index, cmd.arg.c_str());
            }
            break;
        case SimCommand::Help:
            fputs(
                "命令：resize W H | window windowed|borderless|fullscreen | vsync on|off |\n"
                "      fpscap N | holdms N | gpuload X | stats | status | mark NAME | quit\n",
                stdout);
            fflush(stdout);
            break;
        case SimCommand::None:
        default:
            if (!cmd.raw.empty()) SimLog("命令解析失败：%s", cmd.raw.c_str());
            break;
    }
}

// ---------------------------------------------------------------- stdin 线程

static SimStdinThread* g_stdin_self = nullptr;

bool SimStdinThread::Start() {
    if (started) return true;
    InitializeCriticalSection(&cs);
    g_stdin_self = this;
    started = true;
    // 单独一条线程阻塞读 stdin。渲染线程不碰 ReadFile，避免被卡住 ——
    // 一旦渲染线程卡在 I/O 上，帧时间统计就全废了。
    thread = CreateThread(nullptr, 0, &SimStdinThread::Proc, this, 0, nullptr);
    if (!thread) {
        SimLog("创建 stdin 线程失败 err=%lu", (unsigned long)GetLastError());
        return false;
    }
    return true;
}

void SimStdinThread::Stop() {
    if (!started) return;
    InterlockedExchange(&quit, 1);
    if (thread) {
        // 不强行 TerminateThread：读线程可能正卡在 ReadFile 上，
        // 强行杀会泄漏 stdin 句柄并且行为未定义。等它自己因为 EOF 退出，
        // 超时了也只是「有个人还在等输入」，进程退出时内核会收拾。
        WaitForSingleObject(thread, 300);
        CloseHandle(thread);
        thread = nullptr;
    }
    DeleteCriticalSection(&cs);
    started = false;
    g_stdin_self = nullptr;
}

void SimStdinThread::Drain(std::vector<SimCommand>* out) {
    if (!started) return;
    EnterCriticalSection(&cs);
    if (!queue.empty()) {
        out->insert(out->end(), queue.begin(), queue.end());
        queue.clear();
    }
    LeaveCriticalSection(&cs);
}

DWORD WINAPI SimStdinThread::Proc(LPVOID param) {
    SimStdinThread* self = (SimStdinThread*)param;
    // 用 CRT 的 fgets 读 stdin 而不是 ReadFile(GetStdHandle)：CRT 已经把
    // 重定向 / 控制台的差异处理好了，Windows 上也按文本模式读整行。
    char line[1024];
    while (InterlockedCompareExchange(&self->quit, 0, 0) == 0) {
        if (!fgets(line, sizeof(line), stdin)) break;  // EOF：stdin 关了，正常结束
        std::string s(line);
        // fgets 会把超长行的剩余部分留到下一次读，这里先不管（命令都很短）。
        SimCommand c = SimParseCommand(s);
        if (c.kind == SimCommand::None && c.raw.empty()) continue;  // 空行
        EnterCriticalSection(&self->cs);
        self->queue.push_back(c);
        LeaveCriticalSection(&self->cs);
    }
    return 0;
}

// ============================================================================
// 统计
// ============================================================================

// 最近秩分位数（升序序列，p 取 0..100）。
static double Percentile(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    if (sorted.size() == 1) return sorted[0];
    double rank = (p / 100.0) * (double)sorted.size();
    int64_t idx = (int64_t)ceil(rank) - 1;
    if (idx < 0) idx = 0;
    if (idx >= (int64_t)sorted.size()) idx = (int64_t)sorted.size() - 1;
    return sorted[(size_t)idx];
}

// 最差 tail_pct 比例帧的平均帧时间 -> FPS。这就是 1% / 0.1% Low 的算法。
static double LowFps(const std::vector<double>& sorted, double tail_pct, int64_t* count_out) {
    if (sorted.empty()) {
        if (count_out) *count_out = 0;
        return 0.0;
    }
    int64_t n = (int64_t)sorted.size();
    int64_t take = (int64_t)ceil((tail_pct / 100.0) * (double)n);
    if (take < 1) take = 1;
    if (take > n) take = n;
    double sum = 0.0;
    for (int64_t i = n - take; i < n; ++i) sum += sorted[(size_t)i];
    if (count_out) *count_out = take;
    double avg = sum / (double)take;
    return avg > 0.0 ? 1000.0 / avg : 0.0;
}

SimStats SimComputeStats(const std::vector<double>& frame_ms_in,
                         const std::vector<double>& cpu_ms_in,
                         const std::vector<double>& gpu_ms_in,
                         const std::vector<double>& present_ms_in, bool gpu_available,
                         double gpu_freq_hz, const char* gpu_freq_source, double first_t_ms) {
    SimStats st;
    st.gpu_available = gpu_available;
    st.gpu_freq_hz = gpu_freq_hz;
    st.gpu_freq_source = gpu_freq_source ? gpu_freq_source : "none";
    st.frames = (int64_t)frame_ms_in.size();
    if (frame_ms_in.empty()) return st;

    std::vector<double> v(frame_ms_in);
    std::sort(v.begin(), v.end());

    double sum = 0.0;
    for (double x : v) sum += x;
    st.frame_ms_avg = sum / (double)v.size();
    st.frame_ms_min = v.front();
    st.frame_ms_max = v.back();

    // 帧时间并不是「每帧固定 16.67ms」——用总时间 / 帧数算平均帧率比
    // 「1000 / 平均帧时间」更贴近实际观感（后者会被长帧放大）。
    double span = 0.0;
    for (double x : frame_ms_in) span += x;
    st.elapsed_ms = span;
    st.fps_avg = span > 0.0 ? (double)frame_ms_in.size() * 1000.0 / span : 0.0;

    double var = 0.0;
    for (double x : v) var += (x - st.frame_ms_avg) * (x - st.frame_ms_avg);
    st.frame_ms_stddev = sqrt(var / (double)v.size());

    st.p50_ms = Percentile(v, 50.0);
    st.p95_ms = Percentile(v, 95.0);
    st.p99_ms = Percentile(v, 99.0);
    st.p999_ms = Percentile(v, 99.9);

    st.fps_1pct_low = LowFps(v, 1.0, &st.low1_count);
    st.fps_01pct_low = LowFps(v, 0.1, &st.low01_count);

    if (!cpu_ms_in.empty()) {
        double s = 0.0;
        for (double x : cpu_ms_in) s += x;
        st.cpu_ms_avg = s / (double)cpu_ms_in.size();
    }
    if (!present_ms_in.empty()) {
        double s = 0.0;
        for (double x : present_ms_in) s += x;
        st.present_ms_avg = s / (double)present_ms_in.size();
    }
    st.gpu_valid_frames = (int64_t)gpu_ms_in.size();
    if (!gpu_ms_in.empty()) {
        double s = 0.0;
        for (double x : gpu_ms_in) s += x;
        st.gpu_ms_avg = s / (double)gpu_ms_in.size();
    }

    // 1 秒滑动窗口的最好 / 最差帧率：用来发现「平均 60 但偶尔掉到 20」这类
    // 平均帧率完全看不出来的问题。
    {
        const double kWindow = 1000.0;
        size_t lo = 0;
        double acc = 0.0;
        double best = 0.0, worst = 1e30;
        bool any = false;
        for (size_t hi = 0; hi < frame_ms_in.size(); ++hi) {
            acc += frame_ms_in[hi];
            // 窗口累积时间超过 1 秒就前移左边界（保持 acc <= ~1s + 一帧）
            while (acc - frame_ms_in[lo] >= kWindow && lo < hi) {
                acc -= frame_ms_in[lo];
                ++lo;
            }
            if (acc >= kWindow * 0.9 && hi + 1 - lo >= 2) {
                double fps = (double)(hi + 1 - lo) * 1000.0 / acc;
                if (fps > best) best = fps;
                if (fps < worst) worst = fps;
                any = true;
            }
        }
        if (any) {
            st.fps_best_1s = best;
            st.fps_worst_1s = worst;
        }
    }
    (void)first_t_ms;
    return st;
}

// ============================================================================
// JSON
// ============================================================================

void SimJsonEscapeTo(std::string& out, const char* s) {
    if (!s) return;
    for (const char* p = s; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof(b), "\\u%04x", c);
                    out += b;
                } else {
                    out += (char)c;
                }
        }
    }
}

// 统一的数字格式：固定 4 位小数够表示微秒级差异，又不会让 JSON 变得很长。
static void JNum(std::string& o, const char* k, double v, int prec = 4) {
    char b[64];
    if (!(v == v) || v > 1e18 || v < -1e18) {  // NaN / Inf 会让 JSON 解析器炸掉
        snprintf(b, sizeof(b), "null");
    } else {
        snprintf(b, sizeof(b), "%.*f", prec, v);
    }
    o += "\"";
    o += k;
    o += "\":";
    o += b;
    o += ",";
}

static void JInt(std::string& o, const char* k, long long v) {
    char b[48];
    snprintf(b, sizeof(b), "%lld", v);
    o += "\"";
    o += k;
    o += "\":";
    o += b;
    o += ",";
}

static void JBool(std::string& o, const char* k, bool v) {
    o += "\"";
    o += k;
    o += "\":";
    o += v ? "true" : "false";
    o += ",";
}

static void JStr(std::string& o, const char* k, const char* v) {
    o += "\"";
    o += k;
    o += "\":\"";
    SimJsonEscapeTo(o, v ? v : "");
    o += "\",";
}

std::string SimFrameJson(const SimFrameReport& r, const SimConfig& cfg) {
    std::string o;
    o.reserve(720);
    o += "{\"type\":\"frame\",";
    JInt(o, "frame", (long long)r.frame_index);
    JStr(o, "api", cfg.use_dx12 ? "dx12" : "dx11");
    JNum(o, "t_ms", r.t_start_ms);
    JNum(o, "frame_delta_ms", r.frame_delta_ms);
    JNum(o, "cpu_frame_ms", r.cpu_frame_ms);
    JNum(o, "cpu_render_ms", r.cpu_render_ms);
    if (r.gpu_time_valid) {
        JNum(o, "gpu_frame_ms", r.gpu_frame_ms);
    } else {
        // 明确写 null 而不是 0：0 会被下游误当成「GPU 一帧没花时间」。
        o += "\"gpu_frame_ms\":null,";
    }
    JBool(o, "gpu_valid", r.gpu_time_valid);
    JNum(o, "present_ms", r.present_ms);
    JBool(o, "vsync", r.vsync);
    JInt(o, "fps_cap", r.fps_cap);
    JInt(o, "width", r.width);
    JInt(o, "height", r.height);
    JStr(o, "window_mode", SimWindowModeName(r.window_mode));
    JBool(o, "hidden", r.hidden);
    JInt(o, "swapchain_generation", r.swapchain_generation);
    JBool(o, "resize_event", r.resize_event);
    JBool(o, "present_failed", r.present_failed);
    JInt(o, "present_hr", r.present_hr);
    if (o.back() == ',') o.pop_back();
    o += "}";
    return o;
}

std::string SimStatusJson(const SimConfig& cfg, int64_t frame_index, int w, int h,
                          SimWindowMode mode, bool vsync, int fps_cap, double fps_recent) {
    std::string o;
    o += "{\"type\":\"status\",";
    JInt(o, "frame", (long long)frame_index);
    JStr(o, "api", cfg.use_dx12 ? "dx12" : "dx11");
    JInt(o, "width", w);
    JInt(o, "height", h);
    JStr(o, "window_mode", SimWindowModeName(mode));
    JBool(o, "vsync", vsync);
    JInt(o, "fps_cap", fps_cap);
    JNum(o, "fps_recent", fps_recent, 2);
    JBool(o, "hidden", cfg.hidden);
    if (o.back() == ',') o.pop_back();
    o += "}";
    return o;
}

std::string SimSummaryJson(const SimConfig& cfg, const SimStats& st, int64_t total_frames,
                           int swapchain_generation, int64_t resize_events,
                           const char* exit_reason, const char* backend_note) {
    std::string o;
    o.reserve(2048);
    o += "{\"type\":\"summary\",";
    JStr(o, "api", cfg.use_dx12 ? "dx12" : "dx11");
    JStr(o, "exit_reason", exit_reason ? exit_reason : "unknown");
    JInt(o, "total_frames", (long long)total_frames);
    JInt(o, "counted_frames", (long long)st.frames);
    JInt(o, "warmup_frames", cfg.warmup_frames);
    JInt(o, "swapchain_generation", swapchain_generation);
    JInt(o, "resize_events", (long long)resize_events);
    JNum(o, "elapsed_ms", st.elapsed_ms, 3);

    JNum(o, "fps_avg", st.fps_avg, 3);
    JNum(o, "frame_ms_avg", st.frame_ms_avg);
    JNum(o, "frame_ms_min", st.frame_ms_min);
    JNum(o, "frame_ms_max", st.frame_ms_max);
    JNum(o, "frame_ms_stddev", st.frame_ms_stddev);
    JNum(o, "frame_ms_p50", st.p50_ms);
    JNum(o, "frame_ms_p95", st.p95_ms);
    JNum(o, "frame_ms_p99", st.p99_ms);
    JNum(o, "frame_ms_p999", st.p999_ms);
    // 1% / 0.1% Low：对帧时间取 P99 / P99.9 再换算，即 1000/P99。
    JNum(o, "fps_1pct_low", st.fps_1pct_low, 3);
    JNum(o, "fps_01pct_low", st.fps_01pct_low, 3);
    JInt(o, "low1_frame_count", (long long)st.low1_count);
    JInt(o, "low01_frame_count", (long long)st.low01_count);
    JNum(o, "fps_best_1s", st.fps_best_1s, 3);
    JNum(o, "fps_worst_1s", st.fps_worst_1s, 3);

    JNum(o, "cpu_frame_ms_avg", st.cpu_ms_avg);
    JNum(o, "present_ms_avg", st.present_ms_avg);
    JNum(o, "gpu_frame_ms_avg", st.gpu_ms_avg);
    JInt(o, "gpu_valid_frames", (long long)st.gpu_valid_frames);
    JBool(o, "gpu_time_available", st.gpu_available);
    JNum(o, "gpu_timestamp_freq_hz", st.gpu_freq_hz, 1);
    JStr(o, "gpu_timestamp_freq_source", st.gpu_freq_source);

    // 结束时的设置快照：方便脚本确认「测试期间到底改没改过状态」。
    JInt(o, "final_width", cfg.width);
    JInt(o, "final_height", cfg.height);
    JStr(o, "final_window_mode", SimWindowModeName(cfg.window_mode));
    JBool(o, "final_vsync", cfg.vsync);
    JInt(o, "final_fps_cap", cfg.fps_cap);
    JBool(o, "hidden", cfg.hidden);
    JStr(o, "backend_note", backend_note ? backend_note : "");
    if (o.back() == ',') o.pop_back();
    o += "}";
    return o;
}
