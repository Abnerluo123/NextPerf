// 主程序共享状态
#pragma once

#include <windows.h>
#include <string>
#include <vector>

#include "common/np_common.h"
#include "common/np_panel.h"
#include "sensors/np_sensors.h"

namespace npa {

// 本 exe 的构建时间（取自身文件时间戳，标题栏和启动日志里都显示）。
// 用来一眼分辨「跑的是哪份构建」—— 踩过的坑：磁盘上同时存在两份 NextPerf，
// 习惯性打开了旧的那份，于是所有修复看起来都「没生效」，白排查一整轮。
// 不用 __DATE__/__TIME__：clang 为了可复现构建会把它当错误（-Werror,-Wdate-time），
// 而且取文件时间本来就更准。
std::wstring BuildStamp();

// 托盘回调消息与命令 ID（主程序与 UI 层共用）
#define NP_WM_TRAY (WM_USER + 100)
enum {
    NP_TRAY_SHOW = 1001,
    NP_TRAY_OVERLAY = 1002,
    NP_TRAY_MONITOR = 1003,
    NP_TRAY_EXIT = 1004,
    NP_TRAY_ELEVATE = 1005,
    NP_TRAY_LOGDIR = 1006,
};

void AppTrayNotify(UINT notifyMsg);
void AppTrayCommand(int id);
// 托盘图标是否真的注册成功。失败时 UI 要把「关闭窗口」当成退出，
// 否则用户找不到程序也没法退出（只能在任务管理器里杀）。
bool TrayAvailable();

// 游戏列表项：路径 + 运行时状态（由注入器每轮扫描填）
struct GameEntry {
    std::wstring path;
    DWORD pid = 0;        // 当前匹配到的进程，0 = 没在运行
    bool  injected = false;
    bool  hooked = false; // 钩子已接管画面（有新鲜遥测）
};

struct AppState {
    NPConfig      cfg{};
    NPSensors     sensors{};
    NPTelemetry   telemetry{};
    NPHistory     hist{};          // 桌面叠加的图表历史（占用率等）
    np::SensorHub hub;

    bool monitoring = false;     // 监控开关
    bool overlayVisible = false;
    bool uiVisible = true;
    bool isAdmin = false;        // 本进程是否以管理员运行

    // 「上一次操作的结果」要能看见。
    // 主循环每 120ms 刷一次状态栏，如果直接写 statusText，注入失败的提示
    // 会在 120ms 内被覆盖 —— 用户只会看到「未注入」，永远不知道原因（踩过）。
    std::string notice;
    uint64_t    noticeUntil = 0;
    int         noticeLevel = 0;   // 0=普通 1=错误

    // 共享内存（写端）
    HANDLE mapCfg = nullptr;
    HANDLE mapSens = nullptr;
    NPConfig*    shmCfg = nullptr;
    NPSensors*   shmSens = nullptr;

    // 每个被注入进程的遥测块。钩子按 PID 建名字（NPTelemetryShmName），
    // 这里按 PID 分别打开，取「最新鲜」的那一份当当前数据。
    struct TelSlot {
        DWORD        pid = 0;
        HANDLE       map = nullptr;
        NPTelemetry* view = nullptr;
        uint64_t     lastTick = 0;
    };
    std::vector<TelSlot> telSlots;
    DWORD  telemetryPid = 0;     // 当前这份数据来自哪个进程

    // 游戏列表（完整路径 + 运行时状态）
    std::vector<GameEntry> games;
    std::vector<DWORD> injected;

    // 最近一个「不属于本进程」的前台窗口。点「注入到前台进程」时前台窗口
    // 就是本程序自己，所以只能靠这个记住刚才玩的是哪个游戏。
    DWORD        forePid = 0;
    std::wstring foreName;

    std::string statusText;
    std::wstring dllPath;
    std::wstring exeDir;

    uint64_t lastPoll = 0;
    uint64_t lastOverlay = 0;
};

extern AppState gApp;

// ---- 诊断日志（主程序侧）：%TEMP%\NextPerf.log
// 注入失败的原因必须落盘 —— 状态栏会被不停刷新，游戏进程内的钩子日志又
// 只有在 DLL 真的载入之后才有。中间这段（注入本身失败）原来完全没有记录。
void AppLog(const char* fmt, ...);
std::wstring AppLogPath();
// 写一条「给用户看」的提示，noticeMs 毫秒内不会被状态栏刷新覆盖
void SetNotice(const std::string& text, int level = 0, uint32_t noticeMs = 12000);
bool AdminRelaunch();     // 以管理员身份重新启动自己（成功则调用方应退出）

// ---- settings.cpp
bool SettingsLoad();
void SettingsSave();
std::wstring SettingsPath();

// ---- overlay.cpp（桌面分层叠加）
bool OverlayInit(HINSTANCE inst);
void OverlayShutdown();
void OverlayUpdate();
void OverlaySetVisible(bool v);

// ---- injector.cpp
//
// 注入状态锁。为什么必须有它：守护线程（InjectorInit 起的那个）每 800ms 跑一次
// InjectorScanNow，会往 gApp.injected 里 push_back、改写 gApp.games[i].pid；
// 而主线程（消息循环）每 120ms 就在读/遍历它们。std::vector 的 push_back 会扩容，
// 扩容之后另一边正在用的迭代器/引用就是野指针 —— 直接崩，而且崩的是主程序。
// 所有跨线程碰 gApp.injected / gApp.games / gApp.notice 的地方都用它圈起来。
// CRITICAL_SECTION 可重入，同一线程嵌套加锁是安全的。
struct AppLock {
    AppLock();
    ~AppLock();
};

bool InjectorInit();
void InjectorShutdown();
bool InjectInto(DWORD pid);
bool IsInjected(DWORD pid);
void InjectorTick();
void InjectorScanNow();
std::wstring DllPath();
// 进程信息小工具
bool         ProcessAlive(DWORD pid);
std::wstring ProcessImagePath(DWORD pid);
std::wstring ProcessExeName(DWORD pid);   // 带扩展名，原样大小写
bool         ProcessIsWow64(DWORD pid);   // 32 位进程跑在 64 位系统上
void         TelemetryCloseAll();         // 关闭全部遥测映射

// ---- ui.cpp
bool UiCreate(HINSTANCE inst, int cmdShow);
void UiDestroy();
void UiRefresh();      // 状态文本 / 预览刷新
HWND UiWindow();

// ---- main.cpp
void AppPollSensors();
void AppPublish();
void AppTrackForeground();   // 记录最近一个非本进程的前台窗口

}  // namespace npa
