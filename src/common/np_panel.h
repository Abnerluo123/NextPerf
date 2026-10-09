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

enum PanelLevel { PL_NORMAL = 0, PL_ACCENT = 1, PL_WARN = 2, PL_DIM = 3 };

struct PanelRow {
    std::wstring label;
    std::wstring value;
    int          level = PL_NORMAL;
    bool         header = false; // 分组标题行（CPU / GPU / …），无数值

    // 行内小折线图：挂在该行下方，独占一行，带 X/Y 轴
    bool         chart = false;
    const float* series = nullptr;
    uint32_t     chartCap = 0;
    uint32_t     chartWrite = 0;
    uint32_t     chartCount = 0;
    uint32_t     sampleMs = 125;   // 采样间隔（X 轴时间换算用）
    int          unit = 0;         // 0=ms 1=% 2=FPS（轴标注格式）
};

struct PanelData {
    std::wstring title;
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

private:
    void EnsureFormats(float fontSize);
    void ReleaseFormats();

    ID2D1Factory*   d2d_ = nullptr;
    IDWriteFactory* dwrite_ = nullptr;
    IDWriteTextFormat* fmt_ = nullptr;       // 左对齐正文
    IDWriteTextFormat* fmtR_ = nullptr;      // 右对齐数值
    IDWriteTextFormat* fmtSmall_ = nullptr;  // 坐标轴小字
    IDWriteTextFormat* fmtHead_ = nullptr;   // 分组标题
    float fontSize_ = 0.0f;
};

inline D2D1_COLOR_F ToColorF(uint32_t aarrggbb) {
    float a = ((aarrggbb >> 24) & 0xFF) / 255.0f;
    float r = ((aarrggbb >> 16) & 0xFF) / 255.0f;
    float g = ((aarrggbb >> 8) & 0xFF) / 255.0f;
    float b = (aarrggbb & 0xFF) / 255.0f;
    return D2D1::ColorF(r, g, b, a);
}

}  // namespace np
