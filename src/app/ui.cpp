// 主界面：自绘深色 UI（黑白配色、无高亮描边、无 Logo）
//
// 布局全部按 DPI 缩放后一次性算好，列表滚动通过 VisibleRect 折算，
// 不会再出现控件互相压叠的情况。

#include "np_app.h"

#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "common/np_bitmap.h"
#include "common/np_build.h"

namespace npa {

// ---------------------------------------------------------------- 主题（黑白）
static const COLORREF kBack = RGB(0x0D, 0x0F, 0x12);
static const COLORREF kPanel = RGB(0x15, 0x18, 0x1D);
static const COLORREF kPanel2 = RGB(0x1E, 0x22, 0x28);
static const COLORREF kBorder = RGB(0x2B, 0x30, 0x37);
static const COLORREF kText = RGB(0xF2, 0xF4, 0xF6);
static const COLORREF kDim = RGB(0x8A, 0x91, 0x99);

static const int kWinW = 1000;
static const int kWinH = 690;
// 计数器列表高度。只在这里定义一次 —— 原来 Layout() 和 WM_PAINT 里各写了一遍
// S(452)，改一处漏一处就会让列表和游戏区对不上。
// 400 是给「游戏进程」下面两行按钮腾出来的（状态栏固定在 S(658)，不能压）。
static const int kListH = 400;

static float gS = 1.0f;                       // UI 缩放（DPI / 96）
static int S(int v) { return (int)(v * gS + 0.5f); }

// ---------------------------------------------------------------- 计数器定义（分组）
struct CounterDef {
    uint64_t bit;
    const wchar_t* name;
};
struct CounterGroup {
    const wchar_t* name;
    const CounterDef* items;
    int n;
};

static const CounterDef kGFramerate[] = {
    {NP_C_FPS, L"FPS"},
    {NP_C_FRAMETIME, L"帧生成时间"},
    {NP_C_LOW1, L"1% Low"},
    {NP_C_LOW01, L"0.1% Low"},
};
static const CounterDef kGCpu[] = {
    {NP_C_CPU_USAGE, L"CPU 占用率"},
    {NP_C_CPU_TEMP, L"CPU 温度"},
    {NP_C_CPU_CLOCK, L"CPU 频率"},
    {NP_C_CPU_POWER, L"CPU 功耗"},
    {NP_C_CPU_FRAME, L"CPU 帧时间"},
    {NP_C_CPU_BUSY, L"CPU Busy（高级）"},
    {NP_C_CPU_WAIT, L"CPU Wait（高级）"},
};
static const CounterDef kGGpu[] = {
    {NP_C_GPU_NAME, L"显卡型号"},
    {NP_C_GPU_USAGE, L"GPU 占用率"},
    {NP_C_GPU_TEMP, L"GPU 温度"},
    {NP_C_GPU_HOTSPOT, L"GPU 热点 / 显存结温"},
    {NP_C_GPU_POWER, L"GPU 功耗"},
    {NP_C_GPU_CLOCK, L"核心 / 显存频率"},
    {NP_C_FAN, L"风扇转速"},
    {NP_C_GPU_FRAME, L"GPU 帧时间"    },
    {NP_C_VRAM, L"显存占用"},
    {NP_C_GPU_FB, L"显存带宽占用"},
    {NP_C_GPU_VID, L"视频引擎占用"},
    {NP_C_GPU_BUS, L"PCIe 总线占用"},
    {NP_C_RT, L"RT Core 负载"},
    {NP_C_TENSOR, L"Tensor / AI 负载"},
    {NP_C_AI_MODULES, L"AI 模块识别"},
    {NP_C_RESOLUTION, L"渲染 / 输出分辨率"},
};
static const CounterDef kGSystem[] = {
    {NP_C_RAM, L"内存占用"},
    {NP_C_API, L"图形 API 与模式"},
    {NP_C_DRAWS, L"Draw / Dispatch"},
    {NP_C_SENSOR_SRC, L"传感器数据源"},
};
static const CounterDef kGChart[] = {
    {NP_C_GRAPH, L"FPS / 帧时间小图"},
    {NP_C_CHART_FPS, L"Low 帧小图（1% / 0.1%）"},
    {NP_C_CHART_LATENCY, L"帧延迟小图（CPU / GPU）"},
};
static const CounterGroup kGroups[] = {
    {L"帧率与延迟", kGFramerate, (int)(sizeof(kGFramerate) / sizeof(kGFramerate[0]))},
    {L"CPU", kGCpu, (int)(sizeof(kGCpu) / sizeof(kGCpu[0]))},
    {L"GPU", kGGpu, (int)(sizeof(kGGpu) / sizeof(kGGpu[0]))},
    {L"系统", kGSystem, (int)(sizeof(kGSystem) / sizeof(kGSystem[0]))},
    {L"实时图表", kGChart, (int)(sizeof(kGChart) / sizeof(kGChart[0]))},
};
static const int kGroupN = (int)(sizeof(kGroups) / sizeof(kGroups[0]));
static int gTotalRows = 0;   // 计数器列表总行数（含组标题）

// ---------------------------------------------------------------- 控件
enum WType { W_CHECK, W_GROUP, W_BUTTON, W_SLIDER_F, W_SLIDER_I, W_CYCLE, W_GAMEROW };
enum Act {
    A_NONE = 0, A_START, A_STOP, A_SAVE, A_DEFAULT, A_ADDGAME, A_REMGAME, A_INJECTFRONT,
    A_ELEVATE, A_LOGDIR, A_QUIT
};

struct Widget {
    WType type;
    int x = 0, y = 0, w = 0, h = 0;
    std::wstring text;
    uint64_t bit = 0;
    float* pFloat = nullptr;
    float fmin = 0, fmax = 1;
    int* pInt = nullptr;
    int imin = 0, imax = 100;
    int* pCycle = nullptr;
    std::vector<std::wstring> opts;
    std::vector<int> vals;
    int action = A_NONE;
    int index = -1;
    bool hover = false;
};

static std::vector<Widget> gW;
static HWND gMain = nullptr;
static HINSTANCE gInst = nullptr;
static int gSelGame = -1;
static int gDragging = -1;

// 计数器列表滚动
static int gScroll = 0;
static int gListTop = 0;
static int gRowH = 0;
static int gVisibleRows = 18;

static npb::PanelBitmap gPrevBmp;
static np::PanelRenderer gPrevPanel;
static np::PanelData gPrevData;
static bool gPrevOk = false;

static HFONT gFontTitle = nullptr, gFontHead = nullptr, gFontBody = nullptr, gFontSmall = nullptr;

static void ReleaseFonts() {
    if (gFontTitle) { DeleteObject(gFontTitle); gFontTitle = nullptr; }
    if (gFontHead) { DeleteObject(gFontHead); gFontHead = nullptr; }
    if (gFontBody) { DeleteObject(gFontBody); gFontBody = nullptr; }
    if (gFontSmall) { DeleteObject(gFontSmall); gFontSmall = nullptr; }
}

static void MakeFonts() {
    ReleaseFonts();
    // 灰阶抗锯齿：深底上不会出现 ClearType 的彩色边缘
    auto mk = [&](int px, int weight) {
        return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    };
    gFontTitle = mk(S(19), FW_SEMIBOLD);
    gFontHead = mk(S(14), FW_SEMIBOLD);
    gFontBody = mk(S(13), FW_NORMAL);
    gFontSmall = mk(S(11), FW_NORMAL);
}

// ---------------------------------------------------------------- 绘制助手
static void FillR(HDC hdc, int x, int y, int w, int h, COLORREF c, int r) {
    HBRUSH b = CreateSolidBrush(c);
    HGDIOBJ old = SelectObject(hdc, b);
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    RoundRect(hdc, x, y, x + w, y + h, r, r);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
    SelectObject(hdc, old);
    DeleteObject(b);
}
static void StrokeR(HDC hdc, int x, int y, int w, int h, COLORREF c, int r) {
    HBRUSH b = reinterpret_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    HGDIOBJ old = SelectObject(hdc, b);
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    RoundRect(hdc, x, y, x + w, y + h, r, r);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
    SelectObject(hdc, old);
}
static void Txt(HDC hdc, HFONT f, const wchar_t* s, int x, int y, int w, int h, COLORREF c,
                UINT fmt = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) {
    HFONT old = reinterpret_cast<HFONT>(SelectObject(hdc, f));
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, c);
    RECT rc{x, y, x + w, y + h};
    DrawTextW(hdc, s, -1, &rc, fmt);
    SelectObject(hdc, old);
}

