// NextPerf 主程序入口
//
// 职责：
//   * 传感器采集（NVML / NVAPI / ADL / HWiNFO / PDH / WMI，自动优选）
//   * 把配置和传感器写进共享内存，供注入钩子读取
//   * 读回钩子写出的遥测（帧时间 / 帧延迟 / RT、Tensor 推断结果）
//   * 桌面分层叠加窗口 + 系统托盘 + 进程注入看护

#include "np_app.h"

#include <shellapi.h>
#include <algorithm>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "common/np_bitmap.h"
#include "common/np_build.h"
#include "common/np_panel.h"
#include "etw/np_etw.h"

namespace npa {

AppState gApp;

static HINSTANCE gInst = nullptr;
static bool gTrayAdded = false;
static bool gTrayOk = false;       // 托盘图标是否真的注册成功（失败时关闭按钮=退出）
static bool gOverlayOff = false;   // 托盘里的「快速关闭叠加」开关
static bool gWantQuit = false;     // TrackPopupMenu 的模态循环会吃掉 WM_QUIT，菜单关闭后需补发
static NOTIFYICONDATAW gNid{};

// ---------------------------------------------------------------- 共享内存
static bool CreateShm() {
    gApp.mapCfg = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                     sizeof(NPConfig), NP_SHM_CONFIG);
    if (gApp.mapCfg)
        gApp.shmCfg = (NPConfig*)MapViewOfFile(gApp.mapCfg, FILE_MAP_ALL_ACCESS, 0, 0,
                                               sizeof(NPConfig));
    gApp.mapSens = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      sizeof(NPSensors), NP_SHM_SENSORS);
    if (gApp.mapSens)
        gApp.shmSens = (NPSensors*)MapViewOfFile(gApp.mapSens, FILE_MAP_ALL_ACCESS, 0, 0,
                                                 sizeof(NPSensors));
    return gApp.shmCfg != nullptr;
}

// 钩子 DLL 按自己的 PID 建遥测块，主程序这边按 PID 去开。
// 同时注入多个游戏时，每个进程一份，最后取「最新鲜」的那份当当前数据。
static void TryOpenTelemetry() {
    for (DWORD pid : gApp.injected) {
        if (!pid) continue;
        bool have = false;
        for (auto& s : gApp.telSlots) if (s.pid == pid) have = true;
        if (have) continue;

        wchar_t name[64]{};
        NPTelemetryShmName(pid, name, 64);
        HANDLE m = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
        if (!m) continue;
        NPTelemetry* v = (NPTelemetry*)MapViewOfFile(m, FILE_MAP_READ, 0, 0, sizeof(NPTelemetry));
        if (!v || v->magic != NP_MAGIC) {
            if (v) UnmapViewOfFile(v);
            CloseHandle(m);
            continue;
        }
        AppState::TelSlot s;
        s.pid = pid;
        s.map = m;
        s.view = v;
        s.lastTick = v->tickMs;
        gApp.telSlots.push_back(s);
    }
}

// 从所有已打开的遥测块里挑最新鲜的一份
static void PickTelemetry() {
    AppState::TelSlot* best = nullptr;
    for (auto& s : gApp.telSlots) {
        if (!s.view || s.view->magic != NP_MAGIC) continue;
        s.lastTick = s.view->tickMs;
        if (!best || s.lastTick > best->lastTick) best = &s;
    }
    if (!best || best->lastTick == 0) return;
    gApp.telemetry = *best->view;
    gApp.telemetryPid = best->pid;
}

