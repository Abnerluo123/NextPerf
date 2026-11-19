// 注意：winuser.h 里有 #define DrawText DrawTextW 之类的宏，会把
// ID2D1RenderTarget::DrawText 也「改名」。本项目统一以 UNICODE 编译，
// 因此这里直接写 DrawText（会被展开成 DrawTextW，正是 d2d1.h 里声明的名字）。
// 不要在此处 #undef DrawText，否则会找不到成员。
//
// ---------------------------------------------------------------------------
// 视觉风格：向 Apple 的 Metal HUD 看齐
//
// 参考截图里能提炼出的要素（逐条落实在下面的代码里）：
//   1. **三列网格**：左侧标签、中间数值右对齐、右侧 `[ min  max ]` 方括号区间；
//   2. **数值用等宽字体** —— 金属 HUD 那种「数字像刻度一样对齐」的观感就来自这里。
//      中文标签仍用 UI 字体（等宽字体没有中文字形）；
//   3. **按指标分色**：FPS 白、帧时间/CPU 蓝、GPU 绿、内存白，异常值红；
//   4. **方角纯深底**，没有圆角、没有描边、没有阴影；
//   5. **行距极紧**（约 1.18 倍字号，而普通界面是 1.34+）；
//   6. **折线图不加 Y 轴刻度**，改成占满整宽的细线 + 极淡的竖向网格。
//      区间数值改在**上一行的方括号里**给 —— 这正是 Apple 的做法。
// ---------------------------------------------------------------------------

#include "np_panel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace np {

bool PanelRenderer::Init() {
    if (d2d_) return true;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, &d2d_))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(&dwrite_)))) {
        d2d_->Release(); d2d_ = nullptr; return false;
    }
    return true;
}

void PanelRenderer::Shutdown() {
    ReleaseFormats();
    if (dwrite_) { dwrite_->Release(); dwrite_ = nullptr; }
    if (d2d_) { d2d_->Release(); d2d_ = nullptr; }
}

void PanelRenderer::ReleaseFormats() {
    if (fmt_) { fmt_->Release(); fmt_ = nullptr; }
    if (fmtL_) { fmtL_->Release(); fmtL_ = nullptr; }
    if (fmtMonoL_) { fmtMonoL_->Release(); fmtMonoL_ = nullptr; }
    if (fmtR_) { fmtR_->Release(); fmtR_ = nullptr; }
    if (fmtSmall_) { fmtSmall_->Release(); fmtSmall_ = nullptr; }
    if (fmtSmallL_) { fmtSmallL_->Release(); fmtSmallL_ = nullptr; }
    if (fmtHead_) { fmtHead_->Release(); fmtHead_ = nullptr; }
}

