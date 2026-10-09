// 注意：winuser.h 里有 #define DrawText DrawTextW 之类的宏，会把
// ID2D1RenderTarget::DrawText 也「改名」。本项目统一以 UNICODE 编译，
// 因此这里直接写 DrawText（会被展开成 DrawTextW，正是 d2d1.h 里声明的名字）。
// 不要在此处 #undef DrawText，否则会找不到成员。

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
    if (fmtSmall_) { fmtSmall_->Release(); fmtSmall_ = nullptr; }
    if (fmtHead_) { fmtHead_->Release(); fmtHead_ = nullptr; }
}

static IDWriteTextFormat* MakeFmt(IDWriteFactory* f, float size, DWRITE_FONT_WEIGHT w,
                                  DWRITE_TEXT_ALIGNMENT align) {
    const wchar_t* faces[] = {L"Microsoft YaHei UI", L"Microsoft YaHei", L"Segoe UI"};
    for (auto face : faces) {
        IDWriteTextFormat* out = nullptr;
        HRESULT hr = f->CreateTextFormat(face, nullptr, w, DWRITE_FONT_STYLE_NORMAL,
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

void PanelRenderer::EnsureFormats(float fs) {
    if (fmt_ && fabs(fs - fontSize_) < 0.01f) return;
    ReleaseFormats();
    fontSize_ = fs;
    fmt_ = MakeFmt(dwrite_, fs, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    fmtR_ = MakeFmt(dwrite_, fs, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    fmtSmall_ = MakeFmt(dwrite_, fs * 0.72f, DWRITE_FONT_WEIGHT_NORMAL,
                        DWRITE_TEXT_ALIGNMENT_TRAILING);
    fmtHead_ = MakeFmt(dwrite_, fs * 0.80f, DWRITE_FONT_WEIGHT_SEMI_BOLD,
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

void PanelRenderer::Measure(const PanelData& d, const NPConfig& c, float* w, float* h) const {
    if (!fmt_) { *w = 260.0f; *h = 120.0f; return; }
    float pad = 10.0f * c.scale;
    float lh = (float)c.fontHeight * 1.34f * c.scale;
    float hh = lh * 0.82f;
    float chartH = std::max(20.0f, (float)c.graphHeight * 0.45f) * c.scale;
    float labelW = 0, valueW = 0, headW = 0;
    for (auto& r : d.rows) {
        if (r.header) {
            headW = std::max(headW, MeasureW(dwrite_, fmtHead_, r.label));
            continue;
        }
        labelW = std::max(labelW, MeasureW(dwrite_, fmt_, r.label));
        valueW = std::max(valueW, MeasureW(dwrite_, fmt_, r.value));
    }
    float contentW = std::max(headW, labelW + 18.0f * c.scale + valueW);
    *w = std::max(190.0f * c.scale, contentW + pad * 2.0f);
    float rowsH = 0;
    bool firstRow = true;
    for (auto& r : d.rows) {
        if (r.header) rowsH += (firstRow ? 0.0f : 5.0f * c.scale) + hh;
        else {
            rowsH += lh;
            if (r.chart) rowsH += 3.0f * c.scale + chartH;
        }
        firstRow = false;
    }
    *h = rowsH + pad * 2.0f;
}

void PanelRenderer::Render(ID2D1RenderTarget* rt, float x, float y, const PanelData& d,
                           const NPConfig& c) {
    if (!rt || !d2d_) return;
    EnsureFormats((float)c.fontHeight * c.scale);
    if (!fmt_) return;

    float w, h;
    Measure(d, c, &w, &h);
    float pad = 10.0f * c.scale;
    float lh = (float)c.fontHeight * 1.34f * c.scale;
    float hh = lh * 0.82f;
    float chartH = std::max(20.0f, (float)c.graphHeight * 0.45f) * c.scale;
    float gutter = 46.0f * c.scale;     // Y 轴数字占的左列

    // 背景：纯黑 + 独立可调的不透明度；直角，不描边
    D2D1_COLOR_F bg = ToColorF(c.bgColor);
    bg.a = std::clamp(bg.a * c.bgOpacity, 0.0f, 1.0f);
    ID2D1SolidColorBrush* br = nullptr;
    if (FAILED(rt->CreateSolidColorBrush(bg, &br)) || !br) return;

    // 文字：独立的 textOpacity
    D2D1_COLOR_F tx = ToColorF(c.textColor);
    tx.a = std::clamp(tx.a * c.textOpacity, 0.0f, 1.0f);
    ID2D1SolidColorBrush* brText = nullptr;
    rt->CreateSolidColorBrush(tx, &brText);
    ID2D1SolidColorBrush* brDim = nullptr;
    {
        D2D1_COLOR_F dm = tx;
        dm.a *= 0.62f;                  // 标签用同一颜色的低透明度，保持纯黑白
        rt->CreateSolidColorBrush(dm, &brDim);
    }
    ID2D1SolidColorBrush* brAxis = nullptr;
    {
        D2D1_COLOR_F ax = tx;
        ax.a *= 0.28f;                  // 轴线
        rt->CreateSolidColorBrush(ax, &brAxis);
    }
    if (!brText || !brDim || !brAxis) {
        if (brText) brText->Release();
        if (brDim) brDim->Release();
        if (brAxis) brAxis->Release();
        br->Release();
        return;
    }

    rt->FillRectangle(D2D1::RectF(x, y, x + w, y + h), br);

    float cx = x + pad;
    float cy = y + pad;
    float rightX = x + w - pad;

    bool firstRow = true;
    for (auto& r : d.rows) {
        if (r.header) {
            if (!firstRow) cy += 5.0f * c.scale;
            rt->DrawText(r.label.c_str(), (UINT32)r.label.size(), fmtHead_,
                         D2D1::RectF(cx, cy, rightX, cy + hh), brText,
                         D2D1_DRAW_TEXT_OPTIONS_CLIP);
            cy += hh;
            firstRow = false;
            continue;
        }

        // 黑白配色：数值纯白，标签低透明度
        rt->DrawText(r.label.c_str(), (UINT32)r.label.size(), fmt_,
                     D2D1::RectF(cx, cy, rightX, cy + lh), brDim,
                     D2D1_DRAW_TEXT_OPTIONS_CLIP);
        rt->DrawText(r.value.c_str(), (UINT32)r.value.size(), fmtR_,
                     D2D1::RectF(cx, cy, rightX, cy + lh), brText,
                     D2D1_DRAW_TEXT_OPTIONS_CLIP);
        cy += lh;

        // ---------------- 行内小折线图（带 X/Y 轴） ----------------
        if (r.chart && r.series && r.chartCap >= 2) {
            uint32_t n = std::min<uint32_t>(r.chartCount ? r.chartCount : r.chartCap, r.chartCap);
            if (n >= 2) {
                cy += 3.0f * c.scale;
                float plotX = cx + gutter;
                float plotW = rightX - plotX;
                float plotY = cy;
                float plotH = chartH;
                if (plotW > 8.0f) {
                    uint32_t start = (r.chartWrite + r.chartCap - n) % r.chartCap;

                    // 统计可见样本：平均值 + 最大最小
                    double sum = 0.0;
                    float mn = 1e30f, mx = -1e30f;
                    for (uint32_t i = 0; i < n; ++i) {
                        float v = r.series[(start + i) % r.chartCap];
                        sum += v;
                        mn = std::min(mn, v);
                        mx = std::max(mx, v);
                    }
                    float avg = (float)(sum / (double)n);

                    // Y 轴自适应：以平均值为中心，范围取「均值的 35%」与「峰峰值的 60%」的较大者
                    float span = std::max(fabsf(avg) * 0.35f, (mx - mn) * 0.60f);
                    if (span < 0.5f) span = std::max(fabsf(avg) * 0.20f, 0.5f);
                    float yMin = avg - span;
                    float yMax = avg + span;
                    if (r.unit == 1) {                      // 百分比夹到 0..100
                        yMin = std::max(0.0f, yMin);
                        yMax = std::min(100.0f, yMax);
                        if (yMax - yMin < 1.0f) yMax = yMin + 1.0f;
                    } else {
                        yMin = std::max(0.0f, yMin);
                        if (yMax - yMin < 0.01f) yMax = yMin + 0.01f;
                    }
                    float yRange = yMax - yMin;

                    // 轴线：左 + 下
                    rt->DrawLine(D2D1::Point2F(plotX, plotY), D2D1::Point2F(plotX, plotY + plotH),
                                 brAxis, 1.0f * c.scale);
                    rt->DrawLine(D2D1::Point2F(plotX, plotY + plotH),
                                 D2D1::Point2F(plotX + plotW, plotY + plotH), brAxis,
                                 1.0f * c.scale);
                    // 平均线（33% 透明度）：只画在已有数据的区段上
                    {
                        D2D1_COLOR_F mc = tx; mc.a *= 0.33f;
                        ID2D1SolidColorBrush* bm = nullptr;
                        if (SUCCEEDED(rt->CreateSolidColorBrush(mc, &bm)) && bm) {
                            float ay = plotY + plotH * (1.0f - (avg - yMin) / yRange);
                            float x0 = plotX + plotW * (1.0f - (float)(n - 1) / (float)(r.chartCap - 1));
                            rt->DrawLine(D2D1::Point2F(x0, ay),
                                         D2D1::Point2F(plotX + plotW, ay), bm, 1.0f * c.scale);
                            bm->Release();
                        }
                    }

                    // 折线：从右缘向左生长——新点固定在右侧，历史不足时只占右段
                    float prevX = 0, prevY = 0;
                    for (uint32_t i = 0; i < n; ++i) {
                        float v = r.series[(start + i) % r.chartCap];
                        float frac = 1.0f - (float)(n - 1 - i) / (float)(r.chartCap - 1);
                        float px = plotX + plotW * frac;
                        float py = plotY + plotH * (1.0f - std::clamp((v - yMin) / yRange, 0.0f, 1.0f));
                        if (i > 0) rt->DrawLine(D2D1::Point2F(prevX, prevY),
                                                D2D1::Point2F(px, py), brText, 1.2f * c.scale);
                        prevX = px; prevY = py;
                    }

                    // Y 轴数字：只标上下限，平均值只画线不打字
                    wchar_t lb[40];
                    auto yLabel = [&](float v, float yy) {
                        if (r.unit == 0) swprintf(lb, 40, L"%.1f", v);
                        else swprintf(lb, 40, L"%.0f", v);
                        rt->DrawText(lb, (UINT32)wcslen(lb), fmtSmall_,
                                     D2D1::RectF(cx, yy - fontSize_ * 0.45f, plotX - 3.0f * c.scale,
                                                yy + fontSize_ * 0.55f),
                                     brDim, D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    };
                    yLabel(yMax, plotY);
                    yLabel(yMin, plotY + plotH);
                }
                cy += chartH;
            }
        }
        firstRow = false;
    }

    brAxis->Release();
    brDim->Release();
    brText->Release();
    br->Release();
}

}  // namespace np
