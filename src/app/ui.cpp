// 主界面：Apple / macOS Dark 统一风格性能控制台（紧凑布局 · Direct2D + DirectWrite）
// 与 Apple Metal Performance HUD 共享同一中性深色层级与系统语义色，结构紧凑零死区。

#include "np_app.h"

#include <commdlg.h>
#include <d2d1.h>
#include <dwrite.h>
#include <shellapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "common/np_bitmap.h"
#include "common/np_build.h"
#include "etw/np_etw.h"

namespace npa {

namespace {

// ---------------------------------------------------------------- 紧凑窗口几何与 Apple Dark 调色板
constexpr int kWinW = 900;
constexpr int kWinH = 580;
constexpr float kHeaderH = 44.0f;
constexpr float kSideW = 166.0f;
constexpr float kStatusH = 26.0f;

// Tab 0「注入前台游戏」精确逻辑坐标（同步至 tests/verify_frontinject.py）
constexpr float kFrontInjectX = 754.0f;
constexpr float kFrontInjectY = 168.0f;
constexpr float kFrontInjectW = 116.0f;
constexpr float kFrontInjectH = 28.0f;

constexpr uint32_t kColBg        = 0x141416;   // Window Canvas
constexpr uint32_t kColSidebar   = 0x19191B;   // Header & Sidebar Chrome
constexpr uint32_t kColSurface   = 0x1C1C1E;   // Recessed List / Segmented Well
constexpr uint32_t kColCard      = 0x222225;   // Grouped Settings Card
constexpr uint32_t kColCardAlt   = 0x2B2B2F;   // Secondary Button / Track
constexpr uint32_t kColHover     = 0x333338;   // Hover & Active Pill
constexpr uint32_t kColBorder    = 0x2E2E32;   // 1px Apple Hairline Divider
constexpr uint32_t kColBorderHi  = 0x44444A;   // Elevated Hairline
constexpr uint32_t kColText      = 0xF5F5F7;   // Primary Label
constexpr uint32_t kColMuted     = 0x98989D;   // Secondary Label
constexpr uint32_t kColDim       = 0x636366;   // Tertiary Label
constexpr uint32_t kColBlue      = 0x0A84FF;   // Apple System Blue (Primary / Pre)
constexpr uint32_t kColBlueHi    = 0x2997FF;   // Apple Blue Light (Hover / Curve)
constexpr uint32_t kColGreen     = 0x30D158;   // Apple System Green (Live / GPU)
constexpr uint32_t kColOrange    = 0xFF9F0A;   // Apple System Orange (Warn)
constexpr uint32_t kColRed       = 0xFF453A;   // Apple System Red (Alert / Stop)

inline D2D1_COLOR_F C(uint32_t rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f,
                        ((rgb >> 8) & 0xFF) / 255.0f,
                        (rgb & 0xFF) / 255.0f, a);
}

struct RectF {
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
    D2D1_RECT_F d2d() const { return D2D1::RectF(x, y, x + w, y + h); }
};

// ---------------------------------------------------------------- Direct2D 画布
class UiCanvas {
public:
    bool InitFormats() {
        if (!npb::Factory() && !npb::GfxInit()) return false;
        IDWriteFactory* dw = npb::DWrite();
        if (!dw) return false;
        if (fmtBrand_) return true;

        auto mk = [&](const wchar_t* const* faces, int fn, float sz, DWRITE_FONT_WEIGHT wt,
                      IDWriteTextFormat** out, bool trim = true) {
            for (int i = 0; i < fn; ++i) {
                IDWriteTextFormat* tf = nullptr;
                if (SUCCEEDED(dw->CreateTextFormat(faces[i], nullptr, wt,
                                                   DWRITE_FONT_STYLE_NORMAL,
                                                   DWRITE_FONT_STRETCH_NORMAL, sz, L"zh-CN", &tf)) &&
                    tf) {
                    tf->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                    tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    if (trim) {
                        IDWriteInlineObject* sign = nullptr;
                        if (SUCCEEDED(dw->CreateEllipsisTrimmingSign(tf, &sign)) && sign) {
                            DWRITE_TRIMMING tr{};
                            tr.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
                            tf->SetTrimming(&tr, sign);
                            sign->Release();
                        }
                    }
                    *out = tf;
                    return true;
                }
                if (tf) tf->Release();
            }
            return false;
        };

        static const wchar_t* kUiFaces[] = {L"Segoe UI Variable Text", L"Microsoft YaHei UI",
                                            L"Microsoft YaHei"};
        static const wchar_t* kMonoFaces[] = {L"Cascadia Mono", L"Consolas", L"Courier New"};

        bool ok = mk(kUiFaces, 3, 14.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &fmtBrand_) &&
                  mk(kUiFaces, 3, 13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &fmtTitle_) &&
                  mk(kUiFaces, 3, 12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &fmtHead_) &&
                  mk(kUiFaces, 3, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, &fmtBody_) &&
                  mk(kUiFaces, 3, 11.0f, DWRITE_FONT_WEIGHT_NORMAL, &fmtSmall_) &&
                  mk(kUiFaces, 3, 10.0f, DWRITE_FONT_WEIGHT_NORMAL, &fmtMicro_) &&
                  mk(kMonoFaces, 3, 20.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &fmtMonoBig_) &&
                  mk(kMonoFaces, 3, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, &fmtMono_) &&
                  mk(kMonoFaces, 3, 10.0f, DWRITE_FONT_WEIGHT_NORMAL, &fmtMonoSm_);
        if (!ok) ReleaseFormats();
        return ok;
    }

    void ReleaseFormats() {
        auto rel = [](auto*& p) {
            if (p) { p->Release(); p = nullptr; }
        };
        rel(fmtBrand_);
        rel(fmtTitle_);
        rel(fmtHead_);
        rel(fmtBody_);
        rel(fmtSmall_);
        rel(fmtMicro_);
        rel(fmtMonoBig_);
        rel(fmtMono_);
        rel(fmtMonoSm_);
    }

    void ReleaseTarget() {
        if (br_) { br_->Release(); br_ = nullptr; }
        if (rt_) { rt_->Release(); rt_ = nullptr; }
        if (hdc_) {
            if (old_) SelectObject(hdc_, old_);
            DeleteDC(hdc_);
            hdc_ = nullptr;
            old_ = nullptr;
        }
        if (dib_) { DeleteObject(dib_); dib_ = nullptr; }
        w_ = h_ = 0;
    }

    void Shutdown() {
        ReleaseTarget();
        ReleaseFormats();
    }

    bool Ensure(int pxW, int pxH) {
        if (pxW <= 0 || pxH <= 0) return false;
        if (!InitFormats()) return false;
        if (rt_ && br_ && hdc_ && w_ == pxW && h_ == pxH) return true;

        ReleaseTarget();

        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = pxW;
        bi.bmiHeader.biHeight = -pxH;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        HDC scr = GetDC(nullptr);
        hdc_ = CreateCompatibleDC(scr);
        void* bits = nullptr;
        dib_ = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (scr) ReleaseDC(nullptr, scr);
        if (!hdc_ || !dib_) {
            ReleaseTarget();
            return false;
        }
        old_ = SelectObject(hdc_, dib_);

        auto props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
            96.0f, 96.0f);
        if (FAILED(npb::Factory()->CreateDCRenderTarget(&props, &rt_)) || !rt_) {
            ReleaseTarget();
            return false;
        }
        RECT rc{0, 0, pxW, pxH};
        if (FAILED(rt_->BindDC(hdc_, &rc))) {
            ReleaseTarget();
            return false;
        }
        rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        if (FAILED(rt_->CreateSolidColorBrush(C(kColText), &br_)) || !br_) {
            ReleaseTarget();
            return false;
        }
        w_ = pxW;
        h_ = pxH;
        return true;
    }

    ID2D1DCRenderTarget* rt() const { return rt_; }
    ID2D1SolidColorBrush* br(uint32_t rgb, float a = 1.0f) {
        br_->SetColor(C(rgb, a));
        return br_;
    }

    void FillR(float x, float y, float w, float h, uint32_t rgb, float a = 1.0f) {
        if (w <= 0 || h <= 0) return;
        rt_->FillRectangle(D2D1::RectF(x, y, x + w, y + h), br(rgb, a));
    }

