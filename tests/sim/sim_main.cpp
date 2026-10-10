// ============================================================================
//  NextPerf 模拟游戏进程 —— 入口
// ============================================================================
//
//  用法：sim.exe --api=dx11|dx12 [--seconds=N | --frames=N] [选项...]
//        详见 --help 与 tests/sim/README.md
//
//  两个刻意的工程决定：
//
//  1) 用 wWinMain + -Wl,--subsystem:windows，但显式接管标准流
//      项目其它可执行文件都是 GUI 子系统（双击不弹黑框）。模拟器要兼顾
//      「无头 CI（stdout/stdin 被 Python 重定向）」和「手动观察画面」两种用法：
//        * 被重定向时：把 CRT 的 fd 0/1/2 绑到父进程给的管道/文件句柄上，
//          父进程能实时收到 JSON 行，也能往 stdin 写控制命令；
//        * 没被重定向时：AttachConsole(ATTACH_PARENT_PROCESS) 借父进程的控制台，
//          手敲 sim.exe --json 就能在同一个窗口里直接看到输出。
//      一份二进制同时满足两种场景，不用编两版。
//
//  2) 控制命令只在渲染线程执行
//      stdin 由后台线程读，只做「解析 + 入队」；真正的 ResizeBuffers /
//      SetFullscreenState / Present 参数改动一律在渲染线程的帧间隙做。
//      DXGI 与窗口都不是线程安全的，跨线程改设置会得到极难查的偶发崩溃。
// ============================================================================

#include "sim.h"

#include <dxgi1_2.h>   // IDXGIFactory1 / IDXGIAdapter1 / IDXGIOutput / DXGI_ADAPTER_DESC1
#include <shellapi.h>  // CommandLineToArgvW
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <string>
#include <vector>

// 把 CRT 的标准流接到正确的 OS 句柄上。返回 true 表示 stdin 可用（可以起读线程）。
static bool SetupStdio() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    auto usable = [](HANDLE h) {
        return h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN;
    };

    bool out_redirected = usable(hOut) && (GetFileType(hOut) == FILE_TYPE_PIPE ||
                                           GetFileType(hOut) == FILE_TYPE_DISK);
    bool in_usable = usable(hIn);

    if (out_redirected) {
        // 已经被父进程重定向：显式把 fd 1/2 绑到那个句柄上。
        // 只 freopen("CONOUT$") 是不够的（那反而会把重定向覆盖掉）。
        int fd = _open_osfhandle((intptr_t)hOut, _O_TEXT | _O_WRONLY);
        if (fd >= 0) {
            _dup2(fd, 1);
            _dup2(fd, 2);
        }
    } else {
        // 没被重定向：借父进程的控制台（手敲场景）。
        // AttachConsole 失败是正常的（比如从资源管理器双击），不是错误。
        if (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole()) {
            FILE* f = nullptr;
            freopen_s(&f, "CONOUT$", "w", stdout);
            freopen_s(&f, "CONOUT$", "w", stderr);
            if (!in_usable) {
                freopen_s(&f, "CONIN$", "r", stdin);
                in_usable = true;
            }
        }
    }
    if (in_usable) {
        int fd = _open_osfhandle((intptr_t)hIn, _O_TEXT | _O_RDONLY);
        if (fd >= 0) _dup2(fd, 0);
    }
    // 不缓冲：JSON 行必须实时出去，否则脚本要等进程结束才拿到数据，
    // 「运行中对照」就无从谈起。
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    return in_usable;
}

// ---------------------------------------------------------------- 显示输出枚举

static void ListOutputs() {
    IDXGIFactory1* fac = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac))) || !fac) {
        fprintf(stderr, "CreateDXGIFactory1 失败\n");
        return;
    }
    for (UINT ai = 0;; ++ai) {
        IDXGIAdapter1* ad = nullptr;
        if (fac->EnumAdapters1(ai, &ad) != S_OK || !ad) break;
        DXGI_ADAPTER_DESC1 desc{};
        ad->GetDesc1(&desc);
        char name[160] = {0};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name) - 1, nullptr,
                            nullptr);
        printf("adapter %u: %s  vram=%lluMB%s\n", ai, name,
               (unsigned long long)(desc.DedicatedVideoMemory / (1024 * 1024)),
               (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? "  [software]" : "");
        for (UINT oi = 0;; ++oi) {
            IDXGIOutput* out = nullptr;
            if (ad->EnumOutputs(oi, &out) != S_OK || !out) break;
            DXGI_OUTPUT_DESC od{};
            out->GetDesc(&od);
            char oname[64] = {0};
            WideCharToMultiByte(CP_UTF8, 0, od.DeviceName, -1, oname, sizeof(oname) - 1, nullptr,
                                nullptr);
            printf("  output %u: %s  desktop=%ldx%ld  attached=%d\n", oi, oname,
                   (long)(od.DesktopCoordinates.right - od.DesktopCoordinates.left),
                   (long)(od.DesktopCoordinates.bottom - od.DesktopCoordinates.top),
                   (int)od.AttachedToDesktop);
            out->Release();
        }
        ad->Release();
    }
    fac->Release();
    // 刷新率不在这里报：DXGI_OUTPUT_DESC 里没有刷新率字段，要枚举
    // DXGI_MODE_DESC 才知道。这里如实只报像素尺寸，不猜。
    printf("primary screen: %dx%d (px)\n", GetSystemMetrics(SM_CXSCREEN),
           GetSystemMetrics(SM_CYSCREEN));
}

