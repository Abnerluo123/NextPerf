#include "np_build.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace np {

static std::wstring WF(float v, int dec) {
    wchar_t b[48];
    if (dec <= 0) swprintf(b, 48, L"%.0f", v);
    else swprintf(b, 48, L"%.*f", dec, v);
    return std::wstring(b);
}

static std::wstring Num(float v, int dec, const wchar_t* unit) {
    if (v < -1000.0f) return L"—";
    return WF(v, dec) + unit;
}
static std::wstring PctV(float v) {
    if (v < 0.0f) return L"—";
    return WF(v, 0) + L"%";
}
static std::wstring Msv(float v) {
    if (v < 0.0f) return L"—";
    return WF(v, 2) + L" ms";
}
static std::wstring Cv(float v) {
    if (v < -200.0f) return L"—";
    return WF(v, 0) + L" \u00b0C";
}
static std::wstring Wv(float v) {
    if (v < 0) return L"—";
    return WF(v, 0) + L" W";
}
static std::wstring MHzv(float v) {
    if (v < 0) return L"—";
    return WF(v, 0) + L" MHz";
}
static std::wstring GBv(float v) {
    if (v < 0) return L"—";
    return WF(v, 1) + L" GB";
}

// 颜色迟滞：避免数值在阈值附近来回跳时红/白疯狂闪烁。
//
// 这是实测反馈的问题（连续截图能看到颜色一帧一变）。根因是判定用了
// **瞬时值**：例如「帧生成时间 > 33.3ms 标红」，而 frameMs 是上一帧的原始
// 间隔，逐帧在 9~24ms 之间抖，一跨过阈值就闪。
//
// dir = +1：「值越大越糟」（帧时间、温度、占用率）
// dir = -1：「值越小越糟」（FPS）
// 触发与恢复用**两个不同阈值**，并记住上一次的状态。
[[maybe_unused]] static bool HystWarn(int slot, float value, float badOn, float badOff,
                                      int dir) {
    static bool state[24]{};
    static bool init[24]{};
    if (slot < 0 || slot >= 24) return false;
    if (!init[slot]) {
        init[slot] = true;
        state[slot] = (dir > 0) ? (value > badOn) : (value < badOn);
        return state[slot];
    }
    if (state[slot]) {
        bool recovered = (dir > 0) ? (value < badOff) : (value > badOff);
        if (recovered) state[slot] = false;      // 明显好转才恢复
    } else {
        bool worse = (dir > 0) ? (value > badOn) : (value < badOn);
        if (worse) state[slot] = true;           // 明显变差才标红
    }
    return state[slot];
}

// 从历史环形缓冲里统计极值。
// 窗口刻意和折线图**显示的范围一致** —— 金属 HUD 的方括号里就是曲线上
// 看得见的那段区间的小/最大值，这样括号和曲线永远自洽。
static bool HistRange(const float* s, const NPHistory* h, float* mn, float* mx) {
    if (!s || !h || h->count < 2) return false;
    uint32_t n = h->count;
    if (n > NP_HIST_CAP) n = NP_HIST_CAP;
    float lo = 1e30f, hi = -1e30f;
    int valid = 0;
    for (uint32_t i = 0; i < n; ++i) {
        float v = s[i];
        if (!(v > 0.0f)) continue;      // 0 / 负数 = 还没填过的槽，跳过
        lo = std::min(lo, v);
        hi = std::max(hi, v);
        ++valid;
    }
    if (valid < 2) return false;
    *mn = lo;
    *mx = hi;
    return true;
}