    void FillRR(float x, float y, float w, float h, float r, uint32_t rgb, float a = 1.0f) {
        if (w <= 0 || h <= 0) return;
        rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), r, r),
                                  br(rgb, a));
    }

    void DrawRR(float x, float y, float w, float h, float r, uint32_t rgb, float a = 1.0f,
                float stroke = 1.0f) {
        if (w <= 0 || h <= 0) return;
        rt_->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(x + 0.5f, y + 0.5f, x + w - 0.5f, y + h - 0.5f), r, r),
            br(rgb, a), stroke);
    }

    void Card(float x, float y, float w, float h, float r = 9.0f,
              uint32_t fill = kColCard, uint32_t border = kColBorder) {
        FillRR(x, y, w, h, r, fill);
        DrawRR(x, y, w, h, r, border, 1.0f, 1.0f);
    }

    void Text(IDWriteTextFormat* f, const std::wstring& s, float x, float y, float w, float h,
              uint32_t rgb, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
              float a = 1.0f) {
        if (!f || w <= 0 || h <= 0 || s.empty()) return;
        f->SetTextAlignment(align);
        rt_->DrawText(s.c_str(), (UINT32)s.size(), f, D2D1::RectF(x, y, x + w, y + h), br(rgb, a),
                      D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    HDC hdc() const { return hdc_; }

    IDWriteTextFormat* fmtBrand_   = nullptr;
    IDWriteTextFormat* fmtTitle_   = nullptr;
    IDWriteTextFormat* fmtHead_    = nullptr;
    IDWriteTextFormat* fmtBody_    = nullptr;
    IDWriteTextFormat* fmtSmall_   = nullptr;
    IDWriteTextFormat* fmtMicro_   = nullptr;
    IDWriteTextFormat* fmtMonoBig_ = nullptr;
    IDWriteTextFormat* fmtMono_    = nullptr;
    IDWriteTextFormat* fmtMonoSm_  = nullptr;

private:
    HDC                  hdc_ = nullptr;
    HBITMAP              dib_ = nullptr;
    HGDIOBJ              old_ = nullptr;
    int                  w_ = 0, h_ = 0;
    ID2D1DCRenderTarget* rt_ = nullptr;
    ID2D1SolidColorBrush* br_ = nullptr;
};

// ---------------------------------------------------------------- 计数器定义（37 位全覆盖）
struct CounterDef {
    uint64_t       bit;
    const wchar_t* name;
    const wchar_t* tag;
};
struct CounterGroup {
    const wchar_t*    name;
    const CounterDef* items;
    int               n;
};

static const CounterDef kGFramerate[] = {
    {NP_C_FPS,       L"实时帧率 FPS",          L"核心"},
    {NP_C_FRAMETIME, L"帧生成间隔 Pre",        L"核心"},
    {NP_C_LOW1,      L"1% Low 帧率",           L"扩展"},
    {NP_C_LOW01,     L"0.1% Low 帧率",         L"扩展"},
};
static const CounterDef kGCpu[] = {
    {NP_C_CPU_USAGE, L"CPU 总占用率",          L"%"},
    {NP_C_CPU_TEMP,  L"CPU 封装温度",          L"°C"},
    {NP_C_CPU_CLOCK, L"CPU 核心有效频率",      L"MHz"},
    {NP_C_CPU_POWER, L"CPU 封装功耗",          L"W"},
    {NP_C_CPU_FRAME, L"CPU 帧耗时",            L"ms"},
    {NP_C_CPU_BUSY,  L"CPU Busy 活跃阶段",     L"高级"},
    {NP_C_CPU_WAIT,  L"CPU Wait 阻塞等待",     L"高级"},
};
static const CounterDef kGGpu[] = {
    {NP_C_GPU_NAME,    L"顶部显卡型号与代号",   L"顶栏"},
    {NP_C_GPU_USAGE,   L"GPU 核心占用率",       L"%"},
    {NP_C_GPU_TEMP,    L"GPU 核心温度",         L"°C"},
    {NP_C_GPU_HOTSPOT, L"GPU 热点 / 显存结温",  L"°C"},
    {NP_C_GPU_POWER,   L"GPU 整卡功耗",         L"W"},
    {NP_C_GPU_CLOCK,   L"核心 / 显存频率",      L"MHz"},
    {NP_C_FAN,         L"显卡风扇转速",         L"RPM"},
    {NP_C_GPU_FRAME,   L"GPU 执行耗时",         L"核心"},
    {NP_C_VRAM,        L"显存已用 [VRAM]",      L"核心"},
    {NP_C_GPU_FB,      L"显存控制器带宽",       L"%"},
    {NP_C_GPU_VID,     L"视频编解码引擎",       L"%"},
    {NP_C_GPU_BUS,     L"PCIe 总线占用",        L"%"},
    {NP_C_RT,          L"RT Core 光追负载",     L"DXR"},
    {NP_C_TENSOR,      L"Tensor / AI 负载",     L"AI"},
    {NP_C_AI_MODULES,  L"AI 超分 / 帧生成识别", L"模块"},
    {NP_C_RESOLUTION,  L"顶部渲染分辨率 [WxH]", L"顶栏"},
};
static const CounterDef kGSystem[] = {
    {NP_C_RAM,        L"系统内存 Mem",         L"核心"},
    {NP_C_API,        L"图形 API 标识",        L"扩展"},
    {NP_C_DRAWS,      L"Draw / Dispatch 计数", L"D3D12"},
    {NP_C_SENSOR_SRC, L"当前活跃传感器源",     L"诊断"},
    {NP_C_BOTTLENECK, L"实时性能瓶颈诊断",     L"分析"},
    {NP_C_CLOCK_TIME, L"系统实时时钟",         L"时钟"},
};
static const CounterDef kGChart[] = {
    {NP_C_GRAPH,         L"Metal 双通道示波器", L"主图"},
    {NP_C_CHART_USAGE,   L"扩展 CPU/GPU 小图",  L"附加"},
    {NP_C_CHART_FPS,     L"扩展 Low 帧率小图",  L"附加"},
    {NP_C_CHART_LATENCY, L"扩展延迟拆分小图",   L"附加"},
};

static const CounterGroup kGroups[] = {
    {L"帧率与核心",     kGFramerate, (int)(sizeof(kGFramerate) / sizeof(kGFramerate[0]))},
    {L"CPU 处理器",     kGCpu,       (int)(sizeof(kGCpu) / sizeof(kGCpu[0]))},
    {L"GPU 图形与硬件", kGGpu,       (int)(sizeof(kGGpu) / sizeof(kGGpu[0]))},
    {L"系统与引擎诊断", kGSystem,    (int)(sizeof(kGSystem) / sizeof(kGSystem[0]))},
    {L"波形图表开关",   kGChart,     (int)(sizeof(kGChart) / sizeof(kGChart[0]))},
};
constexpr int kGroupN = (int)(sizeof(kGroups) / sizeof(kGroups[0]));
bool gGrpOpen[kGroupN] = {true, true, true, true, true};

// ---------------------------------------------------------------- 命中模型与动作
enum HitKind {
    H_NONE = 0,
    H_NAV_TAB,
    H_ACTION,
    H_GAME_ROW,
    H_GAME_INJ,
    H_GAME_REM,
    H_PRESET,
    H_GROUP_HDR,
    H_COUNTER,
    H_LAYOUT_MODE,
    H_THEME,
    H_CORNER,
    H_OPT_BIT,
    H_SLIDER,
    H_SYS_CYCLE,
    H_SYS_TOGGLE,
};

enum ActionId {
    ACT_NONE = 0,
    ACT_TOGGLE_MON,
    ACT_TOGGLE_OV,
    ACT_BENCH,
    ACT_SAVE,
    ACT_MINIMIZE,
    ACT_CLOSE,
    ACT_FORGET_LEARNED,
    ACT_ADD_GAME,
    ACT_SCAN_GAMES,
    ACT_INJECT_FRONT,
    ACT_COPY_DIAG,
    ACT_RESET_HIST,
    ACT_OPEN_LOGDIR,
    ACT_OPEN_CSVDIR,
    ACT_ELEVATE,
    ACT_QUIT,
};

struct HitItem {
    HitKind  kind = H_NONE;
    RectF    rc{};
    int      arg = 0;
    uint64_t bit = 0;
    int      sliderIdx = -1;
};

struct SliderSpec {
    const wchar_t* label;
    bool           isFloat;
    float          fmin, fmax;
    int            imin, imax;
};

static const SliderSpec kSliders[7] = {
    {L"面板缩放",   true,  0.60f, 2.00f, 0, 0},
    {L"字体大小",   false, 0, 0, 10, 26},
    {L"波形高度",   false, 0, 0, 30, 120},
    {L"背景不透明", true,  0.00f, 1.00f, 0, 0},
    {L"文字不透明", true,  0.20f, 1.00f, 0, 0},
    {L"水平边距",   false, 0, 0, 0, 400},
    {L"垂直边距",   false, 0, 0, 0, 400},
};

struct CounterVisualRow {
    bool              isGroup = false;
    int               groupIdx = 0;
    const CounterDef* cols[2] = {nullptr, nullptr};
};

// 全局 UI 状态
static HWND               gMain = nullptr;
static HINSTANCE          gInst = nullptr;
static float              gS = 1.0f;
static UiCanvas           gCanvas;
static np::PanelRenderer  gPreviewPanel;
static int                gActiveTab = 0;
static int                gGameScroll = 0;
static int                gSelGame = -1;
static int                gCounterScroll = 0;
static int                gCounterTotalRows = 0;
static int                gHoverIdx = -1;
static int                gDraggingSlider = -1;
static RectF              gSliderTrack[7]{};
static bool               gTrackingLeave = false;
static std::vector<HitItem> gHits;
static std::vector<CounterVisualRow> gCounterRows;

static float CurrentClientScale(HWND h) {
    RECT cr{};
    if (h && GetClientRect(h, &cr) && cr.right > 0)
        return (float)cr.right / (float)kWinW;
    return gS > 0.0f ? gS : 1.0f;
}

static int S(int v) { return (int)(v * gS + 0.5f); }

static void ApplyDarkFrame(HWND h) {
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) return;
    using PFnAttr = HRESULT(WINAPI*)(HWND, DWORD, const void*, DWORD);
    using PFnExtend = HRESULT(WINAPI*)(HWND, const void*);
    auto setAttr = reinterpret_cast<PFnAttr>(GetProcAddress(dwm, "DwmSetWindowAttribute"));
    auto extend = reinterpret_cast<PFnExtend>(GetProcAddress(dwm, "DwmExtendFrameIntoClientArea"));
    if (setAttr) {
        BOOL dark = TRUE;
        setAttr(h, 20, &dark, sizeof(dark));       // DWMWA_USE_IMMERSIVE_DARK_MODE
        DWORD corner = 2;                          // DWMWCP_ROUND
        setAttr(h, 33, &corner, sizeof(corner));   // DWMWA_WINDOW_CORNER_PREFERENCE
        COLORREF border = RGB(0x2E, 0x2E, 0x32);
        setAttr(h, 34, &border, sizeof(border));   // DWMWA_BORDER_COLOR
        COLORREF cap = RGB(0x14, 0x14, 0x16);
        setAttr(h, 35, &cap, sizeof(cap));         // DWMWA_CAPTION_COLOR
    }
    if (extend) {
        struct {
            int left, right, top, bottom;
        } m{0, 0, 1, 0};
        extend(h, &m);
    }
    FreeLibrary(dwm);
}

static void RebuildCounterRows() {
    gCounterRows.clear();
    for (int gi = 0; gi < kGroupN; ++gi) {
        CounterVisualRow hdr{};
        hdr.isGroup = true;
        hdr.groupIdx = gi;
        gCounterRows.push_back(hdr);
        if (!gGrpOpen[gi]) continue;
        for (int i = 0; i < kGroups[gi].n; i += 2) {
            CounterVisualRow row{};
            row.isGroup = false;
            row.groupIdx = gi;
            row.cols[0] = &kGroups[gi].items[i];
            if (i + 1 < kGroups[gi].n) row.cols[1] = &kGroups[gi].items[i + 1];
            gCounterRows.push_back(row);
        }
    }
    gCounterTotalRows = (int)gCounterRows.size();
    constexpr int kVisRows = 11;
    int maxSc = std::max(0, gCounterTotalRows - kVisRows);
    gCounterScroll = std::clamp(gCounterScroll, 0, maxSc);
}

static int CountEnabledCounters() {
    uint64_t c = gApp.cfg.counters & NP_PRESET_FULL;
    int n = 0;
    while (c) {
        c &= (c - 1);
        ++n;
    }
    return n;
}

static void UpdateSliderValue(int idx, float mx) {
    if (idx < 0 || idx >= 7) return;
    const RectF& tr = gSliderTrack[idx];
    if (tr.w <= 1.0f) return;
    float t = std::clamp((mx - tr.x) / tr.w, 0.0f, 1.0f);
    const SliderSpec& sp = kSliders[idx];
    NPConfig& c = gApp.cfg;
    switch (idx) {
        case 0: c.scale = sp.fmin + (sp.fmax - sp.fmin) * t; break;
        case 1: c.fontHeight = (uint32_t)(sp.imin + (int)((sp.imax - sp.imin) * t + 0.5f)); break;
        case 2: c.graphHeight = (uint32_t)(sp.imin + (int)((sp.imax - sp.imin) * t + 0.5f)); break;
        case 3: c.bgOpacity = sp.fmin + (sp.fmax - sp.fmin) * t; break;
        case 4: c.textOpacity = sp.fmin + (sp.fmax - sp.fmin) * t; break;
        case 5: c.offsetX = sp.imin + (int)((sp.imax - sp.imin) * t + 0.5f); break;
        case 6: c.offsetY = sp.imin + (int)((sp.imax - sp.imin) * t + 0.5f); break;
        default: break;
    }
    AppPublish();
}

static float GetSliderRatio(int idx, std::wstring* valOut) {
    const SliderSpec& sp = kSliders[idx];
    const NPConfig& c = gApp.cfg;
    wchar_t buf[32]{};
    float ratio = 0.0f;
    switch (idx) {
        case 0:
            ratio = (c.scale - sp.fmin) / (sp.fmax - sp.fmin);
            swprintf(buf, 32, L"%.2fx", (double)c.scale);
            break;
        case 1:
            ratio = (float)((int)c.fontHeight - sp.imin) / (float)(sp.imax - sp.imin);
            swprintf(buf, 32, L"%upx", c.fontHeight);
            break;
        case 2:
            ratio = (float)((int)c.graphHeight - sp.imin) / (float)(sp.imax - sp.imin);
            swprintf(buf, 32, L"%upx", c.graphHeight);
            break;
        case 3:
            ratio = (c.bgOpacity - sp.fmin) / (sp.fmax - sp.fmin);
            swprintf(buf, 32, L"%.0f%%", (double)c.bgOpacity * 100.0);
            break;
        case 4:
            ratio = (c.textOpacity - sp.fmin) / (sp.fmax - sp.fmin);
            swprintf(buf, 32, L"%.0f%%", (double)c.textOpacity * 100.0);
            break;
        case 5:
            ratio = (float)(c.offsetX - sp.imin) / (float)(sp.imax - sp.imin);
            swprintf(buf, 32, L"%dpx", c.offsetX);
            break;
        case 6:
            ratio = (float)(c.offsetY - sp.imin) / (float)(sp.imax - sp.imin);
            swprintf(buf, 32, L"%dpx", c.offsetY);
            break;
        default: break;
    }
    if (valOut) *valOut = buf;
    return std::clamp(ratio, 0.0f, 1.0f);
}

static std::wstring ShortExeName(const GameEntry& g) {
    const std::wstring& src = g.path.empty() ? g.name : g.path;
    size_t sl = src.find_last_of(L"\\/");
    return (sl == std::wstring::npos) ? src : src.substr(sl + 1);
}

static std::wstring TrimGpuName(const char* raw) {
    if (!raw || !raw[0]) return L"未检测到 GPU";
    std::string s(raw);
    const char* prefixes[] = {"NVIDIA GeForce ", "NVIDIA ", "AMD Radeon(TM) ", "AMD Radeon ",
                              "Intel(R) Arc(TM) ", "Intel(R) ", "Intel "};
    for (const char* p : prefixes) {
        size_t len = strlen(p);
        if (s.compare(0, len, p) == 0) {
            s.erase(0, len);
            break;
        }
    }
    if (s.size() > 16) {
        const char* suf = " GPU";
        size_t sl = strlen(suf);
        if (s.size() > sl && s.compare(s.size() - sl, sl, suf) == 0) s.erase(s.size() - sl);
    }
    return np::Utf8ToWide(s);
}

// ---------------------------------------------------------------- macOS 原生风格组件绘制
static void DrawAppleSwitch(UiCanvas& cv, float sx, float sy, bool on, bool hovered) {
    float sw = 32.0f, sh = 18.0f;
    uint32_t track = on ? (hovered ? kColBlueHi : kColBlue) : (hovered ? 0x45454Bu : 0x38383Du);
    cv.FillRR(sx, sy, sw, sh, 9.0f, track);
    if (!on) cv.DrawRR(sx, sy, sw, sh, 9.0f, kColBorderHi, 0.6f);
    float kx = on ? (sx + 23.0f) : (sx + 9.0f);
    cv.rt()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, sy + 9.0f), 6.2f, 6.2f),
                         cv.br(0xFFFFFFu));
}

