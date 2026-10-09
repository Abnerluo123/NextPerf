// 桌面叠加窗口（WS_EX_LAYERED 分层窗口，鼠标穿透）
//
// 适用：无边框 / 窗口化游戏，或者用户强制选择桌面叠加模式。
// 独占全屏下系统会绕过 DWM 合成，这类窗口会被盖住，
// 那种情况由注入钩子在游戏内部绘制（见 src/hook）。
#include "np_app.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "common/np_bitmap.h"
#include "common/np_build.h"

namespace npa {

static HWND gHwnd = nullptr;
static npb::PanelBitmap gBmp;
static np::PanelRenderer gPanel;
static np::PanelData gPd;
static bool gVisible = false;
static int  gW = 0, gH = 0;

struct DrawCtx {
    np::PanelRenderer* r;
    np::PanelData* d;
    NPConfig c;
};
static void DrawCb(ID2D1RenderTarget* rt, void* ud) {
    DrawCtx* dc = reinterpret_cast<DrawCtx*>(ud);
    dc->r->Render(rt, 0.0f, 0.0f, *dc->d, dc->c);
}

static LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CLOSE:
            return 0;
        case WM_DESTROY:
            return 0;
        default:
            return DefWindowProcW(h, m, w, l);
    }
}

bool OverlayInit(HINSTANCE inst) {
    if (gHwnd) return true;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"NextPerfOverlayCls";
    RegisterClassExW(&wc);

    gHwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
                                WS_EX_NOACTIVATE,
                            L"NextPerfOverlayCls", L"NextPerf Overlay", WS_POPUP, 0, 0, 8, 8,
                            nullptr, nullptr, inst, nullptr);
    if (!gHwnd) return false;
    if (!gPanel.Init()) {
        // ★ 渲染器初始化失败时必须把这个窗口销毁再返回 false：
        //   否则 gHwnd 一直非空，调用方（OverlayInit 返回 false 后）不会再调
        //   OverlayShutdown，这个分层窗口就永远挂在桌面上了，
        //   而且还会让 OverlayUpdate 以为叠加层可用。
        DestroyWindow(gHwnd);
        gHwnd = nullptr;
        return false;
    }
    ShowWindow(gHwnd, SW_SHOWNOACTIVATE);
    return true;
}

void OverlayShutdown() {
    if (gHwnd) { DestroyWindow(gHwnd); gHwnd = nullptr; }
    gBmp.Release();
    gPanel.Shutdown();
}

void OverlaySetVisible(bool v) {
    gVisible = v;
    if (!gHwnd) return;
    ShowWindow(gHwnd, v ? SW_SHOWNOACTIVATE : SW_HIDE);
}

void OverlayUpdate() {
    if (!gHwnd || !gVisible) return;
    if (!npb::Factory() && !npb::GfxInit()) return;

    // 叠加面板跟前台窗口所在显示器走：按该显示器 DPI 用物理像素光栅化，保证清晰
    HWND fg = GetForegroundWindow();
    UINT dpi = 96;
    HDC scr = GetDC(fg ? fg : nullptr);
    if (scr) { dpi = (UINT)GetDeviceCaps(scr, LOGPIXELSX); ReleaseDC(fg, scr); }
    if (dpi < 96) dpi = 96;
    npb::SetRasterScale(dpi / 96.0f);

    NPConfig cfg = gApp.cfg;
    np::BuildPanelData(gPd, cfg, gApp.sensors, gApp.telemetry, &gApp.hist);
    float pw = 0, ph = 0;
    gPanel.Measure(gPd, cfg, &pw, &ph);
    int w = (int)(pw + 3.0f), h = (int)(ph + 3.0f);
    if (w < 8 || h < 8) return;

    DrawCtx dc{&gPanel, &gPd, cfg};
    if (!gBmp.Render(w, h, DrawCb, &dc)) return;

    // 定位到「前台窗口所在显示器」的右上角；坐标与尺寸都用物理像素
    HMONITOR hm = MonitorFromWindow(fg, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hm, &mi)) {
        mi.rcMonitor.left = 0;
        mi.rcMonitor.top = 0;
        mi.rcMonitor.right = GetSystemMetrics(SM_CXSCREEN);
        mi.rcMonitor.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    int bw = gBmp.width(), bh = gBmp.height();
    float s = npb::RasterScale();
    int x = mi.rcMonitor.right - bw - (int)(cfg.offsetX * s);
    int y = mi.rcMonitor.top + (int)(cfg.offsetY * s);

    POINT pt{x, y};
    SIZE sz{bw, bh};
    POINT src{0, 0};
    // 背景/文字透明度已做进位图像素里（bgOpacity / textOpacity），窗口本身不再整体降透明
    BLENDFUNCTION bf{};
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(gHwnd, nullptr, &pt, &sz, gBmp.dc(), &src, 0, &bf, ULW_ALPHA);
    gW = bw; gH = bh;
}

}  // namespace npa
