// 面板渲染器：用 Direct2D + DirectWrite 绘制半透明参数面板与实时曲线。
// 主程序（桌面分层窗口）与注入钩子（游戏内 D3D11/D3D12 纹理）共用这一份代码，
// 保证两种叠加模式长得一模一样。
#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include <vector>
#include <cstdint>

#include "np_common.h"

namespace np {

inline std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
inline std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// 行配色。PL_* 是原有的语义级别；PC_* 是 Metal HUD 那种「按指标分色」。
// 两者共用一个字段（level），渲染器按值域区分即可，避免改动所有调用点。
enum PanelLevel {
    PL_NORMAL = 0, PL_ACCENT = 1, PL_WARN = 2, PL_DIM = 3,
    // ↓ Metal HUD 风格：FPS 白 / 帧时间(CPU) 蓝 / GPU 绿 / 内存 白
    PL_FPS = 4, PL_CPU = 5, PL_GPU = 6, PL_MEM = 7,
};

struct PanelRow {
    std::wstring label;
    std::wstring value;
    int          level = PL_NORMAL;
    bool         header = false; // 分组标题行（CPU / GPU / …），无数值
    // 是分组但**不显示标题**：只留半行高的空白当分隔
    // （用户要求：`帧率与延迟` / `系统` 这两个小标题不要出现）
    bool         hideTitle = false;
    // 本行之前的额外间距（逻辑像素，与行高同一单位）。用于分组之间的留白。
    float        gapBefore = 0.0f;
    // 标签后面的**异色提示**（如「低延迟」），用亮黄色绘制。空 = 无提示。
    // 会参与标签列宽度计算，所以出现/消失时面板最多变宽一次（有只增不减的锁定）。
    std::wstring hint;

    // ---- Metal HUD 风格的右侧方括号区间：`[ min  max ]`
    // 空字符串 = 本行不显示方括号。数值由 np_build 从**图表窗口**里统计出来，
    // 和 Apple 的 HUD 一样：括号里的就是曲线可见范围内的小/最大值。
    std::wstring vmin, vmax;
    // 括号里哪一侧该标红（由 np_build 按指标语义决定：
    //   FPS 过低、帧时间/GPU 时间过高 → 红）。
    bool         warnMin = false;
    bool         warnMax = false;

    // 行内 / 独立折线图（含 Metal HUD 双通道示波器支持）
    bool         chart = false;
    bool         dualChart = false;        // true = Metal 独立全宽双波形图（无阴影框、10 刻度竖线）
    const float* series = nullptr;
    const float* series2 = nullptr;        // 第二条叠加波形（如 GPU ms 叠加在 Pre ms 上）
    int          level2 = PL_GPU;
    float        chartFixedMax = 0.0f;     // >0 时 Y 轴固定 [0, chartFixedMax]（桌面模式 %）
    uint32_t     chartCap = 0;
    uint32_t     chartWrite = 0;
    uint32_t     chartCount = 0;
    uint32_t     sampleMs = 125;   // 采样间隔（X 轴时间换算用）
    int          unit = 0;         // 0=ms 1=% 2=FPS（轴标注格式）

    // ---- Apple Metal Performance HUD 行增强属性
    bool         metalCore = false;        // true = 标签本身随指标着色并使用等宽字体（FPS:/Pre:/GPU:/Mem:）
    std::wstring rightBracket;             // 右侧单框值（如 Mem 行右对齐的 [3.14GB]）
    int          rightBracketLevel = PL_GPU;
    bool         splitSubHeader = false;   // 图表下方的左右对齐子标题行（如 CyberDemo.exe / D3D12）
};

struct PanelData {
    std::wstring title;                    // 横向精简条使用的单行标题
    // Metal HUD 两行设备顶栏
    std::wstring gpuHeaderLeft;            // 第 1 行左：短显卡名（如 RTX 5080 Laptop）
    std::wstring gpuHeaderRight;           // 第 1 行右：分辨率 [2560x1440]
    std::wstring modeHeaderPrefix;         // 第 2 行左前缀：1.0x / Desktop
    std::wstring modeHeaderTag;            // 第 2 行左标签：Direct / Composited / Idle
    bool         modeHeaderWarn = false;   // Composited 为窗口合成路径，标 Apple 红提示
    std::wstring modeHeaderRight;          // 第 2 行右：刷新率 160Hz
    std::vector<PanelRow> rows;
};

class PanelRenderer {
public:
    bool  Init();
    void  Shutdown();
    bool  ok() const { return d2d_ != nullptr && dwrite_ != nullptr; }