static void DrawQuietBtn(UiCanvas& cv, const HitItem& item, const std::wstring& txt,
                         bool hovered, bool primary = false, uint32_t fgOverride = 0) {
    if (primary) {
        cv.FillRR(item.rc.x, item.rc.y, item.rc.w, item.rc.h, 7.0f,
                  hovered ? kColBlueHi : kColBlue);
        cv.Text(cv.fmtSmall_, txt, item.rc.x + 6.0f, item.rc.y, item.rc.w - 12.0f, item.rc.h,
                0xFFFFFFu, DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }
    uint32_t bg = hovered ? kColHover : kColCardAlt;
    uint32_t fg = fgOverride ? fgOverride : kColText;
    cv.FillRR(item.rc.x, item.rc.y, item.rc.w, item.rc.h, 7.0f, bg);
    cv.DrawRR(item.rc.x, item.rc.y, item.rc.w, item.rc.h, 7.0f,
              hovered ? kColBorderHi : kColBorder);
    cv.Text(cv.fmtSmall_, txt, item.rc.x + 6.0f, item.rc.y, item.rc.w - 12.0f, item.rc.h, fg,
            DWRITE_TEXT_ALIGNMENT_CENTER);
}

static void DrawNavIcon(UiCanvas& cv, int tab, float x, float y, uint32_t col) {
    auto* br = cv.br(col);
    switch (tab) {
        case 0:
            cv.DrawRR(x, y + 1.0f, 13.0f, 11.0f, 2.5f, col, 0.9f, 1.3f);
            cv.rt()->DrawLine(D2D1::Point2F(x + 2.0f, y + 8.0f), D2D1::Point2F(x + 5.0f, y + 4.5f), br, 1.3f);
            cv.rt()->DrawLine(D2D1::Point2F(x + 5.0f, y + 4.5f), D2D1::Point2F(x + 8.0f, y + 8.5f), br, 1.3f);
            cv.rt()->DrawLine(D2D1::Point2F(x + 8.0f, y + 8.5f), D2D1::Point2F(x + 11.0f, y + 4.0f), br, 1.3f);
            break;
        case 1:
            cv.DrawRR(x, y + 1.5f, 13.0f, 10.0f, 2.5f, col, 0.9f, 1.3f);
            cv.FillRR(x + 6.0f, y + 3.5f, 5.0f, 4.5f, 1.2f, col, 0.85f);
            break;
        case 2:
            for (int i = 0; i < 3; ++i) {
                float ly = y + 2.5f + i * 4.0f;
                cv.rt()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + 1.5f, ly), 1.3f, 1.3f), br);
                cv.rt()->DrawLine(D2D1::Point2F(x + 4.5f, ly), D2D1::Point2F(x + 12.5f, ly), br, 1.3f);
            }
            break;
        default:
            for (int i = 0; i < 3; ++i) {
                float ly = y + 2.5f + i * 4.0f;
                float kx = (i == 1) ? (x + 8.5f) : (x + 4.0f);
                cv.rt()->DrawLine(D2D1::Point2F(x + 0.5f, ly), D2D1::Point2F(x + 12.5f, ly), br, 1.2f);
                cv.rt()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, ly), 2.0f, 2.0f), br);
            }
            break;
    }
}

static void DrawHeaderAndSidebar(UiCanvas& cv, const std::vector<GameEntry>& gamesSnap) {
    cv.FillR(0, 0, kWinW, kHeaderH, kColSidebar);
    cv.FillR(0, kHeaderH, kSideW, kWinH - kHeaderH - kStatusH, kColSidebar);
    cv.FillR(0, kWinH - kStatusH, kWinW, kStatusH, kColSidebar);
    cv.FillR(0, kHeaderH - 1.0f, kWinW, 1.0f, kColBorder);
    cv.FillR(kSideW - 1.0f, kHeaderH, 1.0f, kWinH - kHeaderH - kStatusH, kColBorder);
    cv.FillR(0, kWinH - kStatusH, kWinW, 1.0f, kColBorder);

    // 品牌徽标
    cv.FillRR(14.0f, 11.0f, 22.0f, 22.0f, 6.0f, kColCardAlt);
    cv.DrawRR(14.0f, 11.0f, 22.0f, 22.0f, 6.0f, kColBorderHi);
    cv.rt()->DrawLine(D2D1::Point2F(18.0f, 25.0f), D2D1::Point2F(23.0f, 18.0f), cv.br(kColBlueHi), 1.5f);
    cv.rt()->DrawLine(D2D1::Point2F(23.0f, 18.0f), D2D1::Point2F(27.0f, 24.0f), cv.br(kColBlueHi), 1.5f);
    cv.rt()->DrawLine(D2D1::Point2F(27.0f, 24.0f), D2D1::Point2F(32.0f, 16.0f), cv.br(kColGreen), 1.5f);
    cv.Text(cv.fmtBrand_, L"NextPerf", 42.0f, 6.0f, 100.0f, 32.0f, kColText);

    // 顶部右侧克制工具控件
    struct HBtn { float x, w; int act; };
    const HBtn hbtns[] = {
        {496.0f, 86.0f, ACT_TOGGLE_MON},
        {590.0f, 86.0f, ACT_TOGGLE_OV},
        {684.0f, 92.0f, ACT_BENCH},
        {816.0f, 30.0f, ACT_MINIMIZE},
        {854.0f, 30.0f, ACT_CLOSE},
    };
    for (const auto& b : hbtns) {
        HitItem it{H_ACTION, {b.x, 8.0f, b.w, 28.0f}, b.act, 0, -1};
        int idx = (int)gHits.size();
        gHits.push_back(it);
        bool hov = (gHoverIdx == idx);
        if (b.act == ACT_TOGGLE_MON) {
            cv.FillRR(b.x, 8.0f, b.w, 28.0f, 7.0f, hov ? kColHover : kColCard);
            cv.DrawRR(b.x, 8.0f, b.w, 28.0f, 7.0f, kColBorder);
            cv.rt()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(b.x + 14.0f, 22.0f), 3.2f, 3.2f),
                                 cv.br(gApp.monitoring ? kColGreen : kColOrange));
            cv.Text(cv.fmtSmall_, gApp.monitoring ? L"监控中" : L"已暂停",
                    b.x + 22.0f, 8.0f, b.w - 28.0f, 28.0f, kColText);
        } else if (b.act == ACT_TOGGLE_OV) {
            bool ovOn = AppOverlayVisible();
            cv.FillRR(b.x, 8.0f, b.w, 28.0f, 7.0f, hov ? kColHover : kColCard);
            cv.DrawRR(b.x, 8.0f, b.w, 28.0f, 7.0f, kColBorder);
            cv.rt()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(b.x + 13.0f, 22.0f), 3.2f, 3.2f),
                                 cv.br(ovOn ? kColBlueHi : kColDim));
            cv.Text(cv.fmtSmall_, ovOn ? L"HUD 显示" : L"HUD 隐藏",
                    b.x + 21.0f, 8.0f, b.w - 26.0f, 28.0f, ovOn ? kColText : kColMuted);
        } else if (b.act == ACT_BENCH) {
            bool rec = BenchmarkActive();
            cv.FillRR(b.x, 8.0f, b.w, 28.0f, 7.0f,
                      rec ? kColRed : (hov ? kColHover : kColCard), rec ? 0.18f : 1.0f);
            cv.DrawRR(b.x, 8.0f, b.w, 28.0f, 7.0f, rec ? kColRed : kColBorder,
                      rec ? 0.7f : 1.0f);
            cv.Text(cv.fmtSmall_, rec ? L"■ 停止录制" : L"录制基准",
                    b.x + 6.0f, 8.0f, b.w - 12.0f, 28.0f, rec ? kColRed : kColText,
                    DWRITE_TEXT_ALIGNMENT_CENTER);
        } else if (b.act == ACT_MINIMIZE) {
            cv.FillRR(b.x, 8.0f, b.w, 28.0f, 6.0f, hov ? kColHover : kColSidebar);
            cv.rt()->DrawLine(D2D1::Point2F(b.x + 9.0f, 22.0f),
                              D2D1::Point2F(b.x + 21.0f, 22.0f),
                              cv.br(hov ? kColText : kColMuted), 1.4f);
        } else if (b.act == ACT_CLOSE) {
            cv.FillRR(b.x, 8.0f, b.w, 28.0f, 6.0f, hov ? 0xC9342Du : kColSidebar);
            float cx = b.x + b.w * 0.5f, cy = 22.0f;
            auto* bClose = cv.br(hov ? 0xFFFFFFu : kColMuted);
            cv.rt()->DrawLine(D2D1::Point2F(cx - 4.0f, cy - 4.0f),
                              D2D1::Point2F(cx + 4.0f, cy + 4.0f), bClose, 1.4f);
            cv.rt()->DrawLine(D2D1::Point2F(cx + 4.0f, cy - 4.0f),
                              D2D1::Point2F(cx - 4.0f, cy + 4.0f), bClose, 1.4f);
        }
    }

    // 侧边导航栏 (166px 紧凑侧栏)
    const wchar_t* tabLabels[4] = {
        L"概览与游戏",
        L"HUD 外观",
        L"监控指标",
        L"系统与诊断",
    };
    for (int t = 0; t < 4; ++t) {
        float ty = 58.0f + t * 36.0f;
        HitItem it{H_NAV_TAB, {10.0f, ty, 146.0f, 32.0f}, t, 0, -1};
        int idx = (int)gHits.size();
        gHits.push_back(it);
        bool act = (gActiveTab == t);
        bool hov = (gHoverIdx == idx);
        if (act) cv.FillRR(10.0f, ty, 146.0f, 32.0f, 7.0f, kColHover);
        else if (hov) cv.FillRR(10.0f, ty, 146.0f, 32.0f, 7.0f, kColCard);
        DrawNavIcon(cv, t, 20.0f, ty + 9.5f, act ? kColBlueHi : kColMuted);
        cv.Text(act ? cv.fmtHead_ : cv.fmtBody_, tabLabels[t], 40.0f, ty, 108.0f, 32.0f,
                act ? kColText : kColMuted);
    }

    // 侧栏底部紧凑硬件卡 (x=10, y=366, w=146, h=140)
    const NPSensors& s = gApp.sensors;
    cv.Card(10.0f, 366.0f, 146.0f, 140.0f, 8.0f, kColCard, kColBorder);
    cv.Text(cv.fmtHead_, TrimGpuName(s.gpuName), 18.0f, 374.0f, 130.0f, 18.0f, kColText);

    wchar_t buf[128]{};
    swprintf(buf, 128, L"CPU %3.0f%% %4.0fM", (double)std::max(0.0f, s.cpuUsage),
             (double)std::max(0.0f, s.cpuClock));
    cv.Text(cv.fmtMonoSm_, buf, 18.0f, 398.0f, 130.0f, 16.0f, kColMuted);

    swprintf(buf, 128, L"GPU %3.0f%%  %3.0f°C", (double)std::max(0.0f, s.gpuUsage),
             (double)std::max(0.0f, s.gpuTemp));
    cv.Text(cv.fmtMonoSm_, buf, 18.0f, 416.0f, 130.0f, 16.0f, kColMuted);

    swprintf(buf, 128, L"显存 %4.1f/%.1fG", (double)std::max(0.0f, s.vramUsedGB),
             (double)std::max(0.0f, s.vramTotalGB));
    cv.Text(cv.fmtMonoSm_, buf, 18.0f, 434.0f, 130.0f, 16.0f, kColMuted);

    swprintf(buf, 128, L"内存 %4.1f/%.1fG", (double)std::max(0.0f, s.ramUsedGB),
             (double)std::max(0.0f, s.ramTotalGB));
    cv.Text(cv.fmtMonoSm_, buf, 18.0f, 452.0f, 130.0f, 16.0f, kColMuted);

    cv.FillR(18.0f, 474.0f, 130.0f, 1.0f, kColBorder);
    bool embedOk = HookEmbedInfo(nullptr, nullptr);
    cv.rt()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(22.0f, 489.0f), 3.0f, 3.0f),
                         cv.br(embedOk ? kColGreen : kColOrange));
    swprintf(buf, 128, L"%ls · %ls", embedOk ? L"单文件便携" : L"外置DLL",
             gApp.isAdmin ? L"管理员" : L"标准");
    cv.Text(cv.fmtMicro_, buf, 30.0f, 480.0f, 120.0f, 18.0f, kColMuted);

    // 侧栏底部保存配置
    {
        HitItem saveHit{H_ACTION, {10.0f, 514.0f, 146.0f, 28.0f}, ACT_SAVE, 0, -1};
        int sIdx = (int)gHits.size();
        gHits.push_back(saveHit);
        DrawQuietBtn(cv, saveHit, L"保存当前配置", gHoverIdx == sIdx);
    }

    // 底部极简状态栏
    uint32_t dotCol = kColDim;
    if (gApp.noticeLevel && !gApp.notice.empty()) dotCol = kColRed;
    else if (gApp.monitoring && gApp.telemetryLive) dotCol = kColGreen;
    else if (gApp.monitoring) dotCol = kColBlueHi;
    else dotCol = kColOrange;
    cv.rt()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(16.0f, kWinH - kStatusH * 0.5f), 3.2f, 3.2f),
                         cv.br(dotCol));
    std::wstring stText = np::Utf8ToWide(gApp.statusText);
    cv.Text(cv.fmtSmall_, stText, 26.0f, kWinH - kStatusH, 620.0f, kStatusH,
            (gApp.noticeLevel && !gApp.notice.empty()) ? kColRed : kColMuted);
    cv.Text(cv.fmtMicro_, L"Build " + BuildStamp() + L" · Ctrl+Shift+O",
            650.0f, kWinH - kStatusH, 236.0f, kStatusH, kColDim,
            DWRITE_TEXT_ALIGNMENT_TRAILING);
}