void AppPollSensors() {
    gApp.hub.Poll(gApp.sensors);
    // 显示模式走系统接口（EnumDisplaySettings）—— 必须在 Poll 之后写，
    // 因为 Poll 开头会把整个 NPSensors 清空（踩过：写在前面会被覆盖成 0）。
    {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm)) {
            gApp.sensors.screenW = dm.dmPelsWidth;
            gApp.sensors.screenH = dm.dmPelsHeight;
            gApp.sensors.refreshHz = dm.dmDisplayFrequency > 1 ? dm.dmDisplayFrequency : 0;
        }
    }
    // 顺带按「当前正在读的那个游戏进程」取 GPU 引擎数据。
    // 走系统 PDH 计数器，不需要注入、不需要管理员，而且与锁帧 / Reflex /
    // 多线程提交都无关 —— 比钩子里推算的 GPU 忙时间可靠得多。
    DWORD pid;
    {
        // ★ gApp.injected 由守护线程（注入看护）维护，读的时候必须和它同一把锁，
        //   否则正好撞上 push_back 扩容就会读到野指针。
        AppLock lk;
        pid = gApp.telemetryPid;
        if (!pid && !gApp.injected.empty()) pid = gApp.injected.back();
    }
    // ETW 跟随同一个目标进程。它是**外置**的（不注入游戏），数据来自内核的
    // DxgKrnl Present 事件 —— 与锁帧 / Reflex / 多线程提交全都无关。
    np::Etw().SetTargetPid((uint32_t)pid);

    // ★ 只把 **Low 帧**换成 ETW 的读数，其它一律不碰。
    //
    // 为什么只改 Low：钩子量的是「真 Present 调用之间的间隔」，而这游戏每帧调
    // 两次 Present（TEST + 真），真 Present 在帧内的相位每帧都在变 ——
    // 实测单样本在 9.48~23.83ms 之间抖。**平均值对，但每个样本都是脏的**，
    // 排序取「最慢 1%」时取到的正是那批假的 23.8ms 样本，于是流畅的 60fps
    // 会显示 1%Low ≈ 38~42。ETW 的事件在真正 flip 处触发，没有这个抖动。
    //
    // 帧率/帧时间/GPU 时间**不覆盖** —— 那些本来就已经对了，
    // 多覆盖一次只会把对的数弄坏（上一版就是这么翻车的）。
    {
        np::EtwMonitor::Result er{};
        // ★ 必须带 pid 判断：没有目标进程时 ETW 的数据是「全系统所有进程的
        //   present 事件」混出来的（桌面、浏览器、播放器都算），拿它去覆盖
        //   Low 帧等于把别人的数字塞进面板。
        if (pid && np::Etw().active() && np::Etw().Snapshot(&er)) {
            if (er.low1Fps > 0.1f) gApp.telemetry.fpsLow1 = er.low1Fps;
            if (er.low01Fps > 0.1f) gApp.telemetry.fpsLow01 = er.low01Fps;
        }
    }

    // 帧计数：钩子和 ETW 两个来源取大的那个。
    //
    // 为什么必须这样：GPU 帧时间 = 「GPU 累计时间 ÷ 帧数」，而原来帧数**只**
    // 取自钩子。一旦钩子没挂上（比如 NextPerf 不是以管理员运行、注入被拒），
    // 帧数恒为 0 → GPU 帧时间永远算出 -1 → 面板退回显示钩子那个空值。
    // 实测日志里就是这个现象：admin=no → attached=0 → 帧数=0 → GPU 帧时间 = -1。
    // ETW 是外置的，不需要注入，所以它也能当帧数来源。
    uint32_t frameCount = gApp.telemetry.frameTotal;
    {
        np::EtwMonitor::Result er{};
        if (np::Etw().active() && np::Etw().Snapshot(&er) && er.presentN > frameCount)
            frameCount = er.presentN;
    }
    if (pid) {
        gApp.hub.PollGameGpu(pid, frameCount, gApp.sensors);
        // 每 5 秒把「每个数据是从哪来的」记一条。
        // 用户要求：来源写日志就行，UI 不用标注 —— 这样界面干净，
        // 排查时又一眼能看出某项到底是系统给的还是我们推算的。
        static uint64_t lastLog = 0;
        uint64_t nowMs = GetTickCount64();
        if (nowMs - lastLog >= 5000) {
            lastLog = nowMs;
            const NPSensors& s = gApp.sensors;
            const NPTelemetry& t = gApp.telemetry;
            AppLog("---- 数据来源 (pid=%lu) ----", (unsigned long)pid);
            // ETW：**外置**的帧计时（内核 DxgKrnl Present 事件）。
            // 先只写日志、不上面板 —— 等它和游戏 OSD 对得上再接进界面，
            // 免得你看到一堆还没验证过的数。
            {
                np::EtwMonitor::Result er{};
                bool have = np::Etw().Snapshot(&er);
                AppLog("  [ETW] %s | %s", np::Etw().active() ? "运行中" : "未运行",
                       np::Etw().status().c_str());
                if (have) {
                    AppLog("  [ETW] 外置帧计时 : %.1f fps / %.2f ms | 1%%Low %.1f | 0.1%%Low %.1f "
                           "| present 累计 %u (窗口 %u)",
                           er.fps, er.frameMs, er.low1Fps, er.low01Fps, er.presentN, er.inWindow);
                    // 诊断：实际匹配上的事件名。ETW 报的帧率若是真实值的两倍，
                    // 这里就能直接看出是哪种事件被重复算了。
                    std::string names;
                    for (int i = 0; i < er.evNameN && i < 6; ++i) {
                        char nb[192] = {0};
                        WideCharToMultiByte(CP_UTF8, 0, er.evNames[i], -1, nb, sizeof(nb) - 1,
                                            nullptr, nullptr);
                        if (!names.empty()) names += " | ";
                        names += nb;
                    }
                    AppLog("  [ETW] 匹配到的事件名 : %s", names.empty() ? "(暂无)" : names.c_str());
                } else {
                    AppLog("  [ETW] 还没有数据（没抓到该进程的 Present 事件）");
                }
            }
            AppLog("  显示模式      : %ux%u @ %uHz   <- EnumDisplaySettings(系统)",
                   s.screenW, s.screenH, s.refreshHz);
            AppLog("  输出分辨率    : %ux%u          <- DXGI SwapChain GetDesc(钩子/驱动)",
                   t.windowW, t.windowH);
            AppLog("  渲染分辨率    : %ux%u          <- ID3D12GraphicsCommandList::RSSetViewports"
                   "(钩子, 已按输出宽高比过滤)", t.renderW, t.renderH);
            AppLog("  GPU 帧时间    : %.2f ms       <- PDH \\GPU Engine(pid_%lu_*)\\Running Time"
                   "(系统/驱动, 单位100ns)", s.gpuBusyMs, (unsigned long)pid);
            if (!gApp.hub.gameGpuDiag().empty()) AppLog("  (PDH 未出数原因 : %s)", gApp.hub.gameGpuDiag().c_str());
            AppLog("  AI 引擎       : compute=%.1f%% ofa=%.1f%%  <- PDH \\GPU Engine(*)\\"
                   "Utilization Percentage(engtype_compute / engtype_ofa)",
                   s.engCompute, s.engOfa);
            AppLog("  帧率/帧时间   : %.1f fps / %.2f ms  <- IDXGISwapChain::Present 间隔"
                   "(钩子, 已滤 DXGI_PRESENT_TEST)", t.fps, t.frameMs);
            AppLog("  CPU 帧时间    : %.2f ms       <- 帧周期 - Present 阻塞 %.2f ms(钩子推算)",
                   t.cpuFrameMs, t.msInPresent);
            AppLog("  Low 帧        : 1%%=%.1f 0.1%%=%.1f <- 最近1200帧样本推算(钩子)",
                   t.fpsLow1, t.fpsLow01);
            AppLog("  GPU 占用/温度 : %.0f%% / %.0fC    <- NVML(厂商SDK)",
                   s.gpuUsage, s.gpuTemp);
            AppLog("  显存/内存     : %.1f/%.1f GB, %.1f/%.1f GB <- NVML + GlobalMemoryStatusEx",
                   s.vramUsedGB, s.vramTotalGB, s.ramUsedGB, s.ramTotalGB);
            AppLog("  数据源总览    : %s", gApp.hub.Describe().c_str());
            AppLog("  钩子: attached=%d 帧数=%u api=%u flags=0x%02X",
                   t.attached, t.frameTotal, t.gfxApi, t.hookFlags);
        }
    } else {
        gApp.sensors.gpuBusyMs = -1.0f;
    }
}