// ---------------------------------------------------------------- 布局
static void Add(int type, int x, int y, int w, int h, const wchar_t* text) {
    Widget wd;
    wd.type = (WType)type;
    wd.x = x; wd.y = y; wd.w = w; wd.h = h;
    wd.text = text;
    gW.push_back(wd);
}

static void Layout() {
    gW.clear();

    const int LX = S(20), LW = S(460);
    const int RX = S(500), RW = S(480);

    // ---------- 左：计数器（分组、可滚动） ----------
    gListTop = S(42);
    gRowH = S(24);
    const int listH = S(kListH);
    gVisibleRows = std::max(1, listH / gRowH);
    int rowOrd = 0;
    for (int g = 0; g < kGroupN; ++g) {
        Widget hd;
        hd.type = W_GROUP;
        hd.x = LX;
        hd.y = gListTop + rowOrd * gRowH;   // 逻辑坐标，绘制时经 VisibleRect 折算
        hd.w = LW;
        hd.h = gRowH;
        hd.text = kGroups[g].name;
        hd.index = rowOrd++;
        gW.push_back(hd);
        for (int i = 0; i < kGroups[g].n; ++i) {
            Widget wd;
            wd.type = W_CHECK;
            wd.x = LX;
            wd.y = gListTop + rowOrd * gRowH;
            wd.w = LW;
            wd.h = gRowH;
            wd.bit = kGroups[g].items[i].bit;
            wd.text = kGroups[g].items[i].name;
            wd.index = rowOrd++;
            gW.push_back(wd);
        }
    }
    gTotalRows = rowOrd;
    int maxScroll = std::max(0, gTotalRows - gVisibleRows);
    gScroll = std::clamp(gScroll, 0, maxScroll);

    // ---------- 左：游戏列表 ----------
    const int gameTop = gListTop + listH + S(16);
    for (int i = 0; i < 3; ++i) {
        Add(W_GAMEROW, LX, gameTop + i * S(32), LW, S(28), L"");
        gW.back().index = i;
    }
    int by = gameTop + 3 * S(32) + S(10);
    Add(W_BUTTON, LX, by, S(140), S(32), L"添加游戏 exe");
    gW.back().action = A_ADDGAME;
    Add(W_BUTTON, LX + S(150), by, S(110), S(32), L"移除选中");
    gW.back().action = A_REMGAME;
    Add(W_BUTTON, LX + S(270), by, S(190), S(32), L"注入到上一个游戏");
    gW.back().action = A_INJECTFRONT;
    // 第二行：注入失败十有八九是权限不够，给一个一键提权的入口；
    // 再给一个**界面内的退出按钮** —— 托盘图标万一没注册上，这是唯一的出路。
    int by2 = by + S(38);
    Add(W_BUTTON, LX, by2, S(190), S(32), L"以管理员身份重启");
    gW.back().action = A_ELEVATE;
    Add(W_BUTTON, LX + S(200), by2, S(140), S(32), L"打开日志文件夹");
    gW.back().action = A_LOGDIR;
    Add(W_BUTTON, LX + S(350), by2, S(110), S(32), L"退出程序");
    gW.back().action = A_QUIT;

    // ---------- 右：外观 ----------
    int ry = S(288);
    auto sliderF = [&](const wchar_t* t, float* p, float a, float b) {
        Widget wd;
        wd.type = W_SLIDER_F; wd.x = RX; wd.y = ry; wd.w = RW; wd.h = S(30);
        wd.text = t; wd.pFloat = p; wd.fmin = a; wd.fmax = b;
        gW.push_back(wd);
        ry += S(30);
    };
    auto sliderI = [&](const wchar_t* t, int* p, int a, int b) {
        Widget wd;
        wd.type = W_SLIDER_I; wd.x = RX; wd.y = ry; wd.w = RW; wd.h = S(30);
        wd.text = t; wd.pInt = p; wd.imin = a; wd.imax = b;
        gW.push_back(wd);
        ry += S(30);
    };
    sliderF(L"缩放", &gApp.cfg.scale, 0.75f, 2.0f);
    sliderF(L"背景不透明度", &gApp.cfg.bgOpacity, 0.05f, 1.0f);
    sliderF(L"字体不透明度", &gApp.cfg.textOpacity, 0.10f, 1.0f);
    sliderI(L"字号", (int*)&gApp.cfg.fontHeight, 10, 24);
    sliderI(L"图表高度", (int*)&gApp.cfg.graphHeight, 30, 140);
    sliderI(L"水平偏移", (int*)&gApp.cfg.offsetX, 0, 400);
    sliderI(L"垂直偏移", (int*)&gApp.cfg.offsetY, 0, 400);

    // ---------- 右：行为 ----------
    ry += S(36);   // 与上方滑杆区留出标题空间
    auto cycle = [&](const wchar_t* t, int* p, std::vector<std::wstring> opts,
                     std::vector<int> vals, int x, int y2, int w) {
        Widget wd;
        wd.type = W_CYCLE; wd.x = x; wd.y = y2; wd.w = w; wd.h = S(30);
        wd.text = t; wd.pCycle = p; wd.opts = std::move(opts); wd.vals = std::move(vals);
        gW.push_back(wd);
    };
    int half = RW / 2 - S(4);
    cycle(L"叠加模式", (int*)&gApp.cfg.overlayMode,
          {L"自动", L"强制游戏内", L"强制桌面叠加"}, {0, 1, 2}, RX, ry, half);
    cycle(L"叠加刷新率", (int*)&gApp.cfg.updateHz, {L"60 Hz", L"30 Hz", L"20 Hz", L"10 Hz"},
          {60, 30, 20, 10}, RX + RW / 2 + S(4), ry, half);
    ry += S(38);
    cycle(L"传感器轮询", (int*)&gApp.cfg.pollMs, {L"200 ms", L"350 ms", L"500 ms", L"1000 ms"},
          {200, 350, 500, 1000}, RX, ry, half);
    cycle(L"深度引擎钩子", (int*)&gApp.cfg.deepEngineHook, {L"开启", L"关闭"}, {1, 0},
          RX + RW / 2 + S(4), ry, half);
    // 3D 窗口自动注入：用户要求给开关，**默认关闭**（误注入会打扰无关程序）
    cycle(L"自动注入 3D 窗口", (int*)&gApp.cfg.autoInject, {L"关闭", L"开启"}, {0, 1},
          RX, ry, half);
    // Low 帧口径：默认「窗口平均」——与驱动面板/游戏内 overlay 的口径一致
    // （实测 RE8 锁 60：窗口平均 57.1 vs 严格 53.8，驱动显示 59）。
    // 想抓单帧卡顿就切到「严格」。
    cycle(L"Low 帧口径", (int*)&gApp.cfg.lowStrict, {L"窗口平均", L"严格"}, {0, 1}, RX, ry, half);
    ry += S(38);
    cycle(L"模拟数据（预览）", (int*)&gApp.cfg.simulate, {L"关闭", L"开启"}, {0, 1}, RX, ry, half);
    // 游戏已经在跑时，Present 只能靠自己造一条临时交换链去挂（见 ProbeSwapChainVtable）。
    // 个别游戏对注入期创建设备敏感，留个开关能关掉。
    cycle(L"注入探测交换链", (int*)&gApp.cfg.vtableProbe, {L"开启", L"关闭"}, {1, 0},
          RX + RW / 2 + S(4), ry, half);

    // ---------- 右：按钮 ----------
    ry += S(46);
    Add(W_BUTTON, RX, ry, S(106), S(32), L"开始监控");
    gW.back().action = A_START;
    Add(W_BUTTON, RX + S(114), ry, S(106), S(32), L"停止监控");
    gW.back().action = A_STOP;
    Add(W_BUTTON, RX + S(228), ry, S(106), S(32), L"保存配置");
    gW.back().action = A_SAVE;
    Add(W_BUTTON, RX + S(342), ry, S(130), S(32), L"恢复默认");
    gW.back().action = A_DEFAULT;
}