static void DrawDualSparkline(UiCanvas& cv, float x, float y, float w, float h, bool live) {
    const NPHistory& hs = gApp.hist;
    if (hs.count < 2) return;
    uint32_t take = std::min<uint32_t>(hs.count, 56);
    uint32_t start = (hs.write + NP_HIST_CAP - take) % NP_HIST_CAP;

    float vmax = 0.0f;
    for (uint32_t i = 0; i < take; ++i) {
        uint32_t idx = (start + i) % NP_HIST_CAP;
        if (live) {
            vmax = std::max(vmax, hs.latFrame[idx]);
            vmax = std::max(vmax, hs.latGpu[idx]);
        } else {
            vmax = std::max(vmax, hs.usageCpu[idx]);
            vmax = std::max(vmax, hs.usageGpu[idx]);
        }
    }
    vmax = live ? std::max(8.0f, vmax * 1.15f) : std::clamp(vmax * 1.40f, 25.0f, 100.0f);

    auto* gridBr = cv.br(0xFFFFFFu, 0.10f);
    for (int i = 0; i <= 4; ++i) {
        float gx = x + w * (float)i / 4.0f;
        cv.rt()->DrawLine(D2D1::Point2F(gx, y), D2D1::Point2F(gx, y + h), gridBr, 1.0f);
    }
    cv.rt()->DrawLine(D2D1::Point2F(x, y + h), D2D1::Point2F(x + w, y + h), cv.br(0xFFFFFFu, 0.25f),
                      1.0f);

    auto plot = [&](const float* arr, uint32_t col) {
        auto* br = cv.br(col, 0.95f);
        float px0 = 0.0f, py0 = 0.0f;
        for (uint32_t i = 0; i < take; ++i) {
            float v = arr[(start + i) % NP_HIST_CAP];
            float px = x + w * ((float)i / (float)(take - 1));
            float py = y + h * (1.0f - std::clamp(v / vmax, 0.0f, 1.0f));
            if (i > 0) cv.rt()->DrawLine(D2D1::Point2F(px0, py0), D2D1::Point2F(px, py), br, 1.35f);
            px0 = px;
            py0 = py;
        }
    };
    if (live) {
        plot(hs.latFrame, kColBlueHi);
        plot(hs.latGpu, kColGreen);
    } else {
        plot(hs.usageCpu, kColBlueHi);
        plot(hs.usageGpu, kColGreen);
    }
}

static const wchar_t* ApiShortLabel(uint32_t api) {
    switch (api) {
        case NP_API_D3D12:  return L"D3D12";
        case NP_API_D3D11:  return L"D3D11";
        case NP_API_VULKAN: return L"Vulkan";
        case NP_API_D3D9:   return L"D3D9";
        case NP_API_OPENGL: return L"OpenGL";
        default:            return L"Desktop";
    }
}

static void DrawTabOverview(UiCanvas& cv, const std::vector<GameEntry>& gamesSnap) {
    const NPSensors& s = gApp.sensors;
    const NPTelemetry& t = gApp.telemetry;
    bool live = (gApp.telemetryLive && t.attached) || gApp.cfg.simulate != 0;
    wchar_t buf[128]{};

    // 1) 顶部 4 列紧凑遥测卡 (x=178, y=56, w=708, h=92)
    cv.Card(178.0f, 56.0f, 708.0f, 92.0f);
    for (int i = 1; i <= 3; ++i) {
        float vx = 178.0f + i * 177.0f;
        cv.FillR(vx, 66.0f, 1.0f, 72.0f, kColBorder);
    }

    // Col 0: FPS / 波形
    {
        float x = 190.0f, y = 64.0f, w = 153.0f;
        cv.Text(cv.fmtSmall_, L"实时帧率", x, y, 80.0f, 15.0f, kColMuted);
        cv.Text(cv.fmtMonoSm_, gApp.cfg.simulate ? L"Sim" : ApiShortLabel(t.gfxApi),
                x + 80.0f, y, w - 80.0f, 15.0f, live ? kColBlueHi : kColDim,
                DWRITE_TEXT_ALIGNMENT_TRAILING);
        if (live && t.fps > 0.1f) {
            swprintf(buf, 128, L"%.1f", (double)t.fps);
            cv.Text(cv.fmtMonoBig_, buf, x, y + 16.0f, 88.0f, 24.0f, kColText);
            swprintf(buf, 128, L"Pre %.1f · 1%% %.0f",
                     (double)(t.frameMsAvg > 0.05f ? t.frameMsAvg : t.frameMs),
                     (double)t.fpsLow1);
            cv.Text(cv.fmtMicro_, buf, x, y + 41.0f, w, 15.0f, kColMuted);
        } else {
            cv.Text(cv.fmtMonoBig_, L"--", x, y + 16.0f, 88.0f, 24.0f, kColDim);
            cv.Text(cv.fmtMicro_, L"桌面 CPU / GPU 双波形", x, y + 41.0f, w, 15.0f, kColDim);
        }
        DrawDualSparkline(cv, x, y + 59.0f, w, 17.0f, live);
    }

    // Col 1: CPU
    {
        float x = 367.0f, y = 64.0f, w = 153.0f;
        cv.rt()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + 4.0f, y + 7.5f), 2.8f, 2.8f),
                             cv.br(kColBlueHi));
        cv.Text(cv.fmtSmall_, L"CPU 占用", x + 11.0f, y, 76.0f, 15.0f, kColMuted);
        if (s.cpuClock > 10.0f) {
            swprintf(buf, 128, L"%.0fM", (double)s.cpuClock);
            cv.Text(cv.fmtMonoSm_, buf, x + 84.0f, y, w - 84.0f, 15.0f, kColMuted,
                    DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
        if (s.cpuUsage >= 0.0f) {
            swprintf(buf, 128, L"%.1f%%", (double)s.cpuUsage);
            cv.Text(cv.fmtMonoBig_, buf, x, y + 16.0f, w, 24.0f, kColText);
            float pct = std::clamp(s.cpuUsage / 100.0f, 0.0f, 1.0f);
            cv.FillRR(x, y + 46.0f, w, 4.0f, 2.0f, kColCardAlt);
            cv.FillRR(x, y + 46.0f, std::max(4.0f, w * pct), 4.0f, 2.0f,
                      s.cpuUsage >= 90.0f ? kColOrange : kColBlueHi);
        } else {
            cv.Text(cv.fmtMonoBig_, L"--", x, y + 16.0f, w, 24.0f, kColDim);
        }
        wchar_t tb[24] = L"--", pb[24] = L"--";
        if (s.cpuTemp >= -200.0f) swprintf(tb, 24, L"%.0f°C", (double)s.cpuTemp);
        if (s.cpuPower >= 0.0f) swprintf(pb, 24, L"%.0fW", (double)s.cpuPower);
        swprintf(buf, 128, L"温度 %ls · 功耗 %ls", tb, pb);
        cv.Text(cv.fmtMicro_, buf, x, y + 58.0f, w, 16.0f, kColMuted);
    }

    // Col 2: GPU
    {
        float x = 544.0f, y = 64.0f, w = 153.0f;
        cv.rt()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + 4.0f, y + 7.5f), 2.8f, 2.8f),
                             cv.br(kColGreen));
        cv.Text(cv.fmtSmall_, L"GPU 占用", x + 11.0f, y, 76.0f, 15.0f, kColMuted);
        if (s.gpuClock > 10.0f) {
            swprintf(buf, 128, L"%.0fM", (double)s.gpuClock);
            cv.Text(cv.fmtMonoSm_, buf, x + 84.0f, y, w - 84.0f, 15.0f, kColMuted,
                    DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
        if (s.gpuUsage >= 0.0f) {
            swprintf(buf, 128, L"%.1f%%", (double)s.gpuUsage);
            cv.Text(cv.fmtMonoBig_, buf, x, y + 16.0f, w, 24.0f, kColText);
            float pct = std::clamp(s.gpuUsage / 100.0f, 0.0f, 1.0f);
            cv.FillRR(x, y + 46.0f, w, 4.0f, 2.0f, kColCardAlt);
            cv.FillRR(x, y + 46.0f, std::max(4.0f, w * pct), 4.0f, 2.0f,
                      s.gpuTemp >= 85.0f ? kColRed : kColGreen);
        } else {
            cv.Text(cv.fmtMonoBig_, L"--", x, y + 16.0f, w, 24.0f, kColDim);
        }
        wchar_t tb[24] = L"--", pb[24] = L"--";
        if (s.gpuTemp >= -200.0f) swprintf(tb, 24, L"%.0f°C", (double)s.gpuTemp);
        if (s.gpuPower >= 0.0f) swprintf(pb, 24, L"%.0fW", (double)s.gpuPower);
        swprintf(buf, 128, L"温度 %ls · 功耗 %ls", tb, pb);
        cv.Text(cv.fmtMicro_, buf, x, y + 58.0f, w, 16.0f, kColMuted);
    }

    // Col 3: 瓶颈与显存 / 基准
    {
        float x = 721.0f, y = 64.0f, w = 153.0f;
        cv.Text(cv.fmtSmall_, L"瓶颈与显存", x, y, w, 15.0f, kColMuted);
        std::wstring wb;
        int bLvl = np::PL_NORMAL;
        np::NPComputeBottleneck(s, t, &wb, &bLvl);
        if (wb.empty()) wb = L"待机 / 均衡";
        uint32_t bCol = (bLvl == np::PL_WARN) ? kColRed
                      : (bLvl == np::PL_ACCENT ? kColOrange
                      : (bLvl == np::PL_GPU ? kColGreen
                      : (bLvl == np::PL_CPU ? kColBlueHi : kColMuted)));
        cv.FillRR(x, y + 19.0f, w, 22.0f, 6.0f, kColSurface);
        cv.rt()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + 11.0f, y + 30.0f), 3.0f, 3.0f),
                             cv.br(bCol));
        cv.Text(cv.fmtSmall_, wb, x + 20.0f, y + 19.0f, w - 26.0f, 22.0f, kColText);

        if (gApp.lastBench.valid) {
            swprintf(buf, 128, L"均%.1f · 1%% %.1f", (double)gApp.lastBench.avgFps,
                     (double)gApp.lastBench.low1Fps);
            cv.Text(cv.fmtMonoSm_, buf, x, y + 46.0f, w, 15.0f, kColBlueHi);
            swprintf(buf, 128, L"%.1f - %.1f FPS", (double)gApp.lastBench.minFps,
                     (double)gApp.lastBench.maxFps);
            cv.Text(cv.fmtMicro_, buf, x, y + 61.0f, w, 15.0f, kColMuted);
        } else {
            swprintf(buf, 128, L"显存 %.2f / %.1fG", (double)std::max(0.0f, s.vramUsedGB),
                     (double)std::max(0.0f, s.vramTotalGB));
            cv.Text(cv.fmtMonoSm_, buf, x, y + 46.0f, w, 15.0f, kColText);
            cv.Text(cv.fmtMicro_, L"Ctrl+Shift+B 录制 CSV", x, y + 61.0f, w, 15.0f, kColDim);
        }
    }

    // 2) 游戏库与注入 Grouped Card (x=178, y=158, w=708, h=384)
    cv.Card(178.0f, 158.0f, 708.0f, 384.0f);
    cv.Text(cv.fmtTitle_, L"游戏库与注入", 192.0f, 168.0f, 118.0f, 28.0f, kColText);
    swprintf(buf, 128, L"%zu 款 · 64/32位", gamesSnap.size());
    cv.Text(cv.fmtSmall_, buf, 294.0f, 168.0f, 120.0f, 28.0f, kColMuted);

    struct ToolBtn { float x, w; int act; const wchar_t* label; bool primary; };
    const ToolBtn tbtns[] = {
        {506.0f,  64.0f, ACT_FORGET_LEARNED, L"清学习",     false},
        {576.0f,  64.0f, ACT_ADD_GAME,       L"+ 添加",     false},
        {646.0f, 100.0f, ACT_SCAN_GAMES,     L"扫描运行中", false},
        {kFrontInjectX, kFrontInjectW, ACT_INJECT_FRONT, L"注入前台游戏", true},
    };
    for (const auto& b : tbtns) {
        float by = b.primary ? kFrontInjectY : 168.0f;
        float bh = b.primary ? kFrontInjectH : 28.0f;
        HitItem it{H_ACTION, {b.x, by, b.w, bh}, b.act, 0, -1};
        int idx = (int)gHits.size();
        gHits.push_back(it);
        DrawQuietBtn(cv, it, b.label, gHoverIdx == idx, b.primary);
    }

    // 紧凑 6 行可滚动列表井 (x=192, y=206, w=680, h=298)
    constexpr int kVisGames = 6;
    int maxGs = std::max(0, (int)gamesSnap.size() - kVisGames);
    gGameScroll = std::clamp(gGameScroll, 0, maxGs);

    cv.FillRR(192.0f, 206.0f, 680.0f, 298.0f, 8.0f, kColSurface);
    cv.DrawRR(192.0f, 206.0f, 680.0f, 298.0f, 8.0f, kColBorder);

    if (gamesSnap.empty()) {
        cv.Text(cv.fmtBody_, L"列表为空 · 点击「扫描运行中」或「+ 添加」收录 3D 游戏",
                210.0f, 206.0f, 644.0f, 298.0f, kColDim, DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        for (int row = 0; row < kVisGames; ++row) {
            int slot = gGameScroll + row;
            if (slot >= (int)gamesSnap.size()) break;
            const GameEntry& ge = gamesSnap[slot];
            float rx = 193.0f, ry = 208.0f + row * 49.0f, rw = 670.0f, rh = 48.0f;

            HitItem rHit{H_GAME_ROW, {rx, ry, rw - 160.0f, rh}, slot, 0, -1};
            int rIdx = (int)gHits.size();
            gHits.push_back(rHit);
            bool sel = (gSelGame == slot);
            bool hov = (gHoverIdx == rIdx);
            if (sel || hov) {
                cv.FillRR(rx + 4.0f, ry + 3.0f, rw - 8.0f, rh - 6.0f, 6.0f,
                          sel ? kColHover : kColCardAlt, 0.75f);
            }
            if (row < kVisGames - 1 && slot + 1 < (int)gamesSnap.size()) {
                cv.FillR(rx + 14.0f, ry + rh - 1.0f, rw - 28.0f, 1.0f, kColBorder);
            }

            uint32_t dotCol = kColDim;
            wchar_t st[64] = L"未运行";
            uint32_t stCol = kColDim;
            if (ge.pid) {
                bool mine = (gApp.telemetryPid == ge.pid);
                bool stLive = t.attached && mine && gApp.telemetryLive;
                if (stLive) {
                    swprintf(st, 64, L"%ls · %.0f FPS", ApiShortLabel(t.gfxApi), (double)t.fps);
                    dotCol = kColGreen;
                    stCol = kColGreen;
                } else if (ge.injected) {
                    if (mine && t.gfxApi == NP_API_VULKAN) wcscpy_s(st, L"Vulkan · 桌面 HUD");
                    else if (mine && t.gfxApi == NP_API_OPENGL) wcscpy_s(st, L"OpenGL · 无帧数据");
                    else swprintf(st, 64, L"已注入 PID %lu", (unsigned long)ge.pid);
                    dotCol = kColBlueHi;
                    stCol = kColBlueHi;
                } else {
                    uint32_t rem = GameAutoInjectRemainingMs(ge);
                    if (rem > 0) {
                        swprintf(st, 64, L"%us 后自动注入", (unsigned)((rem + 999) / 1000));
                        dotCol = kColOrange;
                        stCol = kColOrange;
                    } else {
                        swprintf(st, 64, L"运行中 PID %lu", (unsigned long)ge.pid);
                        dotCol = kColMuted;
                        stCol = kColText;
                    }
                }
            }
            cv.rt()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(rx + 18.0f, ry + rh * 0.5f),
                                               3.2f, 3.2f),
                                 cv.br(dotCol));

            std::wstring exeShort = ShortExeName(ge);
            cv.Text(cv.fmtMono_, exeShort, rx + 30.0f, ry, 200.0f, rh, kColText);
            cv.Text(cv.fmtMicro_, ge.learned ? L"学习" : L"手动", rx + 236.0f, ry,
                    48.0f, rh, kColDim);
            cv.Text(cv.fmtSmall_, st, rx + 290.0f, ry, 206.0f, rh, stCol);

            HitItem injHit{H_GAME_INJ, {rx + 506.0f, ry + 10.0f, 82.0f, 28.0f}, slot, 0, -1};
            int injIdx = (int)gHits.size();
            gHits.push_back(injHit);
            bool injHov = (gHoverIdx == injIdx);
            if (ge.injected) DrawQuietBtn(cv, injHit, L"卸载", injHov, false, kColOrange);
            else if (ge.pid) DrawQuietBtn(cv, injHit, L"注入", injHov, true);
            else cv.Text(cv.fmtMicro_, L"—", injHit.rc.x, injHit.rc.y, injHit.rc.w, injHit.rc.h,
                         kColDim, DWRITE_TEXT_ALIGNMENT_CENTER);

            HitItem remHit{H_GAME_REM, {rx + 596.0f, ry + 10.0f, 62.0f, 28.0f}, slot, 0, -1};
            int remIdx = (int)gHits.size();
            gHits.push_back(remHit);
            DrawQuietBtn(cv, remHit, L"移除", gHoverIdx == remIdx, false, kColMuted);
        }
    }

    if ((int)gamesSnap.size() > kVisGames) {
        float trX = 865.0f, trY = 214.0f, trH = 282.0f;
        cv.FillRR(trX, trY, 3.0f, trH, 1.5f, kColCardAlt);
        float thH = std::max(24.0f, trH * (float)kVisGames / (float)gamesSnap.size());
        float thY = trY + (trH - thH) * ((float)gGameScroll / (float)std::max(1, maxGs));
        cv.FillRR(trX, thY, 3.0f, thH, 1.5f, kColBorderHi);
    }

    std::wstring fore = gApp.foreName.empty()
                            ? L"尚未捕获游戏前台（切回游戏窗口后再点「注入前台游戏」）"
                            : (L"最近前台：" + gApp.foreName + L" (PID " +
                               std::to_wstring(gApp.forePid) + L")");
    cv.Text(cv.fmtSmall_, fore, 192.0f, 512.0f, 470.0f, 24.0f, kColMuted);
    cv.Text(cv.fmtMicro_,
            IsAsyncJobRunning() ? L"后台任务执行中…" : L"滚轮翻页 · 延迟 10s 安全注入",
            664.0f, 512.0f, 208.0f, 24.0f, IsAsyncJobRunning() ? kColOrange : kColDim,
            DWRITE_TEXT_ALIGNMENT_TRAILING);
}