    // 计算面板尺寸（逻辑像素）
    void  Measure(const PanelData& d, const NPConfig& c, float* w, float* h) const;

    // 在指定 D2D 渲染目标上绘制；(x,y) 为左上角，通常放在右上角
    void  Render(ID2D1RenderTarget* rt, float x, float y, const PanelData& d, const NPConfig& c);

    ID2D1Factory*   factory() const { return d2d_; }
    IDWriteFactory* dwrite() const { return dwrite_; }

    // 诊断用：Measure() 算出的各列宽度（定位「面板宽度随数值变化」是谁在变）
    struct Diag {
        float labelW = 0, valueW = 0, rngW = 0, headW = 0, charW = 0, contentW = 0;
        float rowH = 0;    // 正文行高
        float sepH = 0;    // 隐藏分组留下的半行高分隔
    };
    mutable Diag diag;

    // 面板宽度锁定值：只增不减。行是随数据陆续出现的，
    // 允许回落的话面板会随行的有无反复伸缩。字号或监控项变化时重置。
    mutable float latchedW_ = 0.0f;
    mutable uint64_t lastCounters_ = 0;
    mutable uint32_t lastModeBits_ = 0;

private:
    struct Grid {
        float charW = 8.0f;
        float labelW = 0, valueW = 0;
        float coreLabelW = 0, coreValW = 0, coreRngW = 0, extW = 0;
        float wMin = 0, wMax = 0;
        float openW = 0, gapW = 0, closeW = 0;
        float rngW = 0;
    };
    void MeasureInternal(const PanelData& d, const NPConfig& c, float* w, float* h,
                         Grid* gridOut) const;
    void EnsureFormats(float fontSize);
    void ReleaseFormats();

    ID2D1Factory*   d2d_ = nullptr;
    IDWriteFactory* dwrite_ = nullptr;
    IDWriteTextFormat* fmt_ = nullptr;       // 左对齐正文（中文标签，UI 字体）
    IDWriteTextFormat* fmtL_ = nullptr;      // 同 fmtHead_ 左对齐（保留）
    IDWriteTextFormat* fmtMonoL_ = nullptr;  // 左对齐等宽（Metal 顶栏 / FPS: Pre: GPU: Mem:）
    IDWriteTextFormat* fmtR_ = nullptr;      // 右对齐数值（等宽 —— 金属 HUD 的数字列）
    IDWriteTextFormat* fmtSmall_ = nullptr;  // 右对齐小字（括号里的数字，等宽）
    IDWriteTextFormat* fmtSmallL_ = nullptr; // 左对齐小字（左括号，等宽）
    IDWriteTextFormat* fmtHead_ = nullptr;   // 分组标题
    float fontSize_ = 0.0f;
    // 字体自身的行距（由 DWrite 实测）。行高不能小于它，否则 DWRITE 的 CLIP
    // 会把字形上下裁掉，各行的视觉高低也会不齐。
    float lineHeight_ = 0.0f;
};

inline D2D1_COLOR_F ToColorF(uint32_t aarrggbb) {
    float a = ((aarrggbb >> 24) & 0xFF) / 255.0f;
    float r = ((aarrggbb >> 16) & 0xFF) / 255.0f;
    float g = ((aarrggbb >> 8) & 0xFF) / 255.0f;
    float b = (aarrggbb & 0xFF) / 255.0f;
    return D2D1::ColorF(r, g, b, a);
}

}  // namespace np