struct RowAcc {
    PanelData& d;
    bool showRange = false;
    std::vector<PanelRow> pending;
    void row(const wchar_t* label, std::wstring value, int level = PL_NORMAL) {
        PanelRow r;
        r.label = label;
        r.value = std::move(value);
        r.level = level;
        pending.push_back(std::move(r));
    }
    void rowRange(const wchar_t* label, std::wstring value, int level, bool active,
                  const float* series, const NPHistory* h, int dec, float warnMinBelow,
                  float warnMaxAbove) {
        row(label, std::move(value), level);
        if (!showRange || !active) return;
        PanelRow& r = pending.back();
        float mn = 0, mx = 0;
        if (!HistRange(series, h, &mn, &mx)) return;
        r.vmin = WF(mn, dec);
        r.vmax = WF(mx, dec);
        r.warnMin = (mn < warnMinBelow);
        r.warnMax = (mx > warnMaxAbove);
    }
    // 一组收齐：有内容才输出分组标题，避免空标题
    // hideTitle=true：保留分组（行序不变）但不画小标题，只留半行高的分隔
    void flush(const wchar_t* group, bool hideTitle = false, float gapBefore = 0.0f) {
        if (pending.empty()) return;
        PanelRow hd;
        hd.label = group;
        hd.header = true;
        hd.hideTitle = hideTitle;
        hd.gapBefore = gapBefore;
        d.rows.push_back(std::move(hd));
        for (auto& r : pending) d.rows.push_back(std::move(r));
        pending.clear();
    }
};

static bool IsWideChar(wchar_t ch) {
    return (ch >= 0x1100 && (ch <= 0x115F || ch == 0x2329 || ch == 0x232A ||
            (ch >= 0x2E80 && ch <= 0xA4CF) || (ch >= 0xAC00 && ch <= 0xD7A3) ||
            (ch >= 0xF900 && ch <= 0xFAFF) || (ch >= 0xFE30 && ch <= 0xFE6F) ||
            (ch >= 0xFF00 && ch <= 0xFF60) || (ch >= 0xFFE0 && ch <= 0xFFE6)));
}

// 按**显示格数**截断字符串（中文等全角字符算 2 格）。
// 面板数值列固定 14 格，超过会被裁掉看不见 —— 所以在数据层先截断并加省略号。
static std::wstring FitCols(const std::wstring& s, size_t maxCols) {
    size_t cols = 0, i = 0;
    for (; i < s.size(); ++i) {
        size_t w = IsWideChar(s[i]) ? 2 : 1;
        if (cols + w > maxCols) break;
        cols += w;
    }
    if (i >= s.size()) return s;
    std::wstring out = s.substr(0, i);
    // 末尾换成省略号（省略号本身占 1 格）
    while (!out.empty() && cols + 1 > maxCols) {
        size_t w = IsWideChar(out.back()) ? 2 : 1;
        out.pop_back();
        cols -= w;
    }
    return out + L"…";
}

static std::wstring ApiName(uint32_t api, uint32_t presentMode) {
    const wchar_t* a = L"未知";
    switch (api) {
        case NP_API_D3D9: a = L"D3D9"; break;
        case NP_API_D3D11: a = L"D3D11"; break;
        case NP_API_D3D12: a = L"D3D12"; break;
        case NP_API_VULKAN: a = L"Vulkan"; break;
        case NP_API_OPENGL: a = L"OpenGL"; break;
        default: a = L"未接入"; break;
    }
    const wchar_t* m = L"";
    switch (presentMode) {
        case NP_PM_EXCLUSIVE: m = L" 独占全屏"; break;
        case NP_PM_BORDERLESS: m = L" 无边框"; break;
        case NP_PM_WINDOWED: m = L" 窗口"; break;
    }
    return std::wstring(a) + m;
}

static std::wstring AiModuleText(uint32_t m) {
    if (!m) return L"无";
    std::wstring s;
    auto add = [&](const wchar_t* t) {
        if (!s.empty()) s += L",";
        s += t;
    };
    if (m & NP_AI_DLSS_SR) add(L"DLSS-SR");
    if (m & NP_AI_DLSS_RR) add(L"DLSS-RR");
    if (m & NP_AI_DLSS_FG) add(L"DLSS-FG");
    if (m & NP_AI_FSR) add(L"FSR");
    if (m & NP_AI_XESS) add(L"XeSS");
    if (m & NP_AI_DIRECTML) add(L"DirectML");
    if (m & NP_AI_ORT) add(L"ONNX");
    if (m & NP_AI_OTHER) add(L"其他");
    return s;
}