static void DrawTabAppearance(UiCanvas& cv) {
    const NPConfig& cfg = gApp.cfg;

    // 左栏 (x=178, w=398, y=56..542)
    // 1) 排版形态与材质风格卡 (y=56, h=124)
    cv.Card(178.0f, 56.0f, 398.0f, 124.0f);
    cv.Text(cv.fmtHead_, L"排版形态", 192.0f, 64.0f, 96.0f, 28.0f, kColText);

    cv.FillRR(306.0f, 64.0f, 256.0f, 28.0f, 7.0f, kColSurface);
    cv.DrawRR(306.0f, 64.0f, 256.0f, 28.0f, 7.0f, kColBorder);
    bool horiz = NPConfigHorizontal(cfg);
    for (int m = 0; m < 2; ++m) {
        float bx = 309.0f + m * 125.0f;
        HitItem hi{H_LAYOUT_MODE, {bx, 67.0f, 125.0f, 22.0f}, m, 0, -1};
        int idx = (int)gHits.size();
        gHits.push_back(hi);
        bool act = ((m == 1) == horiz);
        bool hov = (gHoverIdx == idx);
        if (act) {
            cv.FillRR(bx, 67.0f, 125.0f, 22.0f, 5.0f, kColHover);
            cv.DrawRR(bx, 67.0f, 125.0f, 22.0f, 5.0f, kColBorderHi);
        } else if (hov) {
            cv.FillRR(bx, 67.0f, 125.0f, 22.0f, 5.0f, kColCardAlt);
        }
        cv.Text(cv.fmtMicro_, m == 0 ? L"Metal 竖版" : L"极简横条", bx, 67.0f, 125.0f, 22.0f,
                act ? kColText : kColMuted, DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    cv.FillR(192.0f, 100.0f, 370.0f, 1.0f, kColBorder);
    cv.Text(cv.fmtSmall_, L"HUD 材质与背景", 192.0f, 106.0f, 140.0f, 18.0f, kColMuted);
    struct ThemeSpec { const wchar_t* name; uint32_t bg, bd; };
    const ThemeSpec themes[4] = {
        {L"Metal 黑",  0x060608u, 0x3A3A3Fu},
        {L"液态暗灰",  0x1C1C20u, 0x525258u},
        {L"浅雾透明",  0x111114u, 0x2C2C30u},
        {L"极简纯字",  0x141416u, 0x636366u},
    };
    uint32_t curTheme = NPConfigTheme(cfg);
    for (int t = 0; t < 4; ++t) {
        float tx = 192.0f + t * 94.0f, ty = 130.0f, tw = 88.0f, th = 38.0f;
        HitItem ti{H_THEME, {tx, ty, tw, th}, t, 0, -1};
        int idx = (int)gHits.size();
        gHits.push_back(ti);
        bool act = ((uint32_t)t == curTheme);
        bool hov = (gHoverIdx == idx);
        cv.FillRR(tx, ty, tw, th, 7.0f, act ? kColHover : (hov ? kColCardAlt : kColSurface));
        cv.DrawRR(tx, ty, tw, th, 7.0f, act ? kColBlue : kColBorder, 1.0f, act ? 1.5f : 1.0f);
        cv.FillRR(tx + 7.0f, ty + 10.0f, 18.0f, 18.0f, 4.0f, themes[t].bg);
        cv.DrawRR(tx + 7.0f, ty + 10.0f, 18.0f, 18.0f, 4.0f, themes[t].bd);
        if (t == 3) {
            cv.Text(cv.fmtMicro_, L"A", tx + 7.0f, ty + 10.0f, 18.0f, 18.0f, kColText,
                    DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {
            cv.FillRR(tx + 10.0f, ty + 14.0f, 12.0f, 2.0f, 1.0f, kColBlueHi);
            cv.FillRR(tx + 10.0f, ty + 18.0f, 9.0f, 2.0f, 1.0f, kColGreen);
            cv.FillRR(tx + 10.0f, ty + 22.0f, 11.0f, 2.0f, 1.0f, kColMuted);
        }
        cv.Text(cv.fmtMicro_, themes[t].name, tx + 29.0f, ty, tw - 32.0f, th,
                act ? kColText : kColMuted);
    }

    // 2) 屏幕锚点 + 开关 (y=188, h=114)
    cv.Card(178.0f, 188.0f, 398.0f, 114.0f);
    cv.Text(cv.fmtSmall_, L"屏幕锚点", 192.0f, 190.0f, 100.0f, 36.0f, kColText);
    cv.FillRR(306.0f, 195.0f, 256.0f, 26.0f, 6.0f, kColSurface);
    cv.DrawRR(306.0f, 195.0f, 256.0f, 26.0f, 6.0f, kColBorder);
    struct CornerOpt { int code; const wchar_t* lbl; };
    const CornerOpt corners[4] = {
        {1, L"左上"}, {0, L"右上"}, {3, L"左下"}, {2, L"右下"},
    };
    uint32_t curCorner = NPConfigCorner(cfg);
    for (int i = 0; i < 4; ++i) {
        float cx = 309.0f + i * 62.5f;
        HitItem ci{H_CORNER, {cx, 198.0f, 62.5f, 20.0f}, corners[i].code, 0, -1};
        int idx = (int)gHits.size();
        gHits.push_back(ci);
        bool act = ((uint32_t)corners[i].code == curCorner);
        bool hov = (gHoverIdx == idx);
        if (act) {
            cv.FillRR(cx, 198.0f, 62.5f, 20.0f, 4.0f, kColHover);
            cv.DrawRR(cx, 198.0f, 62.5f, 20.0f, 4.0f, kColBorderHi);
        } else if (hov) {
            cv.FillRR(cx, 198.0f, 62.5f, 20.0f, 4.0f, kColCardAlt);
        }
        cv.Text(cv.fmtMicro_, corners[i].lbl, cx, 198.0f, 62.5f, 20.0f,
                act ? kColText : kColMuted, DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    auto drawSettingSwitchRow = [&](float ry, const wchar_t* lbl, bool on, int arg) {
        cv.FillR(192.0f, ry, 370.0f, 1.0f, kColBorder);
        HitItem hit{H_OPT_BIT, {180.0f, ry + 1.0f, 394.0f, 36.0f}, arg, 0, -1};
        int idx = (int)gHits.size();
        gHits.push_back(hit);
        bool hov = (gHoverIdx == idx);
        if (hov) cv.FillRR(182.0f, ry + 2.0f, 390.0f, 33.0f, 6.0f, kColCardAlt, 0.45f);
        cv.Text(cv.fmtSmall_, lbl, 192.0f, ry + 1.0f, 310.0f, 36.0f, kColText);
        DrawAppleSwitch(cv, 528.0f, ry + 10.0f, on, hov);
    };
    drawSettingSwitchRow(226.0f, L"显示 2 行设备顶栏（显卡 · 分辨率 · Hz）",
                         NPConfigShowHeader(cfg), 1);
    drawSettingSwitchRow(264.0f, L"显示极值区间 [min max]", NPConfigShowRange(cfg), 0);

    // 3) 7 项微调滑杆卡 (y=310, h=232)
    cv.Card(178.0f, 310.0f, 398.0f, 232.0f);
    for (int i = 0; i < 7; ++i) {
        float sy = 318.0f + i * 31.0f;
        HitItem si{H_SLIDER, {192.0f, sy, 370.0f, 27.0f}, i, 0, i};
        int idx = (int)gHits.size();
        gHits.push_back(si);

        std::wstring valStr;
        float ratio = GetSliderRatio(i, &valStr);
        cv.Text(cv.fmtSmall_, kSliders[i].label, 192.0f, sy, 76.0f, 27.0f, kColText);

        RectF tr{274.0f, sy + 11.5f, 232.0f, 4.0f};
        gSliderTrack[i] = tr;
        cv.FillRR(tr.x, tr.y, tr.w, tr.h, 2.0f, kColCardAlt);
        cv.FillRR(tr.x, tr.y, std::max(4.0f, tr.w * ratio), tr.h, 2.0f, kColBlue);
        float kx = tr.x + tr.w * ratio;
        bool act = (gDraggingSlider == i || gHoverIdx == idx);
        cv.rt()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, tr.y + 2.0f),
                                           act ? 6.8f : 5.8f, act ? 6.8f : 5.8f),
                             cv.br(0xFFFFFFu));
        cv.rt()->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(kx, tr.y + 2.0f),
                                           act ? 6.8f : 5.8f, act ? 6.8f : 5.8f),
                             cv.br(0x000000u, 0.35f), 1.0f);

        cv.Text(cv.fmtMonoSm_, valStr, 512.0f, sy, 50.0f, 27.0f, kColMuted,
                DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    // 右栏：实时 HUD 1:1 紧凑预览 (x=586, y=56, w=300, h=486)
    cv.Card(586.0f, 56.0f, 300.0f, 486.0f);
    cv.Text(cv.fmtHead_, L"Metal HUD 实时预览", 598.0f, 62.0f, 160.0f, 22.0f, kColText);

    NPTelemetry prevTel = gApp.telemetry;
    NPSensors   prevSens = gApp.sensors;
    NPHistory   prevHist = gApp.hist;
    bool usingDemo = (!prevTel.attached && prevTel.fps < 1.0f);
    if (usingDemo) {
        prevTel.attached = 1;
        prevTel.gfxApi = NP_API_D3D12;
        prevTel.presentMode = NP_PM_BORDERLESS;
        strcpy_s(prevTel.processName, "CyberDemo.exe");
        prevTel.fps = 142.6f;
        prevTel.fpsAvg = 138.4f;
        prevTel.fpsLow1 = 116.2f;
        prevTel.fpsLow01 = 98.4f;
        prevTel.frameMs = 7.01f;
        prevTel.frameMsAvg = 7.22f;
        prevTel.cpuFrameMs = 4.30f;
        prevTel.cpuFrameMsAvg = 4.45f;
        prevTel.cpuBusyMs = 3.60f;
        prevTel.cpuBusyAvg = 3.72f;
        prevTel.cpuWaitMs = 0.70f;
        prevTel.cpuWaitAvg = 0.73f;
        prevTel.gpuFrameMs = 5.42f;
        prevTel.rtLoad = 22.4f;
        prevTel.rtGpuMs = 1.61f;
        prevTel.tensorLoad = 11.8f;
        prevTel.aiGpuMs = 0.85f;
        prevTel.windowW = 2560;
        prevTel.windowH = 1440;
        prevTel.renderW = 2560;
        prevTel.renderH = 1440;
        prevTel.drawCalls = 1448;
        prevTel.dispatches = 111;
        if (prevSens.cpuUsage < 0.0f) prevSens.cpuUsage = 34.5f;
        if (prevSens.cpuClock < 10.0f) prevSens.cpuClock = 5150.0f;
        if (prevSens.cpuTemp < -200.0f) prevSens.cpuTemp = 64.0f;
        if (prevSens.cpuPower < 0.0f) prevSens.cpuPower = 68.0f;
        if (prevSens.gpuUsage < 0.0f) prevSens.gpuUsage = 92.0f;
        if (prevSens.gpuTemp < -200.0f) prevSens.gpuTemp = 69.0f;
        if (prevSens.gpuPower < 0.0f) prevSens.gpuPower = 245.0f;
        if (prevSens.gpuClock < 10.0f) prevSens.gpuClock = 2640.0f;
        if (prevSens.memClock < 10.0f) prevSens.memClock = 9001.0f;
        if (prevSens.vramTotalGB <= 0.1f) { prevSens.vramUsedGB = 7.42f; prevSens.vramTotalGB = 16.0f; }
        if (prevSens.ramTotalGB <= 0.1f) { prevSens.ramUsedGB = 13.84f; prevSens.ramTotalGB = 32.0f; }
        prevHist = NPHistory{};
    }
    if (usingDemo || prevHist.count < 16) {
        prevHist.sampleMs = 125;
        for (int k = 0; k < 56; ++k) {
            float p = (float)k * 0.34f;
            float f = 136.0f + 16.0f * sinf(p) + 5.0f * cosf(p * 2.1f);
            float ft = 1000.0f / f;
            float gt = ft * (0.64f + 0.14f * cosf(p * 1.4f));
            NPHistoryPush(&prevHist, f, 136.0f, 114.0f, 96.0f, 34.0f + 8.0f * sinf(p),
                          88.0f + 7.0f * cosf(p), ft, ft * 0.55f, gt, ft * 0.45f, ft * 0.10f);
        }
    }

    np::PanelData pd;
    np::BuildPanelData(pd, cfg, prevSens, prevTel, &prevHist);
    float pw = 0.0f, ph = 0.0f;
    gPreviewPanel.Measure(pd, cfg, &pw, &ph);

    wchar_t dimBuf[64]{};
    swprintf(dimBuf, 64, L"%.0f×%.0f DIP", (double)pw, (double)ph);
    cv.Text(cv.fmtMonoSm_, dimBuf, 744.0f, 62.0f, 130.0f, 22.0f, kColMuted,
            DWRITE_TEXT_ALIGNMENT_TRAILING);

    RectF vp{596.0f, 90.0f, 280.0f, 442.0f};
    cv.FillRR(vp.x, vp.y, vp.w, vp.h, 8.0f, 0x0D0E10u);
    cv.DrawRR(vp.x, vp.y, vp.w, vp.h, 8.0f, kColBorder);

    ID2D1DCRenderTarget* rt = cv.rt();
    rt->PushAxisAlignedClip(vp.d2d(), D2D1_ANTIALIAS_MODE_ALIASED);

    cv.FillRR(vp.x + 14.0f, vp.y + 24.0f, vp.w - 28.0f, 160.0f, 10.0f, 0x1A1D22u, 0.55f);
    cv.FillRR(vp.x + 22.0f, vp.y + 220.0f, 170.0f, 115.0f, 12.0f, 0x24262Bu, 0.45f);
    cv.FillRR(vp.x + 125.0f, vp.y + 270.0f, 140.0f, 130.0f, 12.0f, 0x2A2925u, 0.40f);

    if (pw > 1.0f && ph > 1.0f && gPreviewPanel.ok()) {
        float fit = std::min({(vp.w - 16.0f) / pw, (vp.h - 16.0f) / ph, 1.0f});
        float drawW = pw * fit, drawH = ph * fit;
        float ox = vp.x + 8.0f, oy = vp.y + 8.0f;
        if (!horiz) {
            if (curCorner == 0 || curCorner == 2) ox = vp.x + vp.w - drawW - 8.0f;
            if (curCorner == 2 || curCorner == 3) oy = vp.y + vp.h - drawH - 8.0f;
        } else {
            ox = vp.x + (vp.w - drawW) * 0.5f;
            oy = (curCorner == 2 || curCorner == 3) ? (vp.y + vp.h - drawH - 8.0f)
                                                    : (vp.y + 8.0f);
        }

        D2D1::Matrix3x2F prevXf{};
        rt->GetTransform(&prevXf);
        rt->SetTransform(D2D1::Matrix3x2F::Scale(fit, fit) *
                         D2D1::Matrix3x2F::Translation(ox, oy) * prevXf);
        gPreviewPanel.Render(rt, 0.0f, 0.0f, pd, cfg);
        rt->SetTransform(prevXf);
    }
    rt->PopAxisAlignedClip();
}

static void DrawTabCounters(UiCanvas& cv) {
    RebuildCounterRows();

    // 1) 顶部 macOS 分段预设条 (x=178, y=56, w=708, h=48)
    cv.Card(178.0f, 56.0f, 708.0f, 48.0f);
    cv.Text(cv.fmtHead_, L"监控预设", 192.0f, 56.0f, 72.0f, 48.0f, kColText);
    wchar_t cntBuf[64]{};
    swprintf(cntBuf, 64, L"%d / 37", CountEnabledCounters());
    cv.Text(cv.fmtMonoSm_, cntBuf, 260.0f, 56.0f, 64.0f, 48.0f, kColMuted);

    cv.FillRR(344.0f, 65.0f, 428.0f, 30.0f, 7.0f, kColSurface);
    cv.DrawRR(344.0f, 65.0f, 428.0f, 30.0f, 7.0f, kColBorder);

    struct PBtn { float x, w; int presetId; const wchar_t* txt; uint64_t mask; };
    const PBtn pbtns[] = {
        {347.0f, 105.0f, 0, L"Metal 精简", NP_PRESET_MINIMAL},
        {452.0f, 105.0f, 1, L"Metal 标准", NP_PRESET_STANDARD},
        {557.0f, 105.0f, 2, L"深度调优",   NP_PRESET_TUNING},
        {662.0f, 107.0f, 3, L"全部启用",   NP_PRESET_FULL},
    };
    for (const auto& b : pbtns) {
        bool isAct = ((gApp.cfg.counters & NP_PRESET_FULL) == b.mask);
        HitItem it{H_PRESET, {b.x, 68.0f, b.w, 24.0f}, b.presetId, b.mask, -1};
        int idx = (int)gHits.size();
        gHits.push_back(it);
        bool hov = (gHoverIdx == idx);
        if (isAct) {
            cv.FillRR(b.x, 68.0f, b.w, 24.0f, 5.0f, kColHover);
            cv.DrawRR(b.x, 68.0f, b.w, 24.0f, 5.0f, kColBorderHi);
        } else if (hov) {
            cv.FillRR(b.x, 68.0f, b.w, 24.0f, 5.0f, kColCardAlt);
        }
        cv.Text(cv.fmtSmall_, b.txt, b.x, 68.0f, b.w, 24.0f, isAct ? kColText : kColMuted,
                DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    HitItem defHit{H_PRESET, {780.0f, 66.0f, 94.0f, 28.0f}, 4, 0, -1};
    int defIdx = (int)gHits.size();
    gHits.push_back(defHit);
    DrawQuietBtn(cv, defHit, L"恢复默认", gHoverIdx == defIdx);

    // 2) 分组滑块开关列表 (x=178, y=112, w=708, h=430)
    cv.Card(178.0f, 112.0f, 708.0f, 430.0f);
    constexpr int kVisRows = 11;
    const float topY = 122.0f, rowH = 37.0f;

    for (int vi = 0; vi < kVisRows; ++vi) {
        int ri = gCounterScroll + vi;
        if (ri >= (int)gCounterRows.size()) break;
        const CounterVisualRow& cr = gCounterRows[ri];
        float ry = topY + vi * rowH;

        if (cr.isGroup) {
            HitItem gh{H_GROUP_HDR, {190.0f, ry + 2.0f, 674.0f, rowH - 4.0f}, cr.groupIdx, 0, -1};
            int idx = (int)gHits.size();
            gHits.push_back(gh);
            bool hov = (gHoverIdx == idx);
            bool open = gGrpOpen[cr.groupIdx];
            cv.FillRR(190.0f, ry + 2.0f, 674.0f, rowH - 4.0f, 6.0f,
                      hov ? kColHover : kColSurface);
            wchar_t arrow[64]{};
            swprintf(arrow, 64, L"%ls  %ls (%d)", open ? L"▾" : L"▸",
                     kGroups[cr.groupIdx].name, kGroups[cr.groupIdx].n);
            cv.Text(cv.fmtHead_, arrow, 202.0f, ry + 2.0f, 380.0f, rowH - 4.0f, kColText);
            cv.Text(cv.fmtMicro_, open ? L"收起" : L"展开", 760.0f, ry + 2.0f, 92.0f,
                    rowH - 4.0f, kColDim, DWRITE_TEXT_ALIGNMENT_TRAILING);
        } else {
            for (int c = 0; c < 2; ++c) {
                const CounterDef* cd = cr.cols[c];
                if (!cd) continue;
                float bx = (c == 0) ? 190.0f : 532.0f;
                float bw = 332.0f;
                HitItem chHit{H_COUNTER, {bx, ry + 2.0f, bw, rowH - 4.0f}, 0, cd->bit, -1};
                int idx = (int)gHits.size();
                gHits.push_back(chHit);
                bool hov = (gHoverIdx == idx);
                bool on = (gApp.cfg.counters & cd->bit) != 0;

                cv.FillRR(bx, ry + 2.0f, bw, rowH - 4.0f, 6.0f,
                          hov ? kColHover : kColSurface, hov ? 0.9f : 0.65f);
                cv.FillR(bx + 8.0f, ry + rowH - 1.0f, bw - 16.0f, 1.0f, kColBorder, 0.5f);

                cv.Text(cv.fmtBody_, cd->name, bx + 12.0f, ry + 2.0f, 210.0f, rowH - 4.0f,
                        on ? kColText : kColMuted);
                cv.Text(cv.fmtMicro_, cd->tag, bx + 224.0f, ry + 2.0f, 54.0f, rowH - 4.0f, kColDim,
                        DWRITE_TEXT_ALIGNMENT_TRAILING);
                DrawAppleSwitch(cv, bx + bw - 42.0f, ry + (rowH - 18.0f) * 0.5f, on, hov);
            }
        }
    }

    if (gCounterTotalRows > kVisRows) {
        int maxSc = std::max(1, gCounterTotalRows - kVisRows);
        float trX = 872.0f, trY = 126.0f, trH = 400.0f;
        cv.FillRR(trX, trY, 3.0f, trH, 1.5f, kColCardAlt);
        float thH = std::max(26.0f, trH * (float)kVisRows / (float)gCounterTotalRows);
        float thY = trY + (trH - thH) * ((float)gCounterScroll / (float)maxSc);
        cv.FillRR(trX, thY, 3.0f, thH, 1.5f, kColBorderHi);
    }
}

static void DrawTabSystem(UiCanvas& cv) {
    // 左栏：运行与注入设置 (x=178, y=56, w=346, h=300)
    cv.Card(178.0f, 56.0f, 346.0f, 300.0f);
    cv.Text(cv.fmtTitle_, L"运行与注入设置", 192.0f, 62.0f, 200.0f, 22.0f, kColText);

    struct CycRow { float y; const wchar_t* label; std::wstring val; int id; };
    const wchar_t* ovNames[] = {L"智能自动", L"仅游戏内", L"仅桌面", L"全部隐藏"};
    uint32_t ovIdx = std::min(gApp.cfg.overlayMode, 3u);
    wchar_t pollStr[32]{}, hzStr[32]{};
    swprintf(pollStr, 32, L"%u ms", gApp.cfg.pollMs ? gApp.cfg.pollMs : 500u);
    swprintf(hzStr, 32, L"%u Hz", gApp.cfg.updateHz ? gApp.cfg.updateHz : 20u);

    const CycRow cycs[3] = {
        {90.0f,  L"叠加显示策略",   ovNames[ovIdx], 0},
        {122.0f, L"传感器轮询间隔", pollStr, 1},
        {154.0f, L"桌面 HUD 刷新率", hzStr, 2},
    };
    for (const auto& cr : cycs) {
        HitItem hi{H_SYS_CYCLE, {192.0f, cr.y, 318.0f, 28.0f}, cr.id, 0, -1};
        int idx = (int)gHits.size();
        gHits.push_back(hi);
        bool hov = (gHoverIdx == idx);
        cv.FillRR(hi.rc.x, hi.rc.y, hi.rc.w, hi.rc.h, 6.0f, hov ? kColHover : kColSurface);
        cv.DrawRR(hi.rc.x, hi.rc.y, hi.rc.w, hi.rc.h, 6.0f, hov ? kColBorderHi : kColBorder);
        cv.Text(cv.fmtSmall_, cr.label, hi.rc.x + 10.0f, hi.rc.y, 170.0f, 28.0f, kColText);
        cv.Text(cv.fmtSmall_, cr.val + L" ▾", hi.rc.x + 180.0f, hi.rc.y, 128.0f, 28.0f,
                kColBlueHi, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    struct SysBool { const wchar_t* label; bool val; int id; };
    const SysBool bools[5] = {
        {L"游戏库匹配启动后自动延迟注入", gApp.cfg.autoInject != 0, 0},
        {L"允许对自动学习名单自动注入",   gApp.cfg.learnedAutoHook != 0, 1},
        {L"D3D12 深度引擎钩子 (RT / AI)",  gApp.cfg.deepEngineHook != 0, 2},
        {L"允许 VTable 临时探测兜底",      gApp.cfg.vtableProbe != 0, 3},
        {L"离线模拟演示数据流",            gApp.cfg.simulate != 0, 4},
    };
    for (int i = 0; i < 5; ++i) {
        float y = 190.0f + i * 32.0f;
        if (i > 0) cv.FillR(192.0f, y, 318.0f, 1.0f, kColBorder);
        HitItem hi{H_SYS_TOGGLE, {184.0f, y + 1.0f, 334.0f, 31.0f}, bools[i].id, 0, -1};
        int idx = (int)gHits.size();
        gHits.push_back(hi);
        bool hov = (gHoverIdx == idx);
        if (hov) cv.FillRR(186.0f, y + 2.0f, 330.0f, 28.0f, 6.0f, kColCardAlt, 0.5f);
        cv.Text(cv.fmtSmall_, bools[i].label, 192.0f, y + 1.0f, 270.0f, 31.0f,
                bools[i].val ? kColText : kColMuted);
        DrawAppleSwitch(cv, 478.0f, y + 7.5f, bools[i].val, hov);
    }

    // 左下：诊断与工具 (x=178, y=364, w=346, h=178)
    cv.Card(178.0f, 364.0f, 346.0f, 178.0f);
    cv.Text(cv.fmtTitle_, L"诊断与维护工具", 192.0f, 372.0f, 180.0f, 22.0f, kColText);

    struct ToolItem { int col, row, act; const wchar_t* label; uint32_t fg; bool dim; };
    const ToolItem tools[6] = {
        {0, 0, ACT_COPY_DIAG,   L"复制诊断报告", kColBlueHi, false},
        {1, 0, ACT_RESET_HIST,  L"重置历史波形", kColText,   false},
        {0, 1, ACT_OPEN_LOGDIR, L"打开日志目录", kColText,   false},
        {1, 1, ACT_OPEN_CSVDIR, L"打开 CSV 目录", kColText,  false},
        {0, 2, ACT_ELEVATE,     gApp.isAdmin ? L"已是管理员" : L"以管理员重启",
         gApp.isAdmin ? kColDim : kColOrange, gApp.isAdmin},
        {1, 2, ACT_QUIT,        L"完全退出程序", kColRed,    false},
    };
    for (const auto& t : tools) {
        float bx = 192.0f + t.col * 163.0f, by = 402.0f + t.row * 42.0f;
        HitItem hi{H_ACTION, {bx, by, 155.0f, 34.0f}, t.act, 0, -1};
        int idx = (int)gHits.size();
        gHits.push_back(hi);
        bool hov = !t.dim && (gHoverIdx == idx);
        DrawQuietBtn(cv, hi, t.label, hov, false, t.fg);
    }

    // 右栏上半：便携打包与传感器健康状态 (x=534, y=56, w=352, h=340)
    cv.Card(534.0f, 56.0f, 352.0f, 340.0f);
    cv.Text(cv.fmtTitle_, L"便携打包与传感器状态", 548.0f, 62.0f, 220.0f, 22.0f, kColText);

    float sy = 90.0f;
    {
        size_t b64 = 0, b32 = 0;
        bool embedOk = HookEmbedInfo(&b64, &b32);
        wchar_t embNote[96]{};
        swprintf(embNote, 96, L"64位 %.0fKB + 32位 %.0fKB", b64 / 1024.0, b32 / 1024.0);
        cv.FillRR(546.0f, sy, 328.0f, 29.0f, 6.0f, kColSurface);
        cv.rt()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(557.0f, sy + 14.5f), 3.0f, 3.0f),
                             cv.br(embedOk ? kColGreen : kColOrange));
        cv.Text(cv.fmtSmall_, L"单文件便携载荷", 566.0f, sy, 108.0f, 29.0f, kColText);
        cv.Text(cv.fmtMicro_, embedOk ? embNote : L"未检测到内嵌载荷", 674.0f, sy, 190.0f, 29.0f,
                kColMuted, DWRITE_TEXT_ALIGNMENT_TRAILING);
        sy += 33.0f;
    }

    {
        bool etwOn = np::Etw().active();
        cv.FillRR(546.0f, sy, 328.0f, 29.0f, 6.0f, kColSurface);
        cv.rt()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(557.0f, sy + 14.5f), 3.0f, 3.0f),
                             cv.br(etwOn ? kColGreen : kColOrange));
        cv.Text(cv.fmtSmall_, L"ETW 内核呈现", 566.0f, sy, 104.0f, 29.0f, kColText);
        std::wstring diag = np::Utf8ToWide(etwOn ? np::Etw().diagText() : np::Etw().status());
        cv.Text(cv.fmtMicro_, diag, 670.0f, sy, 194.0f, 29.0f, kColMuted,
                DWRITE_TEXT_ALIGNMENT_TRAILING);
        sy += 33.0f;
    }

    const auto& srcs = gApp.hub.sources();
    for (size_t i = 0; i < srcs.size() && sy < 368.0f; ++i) {
        const auto& src = srcs[i];
        cv.FillRR(546.0f, sy, 328.0f, 29.0f, 6.0f, kColSurface);
        uint32_t dotCol = src.used ? kColGreen : (src.present ? kColBlueHi : kColDim);
        cv.rt()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(557.0f, sy + 14.5f), 3.0f, 3.0f),
                             cv.br(dotCol));
        std::wstring sname = np::Utf8ToWide(src.name);
        if (sname == L"LibreHardwareMonitor") sname = L"LHM (WMI)";
        std::wstring snote = np::Utf8ToWide(src.note);
        cv.Text(cv.fmtSmall_, sname, 566.0f, sy, 108.0f, 29.0f,
                src.used || src.present ? kColText : kColDim);
        cv.Text(cv.fmtMicro_, snote, 674.0f, sy, 190.0f, 29.0f, kColMuted,
                DWRITE_TEXT_ALIGNMENT_TRAILING);
        sy += 33.0f;
    }

    // 右栏下半：全局快捷键 (x=534, y=406, w=352, h=136)
    cv.Card(534.0f, 406.0f, 352.0f, 136.0f);
    cv.Text(cv.fmtHead_, L"全局快捷键", 548.0f, 412.0f, 160.0f, 20.0f, kColText);
    struct HkRow { const wchar_t* keys; const wchar_t* desc; };
    const HkRow hks[5] = {
        {L"Ctrl + Shift + O", L"显隐全部 HUD 叠加层"},
        {L"Ctrl + Shift + B", L"启停 CSV 基准录制"},
        {L"Ctrl + Shift + P", L"暂停或恢复后台监控"},
        {L"Ctrl + Shift + R", L"重置实时监控曲线"},
        {L"Ctrl + Shift + L", L"切换 Metal 竖版 / 横条"},
    };
    for (int i = 0; i < 5; ++i) {
        float hy = 435.0f + i * 20.0f;
        cv.FillRR(546.0f, hy + 1.0f, 110.0f, 17.0f, 4.0f, kColCardAlt);
        cv.DrawRR(546.0f, hy + 1.0f, 110.0f, 17.0f, 4.0f, kColBorder);
        cv.Text(cv.fmtMonoSm_, hks[i].keys, 550.0f, hy, 102.0f, 19.0f, kColText,
                DWRITE_TEXT_ALIGNMENT_CENTER);
        cv.Text(cv.fmtMicro_, hks[i].desc, 664.0f, hy, 212.0f, 19.0f, kColMuted);
    }
}

static void RenderAll(HWND hwnd, HDC paintDc) {
    RECT cr{};
    GetClientRect(hwnd, &cr);
    int pxW = std::max(1, (int)(cr.right - cr.left));
    int pxH = std::max(1, (int)(cr.bottom - cr.top));
    float scale = (float)pxW / (float)kWinW;
    if (scale <= 0.1f) scale = 1.0f;

    if (!gCanvas.Ensure(pxW, pxH)) return;
    gHits.clear();

    std::vector<GameEntry> gamesSnap;
    {
        AppLock lk;
        gamesSnap = gApp.games;
    }

    ID2D1DCRenderTarget* rt = gCanvas.rt();
    rt->BeginDraw();
    rt->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
    rt->Clear(C(kColBg));

    DrawHeaderAndSidebar(gCanvas, gamesSnap);
    if (gActiveTab == 0) DrawTabOverview(gCanvas, gamesSnap);
    else if (gActiveTab == 1) DrawTabAppearance(gCanvas);
    else if (gActiveTab == 2) DrawTabCounters(gCanvas);
    else DrawTabSystem(gCanvas);

    HRESULT hr = rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        gCanvas.ReleaseTarget();
        return;
    }
    BitBlt(paintDc, 0, 0, pxW, pxH, gCanvas.hdc(), 0, 0, SRCCOPY);
}