// ---------------------------------------------------------------- 主流程

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {
    // 注意这里用的是 WinMain（LPSTR）而不是 wWinMain：
    //   zig 自带的 mingw crt（crtexewin.c）只定义了 WinMain 入口，
    //   用 wWinMain 会链接失败（undefined symbol: WinMain）。
    //   反正参数要重新解析（WinMain 的 lpCmdLine 对引号处理不好），
    //   所以这个 LPSTR 直接忽略，改用 GetCommandLineW + CommandLineToArgvW：
    //   Python 传带空格的路径是常态，只有宽字符版本能正确处理。
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> storage;
    std::vector<char*> argv;
    for (int i = 0; i < argc; ++i) {
        char buf[4096] = {0};
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, buf, sizeof(buf) - 1, nullptr, nullptr);
        storage.push_back(buf);
    }
    if (wargv) LocalFree(wargv);
    for (auto& s : storage) argv.push_back(const_cast<char*>(s.c_str()));

    bool stdin_usable = SetupStdio();

    // --dump-argv：把解析后的原始参数逐条打到 stderr。排查「脚本传参被
    // Windows / shell 改写」这类问题（引号、空格、编码）非常有用。
    // 这里直接扫原始 argv，所以即使 SimParseArgs 不认这个参数也能生效。
    bool dump_argv = false;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (strcmp(argv[i], "--dump-argv") == 0) dump_argv = true;
    }
    if (dump_argv) {
        fprintf(stderr, "[sim] argc=%llu\n", (unsigned long long)argv.size());
        for (size_t i = 0; i < argv.size(); ++i) {
            fprintf(stderr, "[sim]   argv[%llu] = <%s>\n", (unsigned long long)i, argv[i]);
        }
    }

    SimConfig cfg = SimParseArgs((int)argv.size(), argv.data());
    if (cfg.error == "__help__") {
        SimPrintUsage();
        return 0;
    }
    if (!cfg.error.empty()) {
        fprintf(stderr, "[sim] 参数错误：%s\n\n", cfg.error.c_str());
        SimPrintUsage();
        return 2;
    }
    if (cfg.list_outputs) {
        ListOutputs();
        return 0;
    }

    // 窗口先建：交换链创建需要 HWND。
    // --hidden 只是不 ShowWindow，窗口本身必须存在且客户区尺寸有效 ——
    // flip 模型的交换链在零尺寸窗口上不能 Present。
    HWND hwnd = SimCreateWindow(inst, cfg, cfg.width, cfg.height, cfg.window_mode);
    if (!hwnd) {
        fprintf(stderr, "[sim] 创建窗口失败\n");
        return 1;
    }
    SimApplyWindowMode(hwnd, cfg.window_mode, cfg.width, cfg.height);

    SimEngine* engine = cfg.use_dx12 ? SimCreateEngineDx12() : SimCreateEngineDx11();
    std::string err;
    if (!engine->Init(hwnd, cfg, &err)) {
        fprintf(stderr, "[sim] %s 初始化失败：%s\n", engine->BackendName(), err.c_str());
        delete engine;
        DestroyWindow(hwnd);
        return 1;
    }
    if (cfg.buffers == 0) cfg.buffers = engine->BufferCount();  // 记下实际值，写进汇总

    int width = cfg.width, height = cfg.height;
    SimWindowMode mode = cfg.window_mode;

    // 控制通道：stdin 逐行命令。后台线程只读 + 解析 + 入队。
    SimStdinThread stdin_thread;
    if (!cfg.no_stdin) stdin_thread.Start();

    SimPacer pacer;
    pacer.SetCap(cfg.fps_cap);
    pacer.Reset();

    // 统计序列（不含 warmup）。上限 200 万帧 ≈ 9 小时 @60fps，够用；
    // 超了就只计数不再累积，避免长时间无人值守跑测试把内存吃光。
    const size_t kMaxSamples = 2000000;
    std::vector<double> vec_frame_ms, vec_cpu_ms, vec_gpu_ms, vec_present_ms;
    vec_frame_ms.reserve(1 << 16);
    vec_cpu_ms.reserve(1 << 16);
    vec_gpu_ms.reserve(1 << 16);
    vec_present_ms.reserve(1 << 16);

    int64_t total_frames = 0;
    int64_t resize_events = 0;
    int64_t present_failures = 0;
    double prev_start_ms = 0.0;
    const ULONGLONG t0_ticks = GetTickCount64();
    const int64_t t0_qpc = SimQpcNow();  // 所有 t_ms 的 0 点
    bool quit = false;
    std::string exit_reason = "unknown";
    bool stats_capped = false;

    SimLog("启动：api=%s pid=%lu 退出条件=%s 锁帧=%d vsync=%s 初始 %dx%d %s%s",
           engine->BackendName(), (unsigned long)GetCurrentProcessId(),
           cfg.seconds > 0 ? (std::to_string(cfg.seconds) + " 秒").c_str()
                           : (cfg.frames > 0 ? (std::to_string((long long)cfg.frames) + " 帧").c_str()
                                             : "仅靠控制命令/关窗"),
           cfg.fps_cap, cfg.vsync ? "on" : "off", width, height, SimWindowModeName(mode),
           cfg.hidden ? " [hidden]" : "");
    SimLog("控制通道：往 stdin 逐行写命令（resize/window/vsync/fpscap/stats/status/quit）");

    while (!quit) {
        // 1) 窗口消息（关窗、尺寸变化等）
        if (!SimPumpMessages()) {
            exit_reason = "window_closed";
            break;
        }

        // 2) 锁帧：按 QPC 等到本帧的目标开始时刻
        pacer.WaitForFrameStart();

        // 3) 处理控制命令（只在渲染线程、只在帧间隙做）
        {
            std::vector<SimCommand> cmds;
            stdin_thread.Drain(&cmds);
            bool stats_requested = false;
            for (const SimCommand& c : cmds) {
                SimControlContext ctx;
                ctx.hwnd = hwnd;
                ctx.cfg = &cfg;
                ctx.engine = engine;
                ctx.pacer = &pacer;
                ctx.width = &width;
                ctx.height = &height;
                ctx.mode = &mode;
                ctx.quit = &quit;
                ctx.stats_requested = &stats_requested;
                ctx.frame_index = total_frames;
                ctx.resize_events = &resize_events;
                SimApplyCommand(c, ctx);
                if (quit) break;
            }
            if (stats_requested && !quit) {
                // 「stats」命令：随时给一份汇总，脚本可以分段取数。
                // 区间是「从开始到现在」，不是增量。
                SimStats st = SimComputeStats(vec_frame_ms, vec_cpu_ms, vec_gpu_ms, vec_present_ms,
                                              engine->GpuTimeAvailable(),
                                              engine->GpuTimestampFreqHz(),
                                              engine->GpuTimestampFreqSource(), 0.0);
                SimLogRaw("%s",
                          SimSummaryJson(cfg, st, total_frames, engine->SwapchainGeneration(),
                                         resize_events, "on_demand_stats", engine->Note())
                              .c_str());
            }
        }
        if (quit) {
            exit_reason = "command_quit";
            break;
        }

        // 4) 一帧
        SimFrameReport r;
        r.frame_index = total_frames;
        r.vsync = cfg.vsync;
        r.fps_cap = cfg.fps_cap;
        r.width = width;
        r.height = height;
        r.window_mode = mode;
        r.hidden = cfg.hidden;
        r.swapchain_generation = engine->SwapchainGeneration();

        const int64_t f0 = SimQpcNow();
        const double f0_ms = SimQpcToMs(f0 - t0_qpc);
        const int gen_before = engine->SwapchainGeneration();

        engine->BeginFrame(total_frames);

        // --hold-ms：模拟 CPU 侧的逻辑开销（游戏逻辑 / 动画 / 物理）。
        // 放在 BeginFrame 之后、EndFrame 之前，正好计入本帧 CPU 时间。
        if (cfg.hold_ms > 0) {
            int64_t hold_end = SimQpcNow() + (int64_t)cfg.hold_ms * SimQpcFreq() / 1000;
            SimSpinUntil(hold_end);
        }

        engine->EndFrame(r);

        const int64_t f1 = SimQpcNow();
        r.t_start_ms = f0_ms;
        r.frame_delta_ms = total_frames > 0 ? (f0_ms - prev_start_ms) : 0.0;
        r.cpu_frame_ms = SimQpcToMs(f1 - f0);
        r.cpu_render_ms = r.cpu_frame_ms;  // 单线程模拟：录制+提交就是整段 CPU 开销
        r.swapchain_generation = engine->SwapchainGeneration();
        r.resize_event = r.swapchain_generation != gen_before;
        if (r.resize_event) ++resize_events;
        if (r.present_failed) ++present_failures;

        // GPU 时间戳是异步回读的，这一帧可能还拿不到 —— 拿不到就标 invalid，
        // 绝不用 0 冒充「GPU 没花时间」。
        {
            double gms = 0.0;
            r.gpu_time_valid = engine->TakeLastGpuMs(&gms);
            r.gpu_frame_ms = r.gpu_time_valid ? gms : 0.0;
        }

        prev_start_ms = f0_ms;
        ++total_frames;

        // 5) 输出
        if (cfg.json && (total_frames % cfg.json_every == 0)) {
            SimLogRaw("%s", SimFrameJson(r, cfg).c_str());
        }

        // 6) 统计（跳过 warmup）
        if (total_frames > cfg.warmup_frames && !stats_capped) {
            if (r.frame_delta_ms > 0.0) {
                vec_frame_ms.push_back(r.frame_delta_ms);
                vec_cpu_ms.push_back(r.cpu_frame_ms);
                vec_present_ms.push_back(r.present_ms);
            }
            if (r.gpu_time_valid) vec_gpu_ms.push_back(r.gpu_frame_ms);
            if (vec_frame_ms.size() >= kMaxSamples) {
                stats_capped = true;
                SimLog("注意：统计样本已达上限 %llu 帧，之后只计帧数不再累积分位数样本",
                       (unsigned long long)kMaxSamples);
            }
        }

        // 7) 退出条件
        if (cfg.frames > 0 && total_frames >= cfg.frames) {
            exit_reason = "frame_limit";
            break;
        }
        if (cfg.seconds > 0 && GetTickCount64() - t0_ticks >= (ULONGLONG)cfg.seconds * 1000ULL) {
            exit_reason = "time_limit";
            break;
        }
    }

    stdin_thread.Stop();

    // 结束前把主循环之外的最后一点输入读掉（比如脚本在退出瞬间又写了命令）。
    SimPumpMessages();

    SimStats st =
        SimComputeStats(vec_frame_ms, vec_cpu_ms, vec_gpu_ms, vec_present_ms,
                        engine->GpuTimeAvailable(), engine->GpuTimestampFreqHz(),
                        engine->GpuTimestampFreqSource(), 0.0);

    // 汇总 JSON 永远输出（不管有没有 --json）——这是自报数据的核心交付物。
    fprintf(stdout, "%s\n", SimSummaryJson(cfg, st, total_frames, engine->SwapchainGeneration(),
                                           resize_events, exit_reason.c_str(), engine->Note())
                                .c_str());
    fflush(stdout);

    // 人类可读的摘要走 stderr，手跑时一眼能看，也不会污染 stdout 的纯 JSON。
    fprintf(stderr,
            "[sim] 结束(%s)：共 %lld 帧，计入统计 %lld 帧\n"
            "[sim]   平均帧率 %.2f fps   帧时间 avg %.3f / P50 %.3f / P99 %.3f / P99.9 %.3f ms\n"
            "[sim]   1%% Low %.2f fps   0.1%% Low %.2f fps\n"
            "[sim]   CPU 帧时间 avg %.3f ms   Present 阻塞 avg %.3f ms   GPU avg %.3f ms"
            "（%lld 帧有样本，频率来源 %s）\n"
            "[sim]   交换链重建 %lld 次，Present 失败 %lld 次，最终代数 %d\n",
            exit_reason.c_str(), (long long)total_frames, (long long)st.frames, st.fps_avg,
            st.frame_ms_avg, st.p50_ms, st.p99_ms, st.p999_ms, st.fps_1pct_low, st.fps_01pct_low,
            st.cpu_ms_avg, st.present_ms_avg, st.gpu_ms_avg, (long long)st.gpu_valid_frames,
            st.gpu_freq_source, (long long)resize_events, (long long)present_failures,
            engine->SwapchainGeneration());

    delete engine;
    if (hwnd && IsWindow(hwnd)) DestroyWindow(hwnd);
    return 0;
}