// ---------------------------------------------------------------- 预览
static void PrevDraw(ID2D1RenderTarget* rt, void* ud) {
    np::PanelRenderer* r = reinterpret_cast<np::PanelRenderer*>(ud);
    r->Render(rt, 0.0f, 0.0f, gPrevData, gApp.cfg);
}

static void RenderPreview() {
    gPrevOk = false;
    if (!npb::Factory() && !npb::GfxInit()) return;
    if (!gPrevPanel.Init()) return;
    np::BuildPanelData(gPrevData, gApp.cfg, gApp.sensors, gApp.telemetry, &gApp.hist);
    float pw = 0, ph = 0;
    gPrevPanel.Measure(gPrevData, gApp.cfg, &pw, &ph);
    int w = (int)(pw + 3.0f), h = (int)(ph + 3.0f);
    if (w < 8 || h < 8) return;
    gPrevOk = gPrevBmp.Render(w, h, PrevDraw, &gPrevPanel);
}

// ---------------------------------------------------------------- 交互
static bool VisibleRect(const Widget& wd, int ordinal, RECT* out) {
    (void)ordinal;
    if (wd.type == W_CHECK || wd.type == W_GROUP) {
        int vi = wd.index - gScroll;
        if (vi < 0 || vi >= gVisibleRows) return false;
        out->left = wd.x;
        out->top = gListTop + vi * gRowH;
        out->right = wd.x + wd.w;
        out->bottom = out->top + gRowH;
        return true;
    }
    out->left = wd.x;
    out->top = wd.y;
    out->right = wd.x + wd.w;
    out->bottom = wd.y + wd.h;
    return true;
}