// ---------------------------------------------------------------------------
// 前台窗口跟踪
//
// 点「注入到前台进程」按钮时，前台窗口一定是 NextPerf 自己，所以必须提前记住
// 用户刚才切走的那个游戏。这里有两个坑都踩过：
//
//   坑 1：**靠主循环轮询会漏**。原来 120ms 轮询一次，快速 ALT+TAB 切过去又
//         切回来就可能没采到，用户看到的就是"读不到进程"。
//         改用 WinEvent 钩子：系统在**每一次**前台变化时主动回调，不会漏。
//
//   坑 2：**UWP（微软商店）应用拿到的不是它自己**。商店应用外面套了一层
//         ApplicationFrameHost 的壳（窗口类名 ApplicationFrameWindow），
//         GetWindowThreadProcessId 返回的是**那个壳的宿主进程**。
//         往它注入毫无意义 —— 真正的应用是它的子窗口
//         （Windows.UI.Core.CoreWindow）。所以要往下找一层。
// ---------------------------------------------------------------------------

// 判断窗口是不是 UWP 外壳
static bool IsUwpFrameHost(HWND hwnd) {
    wchar_t cls[64]{};
    if (GetClassNameW(hwnd, cls, 64) && _wcsicmp(cls, L"ApplicationFrameWindow") == 0)
        return true;
    // 兜底：按进程名判断（壳进程固定叫 ApplicationFrameHost.exe）
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return false;
    std::wstring nm = ProcessExeName(pid);
    for (auto& c : nm) c = (wchar_t)towlower(c);
    return nm.find(L"applicationframehost") != std::wstring::npos;
}

