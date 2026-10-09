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

// 中文标签：也要**等宽** —— 否则中英混排时左侧栅格是歪的（用户反馈"左侧字体也没改"）。
// NSimSun（新宋体）是 Windows 自带的**等宽中文**字体，中英文字符宽度一致，
// 能和右侧的等宽数字共用同一套栅格。
static IDWriteTextFormat* MakeFmtUI(IDWriteFactory* f, float size, DWRITE_FONT_WEIGHT w,
                                    DWRITE_TEXT_ALIGNMENT align) {
    static const wchar_t* faces[] = {L"NSimSun", L"SimSun", L"MS Gothic",
                                     L"Microsoft YaHei UI"};
    return MakeFmtFrom(f, faces, 4, size, w, align);
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
    latchedW_ = 0.0f;   // 字号变了，宽度锁重建
    fmt_ = MakeFmtUI(dwrite_, fs, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    fmtR_ = MakeFmtMono(dwrite_, fs, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    fmtSmall_ = MakeFmtMono(dwrite_, fs, DWRITE_FONT_WEIGHT_NORMAL,
                            DWRITE_TEXT_ALIGNMENT_TRAILING);
    fmtSmallL_ = MakeFmtMono(dwrite_, fs, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_TEXT_ALIGNMENT_LEADING);
    // 分组标题（帧率与延迟 / CPU / GPU / 系统）用**与正文同号**的字号。
    // 原来给的是 0.92×，实机看起来明显偏小（用户反馈）。
    // 用一行中英混排的样本文本实测字体行距（GetLineMetrics）。
    // 行高若小于它，D2D1_DRAW_TEXT_OPTIONS_CLIP 就会裁掉字形上下。
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
    if (lineHeight_ <= 0.0f) lineHeight_ = fs * 1.30f;   // 兜底
    // CPU / GPU 标题：与正文同号同间距，但**加粗**（用户要求）
    fmtHead_ = MakeFmtUI(dwrite_, fs, DWRITE_FONT_WEIGHT_BOLD,
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
    // 基础内边距，稍后再加上「四周留黑」（见下方 m.lh 计算之后）
    m.pad = 4.0f * c.scale;
    // 金属 HUD 的行距非常紧：约 1.18 倍字号。普通界面用 1.34 会显得松散。
    // 行高 = max(紧凑值, 字体自身行距)。取字体行距才能保证字形不被 CLIP 裁掉。
    m.lh = std::max((float)c.fontHeight * 1.14f * c.scale, lineH);
    // 四周留一圈黑：固定 3px（用户指定；原为 行高/6 ≈ 2.85）
    m.pad += 3.0f;
    m.headH = m.lh;   // 与正文行同高（字号已改为与正文同号）
    // 金属 HUD 的曲线区比文字区矮得多；原来 0.42 显得又高又空（用户要求缩短）
    m.chartH = std::max(11.0f, (float)c.graphHeight * 0.26f) * c.scale;
    m.rowGap = 1.0f * c.scale;
    m.chartGap = 2.0f * c.scale;
    m.colGap = 2.0f * c.scale;
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
    float charW = 8.0f;           // 一个等宽字符的宽度（栅格单位）
    float labelW = 0, valueW = 0;
    float wMin = 0, wMax = 0;     // 括号里两个数字的固定字段宽（各 6 格）
    float openW = 0, gapW = 0, closeW = 0;
    float rngW = 0;               // 整个方括号组的总宽
};

// ⚠ 量宽度必须用**同一个左对齐格式**，不要用右对齐的 fmtR_ 去量。
//   右对齐（DWRITE_TEXT_ALIGNMENT_TRAILING）的 layout 被给一个很宽的盒子时，
//   GetMetrics().width 可能返回**整个盒子宽度**而不是文本自身宽度，
//   于是 valueW 变成几千像素，面板被撑得极宽、字也像被横向拉开。
//   （重写这一版时我把量宽用的格式从 fmt_ 换成了 fmtR_，属于自己引入的回归。）
static Grid ComputeGrid(IDWriteFactory* f, IDWriteTextFormat* fmtText,
                        IDWriteTextFormat* fmtSmall, const PanelData& d,
                        bool wantRange) {
    Grid g;
    // ⚠ 不要用 MeasureW(L"  ") 去量空格：**DWrite 会把行尾空格裁掉**，量出来接近 0，
    //   结果是括号里的最小值与最大值直接黏在一起（实测显示成 `0.4426.23`）。
    //   等宽字体里每个字符（含空格）宽度相同，用「一个字符宽」推算即可。
    float chW = MeasureW(f, fmtSmall, L"0");
    if (chW <= 0.0f) chW = fmtSmall->GetFontSize() * 0.6f;
    g.charW = chW;
    g.openW = chW;
    g.closeW = chW;
    g.gapW = chW;                 // 一格的间隙

    // ★ 所有列宽都量化到**字符格整数倍**，并且不依赖当前数值的瞬时长度。
    auto cols = [&](float px) { return std::ceil(px / chW - 0.0001f) * chW; };

    float maxLabel = 0, maxValue = 0;
    bool anyRange = false;
    for (auto& r : d.rows) {
        if (r.header) continue;
        maxLabel = std::max(maxLabel, MeasureW(f, fmtText, r.label));
        // ⚠ 必须用**与绘制相同的字体**去量宽度！
        //   数值已改为等宽字体（fmtSmall_ / fmtSmallL_）绘制，若仍用标签字体
        //   （fmt_，NSimSun）去量，量出来会偏小，长值就被裁掉
        //   —— 实测 `1125/9001 MHz` 被裁成 `1125/9001 MH`。
        maxValue = std::max(maxValue, MeasureW(f, fmtSmall, r.value));
        if (HasRange(r)) anyRange = true;
    }
    // 标签集合是固定的，量化后完全稳定
    g.labelW = cols(maxLabel);
    // 数值列：按实际最长值向上取整到 **4 格** 的整数倍。
    //
    // 为什么不硬占 20 格：实测那样面板明显偏宽、数据显得很散（用户反馈）。
    // 为什么量化到 4 格而不是用精确宽度：这样「9% ↔ 100%」这种 1~3 格的
    // 变化不会改变面板宽度，对固定的指标集合完全稳定。
    // 过长的值已在数据层截断（np_build.cpp 的 FitCols），不会撑破列宽。
    {
        // 固定 14 格。数值在数据层被 FitCols 截到 14 格，所以既不会截断显示，
        // 也不会因为某个值变长而撑宽面板 —— 宽度恒定（用户要求 3）。
        (void)maxValue;
        g.valueW = 14.0f * chW;
    }
    // 范围列：min / max 各固定 6 格 —— 数值再长也不会变宽。
    //
    // ★ 必须**无条件预留**，不能写成「有行带范围才预留」。
    //   实测：那样写的话，启动时无数据 → 没有范围 → 面板 266 宽；
    //   一旦拿到数据、范围列出现 → 面板立刻变 421 宽。数据有无之间来回切换，
    //   面板宽度就反复抖 —— 这就是「HUD 宽度随数值一直变化」的真正来源。
    //   代价是：所有范围类指标都关掉时右侧会留一块空位。宁可留白，也不能抖。
    // 范围列：按**实际最长括号宽度**取宽，再向上取整到整字符。
    // 原来固定预留 13 格（min/max 各 5 格），而括号实际只用 7~12 格 ——
    // 多出来的格子全变成了「标签与数值之间那条大空隙」（实机反馈）。
    {
        float natural = 0.0f;
        for (auto& r : d.rows) {
            if (r.header || !HasRange(r)) continue;
            natural = std::max(natural, g.openW + MeasureW(f, fmtSmall, r.vmin) +
                                            g.gapW + MeasureW(f, fmtSmall, r.vmax) +
                                            g.closeW);
        }
        // ★ 宽度必须**纯由配置决定**，绝不能看当前数据：
        //   按「实际最长括号」取宽时，行随数据陆续出现会让它一路长
        //   （诊断日志实测 87.7 -> 96.5 -> 105.3），面板开局几秒持续变宽。
        //   固定 12 格足够容纳最宽的 `[ 0.36 128.68]`。
        g.rngW = wantRange ? 12.0f * chW : 0.0f;
        (void)natural;
    }
    (void)anyRange;
    return g;
}

void PanelRenderer::Measure(const PanelData& d, const NPConfig& c, float* w, float* h) const {
    Metrics M = Layout(c, lineHeight_);
    if (!fmt_ || !fmtR_ || !fmtSmall_) { *w = 240.0f; *h = 110.0f; return; }

    // 用户要求：不再显示 `[min max]` 区间 -> 范围列宽归零，面板整体收窄。
    const bool wantRange = false;
    Grid G = ComputeGrid(dwrite_, fmt_, fmtSmall_, d, wantRange);
    float headW = 0;
    for (auto& r : d.rows)
        if (r.header) headW = std::max(headW, MeasureW(dwrite_, fmtHead_, r.label));

    float contentW = G.labelW + M.colGap + G.valueW + (G.rngW > 0 ? M.colGap + G.rngW : 0.0f);
    contentW = std::max(contentW, headW);
    diag.labelW = G.labelW;
    diag.valueW = G.valueW;
    diag.rngW = G.rngW;
    diag.headW = headW;
    diag.charW = G.charW;
    diag.contentW = contentW;
    diag.rowH = M.lh;
    diag.sepH = 9.0f;   // GPU 组 -> 系统 组的间距（用户指定）
    // 金属 HUD 偏窄：不再额外撑宽。
    // 量化到 4 个字符格（等宽栅格，天然单位）后**只增不减**地锁定 ——
    // 行随数据陆续出现时面板最多长大一次，绝不会来回伸缩。
    {
        // 总宽**不再额外量化**：标签列已量化到整字符、数值列已量化到 4 格、
        // 范围列是固定宽度，各列都已落在字符栅格上；再对总宽做 4 格量化
        // （一档 35px）只会把 290 向上取整成 318，白白宽一圈。
        float want = std::max(170.0f * c.scale, contentW + M.pad * 2.0f);
        if (want > latchedW_) latchedW_ = want;   // 只增不减：行陆续出现时最多长一次
        *w = latchedW_;
    }

    float rowsH = 0;
    bool firstRow = true;
    for (auto& r : d.rows) {
        if (r.header) {
            // 分组高度 = 该组的 gapBefore + 标题高（隐藏标题则为 0）。
            // ⚠ 首行的间距不计入 —— 否则 HUD 顶端会多出一条空行。
            if (!firstRow) rowsH += r.gapBefore;
            if (!r.hideTitle) rowsH += M.headH;
        } else {
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

    Metrics M = Layout(c, lineHeight_);
    float w, h;
    Measure(d, c, &w, &h);

    const float alpha = std::clamp(c.textOpacity, 0.0f, 1.0f);

    // 背景：方角、无描边、无阴影 —— 金属 HUD 就是一块纯深色矩形
    D2D1_COLOR_F bg = ToColorF(c.bgColor);
    bg.a = std::clamp(bg.a * c.bgOpacity, 0.0f, 1.0f);
    ID2D1SolidColorBrush* brBg = nullptr;
    if (FAILED(rt->CreateSolidColorBrush(bg, &brBg)) || !brBg) return;
    // 金属 HUD 的底板是**带轻微圆角**的矩形（不是纯直角）
    {
        float rad = 11.0f * c.scale;
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), rad, rad),
                                 brBg);
    }

    // 标签色：白 × 0.70（金属 HUD 的标签比数值暗一档）
    auto mk = [&](RGB col, float a, ID2D1SolidColorBrush** out) {
        D2D1_COLOR_F cf = D2D1::ColorF(col.r, col.g, col.b, std::clamp(a * alpha, 0.0f, 1.0f));
        return SUCCEEDED(rt->CreateSolidColorBrush(cf, out)) && *out;
    };
    ID2D1SolidColorBrush* brLabel = nullptr;
    ID2D1SolidColorBrush* brGrid = nullptr;
    // 标签亮度与数值一致（1.0），颜色取同一个近白 —— 用户要求视觉统一
    if (!mk({0.95f, 0.95f, 0.95f}, 1.0f, &brLabel) || !mk({1, 1, 1}, 0.10f, &brGrid)) {
        if (brLabel) brLabel->Release();
        if (brGrid) brGrid->Release();
        brBg->Release();
        return;
    }

    // 三列的右边界。**用和 Measure 同一套网格**，否则列会对不齐。
    float labelX = x + M.pad;
    float rightX = x + w - M.pad;

    const bool wantRange = false;   // 同 Measure：区间显示已关闭
    Grid G = ComputeGrid(dwrite_, fmt_, fmtSmall_, d, wantRange);
    // ★ 布局（按用户实机反馈）：标签左对齐，**范围与数值都右对齐**，
    //   而且**范围列右缘紧贴数值列左缘** —— 括号要偏向右侧的数值/单位那边，
    //   而不是夹在标签后面偏左。
    //      [标签] ······ [范围][数值]
    float valueRightEdge = rightX;                            // 数值右缘
    float rngRight = rightX - G.valueW - M.colGap;            // 范围列右缘：贴住数值列
    float rngX = rngRight - G.rngW;

    float cy = y + M.pad;
    bool firstRow = true;

    for (auto& r : d.rows) {
        if (r.header) {
            if (!firstRow) cy += r.gapBefore;   // 分组间距（首行不留）
            if (r.hideTitle) {
                firstRow = false;
                continue;
            }
            // 标题与正文同亮同色，只靠**加粗**区分（用户要求视觉统一）
            RGB hc{0.95f, 0.95f, 0.95f};
            ID2D1SolidColorBrush* bh = nullptr;
            if (mk(hc, 1.0f, &bh)) {
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
                     D2D1::RectF(labelX, cy, labelX + G.labelW, cy + M.lh), brLabel,
                     D2D1_DRAW_TEXT_OPTIONS_CLIP);

        // ---- 数值（等宽、右对齐）
        ID2D1SolidColorBrush* bv = nullptr;
        if (mk(vc, 1.0f, &bv)) {
            // 右对齐贴面板右缘（用户要求 1）。等宽字体保证数字仍纵向成列。
            rt->DrawText(r.value.c_str(), (UINT32)r.value.size(), fmtR_,
                         D2D1::RectF(rngRight + M.colGap, cy, valueRightEdge, cy + M.lh), bv,
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
            mk(vc, 1.0f, &bbrk);
            // 括号里的数字与**数值同色**，不做红色预警（用户要求：数值什么色就是什么色）
            ID2D1SolidColorBrush* bmin = nullptr;
            ID2D1SolidColorBrush* bmax = nullptr;
            mk(vc, 1.0f, &bmin);
            mk(vc, 1.0f, &bmax);

            // 右对齐收在范围列右缘（用户要求）：  [min max]
            // 从右往左定位，`]` 固定贴住列右缘，短括号不会参差。
            // ⚠ `[` 必须**紧贴**最小值 —— 原来在前面多留了一个 gapW，
            //   实机看起来就是 `[ 0.40 22.57]`（`[` 后多一个空格），用户已指出。
            float wmax = MeasureW(dwrite_, fmtSmall_, r.vmax);
            float wmin = MeasureW(dwrite_, fmtSmall_, r.vmin);
            float rEdge = rngX + G.rngW;
            float closeL = rEdge - G.closeW;
            float maxL = closeL - wmax;
            float minR = maxL - G.gapW;
            float minL = minR - wmin;
            float openL = minL - G.openW;
            // ⚠ `[` 用**右对齐**格式绘制，让字形紧贴最小值。
            //   若用左对齐，等宽字体里 `[` 的右侧留白（side bearing）会变成一个
            //   肉眼可见的空格 —— 实机看起来就是 `[ 0.40 22.57]`。
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
                for (int g = 1; g < 6; ++g) {
                    float gx = plotX + plotW * (float)g / 6.0f;
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