static Widget* Hit(int mx, int my) {
    for (size_t i = 0; i < gW.size(); ++i) {
        Widget& wd = gW[i];
        if (wd.type == W_GROUP) continue;   // 组标题不可点
        RECT rc{};
        if (!VisibleRect(wd, (int)i, &rc)) continue;
        if (mx >= rc.left && mx < rc.right && my >= rc.top && my < rc.bottom) return &wd;
    }
    return nullptr;
}

static void DoAction(int action) {
    switch (action) {
        case A_START:
            gApp.monitoring = true;
            OverlaySetVisible(true);
            InjectorScanNow();
            gApp.statusText = "监控已启动";
            break;
        case A_STOP:
            gApp.monitoring = false;
            OverlaySetVisible(false);
            gApp.statusText = "监控已停止";
            break;
        case A_SAVE:
            SettingsSave();
            gApp.statusText = "配置已保存";
            break;
        case A_DEFAULT:
            NPDefaultConfig(&gApp.cfg);
            gApp.statusText = "已恢复默认设置";
            break;
        case A_ADDGAME: {
            wchar_t path[MAX_PATH]{};
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = gMain;
            ofn.lpstrFile = path;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrFilter = L"可执行文件\0*.exe\0全部文件\0*.*\0";
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_EXPLORER;
            ofn.lpstrTitle = L"选择游戏主程序";
            if (GetOpenFileNameW(&ofn)) {
                GameEntry ge;
                ge.path = path;
                {
                    // ★ 加锁：守护线程每 800ms 就遍历一次 gApp.games，
                    //   这里 push_back 触发扩容会让它的迭代器变成野指针（会崩）。
                    AppLock lk;
                    gApp.games.push_back(ge);
                }
                SetNotice("已添加游戏：" + np::WideToUtf8(path), 0, 6000);
                AppLog("add game: %ls", path);
                SettingsSave();
            }
            break;
        }
        case A_REMGAME:
            if (gSelGame >= 0 && gSelGame < (int)gApp.games.size()) {
                AppLog("remove game: %ls", gApp.games[gSelGame].path.c_str());
                {
                    AppLock lk;   // 同上：erase 同样会让守护线程的引用失效
                    gApp.games.erase(gApp.games.begin() + gSelGame);
                }
                gSelGame = -1;
                SettingsSave();
            }
            break;
        case A_QUIT:
            AppLog("quit from UI button");
            PostMessageW(gMain, WM_COMMAND, NP_TRAY_EXIT, 0);
            break;
        case A_ELEVATE:
            if (AdminRelaunch()) {
                SetNotice("已发起提权重启，本进程即将退出", 0, 4000);
                PostMessageW(gMain, WM_COMMAND, NP_TRAY_EXIT, 0);
            }
            break;
        case A_LOGDIR: {
            std::wstring p = AppLogPath();
            size_t s = p.find_last_of(L'\\');
            if (s != std::wstring::npos) p = p.substr(0, s);
            AppLog("open log dir: %ls", p.c_str());
            ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            SetNotice("日志目录：" + np::WideToUtf8(p) + "\\NextPerf.log 与 NextPerfHook.log", 0, 12000);
            break;
        }
        case A_INJECTFRONT: {
            // 语义是「注入到我刚才在玩的那个游戏」，所以**固定用记住的那个进程**。
            //
            // 原来先看 GetForegroundWindow、不是自己就用它 —— 两个问题：
            //   1) 点按钮时前台通常就是 NextPerf 自己（这正是最初报的 bug）；
            //   2) 更糟的是，如果前台是别的程序（终端、资源管理器、浏览器），
            //      我们会真的往那个进程里注入。测试里就撞上了：前台是终端，
            //      于是钩子被塞进了终端而不是游戏。
            // 跟踪线程已经把「上一个不是自己的前台窗口」记在 gApp.forePid 里，
            // 用户切到游戏的那一刻它就被记下了，用它既准确又不会误伤。
            DWORD pid = gApp.forePid;

            if (!pid) {
                SetNotice("还没有可注入的目标：先切到游戏窗口，或「添加游戏 exe」让程序自动盯住它",
                          1);
                break;
            }
            if (!ProcessAlive(pid)) {
                gApp.forePid = 0;
                gApp.foreName.clear();
                SetNotice("上一个前台进程已经退出了，请重新切换到游戏窗口", 1);
                break;
            }
            std::wstring nm = ProcessExeName(pid);
            if (nm.empty()) nm = L"(未知进程)";
            InjectInto(pid);   // 成功/失败都会自己 SetNotice 并写日志
            break;
        }
        default:
            break;
    }
    InvalidateRect(gMain, nullptr, FALSE);
}