static IDWriteTextFormat* MakeFmtFrom(IDWriteFactory* f, const wchar_t* const* faces, int n,
                                      float size, DWRITE_FONT_WEIGHT w,
                                      DWRITE_TEXT_ALIGNMENT align) {
    for (int i = 0; i < n; ++i) {
        IDWriteTextFormat* out = nullptr;
        HRESULT hr = f->CreateTextFormat(faces[i], nullptr, w, DWRITE_FONT_STYLE_NORMAL,
                                         DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", &out);
        if (SUCCEEDED(hr) && out) {
            out->SetTextAlignment(align);
            out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            return out;
        }
        if (out) out->Release();
    }
    return nullptr;
}

// 中文标签：使用现代无衬线字体 Microsoft YaHei UI，与右侧等宽数值及设置界面保持一致，
// 避免 NSimSun 宋体衬线字体混排割裂及 "0. 1%" 点号间距异常问题。
static IDWriteTextFormat* MakeFmtUI(IDWriteFactory* f, float size, DWRITE_FONT_WEIGHT w,
                                    DWRITE_TEXT_ALIGNMENT align) {
    static const wchar_t* faces[] = {L"Segoe UI Variable Text", L"Microsoft YaHei UI",
                                     L"Microsoft YaHei", L"SimSun"};
    return MakeFmtFrom(f, faces, 4, size, w, align);
}

// 数值 / 括号 / Metal 头部：**等宽字体**。这是 Metal HUD 观感的关键 ——
// 等宽让数字上下对齐成列，小数点严格垂直对齐。
static IDWriteTextFormat* MakeFmtMono(IDWriteFactory* f, float size, DWRITE_FONT_WEIGHT w,
                                      DWRITE_TEXT_ALIGNMENT align) {
    static const wchar_t* faces[] = {L"Cascadia Mono", L"Consolas", L"Lucida Console",
                                     L"Courier New"};
    return MakeFmtFrom(f, faces, 4, size, w, align);
}

void PanelRenderer::EnsureFormats(float fs) {
    if (!dwrite_) return;
    if (fmt_ && fmtMonoL_ && fabs(fs - fontSize_) < 0.01f) return;
    ReleaseFormats();
    fontSize_ = fs;
    latchedW_ = 0.0f;   // 字号变了，宽度锁重建
    fmt_ = MakeFmtUI(dwrite_, fs, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    fmtL_ = MakeFmtUI(dwrite_, fs, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
    fmtMonoL_ = MakeFmtMono(dwrite_, fs, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    fmtR_ = MakeFmtMono(dwrite_, fs, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    fmtSmall_ = MakeFmtMono(dwrite_, fs, DWRITE_FONT_WEIGHT_NORMAL,
                            DWRITE_TEXT_ALIGNMENT_TRAILING);
    fmtSmallL_ = MakeFmtMono(dwrite_, fs, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_TEXT_ALIGNMENT_LEADING);
    lineHeight_ = 0.0f;
    if (fmt_ && dwrite_) {
        IDWriteTextLayout* probe = nullptr;
        const wchar_t* sample = L"Ag0.帧\u00b0%";
        if (SUCCEEDED(dwrite_->CreateTextLayout(sample, 8, fmt_, 2048.0f, 2048.0f,
                                                &probe)) && probe) {
            DWRITE_LINE_METRICS lm{};
            UINT32 cnt = 0;
            if (SUCCEEDED(probe->GetLineMetrics(&lm, 1, &cnt)) && cnt > 0)
                lineHeight_ = lm.height;
            probe->Release();
        }
    }
    if (fmtMonoL_ && dwrite_) {
        IDWriteTextLayout* probe = nullptr;
        const wchar_t* sample = L"[2560x1440]";
        if (SUCCEEDED(dwrite_->CreateTextLayout(sample, 11, fmtMonoL_, 2048.0f, 2048.0f,
                                                &probe)) && probe) {
            DWRITE_LINE_METRICS lm{};
            UINT32 cnt = 0;
            if (SUCCEEDED(probe->GetLineMetrics(&lm, 1, &cnt)) && cnt > 0)
                lineHeight_ = std::max(lineHeight_, lm.height);
            probe->Release();
        }
    }
    if (lineHeight_ <= 0.0f) lineHeight_ = fs * 1.30f;   // 兜底
    fmtHead_ = MakeFmtUI(dwrite_, fs, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                         DWRITE_TEXT_ALIGNMENT_LEADING);
}

static float MeasureW(IDWriteFactory* f, IDWriteTextFormat* fmt, const std::wstring& s) {
    if (!fmt || s.empty()) return 0.0f;
    IDWriteTextLayout* lay = nullptr;
    if (FAILED(f->CreateTextLayout(s.c_str(), (UINT32)s.size(), fmt, 4096.0f, 256.0f, &lay)) || !lay)
        return (float)s.size() * fmt->GetFontSize() * 0.6f;
    DWRITE_TEXT_METRICS m{};
    lay->GetMetrics(&m);
    lay->Release();
    return m.width;
}

// ---------------------------------------------------------------- 布局常量
// 集中在这里，测量与绘制共用同一套数字，避免两边不一致（以前就因此错位过）。
namespace {
struct Metrics {
    float pad, lh, headH, chartH, rowGap, chartGap, colGap;
};
Metrics Layout(const NPConfig& c, float lineH) {
    Metrics m{};
    m.pad = 5.0f * c.scale;
    m.lh = std::max((float)c.fontHeight * 1.12f * c.scale, lineH);
    m.headH = m.lh;
    m.chartH = std::max(14.0f, (float)c.graphHeight * 0.28f) * c.scale;
    m.rowGap = 1.0f * c.scale;
    m.chartGap = 2.0f * c.scale;
    m.colGap = 4.0f * c.scale;
    return m;
}
}  // namespace

// Apple System Semantic Colors (macOS Dark / Metal Performance HUD)
struct RGB { float r, g, b; };
static RGB LevelRGB(int level) {
    switch (level) {
        case PL_WARN:   return {1.00f, 0.27f, 0.23f};  // Apple Red #FF453A
        case PL_ACCENT: return {1.00f, 0.62f, 0.04f};  // Apple Orange #FF9F0A
        case PL_DIM:    return {0.60f, 0.60f, 0.62f};  // Apple Secondary #98989D
        case PL_FPS:    return {0.96f, 0.96f, 0.97f};  // Apple Label #F5F5F7
        case PL_CPU:    return {0.16f, 0.59f, 1.00f};  // Apple Blue #2997FF
        case PL_GPU:    return {0.19f, 0.82f, 0.35f};  // Apple Green #30D158
        case PL_MEM:    return {0.96f, 0.96f, 0.97f};  // Apple Label #F5F5F7
        default:        return {0.92f, 0.92f, 0.94f};  // Crisp Extended Label
    }
}

struct HudTheme {
    RGB bg;
    RGB border;
    float borderBaseAlpha;
    RGB header;
    RGB label;
    RGB accent;
    float bgScale;
    bool textShadow;
};

static HudTheme ThemeFor(const NPConfig& c) {
    uint32_t id = NPConfigTheme(c);
    switch (id) {
        case 1:  // 液态暗灰 (macOS Frosted Charcoal)
            return {{0.11f, 0.11f, 0.125f}, {1.00f, 1.00f, 1.00f}, 0.14f,
                    {0.96f, 0.96f, 0.97f}, {0.92f, 0.92f, 0.94f}, {0.96f, 0.96f, 0.97f}, 1.00f, false};
        case 2:  // 浅雾透明 (Ultra Subtle Smoky)
            return {{0.05f, 0.05f, 0.06f}, {1.00f, 1.00f, 1.00f}, 0.07f,
                    {0.96f, 0.96f, 0.97f}, {0.90f, 0.90f, 0.92f}, {0.96f, 0.96f, 0.97f}, 0.62f, false};
        case 3:  // 极简纯字 (Text Only Floating)
            return {{0.00f, 0.00f, 0.00f}, {1.00f, 1.00f, 1.00f}, 0.00f,
                    {0.98f, 0.98f, 0.99f}, {0.94f, 0.94f, 0.96f}, {0.98f, 0.98f, 0.99f}, 0.00f, true};
        default: // 0: Metal 黑 (Authentic Apple Metal Performance HUD)
            return {{0.025f, 0.025f, 0.03f}, {1.00f, 1.00f, 1.00f}, 0.10f,
                    {0.96f, 0.96f, 0.97f}, {0.92f, 0.92f, 0.94f}, {0.96f, 0.96f, 0.97f}, 1.00f, false};
    }
}

// 该行是否显示方括号区间
static inline bool HasRange(const PanelRow& r) { return !r.vmin.empty() || !r.vmax.empty(); }

struct HChip {
    std::wstring label;
    std::wstring value;
    int level = PL_NORMAL;
    bool isTitle = false;
    float labelW = 0.0f;
    float valW = 0.0f;
    float totalW = 0.0f;
    int line = 0;
    float x = 0.0f;
    float y = 0.0f;
};

static void LayoutHorizontalChips(IDWriteFactory* f, IDWriteTextFormat* fmtText,
                                  IDWriteTextFormat* fmtHead, IDWriteTextFormat* fmtVal,
                                  const PanelData& d, const NPConfig& c, float pad, float lh,
                                  std::vector<HChip>* chips, float* outW, float* outH) {
    chips->clear();
    float chW = MeasureW(f, fmtVal, L"0");
    if (chW <= 0.0f) chW = fmtVal->GetFontSize() * 0.6f;
    auto quantVal = [&](float px) {
        float step = 2.0f * chW;
        return std::max(step, std::ceil(px / step - 0.0001f) * step);
    };

    if (!d.title.empty()) {
        HChip hc;
        hc.label = d.title;
        hc.isTitle = true;
        hc.labelW = MeasureW(f, fmtHead, hc.label);
        hc.totalW = hc.labelW;
        chips->push_back(std::move(hc));
    }
    std::wstring section;
    for (const auto& r : d.rows) {
        if (r.dualChart || r.splitSubHeader) continue;
        if (r.header) {
            if (!r.hideTitle && !r.label.empty()) section = r.label;
            else section.clear();
            continue;
        }
        HChip hc;
        if (!section.empty() &&
            (r.label == L"占用" || r.label == L"温度" || r.label == L"功耗")) {
            hc.label = section + L" " + r.label;
        } else {
            hc.label = r.label;
        }
        hc.value = r.value;
        hc.level = r.level;
        hc.labelW = MeasureW(f, fmtText, hc.label);
        hc.valW = quantVal(MeasureW(f, fmtVal, hc.value));
        hc.totalW = hc.labelW + 5.0f * c.scale + hc.valW;
        chips->push_back(std::move(hc));
    }

    const float sepW = 10.0f * c.scale;
    const float maxLineW = 1120.0f * c.scale;
    const float lineH = lh + 2.0f * c.scale;
    const float lineGap = 3.0f * c.scale;
    float curX = 0.0f;
    int curLine = 0;
    float maxUsedW = 0.0f;

    for (size_t i = 0; i < chips->size(); ++i) {
        HChip& hc = (*chips)[i];
        float need = (curX > 0.0f) ? (sepW + hc.totalW) : hc.totalW;
        if (curX > 0.0f && curX + need > maxLineW) {
            maxUsedW = std::max(maxUsedW, curX);
            ++curLine;
            curX = 0.0f;
        }
        if (curX > 0.0f) curX += sepW;
        hc.line = curLine;
        hc.x = pad + curX;
        hc.y = pad + (float)curLine * (lineH + lineGap);
        curX += hc.totalW;
        maxUsedW = std::max(maxUsedW, curX);
    }

    if (chips->empty()) {
        *outW = 180.0f * c.scale;
        *outH = lineH + pad * 2.0f;
    } else {
        int lines = curLine + 1;
        *outW = std::max(140.0f * c.scale, maxUsedW + pad * 2.0f);
        *outH = (float)lines * lineH + (float)(lines - 1) * lineGap + pad * 2.0f;
    }
}

// ---------------------------------------------------------------- 解耦网格（核心列紧密对齐 + 扩展区零死区）
template <typename GridT>
static GridT ComputeGridT(IDWriteFactory* f, IDWriteTextFormat* fmtText,
                          IDWriteTextFormat* fmtMonoL, IDWriteTextFormat* fmtR,
                          IDWriteTextFormat* fmtSmall, const PanelData& d, bool wantRange) {
    GridT g;
    float chW = MeasureW(f, fmtSmall, L"0");
    if (chW <= 0.0f) chW = fmtSmall->GetFontSize() * 0.6f;
    float rChW = MeasureW(f, fmtR, L"0");
    if (rChW <= 0.0f) rChW = fmtR->GetFontSize() * 0.6f;
    g.charW = chW;
    g.openW = chW;
    g.closeW = chW;
    g.gapW = chW;

    auto cols = [&](float px) { return std::ceil(px / chW - 0.0001f) * chW; };
    auto vcols = [&](float px) {
        float step = 2.0f * rChW;
        return std::max(6.0f * rChW, std::ceil(px / step - 0.0001f) * step);
    };

    float maxCoreLbl = 0.0f;
    float maxCoreVal = 0.0f;
    float maxRangeW = 0.0f;
    float maxRightBrkW = 0.0f;
    bool hasAnyRange = false;
    float maxExtRow = 0.0f;

    for (auto& r : d.rows) {
        if (r.header || r.dualChart || r.splitSubHeader) continue;
        if (r.metalCore) {
            maxCoreLbl = std::max(maxCoreLbl, MeasureW(f, fmtMonoL, r.label + r.hint));
            maxCoreVal = std::max(maxCoreVal, MeasureW(f, fmtR, r.value));
            if (!r.vmin.empty() || !r.vmax.empty()) {
                hasAnyRange = true;
                float rw = g.openW + MeasureW(f, fmtSmall, r.vmin) + g.gapW +
                           MeasureW(f, fmtSmall, r.vmax) + g.closeW;
                maxRangeW = std::max(maxRangeW, rw);
            }
            if (!r.rightBracket.empty()) {
                maxRightBrkW = std::max(maxRightBrkW, MeasureW(f, fmtSmall, r.rightBracket) + chW);
            }
        } else {
            float lw = MeasureW(f, fmtText, r.label + r.hint);
            float vw = MeasureW(f, fmtR, r.value);
            float rw = lw + 10.0f * (chW / 8.0f) + vw;
            maxExtRow = std::max(maxExtRow, rw);
        }
    }

    g.coreLabelW = cols(maxCoreLbl);
    g.coreValW = vcols(maxCoreVal);
    if (wantRange && hasAnyRange) {
        g.coreRngW = std::max({13.0f * chW, maxRangeW, maxRightBrkW});
    } else if (maxRightBrkW > 0.0f) {
        g.coreRngW = maxRightBrkW;
    } else {
        g.coreRngW = 0.0f;
    }
    g.extW = cols(maxExtRow);
    g.labelW = g.coreLabelW;
    g.valueW = g.coreValW;
    g.rngW = g.coreRngW;
    return g;
}

void PanelRenderer::MeasureInternal(const PanelData& d, const NPConfig& c, float* w, float* h,
                                    Grid* gridOut) const {
    if (dwrite_) const_cast<PanelRenderer*>(this)->EnsureFormats((float)c.fontHeight * c.scale);
    uint32_t modeBits = c.fpsCap & 0x7Cu;
    if (c.counters != lastCounters_ || modeBits != lastModeBits_) {
        lastCounters_ = c.counters;
        lastModeBits_ = modeBits;
        latchedW_ = 0.0f;
    }
    Metrics M = Layout(c, lineHeight_);
    if (!fmt_ || !fmtMonoL_ || !fmtR_ || !fmtSmall_) {
        *w = 210.0f;
        *h = 110.0f;
        if (gridOut) *gridOut = Grid{};
        return;
    }

    if (NPConfigHorizontal(c)) {
        std::vector<HChip> chips;
        float hw = 0.0f, hh = 0.0f;
        LayoutHorizontalChips(dwrite_, fmt_, fmtHead_, fmtR_, d, c,
                              6.0f * c.scale + 2.0f, M.lh, &chips, &hw, &hh);
        *w = hw;
        *h = hh;
        if (gridOut) *gridOut = Grid{};
        return;
    }

    const bool wantRange = NPConfigShowRange(c);
    Grid G = ComputeGridT<Grid>(dwrite_, fmt_, fmtMonoL_, fmtR_, fmtSmall_, d, wantRange);
    float headW = 0.0f;
    if (!d.gpuHeaderLeft.empty() || !d.gpuHeaderRight.empty()) {
        float spW = MeasureW(dwrite_, fmtMonoL_, L"0");
        float w1 = MeasureW(dwrite_, fmtMonoL_, d.gpuHeaderLeft) +
                   (d.gpuHeaderRight.empty()
                        ? 0.0f
                        : (8.0f * c.scale + MeasureW(dwrite_, fmtMonoL_, d.gpuHeaderRight)));
        float gap2 = (!d.modeHeaderPrefix.empty() && !d.modeHeaderTag.empty()) ? spW : 0.0f;
        float w2 = MeasureW(dwrite_, fmtMonoL_, d.modeHeaderPrefix) + gap2 +
                   MeasureW(dwrite_, fmtMonoL_, d.modeHeaderTag) +
                   (d.modeHeaderRight.empty()
                        ? 0.0f
                        : (8.0f * c.scale + MeasureW(dwrite_, fmtMonoL_, d.modeHeaderRight)));
        headW = std::max({headW, w1, w2});
    }
    for (auto& r : d.rows) {
        if (r.header) {
            headW = std::max(headW, MeasureW(dwrite_, fmtHead_, r.label));
        } else if (r.splitSubHeader) {
            float sw = MeasureW(dwrite_, fmtMonoL_, r.label) +
                       (r.value.empty()
                            ? 0.0f
                            : (8.0f * c.scale + MeasureW(dwrite_, fmtMonoL_, r.value)));
            headW = std::max(headW, sw);
        }
    }

    float coreTotalW = G.coreLabelW + M.colGap + G.coreValW +
                       (G.coreRngW > 0.0f ? M.colGap + G.coreRngW : 0.0f);
    float contentW = std::max({coreTotalW, G.extW, headW});
    diag.labelW = G.coreLabelW;
    diag.valueW = G.coreValW;
    diag.rngW = G.coreRngW;
    diag.headW = headW;
    diag.charW = G.charW;
    diag.contentW = contentW;
    diag.rowH = M.lh;
    diag.sepH = 3.0f * c.scale;

    float want = std::max(156.0f * c.scale, contentW + M.pad * 2.0f);
    if (want > latchedW_) latchedW_ = want;
    *w = latchedW_;

    float dualPlotH = std::max(30.0f, (float)c.graphHeight) * c.scale;
    float rowsH = 0.0f;
    if (!d.gpuHeaderLeft.empty() || !d.gpuHeaderRight.empty()) {
        rowsH += 2.0f * M.lh + 2.0f * c.scale;
    }
    bool firstRow = true;
    for (auto& r : d.rows) {
        if (!firstRow && r.gapBefore > 0.0f) rowsH += r.gapBefore * c.scale;
        if (r.header) {
            if (!r.hideTitle) rowsH += M.headH;
        } else if (r.dualChart) {
            rowsH += 2.0f * c.scale + dualPlotH + 3.0f * c.scale;
        } else if (r.splitSubHeader) {
            rowsH += M.lh;
        } else {
            rowsH += M.lh;
            if (r.chart) rowsH += M.chartGap + M.chartH;
        }
        firstRow = false;
    }
    *h = rowsH + M.pad * 2.0f;
    if (gridOut) *gridOut = G;
}

void PanelRenderer::Measure(const PanelData& d, const NPConfig& c, float* w, float* h) const {
    MeasureInternal(d, c, w, h, nullptr);
}

void PanelRenderer::Render(ID2D1RenderTarget* rt, float x, float y, const PanelData& d,
                           const NPConfig& c) {
    if (!rt || !d2d_) return;
    EnsureFormats((float)c.fontHeight * c.scale);
    if (!fmt_ || !fmtMonoL_ || !fmtR_) return;

    Metrics M = Layout(c, lineHeight_);
    float w = 0.0f, h = 0.0f;
    Grid G{};
    MeasureInternal(d, c, &w, &h, &G);

    const float alpha = std::clamp(c.textOpacity, 0.0f, 1.0f);
    const HudTheme theme = ThemeFor(c);

    float effBgOp = std::clamp(c.bgOpacity * theme.bgScale, 0.0f, 1.0f);
    D2D1_COLOR_F bg = D2D1::ColorF(theme.bg.r, theme.bg.g, theme.bg.b, effBgOp);
    if (NPConfigTheme(c) == 0 && c.bgColor != 0xFF000000u) {
        bg = ToColorF(c.bgColor);
        bg.a = std::clamp(bg.a * effBgOp, 0.0f, 1.0f);
    }
    ID2D1SolidColorBrush* brBg = nullptr;
    if (FAILED(rt->CreateSolidColorBrush(bg, &brBg)) || !brBg) return;
    if (effBgOp > 0.005f) {
        float rad = (NPConfigHorizontal(c) ? 7.0f : 8.0f) * c.scale;
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), rad, rad),
                                 brBg);
        if (theme.borderBaseAlpha > 0.005f && alpha > 0.05f) {
            float borderA = theme.borderBaseAlpha * std::min(1.0f, effBgOp * 1.3f) * alpha;
            ID2D1SolidColorBrush* brEdge = nullptr;
            if (SUCCEEDED(rt->CreateSolidColorBrush(
                    D2D1::ColorF(theme.border.r, theme.border.g, theme.border.b, borderA),
                    &brEdge)) &&
                brEdge) {
                float ins = 0.5f * std::max(1.0f, c.scale);
                float rIn = std::max(1.0f, rad - ins);
                rt->DrawRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(x + ins, y + ins, x + w - ins, y + h - ins),
                                      rIn, rIn),
                    brEdge, std::max(1.0f, 1.0f * c.scale));
                brEdge->Release();
            }
        }
    }

    auto mk = [&](RGB col, float a, ID2D1SolidColorBrush** out) {
        D2D1_COLOR_F cf = D2D1::ColorF(col.r, col.g, col.b, std::clamp(a * alpha, 0.0f, 1.0f));
        return SUCCEEDED(rt->CreateSolidColorBrush(cf, out)) && *out;
    };
    ID2D1SolidColorBrush* brLabel = nullptr;
    ID2D1SolidColorBrush* brGrid = nullptr;
    ID2D1SolidColorBrush* brShadow = nullptr;
    if (!mk(theme.label, 0.94f, &brLabel) || !mk(theme.border, 0.14f, &brGrid)) {
        if (brLabel) brLabel->Release();
        if (brGrid) brGrid->Release();
        brBg->Release();
        return;
    }
    if (theme.textShadow) mk({0.0f, 0.0f, 0.0f}, 0.78f, &brShadow);

    auto drawTxt = [&](const std::wstring& str, IDWriteTextFormat* fmt, const D2D1_RECT_F& rc,
                       ID2D1SolidColorBrush* brush) {
        if (str.empty() || !fmt || !brush) return;
        if (brShadow) {
            float dlt = std::max(1.0f, 1.0f * c.scale);
            D2D1_RECT_F sr{rc.left + dlt, rc.top + dlt, rc.right + dlt, rc.bottom + dlt};
            rt->DrawText(str.c_str(), (UINT32)str.size(), fmt, sr, brShadow,
                         D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        rt->DrawText(str.c_str(), (UINT32)str.size(), fmt, rc, brush,
                     D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };

    // ---------------- 横向极简条模式 ----------------
    if (NPConfigHorizontal(c)) {
        std::vector<HChip> chips;
        float hw = 0.0f, hh = 0.0f;
        float hPad = 6.0f * c.scale + 2.0f;
        LayoutHorizontalChips(dwrite_, fmt_, fmtHead_, fmtR_, d, c, hPad, M.lh, &chips, &hw,
                              &hh);
        float lineH = M.lh + 2.0f * c.scale;
        ID2D1SolidColorBrush* brHead = nullptr;
        mk(theme.header, 1.0f, &brHead);
        for (size_t i = 0; i < chips.size(); ++i) {
            const HChip& hc = chips[i];
            float cx0 = x + hc.x;
            float cy0 = y + hc.y;
            if (i > 0 && chips[i - 1].line == hc.line) {
                float sx = cx0 - 5.0f * c.scale;
                rt->DrawLine(D2D1::Point2F(sx, cy0 + 2.0f * c.scale),
                             D2D1::Point2F(sx, cy0 + lineH - 2.0f * c.scale), brGrid,
                             std::max(1.0f, 1.0f * c.scale));
            }
            if (hc.isTitle) {
                drawTxt(hc.label, fmtHead_,
                        D2D1::RectF(cx0, cy0, cx0 + hc.labelW + 2.0f, cy0 + lineH), brHead);
            } else {
                drawTxt(hc.label, fmt_,
                        D2D1::RectF(cx0, cy0, cx0 + hc.labelW + 2.0f, cy0 + lineH), brLabel);
                ID2D1SolidColorBrush* bv = nullptr;
                if (mk(LevelRGB(hc.level), 1.0f, &bv)) {
                    float vx = cx0 + hc.labelW + 5.0f * c.scale;
                    drawTxt(hc.value, fmtR_, D2D1::RectF(vx, cy0, vx + hc.valW, cy0 + lineH), bv);
                    bv->Release();
                }
            }
        }
        if (brHead) brHead->Release();
        if (brShadow) brShadow->Release();
        brGrid->Release();
        brLabel->Release();
        brBg->Release();
        return;
    }

    // ---------------- Apple Metal 纵向 HUD 模式 ----------------
    float labelX = x + M.pad;
    float rightX = x + w - M.pad;
    float rngX = rightX - G.coreRngW;
    float coreValLeft = labelX + G.coreLabelW + M.colGap;
    float coreValRight = coreValLeft + G.coreValW;
    float cy = y + M.pad;

    // 1) Metal 2 行设备顶栏
    if (!d.gpuHeaderLeft.empty() || !d.gpuHeaderRight.empty()) {
        ID2D1SolidColorBrush* bh = nullptr;
        ID2D1SolidColorBrush* brWarn = nullptr;
        mk(theme.header, 0.96f, &bh);
        mk(LevelRGB(PL_WARN), 1.0f, &brWarn);
        if (bh) {
            float r1W = MeasureW(dwrite_, fmtMonoL_, d.gpuHeaderRight) + 4.0f;
            drawTxt(d.gpuHeaderLeft, fmtMonoL_,
                    D2D1::RectF(labelX, cy, std::max(labelX + 40.0f, rightX - r1W), cy + M.lh),
                    bh);
            if (!d.gpuHeaderRight.empty()) {
                drawTxt(d.gpuHeaderRight, fmtR_,
                        D2D1::RectF(rightX - r1W, cy, rightX, cy + M.lh), bh);
            }
            cy += M.lh;

            float spW = MeasureW(dwrite_, fmtMonoL_, L"0");
            float pfxW = MeasureW(dwrite_, fmtMonoL_, d.modeHeaderPrefix);
            float tagX = labelX + pfxW + ((!d.modeHeaderPrefix.empty() && !d.modeHeaderTag.empty()) ? spW : 0.0f);
            float r2W = MeasureW(dwrite_, fmtMonoL_, d.modeHeaderRight) + 4.0f;
            if (!d.modeHeaderPrefix.empty()) {
                drawTxt(d.modeHeaderPrefix, fmtMonoL_,
                        D2D1::RectF(labelX, cy, labelX + pfxW + 2.0f, cy + M.lh), bh);
            }
            if (!d.modeHeaderTag.empty()) {
                drawTxt(d.modeHeaderTag, fmtMonoL_,
                        D2D1::RectF(tagX, cy, std::max(tagX + 20.0f, rightX - r2W),
                                    cy + M.lh),
                        d.modeHeaderWarn && brWarn ? brWarn : bh);
            }
            if (!d.modeHeaderRight.empty()) {
                drawTxt(d.modeHeaderRight, fmtR_,
                        D2D1::RectF(rightX - r2W, cy, rightX, cy + M.lh), bh);
            }
            cy += M.lh + 2.0f * c.scale;
        }
        if (brWarn) brWarn->Release();
        if (bh) bh->Release();
    }

    auto plotCurve = [&](const float* series, uint32_t cap, uint32_t write, uint32_t n,
                         float px0, float py0, float pw, float ph, float yMin, float yRange,
                         RGB col, float strokePx, bool skipNonPositive) {
        if (!series || n < 2 || yRange <= 0.0f) return;
        ID2D1SolidColorBrush* bl = nullptr;
        if (!mk(col, 1.0f, &bl)) return;
        uint32_t start = (write + cap - n) % cap;
        bool hasPrev = false;
        float prevX = 0.0f, prevY = 0.0f;
        for (uint32_t i = 0; i < n; ++i) {
            float v = series[(start + i) % cap];
            if (skipNonPositive && v <= 0.0f) {
                hasPrev = false;
                continue;
            }
            float frac = (float)i / (float)(n - 1);
            float px = px0 + pw * frac;
            float py = py0 + ph * (1.0f - std::clamp((v - yMin) / yRange, 0.0f, 1.0f));
            if (hasPrev) {
                rt->DrawLine(D2D1::Point2F(prevX, prevY), D2D1::Point2F(px, py), bl, strokePx);
            }
            prevX = px;
            prevY = py;
            hasPrev = true;
        }
        bl->Release();
    };

    float dualPlotH = std::max(30.0f, (float)c.graphHeight) * c.scale;
    bool firstRow = true;
    for (auto& r : d.rows) {
        if (!firstRow && r.gapBefore > 0.0f) cy += r.gapBefore * c.scale;

        if (r.header) {
            if (!r.hideTitle) {
                ID2D1SolidColorBrush* bh = nullptr;
                if (mk(theme.header, 1.0f, &bh)) {
                    drawTxt(r.label, fmtHead_, D2D1::RectF(labelX, cy, rightX, cy + M.headH), bh);
                    bh->Release();
                }
                cy += M.headH;
            }
            firstRow = false;
            continue;
        }

        // ---- 图表下方分段子标题（如 CyberDemo.exe / D3D12）
        if (r.splitSubHeader) {
            ID2D1SolidColorBrush* bh = nullptr;
            if (mk(theme.header, 0.96f, &bh)) {
                float rw = MeasureW(dwrite_, fmtMonoL_, r.value) + 4.0f;
                drawTxt(r.label, fmtMonoL_,
                        D2D1::RectF(labelX, cy, std::max(labelX + 40.0f, rightX - rw), cy + M.lh),
                        bh);
                if (!r.value.empty()) {
                    drawTxt(r.value, fmtR_, D2D1::RectF(rightX - rw, cy, rightX, cy + M.lh), bh);
                }
                bh->Release();
            }
            cy += M.lh;
            firstRow = false;
            continue;
        }

        // ---- Apple Metal 独立双通道示波器图
        if (r.dualChart && r.chart) {
            cy += 2.0f * c.scale;
            float plotX = labelX;
            float plotW = rightX - plotX;
            float plotY = cy;
            float plotH = dualPlotH;
            if (plotW > 8.0f) {
                ID2D1SolidColorBrush* brTick = nullptr;
                ID2D1SolidColorBrush* brBase = nullptr;
                mk({1.0f, 1.0f, 1.0f}, 0.22f, &brTick);
                mk({1.0f, 1.0f, 1.0f}, 0.48f, &brBase);
                float tickW = std::max(1.0f, 1.0f * c.scale);
                if (brTick) {
                    for (int g = 0; g <= 10; ++g) {
                        float gx = plotX + plotW * (float)g / 10.0f;
                        rt->DrawLine(D2D1::Point2F(gx, plotY), D2D1::Point2F(gx, plotY + plotH),
                                     brTick, tickW);
                    }
                    brTick->Release();
                }
                if (brBase) {
                    rt->DrawLine(D2D1::Point2F(plotX, plotY + plotH),
                                 D2D1::Point2F(plotX + plotW, plotY + plotH), brBase, tickW);
                    brBase->Release();
                }

                uint32_t n = std::min<uint32_t>(r.chartCount ? r.chartCount : r.chartCap, r.chartCap);
                if (n >= 2) {
                    float yMin = 0.0f, yMax = r.chartFixedMax;
                    if (yMax <= 0.0f) {
                        float mx = 0.0f;
                        uint32_t start = (r.chartWrite + r.chartCap - n) % r.chartCap;
                        for (uint32_t i = 0; i < n; ++i) {
                            uint32_t idx = (start + i) % r.chartCap;
                            if (r.series && r.series[idx] > 0.0f) mx = std::max(mx, r.series[idx]);
                            if (r.series2 && r.series2[idx] > 0.0f) mx = std::max(mx, r.series2[idx]);
                        }
                        if (r.unit == 1) {
                            yMax = std::clamp(mx * 1.40f, 25.0f, 100.0f);
                        } else {
                            yMax = (mx > 0.5f) ? std::max(4.0f, mx * 1.15f) : 33.3f;
                        }
                    }
                    float yRange = std::max(0.01f, yMax - yMin);
                    float lw = std::max(1.2f, 1.4f * c.scale);
                    plotCurve(r.series, r.chartCap, r.chartWrite, n, plotX, plotY, plotW, plotH,
                              yMin, yRange, LevelRGB(r.level), lw, r.chartFixedMax <= 0.0f);
                    if (r.series2) {
                        plotCurve(r.series2, r.chartCap, r.chartWrite, n, plotX, plotY, plotW, plotH,
                                  yMin, yRange, LevelRGB(r.level2), lw, true);
                    }
                }
            }
            cy += plotH + 3.0f * c.scale;
            firstRow = false;
            continue;
        }

        RGB vc = LevelRGB(r.level);

        // ---- 左侧标签（metalCore 紧跟指标色 + 等宽；扩展行左对齐整宽）
        IDWriteTextFormat* lblFmt = r.metalCore ? fmtMonoL_ : fmt_;
        float lblW = r.metalCore ? G.coreLabelW : std::max(60.0f, rightX - labelX - 40.0f);
        ID2D1SolidColorBrush* bCoreLbl = nullptr;
        if (r.metalCore) mk(vc, 1.0f, &bCoreLbl);
        drawTxt(r.label, lblFmt, D2D1::RectF(labelX, cy, labelX + lblW, cy + M.lh),
                bCoreLbl ? bCoreLbl : brLabel);
        if (bCoreLbl) bCoreLbl->Release();

        if (!r.hint.empty()) {
            float lw = MeasureW(dwrite_, fmt_, r.label);
            ID2D1SolidColorBrush* bHint = nullptr;
            if (mk(LevelRGB(PL_ACCENT), 1.0f, &bHint)) {
                drawTxt(r.hint, fmt_, D2D1::RectF(labelX + lw, cy, labelX + lblW, cy + M.lh),
                        bHint);
                bHint->Release();
            }
        }

        // ---- 数值（核心行紧跟标签右对齐于 coreValRight；扩展行直接贴面板右缘 rightX）
        float valLeft = r.metalCore ? coreValLeft : (labelX + 36.0f * c.scale);
        float valRight = r.metalCore ? std::min(coreValRight, (G.coreRngW > 0.0f ? rngX - M.colGap : rightX))
                                     : rightX;
        ID2D1SolidColorBrush* bv = nullptr;
        if (mk(vc, 1.0f, &bv)) {
            drawTxt(r.value, fmtR_, D2D1::RectF(valLeft, cy, std::max(valLeft + 8.0f, valRight), cy + M.lh),
                    bv);
        }

        // ---- 右侧列（对齐 rightX）：单括号 [VRAM] 或 `[ min  max ]` 区间
        if (!r.rightBracket.empty() && G.coreRngW > 0.0f) {
            ID2D1SolidColorBrush* brk = nullptr;
            if (mk(LevelRGB(r.rightBracketLevel), 0.96f, &brk)) {
                drawTxt(r.rightBracket, fmtSmall_,
                        D2D1::RectF(rngX, cy, rightX, cy + M.lh), brk);
                brk->Release();
            }
        } else if (HasRange(r) && G.coreRngW > 0.0f && bv) {
            float wr = cy;
            auto drawAt = [&](const std::wstring& s, IDWriteTextFormat* fmt,
                              ID2D1SolidColorBrush* b, float fromX, float toX) {
                if (s.empty() || !b) return;
                drawTxt(s, fmt, D2D1::RectF(fromX, wr, toX, wr + M.lh), b);
            };
            ID2D1SolidColorBrush* bbrk = nullptr;
            mk(vc, 0.85f, &bbrk);
            ID2D1SolidColorBrush* bmin = nullptr;
            ID2D1SolidColorBrush* bmax = nullptr;
            mk(r.warnMin ? LevelRGB(PL_WARN) : vc, 0.96f, &bmin);
            mk(r.warnMax ? LevelRGB(PL_WARN) : vc, 0.96f, &bmax);

            float wmax = MeasureW(dwrite_, fmtSmall_, r.vmax);
            float wmin = MeasureW(dwrite_, fmtSmall_, r.vmin);
            float rEdge = rightX;
            float closeL = rEdge - G.closeW;
            float maxL = closeL - wmax;
            float minR = maxL - G.gapW;
            float minL = minR - wmin;
            float openL = minL - G.openW;
            drawAt(L"[", fmtSmall_, bbrk, openL, minL);
            drawAt(r.vmin, fmtSmall_, bmin, minL, minR);
            drawAt(r.vmax, fmtSmall_, bmax, maxL, closeL);
            drawAt(L"]", fmtSmallL_, bbrk, closeL, rEdge);

            if (bbrk) bbrk->Release();
            if (bmin) bmin->Release();
            if (bmax) bmax->Release();
        }
        if (bv) bv->Release();
        cy += M.lh;

        // ---- 可选扩展行内小折线（仅当用户主动开启图表子项时绘制，保持 Metal 极简网格样式）
        if (r.chart && r.series && r.chartCap >= 2) {
            uint32_t n = std::min<uint32_t>(r.chartCount ? r.chartCount : r.chartCap, r.chartCap);
            cy += M.chartGap;
            float plotX = labelX;
            float plotW = rightX - plotX;
            float plotY = cy;
            float plotH = M.chartH;
            if (n >= 2 && plotW > 8.0f) {
                uint32_t start = (r.chartWrite + r.chartCap - n) % r.chartCap;
                double sum = 0.0;
                float mn = 1e30f, mx = -1e30f;
                for (uint32_t i = 0; i < n; ++i) {
                    float v = r.series[(start + i) % r.chartCap];
                    sum += v;
                    mn = std::min(mn, v);
                    mx = std::max(mx, v);
                }
                float avg = (float)(sum / (double)n);
                float span = std::max(fabsf(avg) * 0.35f, (mx - mn) * 0.60f);
                if (span < 0.5f) span = std::max(fabsf(avg) * 0.20f, 0.5f);
                float yMin = std::max(0.0f, avg - span), yMax = avg + span;
                if (r.unit == 1) yMax = std::min(100.0f, yMax);
                if (yMax - yMin < 0.5f) yMax = yMin + 0.5f;
                for (int g = 0; g <= 6; ++g) {
                    float gx = plotX + plotW * (float)g / 6.0f;
                    rt->DrawLine(D2D1::Point2F(gx, plotY), D2D1::Point2F(gx, plotY + plotH),
                                 brGrid, 1.0f * c.scale);
                }
                rt->DrawLine(D2D1::Point2F(plotX, plotY + plotH),
                             D2D1::Point2F(plotX + plotW, plotY + plotH), brGrid,
                             1.0f * c.scale);
                plotCurve(r.series, r.chartCap, r.chartWrite, n, plotX, plotY, plotW, plotH,
                          yMin, yMax - yMin, vc, std::max(1.0f, 1.25f * c.scale), false);
            }
            cy += plotH;
        }
        firstRow = false;
    }

    if (brShadow) brShadow->Release();
    brGrid->Release();
    brLabel->Release();
    brBg->Release();
}

}  // namespace np