// 解析前台窗口对应的**真实应用进程**（UWP 要往下找一层）
static DWORD ResolveForegroundPid(HWND fg) {
    if (!fg) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (!pid) return 0;
    if (!IsUwpFrameHost(fg)) return pid;

    struct Ctx { DWORD host; DWORD found; } ctx{pid, 0};
    EnumChildWindows(
        fg,
        [](HWND child, LPARAM lp) -> BOOL {
            auto* x = reinterpret_cast<Ctx*>(lp);
            DWORD cp = 0;
            GetWindowThreadProcessId(child, &cp);
            // 取第一个 pid 与外壳不同的 —— 那就是被托管的应用
            if (cp && cp != x->host) {
                x->found = cp;
                return FALSE;   // 找到就停
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.found ? ctx.found : pid;
}

// 记住一个前台窗口对应的进程（过滤掉自己）
static void AppRememberForeground(HWND fg) {
    DWORD pid = ResolveForegroundPid(fg);
    if (!pid || pid == GetCurrentProcessId()) return;
    if (pid != gApp.forePid) {
        gApp.forePid = pid;
        gApp.foreName = ProcessExeName(pid);
        if (gApp.foreName.empty()) gApp.foreName = L"(未知进程)";
    }
}

// WinEvent 回调：系统每次前台变化都会叫我们
static void CALLBACK ForegroundWinEvent(HWINEVENTHOOK, DWORD ev, HWND hwnd, LONG idObject,
                                        LONG idChild, DWORD, DWORD) {
    if (ev != EVENT_SYSTEM_FOREGROUND || !hwnd) return;
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    AppRememberForeground(hwnd);
}

static HWINEVENTHOOK gFgHook = nullptr;

// 懒安装（第一次轮询时装上），避免还要去改启动流程
static void AppEnsureForegroundHook() {
    if (gFgHook) return;
    gFgHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                              ForegroundWinEvent, 0, 0,
                              WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
}

void AppTrackForeground() {
    AppEnsureForegroundHook();
    HWND fg = GetForegroundWindow();
    if (fg) {
        DWORD pid = ResolveForegroundPid(fg);
        if (pid && pid != GetCurrentProcessId()) {
            AppRememberForeground(fg);
            return;
        }
    }
    // 前台进程已经退出就作废
    if (gApp.forePid && !ProcessAlive(gApp.forePid)) {
        gApp.forePid = 0;
        gApp.foreName.clear();
    }
}

void AppPublish() {
    if (gApp.shmCfg) {
        NPConfig c = gApp.cfg;
        memcpy(gApp.shmCfg, &c, sizeof(NPConfig));
    }
    if (gApp.shmSens) memcpy(gApp.shmSens, &gApp.sensors, sizeof(NPSensors));

    // TryOpenTelemetry / PickTelemetry 会遍历 gApp.injected（守护线程在改它），
    // 两件事一起放在同一把锁里做完。
    AppLock lk;
    TryOpenTelemetry();
    PickTelemetry();
}

static const char* ApiName(uint32_t api) {
    switch (api) {
        case NP_API_D3D11: return "D3D11";
        case NP_API_D3D12: return "D3D12";
        case NP_API_D3D9: return "D3D9";
        case NP_API_VULKAN: return "Vulkan";
        case NP_API_OPENGL: return "OpenGL";
        default: return "?";
    }
}

// 没有画面数据时，用钩子自报的 flag 说明卡在哪一步
static std::string HookHint(uint32_t flags) {
    if (flags == 0) return "钩子没起来";
    if (!(flags & NP_HOOK_PRESENT)) return "Present 未挂上";
    if (!(flags & NP_HOOK_OVERLAY)) return "叠加资源未就绪";
    return "等待首帧";
}

static void UpdateStatus() {
    char buf[768];
    const NPTelemetry& t = gApp.telemetry;
    bool live = t.attached && (GetTickCount64() - t.tickMs) < 2500;

    // 「上一次操作/失败原因」优先显示，并且有存活时间 —— 不能被 120ms 的
    // 常规刷新冲掉，否则用户永远看不到注入失败的原因（这是原来最大的问题）。
    // ★ 加锁：守护线程（注入看护）注入失败时也会 SetNotice，读写必须互斥。
    {
        AppLock lk;
        if (!gApp.notice.empty() && GetTickCount64() < gApp.noticeUntil) {
            snprintf(buf, sizeof(buf), "%s%s",
                     gApp.noticeLevel ? "⚠ " : "", gApp.notice.c_str());
            gApp.statusText = buf;
            return;
        }
        gApp.notice.clear();
    }

    if (live) {
        snprintf(buf, sizeof(buf),
                 "状态：%s | 已接管 %s (pid %lu) | %s | %.0f FPS | CPU %.0f%% | GPU %.0f%%",
                 gApp.monitoring ? "监控中" : "已停止",
                 t.processName[0] ? t.processName : "?", (unsigned long)gApp.telemetryPid,
                 ApiName(t.gfxApi), t.fps, gApp.sensors.cpuUsage, gApp.sensors.gpuUsage);
    } else if (!gApp.injected.empty()) {
        // 钩子即使挂不上 Present 也会回报「为什么」，优先把它摆出来
        if (t.lastError[0]) {
            snprintf(buf, sizeof(buf),
                     "状态：%s | 已注入 %zu 个进程 · %s | CPU %.0f%% | GPU %.0f%%",
                     gApp.monitoring ? "监控中" : "已停止", gApp.injected.size(), t.lastError,
                     gApp.sensors.cpuUsage, gApp.sensors.gpuUsage);
        } else {
            snprintf(buf, sizeof(buf),
                     "状态：%s | 已注入 %zu 个进程但未接管画面（%s）| CPU %.0f%% | GPU %.0f%%",
                     gApp.monitoring ? "监控中" : "已停止", gApp.injected.size(),
                     HookHint(t.hookFlags).c_str(), gApp.sensors.cpuUsage, gApp.sensors.gpuUsage);
        }
    } else {
        snprintf(buf, sizeof(buf), "状态：%s | 未注入 | 数据源：%s | CPU %.0f%% | GPU %.0f%%",
                 gApp.monitoring ? "监控中" : "已停止", gApp.hub.Describe().c_str(),
                 gApp.sensors.cpuUsage, gApp.sensors.gpuUsage);
    }
    gApp.statusText = buf;
}

static void AddTray(HWND hwnd) {
    if (gTrayAdded) return;
    gNid.cbSize = sizeof(gNid);
    gNid.hWnd = hwnd;
    gNid.uID = 1;
    gNid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    gNid.uCallbackMessage = NP_WM_TRAY;
    gNid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    const wchar_t* tip = L"NextPerf 性能计数器";
    for (int i = 0; i < 127 && tip[i]; ++i) gNid.szTip[i] = tip[i];
    gNid.szTip[127] = 0;
    BOOL okAdd = Shell_NotifyIconW(NIM_ADD, &gNid);
    gTrayAdded = okAdd != FALSE;
    gTrayOk = gTrayAdded;
    // 托盘图标是「唯一退出入口」时的最后一根稻草 —— 加不上必须留下证据，
    // 并且要让主窗口的关闭按钮变成真正的退出（否则只能去任务管理器）。
    AppLog("tray: NIM_ADD -> %s (trayOk=%d)", okAdd ? "ok" : "FAILED", gTrayOk ? 1 : 0);
    if (!gTrayAdded) {
        SetNotice("托盘图标注册失败：关闭主窗口将直接退出程序（不再缩到托盘）", 1, 20000);
    }
}

static void ShowTrayMenu(HWND hwnd) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, NP_TRAY_SHOW,
                IsWindowVisible(hwnd) ? L"隐藏主窗口" : L"显示主窗口");
    AppendMenuW(m, MF_STRING | (gOverlayOff ? MF_UNCHECKED : MF_CHECKED), NP_TRAY_OVERLAY,
                L"叠加面板");
    AppendMenuW(m, MF_STRING | (gApp.monitoring ? MF_CHECKED : MF_UNCHECKED), NP_TRAY_MONITOR,
                gApp.monitoring ? L"监控中（点击暂停）" : L"已暂停（点击恢复）");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (gApp.isAdmin ? MF_GRAYED : 0), NP_TRAY_ELEVATE,
                gApp.isAdmin ? L"已是管理员权限" : L"以管理员身份重启（注入失败时用）");
    AppendMenuW(m, MF_STRING, NP_TRAY_LOGDIR, L"打开日志文件夹");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, NP_TRAY_EXIT, L"完全退出");
    POINT pt{};
    GetCursorPos(&pt);
    // 窗口隐藏时不能抢前台（会失败并导致菜单首击只关闭菜单），只在可见时置顶
    if (IsWindowVisible(hwnd)) SetForegroundWindow(hwnd);
    TrackPopupMenu(m, TPM_LEFTALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    // 经典修复：菜单收回后补一个 WM_NULL，否则部分系统上菜单项第一次点击不生效
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    // 模态循环可能把 WM_QUIT 吃掉：若点了「完全退出」，菜单关闭后补发一次
    if (gWantQuit) PostQuitMessage(0);
}