// ---------------------------------------------------------------- 绘制
static void DrawCounterList(HDC mem) {
    static const COLORREF kHairline = RGB(0x1A, 0x1E, 0x24);
    int drawn = 0;
    for (size_t i = 0; i < gW.size(); ++i) {
        Widget& wd = gW[i];
        if (wd.type != W_CHECK && wd.type != W_GROUP) continue;
        RECT rc{};
        if (!VisibleRect(wd, (int)i, &rc)) continue;
        if (wd.type == W_GROUP) {
            // 组标题：白色半粗，左侧留少量空白
            Txt(mem, gFontHead, wd.text.c_str(), rc.left + S(4), rc.top, wd.w - S(16), gRowH,
                kText);
            ++drawn;
            continue;
        }
        bool on = (gApp.cfg.counters & wd.bit) != 0;
        if (wd.hover) FillR(mem, rc.left, rc.top, wd.w, gRowH - 2, kPanel2, 3);
        int cbx = rc.left + S(18), cby = rc.top + (gRowH - S(13)) / 2;
        FillR(mem, cbx, cby, S(13), S(13), on ? kText : kBorder, 3);
        if (on) {
            HPEN p = CreatePen(PS_SOLID, 2, kBack);
            HGDIOBJ op = SelectObject(mem, p);
            MoveToEx(mem, cbx + S(3), cby + S(7), nullptr);
            LineTo(mem, cbx + S(6), cby + S(10));
            LineTo(mem, cbx + S(11), cby + S(3));
            SelectObject(mem, op);
            DeleteObject(p);
        }
        Txt(mem, gFontBody, wd.text.c_str(), rc.left + S(38), rc.top, S(300), gRowH,
            on ? kText : kDim);
        // 细分隔线，让长列表有层次
        if (++drawn < gVisibleRows && rc.top + gRowH < gListTop + gVisibleRows * gRowH) {
            HPEN p = CreatePen(PS_SOLID, 1, kHairline);
            HGDIOBJ op = SelectObject(mem, p);
            MoveToEx(mem, rc.left + S(38), rc.top + gRowH - 1, nullptr);
            LineTo(mem, rc.left + wd.w - S(12), rc.top + gRowH - 1);
            SelectObject(mem, op);
            DeleteObject(p);
        }
    }

    // 迷你滚动条
    int maxScroll = std::max(1, gTotalRows - gVisibleRows);
    int trackX = S(20) + S(460) - S(6);
    int trackY = gListTop, trackH = gVisibleRows * gRowH;
    FillR(mem, trackX, trackY, S(3), trackH, kPanel2, 2);
    int thumbH = std::max(S(20), (int)(trackH * gVisibleRows / (float)gTotalRows));
    int thumbY = trackY + (int)((trackH - thumbH) * (gScroll / (float)maxScroll));
    FillR(mem, trackX, thumbY, S(3), thumbH, kDim, 2);
}