void NPComputeBottleneck(const NPSensors& s, const NPTelemetry& t, std::wstring* label, int* level) {
    std::wstring txt = L"均衡";
    int lv = PL_NORMAL;
    if (t.attached == 0) {
        if (s.gpuUsage >= 90.0f && s.cpuUsage < 80.0f) {
            txt = L"GPU 满载";
            lv = PL_GPU;
        } else if (s.cpuUsage >= 85.0f && s.gpuUsage < 85.0f) {
            txt = L"CPU 高负载";
            lv = PL_CPU;
        } else {
            txt = L"桌面待机";
            lv = PL_DIM;
        }
    } else {
        float gpuMs = (s.gpuBusyMs >= 0.0f) ? s.gpuBusyMs : t.gpuFrameMs;
        float cpuBusy = (t.cpuBusyAvg > 0.0001f) ? t.cpuBusyAvg : t.cpuBusyMs;
        float cpuWait = (t.cpuWaitAvg > 0.0001f) ? t.cpuWaitAvg : t.cpuWaitMs;
        float ft = (t.frameMsAvg > 0.0001f) ? t.frameMsAvg : t.frameMs;
        bool capped = (s.refreshHz > 0 && t.fps > 1.0f &&
                       fabsf(t.fps - (float)s.refreshHz) <= 2.5f &&
                       s.gpuUsage < 85.0f && cpuWait >= 2.0f);
        if (capped) {
            txt = L"帧率/同步受限";
            lv = PL_DIM;
        } else if (s.gpuUsage >= 90.0f && cpuBusy > gpuMs * 1.15f && s.cpuUsage >= 80.0f) {
            txt = L"CPU+GPU 双高";
            lv = PL_WARN;
        } else if (s.gpuUsage >= 90.0f ||
                   (gpuMs > 0.5f && ft > 0.5f && gpuMs >= ft * 0.80f && gpuMs > cpuBusy * 1.15f)) {
            txt = L"GPU 受限";
            lv = PL_GPU;
        } else if ((cpuBusy > 0.5f && gpuMs > 0.5f && cpuBusy > gpuMs * 1.25f && s.gpuUsage < 85.0f) ||
                   (s.gpuUsage >= 0.0f && s.gpuUsage < 75.0f && s.cpuUsage >= 65.0f)) {
            txt = L"CPU 受限";
            lv = PL_CPU;
        } else if (gpuMs > 0.5f && cpuBusy > 0.5f && gpuMs > cpuBusy * 1.25f) {
            txt = L"GPU 受限";
            lv = PL_GPU;
        } else {
            txt = L"负载较均衡";
            lv = PL_NORMAL;
        }
    }
    if (label) *label = std::move(txt);
    if (level) *level = lv;
}

static std::wstring ShortGpuName(const char* raw) {
    if (!raw || !raw[0]) return L"";
    std::string s(raw);
    const char* prefixes[] = {
        "NVIDIA GeForce ", "NVIDIA ", "AMD Radeon(TM) ", "AMD Radeon ",
        "Intel(R) Arc(TM) ", "Intel(R) ", "Intel "
    };
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
        if (s.size() > sl && s.compare(s.size() - sl, sl, suf) == 0)
            s.erase(s.size() - sl);
    }
    return FitCols(Utf8ToWide(s), 20);
}

static const wchar_t* ApiBadge(uint32_t api) {
    switch (api) {
        case NP_API_D3D9:   return L"D3D9";
        case NP_API_D3D11:  return L"D3D11";
        case NP_API_D3D12:  return L"D3D12";
        case NP_API_VULKAN: return L"Vulkan";
        case NP_API_OPENGL: return L"OpenGL";
        default:            return L"Desktop";
    }
}

static std::wstring MemSizeShort(double gb) {
    if (gb < 0.0) return L"—";
    if (gb < 1.0) {
        wchar_t b[32];
        swprintf(b, 32, L"%.1fMB", gb * 1024.0);
        return std::wstring(b);
    }
    wchar_t b[32];
    swprintf(b, 32, L"%.2fGB", gb);
    return std::wstring(b);
}