static void CleanupAndExit();

bool TrayAvailable() { return gTrayOk; }

void AppTrayNotify(UINT notifyMsg) {
    HWND hwnd = UiWindow();
    switch (notifyMsg) {
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowTrayMenu(hwnd);
            break;
        case WM_LBUTTONUP:
            // 单击图标 = 快速开关叠加
            gOverlayOff = !gOverlayOff;
            if (gOverlayOff) OverlaySetVisible(false);
            break;
        case WM_LBUTTONDBLCLK:
            ShowWindow(hwnd, IsWindowVisible(hwnd) ? SW_HIDE : SW_SHOW);
            break;
        default:
            break;
    }
}

void AppTrayCommand(int id) {
    HWND hwnd = UiWindow();
    switch (id) {
        case NP_TRAY_SHOW:
            ShowWindow(hwnd, IsWindowVisible(hwnd) ? SW_HIDE : SW_SHOW);
            break;
        case NP_TRAY_OVERLAY:
            gOverlayOff = !gOverlayOff;
            if (gOverlayOff) OverlaySetVisible(false);
            break;
        case NP_TRAY_MONITOR:
            gApp.monitoring = !gApp.monitoring;
            if (gApp.monitoring) InjectorScanNow();
            else OverlaySetVisible(false);
            break;
        case NP_TRAY_ELEVATE:
            if (AdminRelaunch()) {
                gWantQuit = true;
                CleanupAndExit();
                EndMenu();
                PostQuitMessage(0);
            }
            break;
        case NP_TRAY_LOGDIR: {
            std::wstring p = AppLogPath();
            size_t s = p.find_last_of(L'\\');
            if (s != std::wstring::npos) p = p.substr(0, s);
            ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        case NP_TRAY_EXIT:
            gWantQuit = true;
            CleanupAndExit();
            EndMenu();            // 强制结束 TrackPopupMenu 的模态循环，让 ShowTrayMenu 走收尾
            PostQuitMessage(0);
            break;
        default:
            break;
    }
}

// 通知所有已注入的游戏卸载钩子，然后释放本进程的全部句柄与共享内存
static bool gCleanedUp = false;
static void CleanupAndExit() {
    using namespace npa;
    if (gCleanedUp) return;
    gCleanedUp = true;
    // 0) 停掉 ETW 会话 —— 不显式停会留下一个同名会话，
    //    下次启动虽然能靠 ERROR_ALREADY_EXISTS 兜住，但会让系统里积垃圾。
    np::Etw().Stop();
    using namespace npa;
    // 1) 广播退出：钩子 worker 每 500ms 轮询一次 quit 标志
    gApp.cfg.quit = 1;
    AppPublish();
    Sleep(900);   // 等钩子完成自卸载（含 100ms 排空中窗）

    // 2) 托盘图标
    if (gTrayAdded) { Shell_NotifyIconW(NIM_DELETE, &gNid); gTrayAdded = false; }

    // 3) 各子系统
    InjectorShutdown();
    OverlayShutdown();
    UiDestroy();

    // 4) 共享内存：解除映射并关闭句柄，对象本体随最后一个句柄关闭而销毁
    TelemetryCloseAll();
    if (gApp.shmCfg) { UnmapViewOfFile(gApp.shmCfg); gApp.shmCfg = nullptr; }
    if (gApp.shmSens) { UnmapViewOfFile(gApp.shmSens); gApp.shmSens = nullptr; }
    if (gApp.mapCfg) { CloseHandle(gApp.mapCfg); gApp.mapCfg = nullptr; }
    if (gApp.mapSens) { CloseHandle(gApp.mapSens); gApp.mapSens = nullptr; }

    npb::GfxShutdown();
    gApp.hub.Shutdown();
}

}  // namespace npa