static void DrawGameList(HDC mem, int gameTop) {
    Txt(mem, gFontHead, L"游戏进程（选择 exe 后自动注入钩子）", S(20), gameTop - S(24), S(460), S(20),
        kText);
    for (auto& wd : gW) {
        if (wd.type != W_GAMEROW) continue;
        int slot = wd.index;
        if (slot < 0 || slot >= (int)gApp.games.size()) continue;
        const GameEntry& ge = gApp.games[slot];
        std::wstring name = ge.path;
        size_t p = name.find_last_of(L"\\/");
        std::wstring short_ = (p == std::wstring::npos) ? name : name.substr(p + 1);
        bool sel = (gSelGame == slot);
        FillR(mem, wd.x, wd.y, wd.w, wd.h, sel ? kPanel2 : kPanel, 4);
        if (sel) StrokeR(mem, wd.x, wd.y, wd.w, wd.h, kDim, 4);
        Txt(mem, gFontBody, short_.c_str(), wd.x + S(10), wd.y, wd.w - S(150), wd.h, kText);

        // 右侧状态：让用户一眼看出「为什么没数据」，而不是只看到「待注入」
        const wchar_t* st = L"待注入";
        if (ge.pid) {
            const NPTelemetry& t = gApp.telemetry;
            bool mine = (gApp.telemetryPid == ge.pid);
            bool live = t.attached && mine && (GetTickCount64() - t.tickMs) < 2500;
            if (live) st = L"● 读取中";
            else if (ge.injected) {
                // 注入成功但拿不到帧：把 API 摆出来，Vulkan/OpenGL 是设计边界
                if (mine && t.gfxApi == NP_API_VULKAN) st = L"● Vulkan · 无帧数据";
                else if (mine && t.gfxApi == NP_API_OPENGL) st = L"● OpenGL · 无帧数据";
                else st = L"● 已注入 · 未接管";
            } else st = L"● 运行中 · 注入失败";
        }
        Txt(mem, gFontBody, st, wd.x + wd.w - S(150), wd.y, S(140), wd.h,
            (ge.pid && ge.injected) ? kText : kDim);
    }
    if (gApp.games.empty())
        Txt(mem, gFontBody, L"（尚未添加游戏，点下方按钮选择 exe）", S(30), gameTop + S(2), S(400),
            S(24), kDim);
}

static void DrawPreview(HDC mem, int boxX, int boxY, int boxW, int boxH) {
    FillR(mem, boxX, boxY, boxW, boxH, kPanel, 6);
    if (!gPrevOk || !gPrevBmp.dc()) {
        Txt(mem, gFontBody, L"预览不可用", boxX, boxY, boxW, boxH, kDim, DT_CENTER | DT_VCENTER);
        return;
    }
    int bw = gPrevBmp.width(), bh = gPrevBmp.height();
    float fitW = (boxW - S(24)) / (float)bw;
    float fitH = (boxH - S(16)) / (float)bh;
    float fit = std::min(fitW, fitH);
    if (fit > 1.0f) fit = 1.0f;                 // 只缩小不放大，避免糊
    int dw = (int)(bw * fit), dh = (int)(bh * fit);
    if (dw < 1 || dh < 1) return;
    BLENDFUNCTION bf{};
    bf.BlendOp = AC_SRC_OVER;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;
    AlphaBlend(mem, boxX + (boxW - dw) / 2, boxY + (boxH - dh) / 2, dw, dh, gPrevBmp.dc(), 0, 0, bw,
               bh, bf);
}