void BuildPanelData(PanelData& out, const NPConfig& c, const NPSensors& s, const NPTelemetry& t,
                    const NPHistory* h) {
    out.rows.clear();
    out.title.clear();
    out.gpuHeaderLeft.clear();
    out.gpuHeaderRight.clear();
    out.modeHeaderPrefix.clear();
    out.modeHeaderTag.clear();
    out.modeHeaderWarn = false;
    out.modeHeaderRight.clear();

    const bool hooked = t.attached != 0;
    const bool active = hooked || (c.simulate != 0);   // 模拟模式下帧数据视为可用
    const bool showCharts = h != nullptr;
    const bool showRange = NPConfigShowRange(c);

    if (NPConfigShowHeader(c)) {
        if (hooked && t.processName[0])
            out.title = FitCols(Utf8ToWide(t.processName), 16) + L" · " + ApiBadge(t.gfxApi);
        else if (c.simulate)
            out.title = L"Sim · D3D12";
        else if (s.gpuName[0])
            out.title = ShortGpuName(s.gpuName);
        else
            out.title = L"NextPerf";

        if ((c.counters & NP_C_GPU_NAME) && s.gpuName[0])
            out.gpuHeaderLeft = ShortGpuName(s.gpuName);
        else if (hooked && t.processName[0])
            out.gpuHeaderLeft = FitCols(Utf8ToWide(t.processName), 20);
        else
            out.gpuHeaderLeft = L"NextPerf";

        if (c.counters & NP_C_RESOLUTION) {
            if (active && t.renderW && t.renderH) {
                out.gpuHeaderRight =
                    L"[" + std::to_wstring(t.renderW) + L"x" + std::to_wstring(t.renderH) + L"]";
            } else if (s.screenW && s.screenH) {
                out.gpuHeaderRight =
                    L"[" + std::to_wstring(s.screenW) + L"x" + std::to_wstring(s.screenH) + L"]";
            }
        }

        if (active) {
            float sc = (t.windowW > 0 && t.renderW > 0) ? ((float)t.renderW / (float)t.windowW)
                                                        : 1.0f;
            out.modeHeaderPrefix = WF(sc, 1) + L"x";
            if (t.presentMode == NP_PM_WINDOWED) {
                out.modeHeaderTag = L"Composited";
                out.modeHeaderWarn = true;
            } else {
                out.modeHeaderTag = L"Direct";
                out.modeHeaderWarn = false;
            }
        } else {
            out.modeHeaderPrefix = L"Desktop";
            out.modeHeaderTag = L"Idle";
            out.modeHeaderWarn = false;
        }
        if (s.refreshHz > 0) out.modeHeaderRight = std::to_wstring(s.refreshHz) + L"Hz";
    }

    auto attachChart = [&](PanelRow& r, const float* series, int unit) {
        r.chart = true;
        r.series = series;
        r.chartCap = NP_HIST_CAP;
        r.chartWrite = h->write;
        r.chartCount = h->count;
        r.sampleMs = h->sampleMs;
        r.unit = unit;
    };

    auto addCore = [&](const wchar_t* label, std::wstring value, int level, bool canRange,
                       const float* series, int rngDec, float warnMinBelow, float warnMaxAbove) {
        PanelRow r;
        r.label = label;
        r.value = std::move(value);
        r.level = level;
        r.metalCore = true;
        if (showRange && canRange && series && h) {
            float mn = 0, mx = 0;
            if (HistRange(series, h, &mn, &mx)) {
                r.vmin = WF(mn, rngDec);
                r.vmax = WF(mx, rngDec);
                r.warnMin = (mn < warnMinBelow);
                r.warnMax = (mx > warnMaxAbove);
            }
        }
        out.rows.push_back(std::move(r));
    };

    // ---------------- 1. Apple Metal HUD 核心 4 行（紧凑高密度排版） ----------------
    if (c.counters & NP_C_FPS) {
        int fdec = (t.fps >= 1000.0f) ? 0 : 1;
        addCore(L"FPS:", active ? WF(t.fps, fdec) : L"—", PL_FPS, active,
                h ? h->fps : nullptr, 1, 30.0f, 1e30f);
    }
    if (c.counters & NP_C_FRAMETIME) {
        float ft = (t.frameMsAvg > 0.0001f) ? t.frameMsAvg : t.frameMs;
        int pdec = (ft >= 100.0f) ? 1 : 2;
        addCore(L"Pre:", active ? WF(ft, pdec) : L"—", PL_CPU, active,
                h ? h->latFrame : nullptr, 2, -1e30f, 36.0f);
    }
    if (c.counters & NP_C_GPU_FRAME) {
        float gpuShown = -1.0f;
        if (s.gpuBusyMs >= 0.0f && active) gpuShown = s.gpuBusyMs;
        else if (active && t.gpuFrameMs > 0.01f) gpuShown = t.gpuFrameMs;
        int gdec = (gpuShown >= 100.0f) ? 1 : 2;
        addCore(L"GPU:", (gpuShown >= 0.0f) ? WF(gpuShown, gdec) : L"—", PL_GPU, active,
                h ? h->latGpu : nullptr, 2, -1e30f, 33.34f);
    }
    if ((c.counters & NP_C_RAM) && (c.counters & NP_C_VRAM)) {
        PanelRow r;
        r.label = L"Mem:";
        r.value = (s.ramUsedGB >= 0.0f) ? (WF(s.ramUsedGB, 1) + L"GB") : L"—";
        r.level = (s.ramPct > 92.0f) ? PL_WARN : PL_MEM;
        r.metalCore = true;
        if (s.vramUsedGB >= 0.0f) {
            r.rightBracket = L"[" + MemSizeShort(s.vramUsedGB) + L"]";
            r.rightBracketLevel = (s.vramPct > 92.0f) ? PL_WARN : PL_GPU;
        }
        out.rows.push_back(std::move(r));
    } else if (c.counters & NP_C_VRAM) {
        PanelRow r;
        r.label = L"VRAM:";
        r.value = (s.vramTotalGB > 0.0f) ? (WF(s.vramUsedGB, 1) + L"/" + WF(s.vramTotalGB, 1) + L"G")
                                         : GBv(s.vramUsedGB);
        r.level = (s.vramPct > 92.0f) ? PL_WARN : PL_GPU;
        r.metalCore = true;
        out.rows.push_back(std::move(r));
    } else if (c.counters & NP_C_RAM) {
        PanelRow r;
        r.label = L"Mem:";
        r.value = (s.ramTotalGB > 0.0f) ? (WF(s.ramUsedGB, 1) + L"/" + WF(s.ramTotalGB, 1) + L"G")
                                        : GBv(s.ramUsedGB);
        r.level = (s.ramPct > 92.0f) ? PL_WARN : PL_MEM;
        r.metalCore = true;
        out.rows.push_back(std::move(r));
    }

    // ---------------- 2. Metal 双通道实时示波器图表（游戏内：Pre+GPU ms；桌面：% 占用） ----------------
    if (showCharts && (c.counters & NP_C_GRAPH)) {
        PanelRow dc;
        dc.dualChart = true;
        dc.chart = true;
        dc.chartCap = NP_HIST_CAP;
        dc.chartWrite = h->write;
        dc.chartCount = h->count;
        dc.sampleMs = h->sampleMs;
        if (active) {
            dc.series = h->latFrame;
            dc.level = PL_CPU;
            dc.series2 = h->latGpu;
            dc.level2 = PL_GPU;
            dc.unit = 0;
            dc.chartFixedMax = 0.0f;
        } else {
            dc.series = h->usageCpu;
            dc.level = PL_CPU;
            dc.series2 = h->usageGpu;
            dc.level2 = PL_GPU;
            dc.unit = 1;
            dc.chartFixedMax = 0.0f;
        }
        out.rows.push_back(std::move(dc));
    }

    // ---------------- 3. 紧凑扩展遥测区（关联指标自动合行，节省纵高与横向留白） ----------------
    std::vector<PanelRow> ext;
    bool needGap = false;
    auto pushExt = [&](const wchar_t* label, std::wstring val, int level = PL_NORMAL) {
        PanelRow r;
        r.label = label;
        r.value = std::move(val);
        r.level = level;
        if (needGap) {
            r.gapBefore = 3.0f;
            needGap = false;
        }
        ext.push_back(std::move(r));
    };
    auto markGap = [&]() {
        if (!ext.empty()) needGap = true;
    };

    if ((c.counters & NP_C_LOW1) && (c.counters & NP_C_LOW01)) {
        std::wstring v = active ? (L"1% " + WF(floorf(t.fpsLow1), 0) + L" · 0.1% " +
                                   WF(floorf(t.fpsLow01), 0))
                                : L"—";
        pushExt(L"Low", std::move(v));
        if (showCharts && (c.counters & NP_C_CHART_FPS)) attachChart(ext.back(), h->low1, 2);
    } else if (c.counters & NP_C_LOW1) {
        pushExt(L"1% Low", active ? (WF(floorf(t.fpsLow1), 0) + L" FPS") : L"—");
        if (showCharts && (c.counters & NP_C_CHART_FPS)) attachChart(ext.back(), h->low1, 2);
    } else if (c.counters & NP_C_LOW01) {
        pushExt(L"0.1% Low", active ? (WF(floorf(t.fpsLow01), 0) + L" FPS") : L"—");
        if (showCharts && (c.counters & NP_C_CHART_FPS)) attachChart(ext.back(), h->low01, 2);
    }

    // CPU（占用 / 温度 / 功耗 多项开启时合并为一行）
    markGap();
    {
        bool u = (c.counters & NP_C_CPU_USAGE) != 0;
        bool tp = (c.counters & NP_C_CPU_TEMP) != 0;
        bool pw = (c.counters & NP_C_CPU_POWER) != 0;
        int nCpu = (u ? 1 : 0) + (tp ? 1 : 0) + (pw ? 1 : 0);
        int cpuLv = (tp && s.cpuTemp >= 95.0f)
                        ? PL_WARN
                        : ((tp && s.cpuTemp >= 85.0f) ? PL_ACCENT : PL_NORMAL);
        if (nCpu >= 2) {
            std::wstring v;
            auto app = [&](const std::wstring& part) {
                if (!v.empty()) v += L" · ";
                v += part;
            };
            if (u) app(PctV(s.cpuUsage));
            if (tp && s.cpuTemp >= -200.0f) app(WF(s.cpuTemp, 0) + L"\u00b0C");
            else if (tp && !u && !pw) app(L"—");
            if (pw && s.cpuPower >= 0.0f) app(WF(s.cpuPower, 0) + L"W");
            if (v.empty()) v = L"—";
            pushExt(L"CPU", std::move(v), cpuLv);
            if (u && showCharts && (c.counters & NP_C_CHART_USAGE))
                attachChart(ext.back(), h->usageCpu, 1);
        } else if (u) {
            pushExt(L"CPU 占用", PctV(s.cpuUsage));
            if (showCharts && (c.counters & NP_C_CHART_USAGE))
                attachChart(ext.back(), h->usageCpu, 1);
        } else if (tp) {
            pushExt(L"CPU 温度", Cv(s.cpuTemp), cpuLv);
        } else if (pw) {
            pushExt(L"CPU 功耗", s.cpuPower > 0.0f ? Wv(s.cpuPower) : L"—");
        }
    }
    if (c.counters & NP_C_CPU_CLOCK) {
        pushExt(L"CPU 频率", s.cpuClock > 0.0f ? MHzv(s.cpuClock) : L"—");
    }
    if (c.counters & NP_C_CPU_FRAME) {
        float cpuShown = t.cpuBusyAvg;
        const float* cpuSeries = h ? h->latBusy : nullptr;
        if (cpuShown <= 0.0001f) {
            cpuShown = t.cpuBusyMs;
            if (cpuShown <= 0.0001f) {
                cpuShown = t.cpuFrameMs;
                cpuSeries = h ? h->latCpu : nullptr;
            }
        }
        pushExt(L"CPU 帧时间", active ? Msv(cpuShown) : L"—");
        if (showCharts && (c.counters & NP_C_CHART_LATENCY)) attachChart(ext.back(), cpuSeries, 0);
    }
    if (c.counters & NP_C_CPU_BUSY) {
        pushExt(L"CPU Busy", active && t.cpuBusyAvg > 0.0001f ? Msv(t.cpuBusyAvg) : L"—", PL_DIM);
        if (showCharts && (c.counters & NP_C_CHART_LATENCY)) attachChart(ext.back(), h->latBusy, 0);
    }
    if (c.counters & NP_C_CPU_WAIT) {
        pushExt(L"CPU Wait", active && t.cpuWaitAvg > 0.0001f ? Msv(t.cpuWaitAvg) : L"—", PL_DIM);
        if (showCharts && (c.counters & NP_C_CHART_LATENCY)) attachChart(ext.back(), h->latWait, 0);
    }

    // GPU（占用 / 温度 / 功耗 多项开启时合并为一行）
    markGap();
    {
        bool u = (c.counters & NP_C_GPU_USAGE) != 0;
        bool tp = (c.counters & NP_C_GPU_TEMP) != 0;
        bool pw = (c.counters & NP_C_GPU_POWER) != 0;
        int nGpu = (u ? 1 : 0) + (tp ? 1 : 0) + (pw ? 1 : 0);
        int gpuLv = (tp && s.gpuTemp >= 90.0f)
                        ? PL_WARN
                        : ((tp && s.gpuTemp >= 80.0f) ? PL_ACCENT : PL_NORMAL);
        if (nGpu >= 2) {
            std::wstring v;
            auto app = [&](const std::wstring& part) {
                if (!v.empty()) v += L" · ";
                v += part;
            };
            if (u) app(PctV(s.gpuUsage));
            if (tp && s.gpuTemp >= -200.0f) app(WF(s.gpuTemp, 0) + L"\u00b0C");
            if (pw && s.gpuPower >= 0.0f) app(WF(s.gpuPower, 0) + L"W");
            if (v.empty()) v = L"—";
            pushExt(L"GPU", std::move(v), gpuLv);
            if (u && showCharts && (c.counters & NP_C_CHART_USAGE))
                attachChart(ext.back(), h->usageGpu, 1);
        } else if (u) {
            pushExt(L"GPU 占用", PctV(s.gpuUsage));
            if (showCharts && (c.counters & NP_C_CHART_USAGE))
                attachChart(ext.back(), h->usageGpu, 1);
        } else if (tp) {
            pushExt(L"GPU 温度", Cv(s.gpuTemp), gpuLv);
        } else if (pw) {
            pushExt(L"GPU 功耗", Wv(s.gpuPower));
        }
    }
    if (c.counters & NP_C_GPU_HOTSPOT) {
        if (s.gpuHotspot > -200.0f)
            pushExt(L"GPU 热点", Cv(s.gpuHotspot), s.gpuHotspot > 95 ? PL_WARN : PL_NORMAL);
        if (s.gpuMemTemp > -200.0f)
            pushExt(L"显存结温", Cv(s.gpuMemTemp), s.gpuMemTemp > 95 ? PL_WARN : PL_NORMAL);
    }
    if (c.counters & NP_C_GPU_CLOCK) {
        std::wstring v = MHzv(s.gpuClock);
        if (s.memClock > 0) v = WF(s.gpuClock, 0) + L"/" + WF(s.memClock, 0) + L" MHz";
        pushExt(L"核心/显存", v);
    }
    if (c.counters & NP_C_FAN) {
        if (s.gpuFanPct > -1.0f) pushExt(L"风扇转速", PctV(s.gpuFanPct));
        else if (s.gpuFanRpm > -1.0f) pushExt(L"风扇转速", WF(s.gpuFanRpm, 0) + L" RPM");
    }
    if (s.engOfa >= 0.0f || s.engCompute >= 0.0f) {
        std::wstring v;
        if (s.engOfa > 0.0f) v += L"OFA " + PctV(s.engOfa);
        if (s.engCompute >= 0.0f) {
            if (!v.empty()) v += L" · ";
            v += L"Cmp " + PctV(s.engCompute);
        }
        if (!v.empty()) pushExt(L"AI 引擎", v);
    }
    if (c.counters & NP_C_GPU_FB && s.domFb >= 0) pushExt(L"显存带宽", PctV(s.domFb), PL_DIM);
    if (c.counters & NP_C_GPU_VID && s.domVid >= 0) pushExt(L"视频引擎", PctV(s.domVid), PL_DIM);
    if (c.counters & NP_C_GPU_BUS && s.domBus >= 0) pushExt(L"PCIe 总线", PctV(s.domBus), PL_DIM);
    if (c.counters & NP_C_RT) {
        std::wstring v;
        int lv = PL_ACCENT;
        if (s.hwRtPct >= 0) v = PctV(s.hwRtPct) + L" (硬件)";
        else if (hooked && t.rtMeasured) {
            v = PctV(t.rtLoad);
            if (t.rtGpuMs > 0) v += L" " + Msv(t.rtGpuMs);
        } else if (hooked && t.rtDispatches) v = L"启用 " + std::to_wstring(t.rtDispatches) + L" 次";
        else if (hooked) { v = L"无 DXR"; lv = PL_DIM; }
        else { v = L"需注入"; lv = PL_DIM; }
        pushExt(L"RT Core", v, lv);
    }
    if (c.counters & NP_C_TENSOR) {
        std::wstring v;
        int lv = PL_ACCENT;
        if (s.hwTensorPct >= 0) v = PctV(s.hwTensorPct) + L" (硬件)";
        else if (hooked && t.tensorMeasured) {
            v = PctV(t.tensorLoad);
            if (t.aiGpuMs > 0) v += L" " + Msv(t.aiGpuMs);
        } else if (s.engOfa > 0.0f) v = PctV(s.engOfa) + L" (OFA)";
        else if (hooked && t.aiModules && s.engCompute > 0.0f) v = PctV(s.engCompute) + L" (Cmp)";
        else if (hooked && t.aiModules) { v = L"AI 已启用"; lv = PL_DIM; }
        else if (hooked) { v = L"无 AI"; lv = PL_DIM; }
        else { v = L"需注入"; lv = PL_DIM; }
        pushExt(L"Tensor", v, lv);
    }
    if (c.counters & NP_C_AI_MODULES && hooked) {
        pushExt(L"AI 模块", AiModuleText(t.aiModules), t.aiModules ? PL_ACCENT : PL_DIM);
    }

    // 系统与引擎渲染计数
    markGap();
    if (c.counters & NP_C_RESOLUTION && hooked && t.windowW &&
        (t.windowW != t.renderW || t.windowH != t.renderH)) {
        std::wstring sv = std::to_wstring(t.renderW) + L"x" + std::to_wstring(t.renderH) + L"→" +
                          std::to_wstring(t.windowW) + L"x" + std::to_wstring(t.windowH);
        pushExt(L"缩放输出", sv, PL_DIM);
    }
    if (c.counters & NP_C_CPU_FRAME) {
        float waitRef = t.cpuWaitAvg > 0.0001f ? t.cpuWaitAvg : t.cpuWaitMs;
        bool known = (t.hookFlags & NP_HOOK_REFLEX_KNOWN) != 0;
        bool lowLatency = active && (known ? ((t.hookFlags & NP_HOOK_REFLEX) != 0)
                                          : (waitRef < 1.6f));
        pushExt(L"低延迟", lowLatency ? L"On" : L"Off", lowLatency ? PL_NORMAL : PL_DIM);
    }
    if (c.counters & NP_C_DRAWS && hooked) {
        pushExt(L"Draw/Disp",
                std::to_wstring(t.drawCalls) + L" / " + std::to_wstring(t.dispatches));
    }
    if (c.counters & NP_C_BOTTLENECK) {
        std::wstring bnLabel;
        int bnLevel = PL_NORMAL;
        NPComputeBottleneck(s, t, &bnLabel, &bnLevel);
        pushExt(L"瓶颈", bnLabel, bnLevel);
    }
    if (c.counters & NP_C_CLOCK_TIME) {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t tb[16];
        swprintf(tb, 16, L"%02d:%02d:%02d", (int)st.wHour, (int)st.wMinute, (int)st.wSecond);
        pushExt(L"时间", tb, PL_DIM);
    }
    if (c.counters & NP_C_SENSOR_SRC && s.sourceText[0]) {
        pushExt(L"数据源", FitCols(Utf8ToWide(s.sourceText), 14), PL_DIM);
    }

    if (!ext.empty() || (c.counters & NP_C_API)) {
        PanelRow sub;
        sub.splitSubHeader = true;
        sub.gapBefore = out.rows.empty() ? 0.0f : 3.0f;
        if (hooked && t.processName[0]) sub.label = FitCols(Utf8ToWide(t.processName), 18);
        else if (c.simulate) sub.label = L"CyberDemo.exe";
        else sub.label = L"系统概览";
        if (c.counters & NP_C_API) sub.value = ApiBadge(t.gfxApi);
        out.rows.push_back(std::move(sub));
        for (auto& er : ext) out.rows.push_back(std::move(er));
    }
}

}  // namespace np