// ---------------------------------------------------------------- WinMain
// ---------------------------------------------------------------- 自检
// 用法：NextPerf.exe --selftest
// 不弹窗口，跑一遍传感器采集 + 面板渲染，把结果写到 selftest.txt。
static int RunSelfTest() {
    using namespace npa;
    // 写在 dist 的上一级（项目根），别把报告塞进交付目录。
    // 注意不能写成 DllPath() + "\\..\\selftest.txt" —— DllPath() 是
    // "...\NextPerfHook.dll"，那是把 dll 当目录再退一级，路径非法，
    // _wfopen 一直失败，报告只进了 stderr（踩过）。
    DllPath();
    FILE* f = _wfopen((gApp.exeDir + L"\\..\\selftest.txt").c_str(), L"wb");
    auto log = [&](const char* fmt, ...) {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        if (f) { fprintf(f, "%s\n", buf); fflush(f); }
        OutputDebugStringA(buf);
        fprintf(stderr, "%s\n", buf);
        fflush(stderr);
    };

    int fails = 0;
    log("NextPerf 自检报告");
    log("=================");

    if (!npb::GfxInit()) { log("[FAIL] Direct2D 初始化失败"); ++fails; }
    else log("[ OK ] Direct2D / DirectWrite 可用");

    gApp.hub.Init();
    for (int i = 0; i < 3; ++i) {
        gApp.hub.Poll(gApp.sensors);
        Sleep(220);
    }
    log("数据源: %s", gApp.hub.Describe().c_str());
    log("显卡: %s", gApp.sensors.gpuName[0] ? gApp.sensors.gpuName : "(未知)");
    log("CPU 占用 %.1f%%  温度 %.1f C", gApp.sensors.cpuUsage, gApp.sensors.cpuTemp);
    log("GPU 占用 %.1f%%  温度 %.1f C  功耗 %.1f W", gApp.sensors.gpuUsage, gApp.sensors.gpuTemp,
        gApp.sensors.gpuPower);
    log("显存 %.2f / %.2f GB   内存 %.2f / %.2f GB", gApp.sensors.vramUsedGB,
        gApp.sensors.vramTotalGB, gApp.sensors.ramUsedGB, gApp.sensors.ramTotalGB);
    log("NVAPI 域: GPU %.1f  FB %.1f  VID %.1f  BUS %.1f", gApp.sensors.domGpu,
        gApp.sensors.domFb, gApp.sensors.domVid, gApp.sensors.domBus);
    log("硬件 RT/Tensor: %.1f / %.1f （-1 表示厂商未开放）", gApp.sensors.hwRtPct,
        gApp.sensors.hwTensorPct);
    if (gApp.sensors.gpuUsage < 0) { log("[WARN] 没有拿到 GPU 占用率"); }

    // 面板渲染
    np::PanelRenderer panel;
    np::PanelData pd;
    npb::PanelBitmap bmp;
    if (!panel.Init()) { log("[FAIL] 面板渲染器初始化失败"); ++fails; }
    else {
        NPDefaultConfig(&gApp.cfg);
        NPClearTelemetry(&gApp.telemetry);
        np::BuildPanelData(pd, gApp.cfg, gApp.sensors, gApp.telemetry, &gApp.hist);
        float w = 0, h = 0;
        panel.Measure(pd, gApp.cfg, &w, &h);
        log("面板尺寸: %.0f x %.0f，行数 %zu", w, h, pd.rows.size());
        struct C { np::PanelRenderer* r; np::PanelData* d; NPConfig c; };
        C ctx{&panel, &pd, gApp.cfg};
        bool ok = bmp.Render((int)w + 3, (int)h + 3,
                             [](ID2D1RenderTarget* rt, void* ud) {
                                 C* c = reinterpret_cast<C*>(ud);
                                 c->r->Render(rt, 0, 0, *c->d, c->c);
                             },
                             &ctx);
        if (!ok) { log("[FAIL] 面板位图渲染失败"); ++fails; }
        else {
            int nonZero = 0;
            const unsigned char* p = bmp.pixels();
            for (int i = 0; i < bmp.stride() * bmp.height(); ++i)
                if (p[i]) { ++nonZero; if (nonZero > 200) break; }
            if (nonZero < 50) { log("[FAIL] 面板位图几乎是空白"); ++fails; }
            else log("[ OK ] 面板位图渲染出 %d 个非空字节", nonZero);
        }
    }

    // 共享内存
    if (!CreateShm()) { log("[FAIL] 共享内存创建失败"); ++fails; }
    else log("[ OK ] 共享内存 Config / Sensors 创建成功");
    log("钩子 DLL: %ls", DllPath().c_str());
    if (GetFileAttributesW(DllPath().c_str()) == INVALID_FILE_ATTRIBUTES) {
        log("[FAIL] 找不到 NextPerfHook.dll");
        ++fails;
    } else {
        log("[ OK ] NextPerfHook.dll 存在");
    }

    log("=================");
    log(fails ? "自检结束：有 %d 项失败" : "自检结束：全部通过", fails);
    if (f) fclose(f);
    gApp.hub.Shutdown();
    // 显式 ExitProcess：这个入口不进主消息循环、也不走 CleanupAndExit，
    // 直接 return 偶尔会把进程留在系统里（--uismoke 就会），
    // 残留进程会锁住 dist\NextPerf.exe 让后续构建失败。自检路径硬退出最省事。
    ExitProcess((UINT)fails);
    return fails;
}