static int HitTestLogical(float lx, float ly) {
    for (int i = (int)gHits.size() - 1; i >= 0; --i) {
        if (gHits[i].rc.contains(lx, ly)) return i;
    }
    return -1;
}

static void DoAction(int act) {
    switch (act) {
        case ACT_TOGGLE_MON:
            gApp.monitoring = !gApp.monitoring;
            if (gApp.monitoring) {
                InjectorScanNow();
                SetNotice("监控已启动", 0, 3000);
            } else {
                OverlaySetVisible(false);
                SetNotice("监控已暂停", 0, 3000);
            }
            AppPublish();
            break;
        case ACT_TOGGLE_OV:
            AppSetOverlayOff(AppOverlayVisible());
            break;
        case ACT_BENCH:
            BenchmarkToggle();
            break;
        case ACT_SAVE:
            SettingsSave();
            SetNotice("配置与游戏列表已保存", 0, 4000);
            break;
        case ACT_MINIMIZE:
            if (gMain) ShowWindow(gMain, SW_MINIMIZE);
            break;
        case ACT_CLOSE:
            if (gMain) PostMessageW(gMain, WM_CLOSE, 0, 0);
            break;
        case ACT_FORGET_LEARNED: {
            int n = ForgetLearned();
            gSelGame = -1;
            SetNotice("已清空学习名单（" + std::to_string(n) + " 条，对应钩子已请求卸载）", 0,
                      6000);
            break;
        }
        case ACT_ADD_GAME: {
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
                std::wstring p = path;
                size_t sl = p.find_last_of(L"\\/");
                std::wstring nm = (sl == std::wstring::npos) ? p : p.substr(sl + 1);
                for (auto& c : nm) c = (wchar_t)towlower(c);
                ge.name = nm;
                {
                    AppLock lk;
                    gApp.games.push_back(ge);
                }
                SettingsSave();
                SetNotice("已添加游戏：" + np::WideToUtf8(path), 0, 6000);
                AppLog("add game: %ls", path);
            }
            break;
        }
        case ACT_SCAN_GAMES:
            RequestScanGamesAsync();
            break;
        case ACT_INJECT_FRONT:
            RequestInjectFrontAsync();
            break;
        case ACT_COPY_DIAG:
            AppCopyDiagToClipboard();
            break;
        case ACT_RESET_HIST:
            AppResetHistory();
            break;
        case ACT_OPEN_LOGDIR: {
            std::wstring p = AppLogPath();
            size_t s = p.find_last_of(L'\\');
            if (s != std::wstring::npos) p = p.substr(0, s);
            ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            SetNotice("已打开日志目录：" + np::WideToUtf8(p), 0, 6000);
            break;
        }
        case ACT_OPEN_CSVDIR: {
            wchar_t tmp[MAX_PATH]{};
            if (!GetTempPathW(MAX_PATH, tmp)) wcscpy_s(tmp, L".");
            ShellExecuteW(nullptr, L"open", tmp, nullptr, nullptr, SW_SHOWNORMAL);
            SetNotice("已打开 CSV 输出目录", 0, 5000);
            break;
        }
        case ACT_ELEVATE:
            if (!gApp.isAdmin && AdminRelaunch()) {
                SetNotice("正在以管理员权限重启…", 0, 3000);
                PostMessageW(gMain, WM_COMMAND, NP_TRAY_EXIT, 0);
            }
            break;
        case ACT_QUIT:
            PostMessageW(gMain, WM_COMMAND, NP_TRAY_EXIT, 0);
            break;
        default: break;
    }
    if (gMain) InvalidateRect(gMain, nullptr, FALSE);
}