static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CREATE:
            MakeFonts();
            SetTimer(h, 1, 250, nullptr);
            return 0;

        case WM_TIMER:
            UiRefresh();
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_DPICHANGED: {
            UINT dpi = HIWORD(w);
            if (dpi) {
                gS = dpi / 96.0f;
                MakeFonts();
                Layout();
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            int mx = GET_X_LPARAM(l), my = GET_Y_LPARAM(l);
            bool changed = false;
            for (size_t i = 0; i < gW.size(); ++i) {
                Widget& wd = gW[i];
                RECT rc{};
                bool hov = VisibleRect(wd, (int)i, &rc) && mx >= rc.left && mx < rc.right &&
                           my >= rc.top && my < rc.bottom;
                if (hov != wd.hover) { wd.hover = hov; changed = true; }
            }
            if (gDragging >= 0 && gDragging < (int)gW.size()) {
                Widget& wd = gW[gDragging];
                // ★ 这里的 120/130 必须和 WM_PAINT 里画轨道用的 sx = wd.x + S(120)、
                //   sw = wd.w - S(130) 完全一致。原来拖拽用的是 130/140，
                //   于是滑块永远比鼠标偏 10 像素（拖到最右边也到不了 100%）。
                float t = (float)(mx - (wd.x + S(120))) / (float)(wd.w - S(130));
                t = std::clamp(t, 0.0f, 1.0f);
                if (wd.type == W_SLIDER_F && wd.pFloat)
                    *wd.pFloat = wd.fmin + (wd.fmax - wd.fmin) * t;
                else if (wd.type == W_SLIDER_I && wd.pInt)
                    *wd.pInt = wd.imin + (int)((wd.imax - wd.imin) * t + 0.5f);
                changed = true;
            }
            if (changed) InvalidateRect(h, nullptr, FALSE);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            int mx = GET_X_LPARAM(l), my = GET_Y_LPARAM(l);
            Widget* wd = Hit(mx, my);
            if (!wd) return 0;
            if (wd->type == W_CHECK) {
                gApp.cfg.counters ^= wd->bit;
                InvalidateRect(h, nullptr, FALSE);
            } else if (wd->type == W_CYCLE && wd->pCycle) {
                int n = (int)wd->opts.size();
                if (n) {
                    int cur = 0;
                    for (int k = 0; k < n; ++k)
                        if (wd->vals.empty() ? (k == *wd->pCycle) : (wd->vals[k] == *wd->pCycle))
                            cur = k;
                    int nxt = (cur + 1) % n;
                    *wd->pCycle = wd->vals.empty() ? nxt : wd->vals[nxt];
                }
                InvalidateRect(h, nullptr, FALSE);
            } else if (wd->type == W_SLIDER_F || wd->type == W_SLIDER_I) {
                gDragging = (int)(wd - &gW[0]);
                InvalidateRect(h, nullptr, FALSE);
            } else if (wd->type == W_GAMEROW) {
                gSelGame = wd->index;
                InvalidateRect(h, nullptr, FALSE);
            } else if (wd->type == W_BUTTON) {
                DoAction(wd->action);
            }
            return 0;
        }

        case WM_LBUTTONUP:
            gDragging = -1;
            return 0;

        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(w);
            gScroll -= delta / (int)(60 * gS + 1);
            int maxScroll = std::max(0, gTotalRows - gVisibleRows);
            gScroll = std::clamp(gScroll, 0, maxScroll);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(h, &ps);
            RECT cr{};
            GetClientRect(h, &cr);
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP dib = CreateCompatibleBitmap(hdc, cr.right, cr.bottom);
            HGDIOBJ old = SelectObject(mem, dib);

            HBRUSH back = CreateSolidBrush(kBack);
            FillRect(mem, &cr, back);
            DeleteObject(back);

            const int LX = S(20);
            const int RX = S(500), RW = S(480);
            const int gameTop = gListTop + S(kListH) + S(16);

            Txt(mem, gFontHead, L"显示项目", LX, S(14), S(200), S(20), kText);
            Txt(mem, gFontSmall, L"滚轮滚动", LX + S(200), S(16), S(120), S(18), kDim);
            DrawCounterList(mem);
            DrawGameList(mem, gameTop);

            Txt(mem, gFontHead, L"叠加预览", RX, S(14), S(200), S(20), kText);
            DrawPreview(mem, RX, S(40), RW, S(200));

            // 分区标题按控件实际位置算，避免和滑杆/下拉框重叠
            int firstSliderY = 1 << 30, firstCycleY = 1 << 30;
            for (auto& wd : gW) {
                if (wd.type == W_SLIDER_F || wd.type == W_SLIDER_I)
                    firstSliderY = std::min(firstSliderY, wd.y);
                else if (wd.type == W_CYCLE)
                    firstCycleY = std::min(firstCycleY, wd.y);
            }
            Txt(mem, gFontHead, L"外观", RX, firstSliderY - S(26), S(200), S(20), kText);
            if (firstCycleY < (1 << 30))
                Txt(mem, gFontHead, L"行为", RX, firstCycleY - S(26), S(200), S(20), kText);

            for (auto& wd : gW) {
                if (wd.type == W_CHECK || wd.type == W_GAMEROW) continue;
                if (wd.type == W_BUTTON) {
                    FillR(mem, wd.x, wd.y, wd.w, wd.h, wd.hover ? kPanel2 : kPanel, 5);
                    StrokeR(mem, wd.x, wd.y, wd.w, wd.h, kBorder, 5);
                    Txt(mem, gFontBody, wd.text.c_str(), wd.x, wd.y, wd.w, wd.h, kText, DT_CENTER);
                } else if (wd.type == W_CYCLE) {
                    FillR(mem, wd.x, wd.y, wd.w, wd.h, wd.hover ? kPanel2 : kPanel, 5);
                    StrokeR(mem, wd.x, wd.y, wd.w, wd.h, kBorder, 5);
                    int idx = 0;
                    if (wd.pCycle) {
                        int n = (int)wd.opts.size();
                        for (int k = 0; k < n; ++k)
                            if (wd.vals.empty() ? (k == *wd.pCycle) : (wd.vals[k] == *wd.pCycle))
                                idx = k;
                    }
                    std::wstring v = (idx < (int)wd.opts.size()) ? wd.opts[idx] : L"-";
                    std::wstring line = wd.text + L"  " + v;
                    Txt(mem, gFontBody, line.c_str(), wd.x + S(10), wd.y, wd.w - S(20), wd.h,
                        kText);
                } else if (wd.type == W_SLIDER_F || wd.type == W_SLIDER_I) {
                    int sx = wd.x + S(120), sw = wd.w - S(130);
                    wchar_t val[32];
                    float t = 0;
                    if (wd.type == W_SLIDER_F && wd.pFloat) {
                        t = (*wd.pFloat - wd.fmin) / (wd.fmax - wd.fmin);
                        swprintf(val, 32, L"%.2f", *wd.pFloat);
                    } else if (wd.type == W_SLIDER_I && wd.pInt) {
                        t = (float)(*wd.pInt - wd.imin) / (float)(wd.imax - wd.imin);
                        swprintf(val, 32, L"%d", *wd.pInt);
                    }
                    t = std::clamp(t, 0.0f, 1.0f);
                    Txt(mem, gFontBody, wd.text.c_str(), wd.x, wd.y, S(115), wd.h, kDim);
                    Txt(mem, gFontBody, val, wd.x + S(60), wd.y, S(56), wd.h, kText, DT_RIGHT);
                    int sy = wd.y + wd.h / 2 - S(2);
                    FillR(mem, sx, sy, sw, S(4), kBorder, 2);
                    FillR(mem, sx, sy, (int)(sw * t), S(4), kText, 2);
                    FillR(mem, sx + (int)(sw * t) - S(4), wd.y + wd.h / 2 - S(6), S(8), S(12),
                          kText, 4);
                }
            }

            std::wstring st = np::Utf8ToWide(gApp.statusText);
            Txt(mem, gFontBody, st.c_str(), S(20), S(658), S(460), S(22), kDim);

            BitBlt(hdc, 0, 0, cr.right, cr.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(dib);
            DeleteDC(mem);
            EndPaint(h, &ps);
            return 0;
        }

        case WM_CLOSE:
            // 托盘图标没注册成功时，「缩到托盘」就等于让用户再也找不到程序
            // （用户反馈：退出只能进任务管理器）。那种情况下关闭 = 真退出。
            if (!TrayAvailable()) {
                PostMessageW(h, WM_COMMAND, NP_TRAY_EXIT, 0);
                return 0;
            }
            ShowWindow(h, SW_HIDE);
            return 0;

        case NP_WM_TRAY:
            AppTrayNotify(LOWORD(l));
            return 0;

        case WM_COMMAND:
            // lParam==0 表示来自托盘菜单（TrackPopupMenu 模态循环直接派发到这里）
            if (l == 0) {
                AppTrayCommand(LOWORD(w));
                return 0;
            }
            break;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

bool UiCreate(HINSTANCE inst, int cmdShow) {
    gInst = inst;

    // DPI：不开启的话整张窗口会被系统拉伸，文字发虚
    HDC scr = GetDC(nullptr);
    UINT dpi = (UINT)GetDeviceCaps(scr, LOGPIXELSX);
    ReleaseDC(nullptr, scr);
    gS = dpi / 96.0f;
    if (gS <= 0.0f) gS = 1.0f;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(kBack);
    wc.lpszClassName = L"NextPerfMainCls";
    RegisterClassExW(&wc);

    gMain = CreateWindowExW(0, L"NextPerfMainCls",
                            (L"NextPerf  [" + BuildStamp() + L"]").c_str(),
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                            CW_USEDEFAULT, CW_USEDEFAULT, S(kWinW), S(kWinH), nullptr, nullptr,
                            inst, nullptr);
    if (!gMain) return false;

    RECT cr{}, wr{};
    GetClientRect(gMain, &cr);
    GetWindowRect(gMain, &wr);
    int fw = (wr.right - wr.left) - (cr.right - cr.left);
    int fh = (wr.bottom - wr.top) - (cr.bottom - cr.top);
    SetWindowPos(gMain, nullptr, 0, 0, S(kWinW) + fw, S(kWinH) + fh, SWP_NOMOVE | SWP_NOZORDER);

    Layout();
    ShowWindow(gMain, cmdShow);
    UpdateWindow(gMain);
    return true;
}

HWND UiWindow() { return gMain; }

void UiDestroy() {
    gPrevBmp.Release();
    gPrevPanel.Shutdown();
    ReleaseFonts();
}

void UiRefresh() {
    static uint64_t last = 0;
    uint64_t now = GetTickCount64();
    if (now - last < 200) return;
    last = now;
    RenderPreview();
    if (gMain) InvalidateRect(gMain, nullptr, FALSE);
}

}  // namespace npa
