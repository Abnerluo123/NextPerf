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

// 中文标签：UI 字体（等宽字体没有中文字形）
static IDWriteTextFormat* MakeFmtUI(IDWriteFactory* f, float size, DWRITE_FONT_WEIGHT w,
                                    DWRITE_TEXT_ALIGNMENT align) {
    static const wchar_t* faces[] = {L"Microsoft YaHei UI", L"Microsoft YaHei", L"Segoe UI"};
    return MakeFmtFrom(f, faces, 3, size, w, align);
}

// 数值 / 括号：**等宽字体**。这是 Metal HUD 观感的关键 ——
// 等宽让数字上下对齐成列，多位数也不会左右跳。
static IDWriteTextFormat* MakeFmtMono(IDWriteFactory* f, float size, DWRITE_FONT_WEIGHT w,
                                      DWRITE_TEXT_ALIGNMENT align) {
    static const wchar_t* faces[] = {L"Cascadia Mono", L"Consolas", L"Lucida Console",
                                     L"Courier New"};
    return MakeFmtFrom(f, faces, 4, size, w, align);
}

void PanelRenderer::EnsureFormats(float fs) {
    if (fmt_ && fabs(fs - fontSize_) < 0.01f) return;
    ReleaseFormats();
    fontSize_ = fs;
    fmt_ = MakeFmtUI(dwrite_, fs, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    fmtR_ = MakeFmtMono(dwrite_, fs, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    fmtSmall_ = MakeFmtMono(dwrite_, fs, DWRITE_FONT_WEIGHT_NORMAL,
                            DWRITE_TEXT_ALIGNMENT_TRAILING);
    fmtSmallL_ = MakeFmtMono(dwrite_, fs, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_TEXT_ALIGNMENT_LEADING);
    fmtHead_ = MakeFmtUI(dwrite_, fs * 0.92f, DWRITE_FONT_WEIGHT_SEMI_BOLD,
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
Metrics Layout(const NPConfig& c) {
    Metrics m{};
    m.pad = 7.0f * c.scale;
    // 金属 HUD 的行距非常紧：约 1.18 倍字号。普通界面用 1.34 会显得松散。
    m.lh = (float)c.fontHeight * 1.18f * c.scale;
    m.headH = m.lh * 0.92f;
    m.chartH = std::max(16.0f, (float)c.graphHeight * 0.42f) * c.scale;
    m.rowGap = 1.0f * c.scale;
    m.chartGap = 2.0f * c.scale;
    m.colGap = 6.0f * c.scale;
    return m;
}
}  // namespace

// 行配色：Metal HUD 按指标分色
struct RGB { float r, g, b; };
static RGB LevelRGB(int level) {
    switch (level) {
        case PL_WARN:  return {1.00f, 0.42f, 0.42f};   // 红
        case PL_ACCENT:return {1.00f, 0.78f, 0.35f};   // 琥珀
        case PL_DIM:   return {0.62f, 0.62f, 0.62f};   // 灰
        case PL_FPS:   return {0.95f, 0.95f, 0.95f};   // 白
        case PL_CPU:   return {0.39f, 0.65f, 1.00f};   // 蓝
        case PL_GPU:   return {0.49f, 0.85f, 0.49f};   // 绿
        case PL_MEM:   return {0.95f, 0.95f, 0.95f};   // 白
        default:       return {0.93f, 0.93f, 0.93f};   // 普通行：近白
    }
}

// 该行是否显示方括号区间
static inline bool HasRange(const PanelRow& r) { return !r.vmin.empty() || !r.vmax.empty(); }

// ---------------------------------------------------------------- 三列网格
// 把「各列的最大宽度」算一次，**测量与绘制共用同一套数字**。
// 之前测量和绘制各算各的，结果是括号和数值列对不齐（截图里一眼能看出来）。
struct Grid {
    float labelW = 0, valueW = 0;
    float wMin = 0, wMax = 0;     // 括号里两个数字的字段宽（取全表最大，保证纵向对齐）
    float openW = 0, gapW = 0, closeW = 0;
    float rngW = 0;               // 整个方括号组的总宽
};

static Grid ComputeGrid(IDWriteFactory* f, IDWriteTextFormat* fmtLabel,
                        IDWriteTextFormat* fmtVal, IDWriteTextFormat* fmtSmall,
                        const PanelData& d) {
    Grid g;
    // ⚠ 不要用 MeasureW(L"  ") 去量空格：**DWrite 会把行尾空格裁掉**，量出来接近 0，
    //   结果是括号里的最小值与最大值直接黏在一起（实测显示成 `0.4426.23`）。
    //   等宽字体里每个字符（含空格）宽度相同，用「一个字符宽」推算即可。
    float chW = MeasureW(f, fmtSmall, L"0");
    if (chW <= 0.0f) chW = fmtSmall->GetFontSize() * 0.6f;
    g.openW = chW;
    g.closeW = chW;
    g.gapW = chW * 1.6f;          // 约 1.6 个字符宽的间隙
    for (auto& r : d.rows) {
        if (r.header) continue;
        g.labelW = std::max(g.labelW, MeasureW(f, fmtLabel, r.label));
        g.valueW = std::max(g.valueW, MeasureW(f, fmtVal, r.value));
        if (HasRange(r)) {
            g.wMin = std::max(g.wMin, MeasureW(f, fmtSmall, r.vmin));
            g.wMax = std::max(g.wMax, MeasureW(f, fmtSmall, r.vmax));
        }
    }
    if (g.wMin > 0.0f || g.wMax > 0.0f)
        g.rngW = g.openW + g.gapW + g.wMin + g.gapW + g.wMax + g.gapW + g.closeW;
    return g;
}

void PanelRenderer::Measure(const PanelData& d, const NPConfig& c, float* w, float* h) const {
    Metrics M = Layout(c);
    if (!fmt_ || !fmtR_ || !fmtSmall_) { *w = 240.0f; *h = 110.0f; return; }

    Grid G = ComputeGrid(dwrite_, fmt_, fmtR_, fmtSmall_, d);
    float headW = 0;
    for (auto& r : d.rows)
        if (r.header) headW = std::max(headW, MeasureW(dwrite_, fmtHead_, r.label));

    float contentW = G.labelW + M.colGap + G.valueW + (G.rngW > 0 ? M.colGap + G.rngW : 0.0f);
    contentW = std::max(contentW, headW);
    // 金属 HUD 偏窄：不再额外撑宽
    *w = std::max(170.0f * c.scale, contentW + M.pad * 2.0f);

    float rowsH = 0;
    bool firstRow = true;
    for (auto& r : d.rows) {
        if (r.header) rowsH += (firstRow ? 0.0f : M.rowGap * 3.0f) + M.headH;
        else {
            rowsH += M.lh;
            if (r.chart) rowsH += M.chartGap + M.chartH;
        }
        firstRow = false;
    }
    *h = rowsH + M.pad * 2.0f;
}

void PanelRenderer::Render(ID2D1RenderTarget* rt, float x, float y, const PanelData& d,
                           const NPConfig& c) {
    if (!rt || !d2d_) return;
    EnsureFormats((float)c.fontHeight * c.scale);
    if (!fmt_ || !fmtR_) return;

    Metrics M = Layout(c);
    float w, h;
    Measure(d, c, &w, &h);

    const float alpha = std::clamp(c.textOpacity, 0.0f, 1.0f);

    // 背景：方角、无描边、无阴影 —— 金属 HUD 就是一块纯深色矩形
    D2D1_COLOR_F bg = ToColorF(c.bgColor);
    bg.a = std::clamp(bg.a * c.bgOpacity, 0.0f, 1.0f);
    ID2D1SolidColorBrush* brBg = nullptr;
    if (FAILED(rt->CreateSolidColorBrush(bg, &brBg)) || !brBg) return;
    rt->FillRectangle(D2D1::RectF(x, y, x + w, y + h), brBg);

    // 标签色：白 × 0.70（金属 HUD 的标签比数值暗一档）
    auto mk = [&](RGB col, float a, ID2D1SolidColorBrush** out) {
        D2D1_COLOR_F cf = D2D1::ColorF(col.r, col.g, col.b, std::clamp(a * alpha, 0.0f, 1.0f));
        return SUCCEEDED(rt->CreateSolidColorBrush(cf, out)) && *out;
    };
    ID2D1SolidColorBrush* brLabel = nullptr;
    ID2D1SolidColorBrush* brGrid = nullptr;
    if (!mk({0.88f, 0.88f, 0.88f}, 0.70f, &brLabel) || !mk({1, 1, 1}, 0.10f, &brGrid)) {
        if (brLabel) brLabel->Release();
        if (brGrid) brGrid->Release();
        brBg->Release();
        return;
    }

    // 三列的右边界。**用和 Measure 同一套网格**，否则列会对不齐。
    float labelX = x + M.pad;
    float rightX = x + w - M.pad;

    Grid G = ComputeGrid(dwrite_, fmt_, fmtR_, fmtSmall_, d);
    float rngX = rightX - G.rngW;                       // 方括号组左边界
    float valueRight = (G.rngW > 0.0f) ? rngX - M.colGap : rightX;
    // 括号内部：min / max 各占一个**固定宽度**的格子，保证每行纵向对齐。
    // （之前把两者画在同一条竖线附近 → 直接重叠，截图里有一个数字整个消失。）
    float minRight = rngX + G.openW + G.gapW + G.wMin;
    float maxRight = minRight + G.gapW + G.wMax;
    float closeX = maxRight + G.gapW;

    float cy = y + M.pad;
    bool firstRow = true;

    for (auto& r : d.rows) {
        if (r.header) {
            if (!firstRow) cy += M.rowGap * 3.0f;
            RGB hc{0.80f, 0.80f, 0.80f};
            ID2D1SolidColorBrush* bh = nullptr;
            if (mk(hc, 0.60f, &bh)) {
                rt->DrawText(r.label.c_str(), (UINT32)r.label.size(), fmtHead_,
                             D2D1::RectF(labelX, cy, rightX, cy + M.headH), bh,
                             D2D1_DRAW_TEXT_OPTIONS_CLIP);
                bh->Release();
            }
            cy += M.headH;
            firstRow = false;
            continue;
        }

        RGB vc = LevelRGB(r.level);

        // ---- 标签
        rt->DrawText(r.label.c_str(), (UINT32)r.label.size(), fmt_,
                     D2D1::RectF(labelX, cy, valueRight - M.colGap, cy + M.lh), brLabel,
                     D2D1_DRAW_TEXT_OPTIONS_CLIP);

        // ---- 数值（等宽、右对齐）
        ID2D1SolidColorBrush* bv = nullptr;
        if (mk(vc, 1.0f, &bv)) {
            rt->DrawText(r.value.c_str(), (UINT32)r.value.size(), fmtR_,
                         D2D1::RectF(labelX, cy, valueRight, cy + M.lh), bv,
                         D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        // ---- `[ min  max ]` 方括号区间
        if (HasRange(r) && bv) {
            float wr = cy;
            auto drawAt = [&](const std::wstring& s, IDWriteTextFormat* fmt,
                              ID2D1SolidColorBrush* b, float fromX, float toX) {
                if (s.empty() || !b) return;
                rt->DrawText(s.c_str(), (UINT32)s.size(), fmt,
                             D2D1::RectF(fromX, wr, toX, wr + M.lh), b,
                             D2D1_DRAW_TEXT_OPTIONS_CLIP);
            };
            ID2D1SolidColorBrush* bbrk = nullptr;
            mk(vc, 0.70f, &bbrk);
            ID2D1SolidColorBrush* bmin = nullptr;
            ID2D1SolidColorBrush* bmax = nullptr;
            mk(r.warnMin ? RGB{1.0f, 0.42f, 0.42f} : vc, r.warnMin ? 1.0f : 0.62f, &bmin);
            mk(r.warnMax ? RGB{1.0f, 0.42f, 0.42f} : vc, r.warnMax ? 1.0f : 0.62f, &bmax);

            drawAt(L"[", fmtSmallL_, bbrk, rngX, minRight);
            // 两个数各自在自己的格子里**右对齐**
            drawAt(r.vmin, fmtSmall_, bmin, rngX, minRight);
            drawAt(r.vmax, fmtSmall_, bmax, minRight, maxRight);
            drawAt(L"]", fmtSmallL_, bbrk, closeX, rightX);

            if (bbrk) bbrk->Release();
            if (bmin) bmin->Release();
            if (bmax) bmax->Release();
        }
        if (bv) bv->Release();
        cy += M.lh;

        // ---------------- 折线图（金属 HUD 风格：占满整宽、无刻度、淡竖网格）
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
                float yMin = avg - span, yMax = avg + span;
                if (r.unit == 1) {
                    yMin = std::max(0.0f, yMin);
                    yMax = std::min(100.0f, yMax);
                    if (yMax - yMin < 1.0f) yMax = yMin + 1.0f;
                } else {
                    yMin = std::max(0.0f, yMin);
                    if (yMax - yMin < 0.01f) yMax = yMin + 0.01f;
                }
                float yRange = yMax - yMin;

                // 极淡的竖向网格（金属 HUD 的网格感就来自这几条线）
                for (int g = 1; g < 8; ++g) {
                    float gx = plotX + plotW * (float)g / 8.0f;
                    rt->DrawLine(D2D1::Point2F(gx, plotY), D2D1::Point2F(gx, plotY + plotH),
                                 brGrid, 1.0f * c.scale);
                }
                // 上下边线，把曲线区框出来
                rt->DrawLine(D2D1::Point2F(plotX, plotY), D2D1::Point2F(plotX + plotW, plotY),
                             brGrid, 1.0f * c.scale);
                rt->DrawLine(D2D1::Point2F(plotX, plotY + plotH),
                             D2D1::Point2F(plotX + plotW, plotY + plotH), brGrid,
                             1.0f * c.scale);

                // 折线本体：指标色、1px，新点在右
                ID2D1SolidColorBrush* bl = nullptr;
                if (mk(vc, 1.0f, &bl)) {
                    float prevX = 0, prevY = 0;
                    for (uint32_t i = 0; i < n; ++i) {
                        float v = r.series[(start + i) % r.chartCap];
                        // ★ 把已有样本**铺满整个宽度**（金属 HUD 就是这样）。
                        //   原来按「固定时间轴」定位 —— 数据不足时线只挤在右边缘一小段，
                        //   看起来像坏掉了（截图里 FPS 曲线几乎空白就是这个原因）。
                        float frac = (n > 1) ? (float)i / (float)(n - 1) : 0.0f;
                        float px = plotX + plotW * frac;
                        float py = plotY + plotH *
                                            (1.0f - std::clamp((v - yMin) / yRange, 0.0f, 1.0f));
                        if (i > 0)
                            rt->DrawLine(D2D1::Point2F(prevX, prevY), D2D1::Point2F(px, py), bl,
                                         std::max(1.0f, 1.0f * c.scale));
                        prevX = px;
                        prevY = py;
                    }
                    bl->Release();
                }
            }
            cy += plotH;
        }
        firstRow = false;
    }

    brGrid->Release();
    brLabel->Release();
    brBg->Release();
}

}  // namespace np