// UI 冒烟：真正创建一次窗口并跑几轮消息循环，确保 WM_PAINT 路径不会崩
static int RunUiSmoke(HINSTANCE inst) {
    using namespace npa;
    if (!npb::GfxInit()) return 1;
    gApp.hub.Init();
    gApp.hub.Poll(gApp.sensors);
    if (!UiCreate(inst, SW_SHOW)) return 2;
    if (!OverlayInit(inst)) return 3;
    gApp.monitoring = true;
    OverlaySetVisible(true);

    for (int i = 0; i < 60; ++i) {
        MSG m{};
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        AppPollSensors();
        AppPublish();
        OverlayUpdate();
        UiRefresh();
        Sleep(30);
    }
    OverlaySetVisible(false);
    OverlayShutdown();
    UiDestroy();
    npb::GfxShutdown();
    gApp.hub.Shutdown();
    // 见 RunSelfTest 的说明：这条路径必须硬退出，否则会留残留进程
    ExitProcess(0);
    return 0;
}

// 进程级 DPI 感知：不开的话整张窗口会被 DWM 拉伸，文字发糊
static void EnableDpiAwareness() {
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (!u32) return;
    typedef BOOL(WINAPI * FnSetCtx)(void*);
    FnSetCtx setCtx = reinterpret_cast<FnSetCtx>(
        GetProcAddress(u32, "SetProcessDpiAwarenessContext"));
    if (setCtx) {
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = -4，失败再退到 SYSTEM_AWARE = -2
        if (setCtx(reinterpret_cast<void*>(-4))) return;
        if (setCtx(reinterpret_cast<void*>(-2))) return;
        return;
    }
    SetProcessDPIAware();
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR cmdLine, int cmdShow) {
    using namespace npa;

    EnableDpiAwareness();
    gInst = inst;
    NPClearSensors(&gApp.sensors);
    NPClearTelemetry(&gApp.telemetry);
    SettingsLoad();

    if (cmdLine && strstr(cmdLine, "--selftest")) return RunSelfTest() ? 2 : 0;
    if (cmdLine && strstr(cmdLine, "--uismoke")) return RunUiSmoke(inst);

    if (!npb::GfxInit()) {
        MessageBoxW(nullptr, L"无法初始化 Direct2D，程序将退出。", L"NextPerf", MB_ICONERROR);
        return 1;
    }

    gApp.hub.Init();
    CreateShm();
    SettingsSave();

    // 把「这次跑的是什么环境」一次性写进日志。用户反馈问题时只要给这一个文件，
    // 就能排除掉版本不对、DLL 不存在、权限不够这些最常见的原因。
    {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dll = DllPath();
        BOOL admin = FALSE;
        PSID admins = nullptr;
        SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
        if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                     DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &admins)) {
            CheckTokenMembership(nullptr, admins, &admin);
            FreeSid(admins);
        }
        gApp.isAdmin = admin != FALSE;
        AppLog("---- NextPerf 启动 ----");
        AppLog("build    : %ls", BuildStamp().c_str());
        AppLog("exe      : %ls", exe);
        AppLog("hook dll : %ls (%s)", dll.c_str(),
               GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES ? "不存在!" : "存在");
        AppLog("admin    : %s", gApp.isAdmin ? "yes" : "no");
        AppLog("log      : %ls", AppLogPath().c_str());
        AppLog("游戏列表 : %zu 项", gApp.games.size());
        for (auto& g : gApp.games) AppLog("  - %ls", g.path.c_str());
        if (!gApp.games.empty() && GetFileAttributesW(gApp.games[0].path.c_str()) ==
                                       INVALID_FILE_ATTRIBUTES)
            AppLog("  !! 上面第一个 exe 路径不存在（盘符/路径变了？）");
        if (!gApp.isAdmin)
            AppLog("提示     : 非管理员运行。若游戏以管理员启动，注入会被拒（err=5）");

        // ETW 外置帧计时：需要管理员。失败不影响其它功能，只写日志说明原因。
        if (np::Etw().Start())
            AppLog("etw      : 已启动（%s）", np::Etw().status().c_str());
        else
            AppLog("etw      : 启动失败 —— %s", np::Etw().status().c_str());
    }

    if (!UiCreate(inst, cmdShow)) return 1;
    if (!OverlayInit(inst)) {
        MessageBoxW(nullptr, L"叠加窗口创建失败。", L"NextPerf", MB_ICONWARNING);
    }
    InjectorInit();

    AddTray(UiWindow());

    gApp.monitoring = true;
    OverlaySetVisible(true);
    InjectorScanNow();

    HWND hwnd = UiWindow();
    MSG msg{};
    uint64_t lastTick = 0;

    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_QUIT) break;

        // 托盘消息与菜单命令统一在主窗口过程（MainProc）里处理：
        // TrackPopupMenu 的模态循环会把 WM_COMMAND 直接派发给窗口过程，
        // 在这里拦截反而收不到菜单点击（这就是之前"右键菜单点了没反应"的根因）。

        uint64_t now = GetTickCount64();
        if (now - lastTick >= 120) {
            lastTick = now;
            AppTrackForeground();
            if (gApp.monitoring) {
                AppPollSensors();
                AppPublish();
                if (gApp.cfg.simulate) {
                    // 模拟数据（开发预览用，正式版删除）：叠加起伏的正弦 + 噪声
                    float ph = (float)(now % 200000) * 0.001f;
                    float fps = 97.0f + 26.0f * sinf(ph / 6.3f) + 11.0f * sinf(ph / 1.31f) +
                                5.0f * sinf(ph * 1.73f);
                    if (fps < 20) fps = 20;
                    float ft = 1000.0f / fps;
                    float avg = fps * (0.965f + 0.02f * sinf(ph / 17.0f));
                    float low1 = fps * (0.86f + 0.05f * sinf(ph / 9.0f));
                    float low01 = fps * (0.71f + 0.07f * sinf(ph / 5.2f));
                    float cpu = 42.0f + 30.0f * sinf(ph / 8.1f) + 8.0f * sinf(ph * 2.9f);
                    float gpu = 78.0f + 18.0f * sinf(ph / 7.3f) + 5.0f * sinf(ph * 3.7f);
                    cpu = std::clamp(cpu, 2.0f, 99.0f);
                    gpu = std::clamp(gpu, 2.0f, 99.0f);
                    NPTelemetry& tm = gApp.telemetry;
                    tm.fps = fps; tm.fpsAvg = avg; tm.fpsLow1 = low1; tm.fpsLow01 = low01;
                    tm.frameMs = ft;
                    tm.cpuFrameMs = ft * (0.52f + 0.10f * sinf(ph / 4.4f));
                    tm.gpuFrameMs = ft * (0.82f + 0.08f * sinf(ph / 3.1f));
                    gApp.hist.sampleMs = 125;
                    // 演示模式：CPUBusy/CPUWait 也造出合理数值，否则这两条曲线恒为 0
                    tm.cpuBusyMs = tm.frameMs * (0.30f + 0.06f * sinf(ph / 5.2f));
                    tm.cpuWaitMs = tm.frameMs - tm.cpuBusyMs;
                    NPHistoryPush(&gApp.hist, fps, avg, low1, low01, cpu, gpu, tm.frameMs,
                                  tm.cpuFrameMs, tm.gpuFrameMs,
                                  tm.cpuBusyMs, tm.cpuWaitMs);
                } else {
                    NPHistoryPush(&gApp.hist, gApp.telemetry.fps, gApp.telemetry.fpsAvg,
                                  gApp.telemetry.fpsLow1, gApp.telemetry.fpsLow01,
                                  gApp.sensors.cpuUsage, gApp.sensors.gpuUsage,
                                  gApp.telemetry.frameMs, gApp.telemetry.cpuFrameMs,
                                  gApp.telemetry.gpuFrameMs,
                                  gApp.telemetry.cpuBusyMs, gApp.telemetry.cpuWaitMs);
                }
                InjectorTick();

                bool attached = gApp.telemetry.attached &&
                                (now - gApp.telemetry.tickMs) < 2500;
                bool wantDesktop = !gOverlayOff && gApp.monitoring &&
                                   (gApp.cfg.overlayMode == 2 ||
                                    (!attached && gApp.cfg.overlayMode != 1));
                OverlaySetVisible(wantDesktop);
                OverlayUpdate();
                UpdateStatus();
            }
        }

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CleanupAndExit();
    SettingsSave();
    return 0;
}