static void HandleHitClick(HWND hwnd, const HitItem& hi, float lx) {
    switch (hi.kind) {
        case H_NAV_TAB:
            gActiveTab = hi.arg;
            gHoverIdx = -1;
            break;
        case H_ACTION:
            DoAction(hi.arg);
            break;
        case H_GAME_ROW:
            gSelGame = hi.arg;
            break;
        case H_GAME_INJ: {
            DWORD pid = 0;
            bool injected = false;
            {
                AppLock lk;
                if (hi.arg >= 0 && hi.arg < (int)gApp.games.size()) {
                    pid = gApp.games[hi.arg].pid;
                    injected = gApp.games[hi.arg].injected;
                }
            }
            if (injected && pid) {
                RequestHookDetach(pid);
                SetNotice("已向该游戏下发卸载钩子指令", 0, 5000);
            } else if (pid) {
                RequestInjectPidAsync(pid);
            }
            break;
        }
        case H_GAME_REM:
            if (hi.arg >= 0) {
                ForgetGameAt(hi.arg);
                gSelGame = -1;
            }
            break;
        case H_PRESET:
            if (hi.arg == 4) {
                NPDefaultConfig(&gApp.cfg);
                gApp.cfg.pauseHook = gApp.monitoring ? 0u : 1u;
            } else {
                gApp.cfg.counters = hi.bit;
            }
            AppPublish();
            break;
        case H_GROUP_HDR:
            if (hi.arg >= 0 && hi.arg < kGroupN) {
                gGrpOpen[hi.arg] = !gGrpOpen[hi.arg];
                RebuildCounterRows();
            }
            break;
        case H_COUNTER:
            gApp.cfg.counters ^= hi.bit;
            AppPublish();
            break;
        case H_LAYOUT_MODE:
            NPConfigSetHorizontal(&gApp.cfg, hi.arg == 1);
            AppPublish();
            break;
        case H_THEME:
            NPConfigSetTheme(&gApp.cfg, (uint32_t)hi.arg);
            AppPublish();
            break;
        case H_CORNER:
            NPConfigSetCorner(&gApp.cfg, (uint32_t)hi.arg);
            AppPublish();
            break;
        case H_OPT_BIT:
            if (hi.arg == 0) NPConfigSetShowRange(&gApp.cfg, !NPConfigShowRange(gApp.cfg));
            else if (hi.arg == 1) NPConfigSetShowHeader(&gApp.cfg, !NPConfigShowHeader(gApp.cfg));
            AppPublish();
            break;
        case H_SLIDER:
            gDraggingSlider = hi.sliderIdx;
            SetCapture(hwnd);
            UpdateSliderValue(hi.sliderIdx, lx);
            break;
        case H_SYS_CYCLE:
            if (hi.arg == 0) {
                gApp.cfg.overlayMode = (gApp.cfg.overlayMode + 1) % 4;
            } else if (hi.arg == 1) {
                static const uint32_t kPolls[] = {100, 250, 500, 1000};
                uint32_t cur = gApp.cfg.pollMs ? gApp.cfg.pollMs : 500u;
                int next = 0;
                for (int i = 0; i < 4; ++i)
                    if (kPolls[i] == cur) { next = (i + 1) % 4; break; }
                gApp.cfg.pollMs = kPolls[next];
            } else if (hi.arg == 2) {
                static const uint32_t kHz[] = {10, 20, 30, 60};
                uint32_t cur = gApp.cfg.updateHz ? gApp.cfg.updateHz : 20u;
                int next = 0;
                for (int i = 0; i < 4; ++i)
                    if (kHz[i] == cur) { next = (i + 1) % 4; break; }
                gApp.cfg.updateHz = kHz[next];
            }
            AppPublish();
            break;
        case H_SYS_TOGGLE:
            if (hi.arg == 0) gApp.cfg.autoInject = gApp.cfg.autoInject ? 0u : 1u;
            else if (hi.arg == 1) gApp.cfg.learnedAutoHook = gApp.cfg.learnedAutoHook ? 0u : 1u;
            else if (hi.arg == 2) gApp.cfg.deepEngineHook = gApp.cfg.deepEngineHook ? 0u : 1u;
            else if (hi.arg == 3) gApp.cfg.vtableProbe = gApp.cfg.vtableProbe ? 0u : 1u;
            else if (hi.arg == 4) gApp.cfg.simulate = gApp.cfg.simulate ? 0u : 1u;
            AppPublish();
            break;
        default: break;
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CREATE:
            gPreviewPanel.Init();
            RebuildCounterRows();
            SetTimer(h, 2, 250, nullptr);
            return 0;

        case WM_NCCALCSIZE:
            if (w) return 0;
            break;

        case WM_NCACTIVATE:
            if (!IsIconic(h)) return TRUE;
            break;

        case WM_NCHITTEST: {
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            ScreenToClient(h, &pt);
            float s = CurrentClientScale(h);
            float lx = (float)pt.x / s, ly = (float)pt.y / s;
            if (ly >= 0.0f && ly < kHeaderH && HitTestLogical(lx, ly) < 0) return HTCAPTION;
            return HTCLIENT;
        }

        case WM_TIMER:
            if (w == 2) InvalidateRect(h, nullptr, FALSE);
            return 0;

        case WM_USER + 101:
            InvalidateRect(h, nullptr, FALSE);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_DPICHANGED: {
            UINT dpi = HIWORD(w);
            if (dpi) {
                gS = dpi / 96.0f;
                if (const RECT* prc = reinterpret_cast<const RECT*>(l)) {
                    SetWindowPos(h, nullptr, prc->left, prc->top, prc->right - prc->left,
                                 prc->bottom - prc->top, SWP_NOZORDER | SWP_NOACTIVATE);
                }
                gCanvas.ReleaseTarget();
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (!gTrackingLeave) {
                TRACKMOUSEEVENT tme{};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = h;
                if (TrackMouseEvent(&tme)) gTrackingLeave = true;
            }
            float s = CurrentClientScale(h);
            float lx = (float)GET_X_LPARAM(l) / s;
            float ly = (float)GET_Y_LPARAM(l) / s;
            if (gDraggingSlider >= 0) {
                UpdateSliderValue(gDraggingSlider, lx);
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            int hov = HitTestLogical(lx, ly);
            if (hov != gHoverIdx) {
                gHoverIdx = hov;
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            gTrackingLeave = false;
            if (gHoverIdx >= 0) {
                gHoverIdx = -1;
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONDOWN: {
            float s = CurrentClientScale(h);
            float lx = (float)GET_X_LPARAM(l) / s;
            float ly = (float)GET_Y_LPARAM(l) / s;
            int idx = HitTestLogical(lx, ly);
            if (idx >= 0 && idx < (int)gHits.size()) {
                HandleHitClick(h, gHits[idx], lx);
            }
            return 0;
        }

        case WM_LBUTTONUP:
            if (gDraggingSlider >= 0) {
                gDraggingSlider = -1;
                ReleaseCapture();
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;

        case WM_CAPTURECHANGED:
            gDraggingSlider = -1;
            return 0;

        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(w);
            int step = (delta > 0) ? -1 : 1;
            if (gActiveTab == 0) {
                size_t nGames = 0;
                { AppLock lk; nGames = gApp.games.size(); }
                int maxGs = std::max(0, (int)nGames - 6);
                gGameScroll = std::clamp(gGameScroll + step, 0, maxGs);
                InvalidateRect(h, nullptr, FALSE);
            } else if (gActiveTab == 2) {
                int maxCs = std::max(0, gCounterTotalRows - 11);
                gCounterScroll = std::clamp(gCounterScroll + step, 0, maxCs);
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(h, &ps);
            RenderAll(h, hdc);
            EndPaint(h, &ps);
            return 0;
        }

        case WM_CLOSE:
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
            if (l == 0) {
                AppTrayCommand(LOWORD(w));
                return 0;
            }
            break;

        case WM_DESTROY:
            KillTimer(h, 2);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

bool UiCreate(HINSTANCE inst, int cmdShow) {
    gInst = inst;
    gActiveTab = 0;
    gHoverIdx = -1;
    gDraggingSlider = -1;

    HDC scr = GetDC(nullptr);
    UINT dpi = scr ? (UINT)GetDeviceCaps(scr, LOGPIXELSX) : 96u;
    if (scr) ReleaseDC(nullptr, scr);
    gS = dpi / 96.0f;
    if (gS <= 0.0f) gS = 1.0f;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(0x14, 0x14, 0x16));
    wc.lpszClassName = L"NextPerfMainCls";
    RegisterClassExW(&wc);

    gMain = CreateWindowExW(0, L"NextPerfMainCls",
                            (L"NextPerf  [" + BuildStamp() + L"]").c_str(),
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                            CW_USEDEFAULT, CW_USEDEFAULT, S(kWinW), S(kWinH),
                            nullptr, nullptr, inst, nullptr);
    if (!gMain) return false;

    ApplyDarkFrame(gMain);
    SetWindowPos(gMain, nullptr, 0, 0, S(kWinW), S(kWinH),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);

    ShowWindow(gMain, cmdShow);
    UpdateWindow(gMain);
    return true;
}

HWND UiWindow() { return gMain; }

void UiDestroy() {
    if (gMain) {
        DestroyWindow(gMain);
        gMain = nullptr;
    }
    gPreviewPanel.Shutdown();
    gCanvas.Shutdown();
}

void UiRefresh() {
    static uint64_t last = 0;
    uint64_t now = GetTickCount64();
    if (now - last < 120) return;
    last = now;
    if (gMain) InvalidateRect(gMain, nullptr, FALSE);
}

}  // namespace npa
