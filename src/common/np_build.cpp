#include "np_build.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

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
static std::wstring PctV(float v) { return Num(v, 0, L"%"); }
static std::wstring Msv(float v) { return Num(v, 2, L" ms"); }
static std::wstring Cv(float v) {
    if (v < -200.0f) return L"—";
    return WF(v, 0) + L" ℃";
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
    std::vector<PanelRow> pending;
    void row(const wchar_t* label, std::wstring value, int level = PL_NORMAL) {
        PanelRow r;
        r.label = label;
        r.value = std::move(value);
        r.level = level;
        pending.push_back(std::move(r));
    }
    // 金属 HUD 风格：数值 + 右侧 `[ min  max ]` 方括号区间 + 指标配色。
    //   warnMinBelow / warnMaxAbove 决定括号里哪一侧标红；
    //   不想标红就传 -1e30f / 1e30f。
    void rowRange(const wchar_t* label, std::wstring value, int level, bool active,
                  const float* series, const NPHistory* h, int dec, float warnMinBelow,
                  float warnMaxAbove) {
        // ★ 用户要求：**不再显示 `[min max]` 区间**，只显示数值本身。
        //   保留函数签名（调用点不用改），想恢复只要把下面这段注释放开、
        //   并把 np_panel.cpp 里的 wantRange 置回 true 即可。
        (void)active; (void)series; (void)h; (void)dec;
        (void)warnMinBelow; (void)warnMaxAbove;
        row(label, std::move(value), level);
        // PanelRow& r = pending.back();
        // if (!active) return;
        // float mn = 0, mx = 0;
        // if (!HistRange(series, h, &mn, &mx)) return;
        // r.vmin = WF(mn, dec);
        // r.vmax = WF(mx, dec);
        // r.warnMin = (mn < warnMinBelow);
        // r.warnMax = (mx > warnMaxAbove);
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

// 按**显示格数**截断字符串（中文等全角字符算 2 格）。
// 面板数值列固定 20 格，超过会被裁掉看不见 —— 所以在数据层先截断并加省略号。
static std::wstring FitCols(const std::wstring& s, size_t maxCols) {
    size_t cols = 0, i = 0;
    for (; i < s.size(); ++i) {
        size_t w = (s[i] >= 0x1100 && (s[i] <= 0x115F || s[i] == 0x2329 || s[i] == 0x232A ||
                    (s[i] >= 0x2E80 && s[i] <= 0xA4CF) || (s[i] >= 0xAC00 && s[i] <= 0xD7A3) ||
                    (s[i] >= 0xF900 && s[i] <= 0xFAFF) || (s[i] >= 0xFE30 && s[i] <= 0xFE6F) ||
                    (s[i] >= 0xFF00 && s[i] <= 0xFF60) || (s[i] >= 0xFFE0 && s[i] <= 0xFFE6)))
                       ? 2 : 1;
        if (cols + w > maxCols) break;
        cols += w;
    }
    if (i >= s.size()) return s;
    std::wstring out = s.substr(0, i);
    // 末尾换成省略号（省略号本身占 1 格）
    while (!out.empty() && cols + 1 > maxCols) {
        size_t w = (out.back() > 0x1100) ? 2 : 1;
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

void BuildPanelData(PanelData& out, const NPConfig& c, const NPSensors& s, const NPTelemetry& t,
                    const NPHistory* h) {
    out.rows.clear();
    out.title.clear();   // 不显示任何 Logo / 标题，保持极简

    const bool hooked = t.attached != 0;
    const bool active = hooked || (c.simulate != 0);   // 模拟模式下帧数据视为可用
    const bool showCharts = h != nullptr;
    // ⚠ 不能在这里 return。
    //   原来写的是「if (h == nullptr) return;  无历史数据时帧率行也没有图可画」——
    //   但那样 CPU / GPU / 内存这些**根本不需要历史数据**的行也会一起消失，
    //   把「画不了曲线」错误地当成了「什么都别显示」。
    //   实际调用点目前都传了非空指针，所以它是一颗埋着的雷，拆掉。
    //   没有历史数据时下面每处 attach() 都已经被 showCharts 挡住了。
    RowAcc acc{out};

    // ---------------- 帧率与延迟 ----------------
    // 小图开关：NP_C_GRAPH=FPS/帧时间，NP_C_CHART_FPS=Low 帧，NP_C_CHART_LATENCY=帧延迟
    auto attach = [&](PanelRow& r, const float* series, int unit) {
        r.chart = true;
        r.series = series;
        r.chartCap = NP_HIST_CAP;
        r.chartWrite = h->write;
        r.chartCount = h->count;
        r.sampleMs = h->sampleMs;
        r.unit = unit;
    };
    if (c.counters & NP_C_FPS) {
        // 金属 HUD：FPS 用白色；明显偏低时数值转红（带迟滞，避免临界值闪红）
        acc.rowRange(L"FPS", active ? WF(t.fps, 0) : L"—",
                     PL_FPS, active,
                     h ? h->fps : nullptr, h, 0, 28.0f, 1e30f);
        if (showCharts && (c.counters & NP_C_GRAPH)) attach(acc.pending.back(), h->fps, 2);
    }
    if (c.counters & NP_C_FRAMETIME) {
        // ⚠ 这一行的**数值与曲线颜色固定**，不随帧时间跳红。
        //   原来判据用瞬时 t.frameMs（逐帧在 9~24ms 之间抖），跨过阈值就闪红；
        //   即便改成平滑值 + 迟滞，在 30fps 这类场景里也会「进去就出不来」
        //   （触发 33.3ms、恢复要 22ms，于是整行长期红着）—— 两种表现都不对。
        //   金属 HUD 的做法是：**数值保持指标色，只有括号里的极值才标红**，照此办理。
        acc.rowRange(L"帧时间",
                     active ? Msv(t.frameMsAvg > 0.0001f ? t.frameMsAvg : t.frameMs) : L"—",
                     PL_MEM, active, h ? h->latFrame : nullptr, h, 1, -1e30f, 36.0f);
        if (showCharts && (c.counters & NP_C_GRAPH)) attach(acc.pending.back(), h->latFrame, 0);
    }
    if (c.counters & NP_C_LOW1) {
        acc.row(L"1% Low", active ? WF(t.fpsLow1, 0) + L" FPS" : L"—");
        // 不画曲线：用户明确要求「low 帧不需要画曲线，显示出数值就好」
    }
    if (c.counters & NP_C_LOW01) {
        acc.row(L"0.1% Low", active ? WF(t.fpsLow01, 0) + L" FPS" : L"—");
    }
    acc.flush(L"帧率与延迟", true, 0.0f);    // 首组：顶端不留

    // ---------------- CPU ----------------
    if (c.counters & NP_C_CPU_USAGE) {
        // 金属 HUD 的配色语言：CPU 侧的指标一律蓝
        int lv = PL_CPU;
        acc.row(L"占用", PctV(s.cpuUsage), lv);
    }
    if (c.counters & NP_C_CPU_TEMP) {
        int lv = PL_CPU;
        acc.row(L"温度", Cv(s.cpuTemp), lv);
    }
    if (c.counters & NP_C_CPU_CLOCK) {
        // CPU 当前频率：优先 CallNtPowerInformation（每核实测 MHz，取最高核心）。
        // 回退路径是 PDH 的「标称 × 性能百分比」，会系统性偏低。
        acc.row(L"CPU 频率", s.cpuClock > 0.0f ? MHzv(s.cpuClock) : L"—", PL_CPU);
    }
    if (c.counters & NP_C_CPU_POWER) {
        acc.row(L"CPU 功耗", s.cpuPower > 0.0f ? Wv(s.cpuPower) : L"—", PL_CPU);
    }
    if (c.counters & NP_C_CPU_FRAME) {
        // 金属 HUD 的 "Pre" 是蓝色 —— CPU 侧的时间类指标统一用蓝
        // ★ 显示值 = Busy + Wait 的**平滑值之和**（与下面两行显示的正是同一对量）。
        //   hook 里的瞬时值也是 busy+wait，但面板上 Busy/Wait 显示的是 EMA；
        //   若这里显示瞬时值，三行就永远加不上 —— 用户实测抓到过这个不一致。
        //   （用户澄清：他之前说「改回老口径」是误以为我拿 NVIDIA API 的数值替代了
        //    他自己的量，实际期望就是平滑值之和。）
        float cpuShown = t.cpuBusyAvg + t.cpuWaitAvg;
        if (cpuShown <= 0.0001f) cpuShown = t.cpuFrameMs;   // 平均值还没建立时的兜底
        acc.rowRange(L"CPU 帧时间", active ? Msv(cpuShown) : L"—", PL_CPU, active,
                     h ? h->latCpu : nullptr, h, 1, -1e30f, 33.34f);
        if (showCharts && (c.counters & NP_C_CHART_LATENCY)) attach(acc.pending.back(), h->latCpu, 0);

        // 低延迟状态不再作为本行的黄色后缀显示 —— 已改为系统分组里的独立一行
        // 「低延迟 On/Off」，判据也换成了 CPU Wait（见系统分组处）。
    }

    // ---- CPU Busy / CPU Wait 两个半边（PresentMon 口径，二者之和 = 帧周期）
    // **默认不显示**（用户要求：保留但默认关），而且它们之和已经作为「CPU 帧时间」
    // 显示出来了，所以只在用户主动勾选时才单独列出。颜色与 CPU 其他参数一致（PL_CPU）。
    if (c.counters & NP_C_CPU_BUSY) {
        acc.row(L"CPU Busy", active && t.cpuBusyAvg > 0.0001f ? Msv(t.cpuBusyAvg) : L"—",
                PL_CPU);
        if (showCharts && (c.counters & NP_C_CHART_LATENCY)) attach(acc.pending.back(), h->latBusy, 0);
    }
    if (c.counters & NP_C_CPU_WAIT) {
        acc.row(L"CPU Wait", active && t.cpuWaitAvg > 0.0001f ? Msv(t.cpuWaitAvg) : L"—",
                PL_CPU);
        if (showCharts && (c.counters & NP_C_CHART_LATENCY)) attach(acc.pending.back(), h->latWait, 0);
    }
    acc.flush(L"CPU", false, 9.0f);      // FPS 组 -> CPU 组：9px

    // ---------------- GPU ----------------
    // 分辨率的值先算好，等进入「系统」分组时再入行（用户要求归到系统组）
    std::wstring resText;
    if (c.counters & NP_C_GPU_NAME && s.gpuName[0]) {
        std::wstring n = Utf8ToWide(s.gpuName);
        n = FitCols(n, 14);
        acc.row(L"显卡", n, PL_DIM);
    }
    if (c.counters & NP_C_GPU_USAGE) {
        // GPU 侧的指标一律绿
        int lv = PL_GPU;
        acc.row(L"占用", PctV(s.gpuUsage), lv);
    }
    if (c.counters & NP_C_GPU_TEMP) {
        int lv = PL_GPU;
        acc.row(L"温度", Cv(s.gpuTemp), lv);
    }
    if (c.counters & NP_C_GPU_HOTSPOT) {
        if (s.gpuHotspot > -200.0f) acc.row(L"热点", Cv(s.gpuHotspot),
                                            s.gpuHotspot > 95 ? PL_WARN : PL_NORMAL);
        if (s.gpuMemTemp > -200.0f) acc.row(L"显存结温", Cv(s.gpuMemTemp),
                                            s.gpuMemTemp > 95 ? PL_WARN : PL_NORMAL);
    }
    if (c.counters & NP_C_GPU_POWER) {
        // 只显示当前功耗。用户明确要求：不要带最大功耗（"不需要读取最大功耗"）
        std::wstring v = Wv(s.gpuPower);
        acc.row(L"功耗", v);
    }
    if (c.counters & NP_C_GPU_CLOCK) {
        std::wstring v = MHzv(s.gpuClock);
        // 写成 `2002/14001 MHz` 而非 `2002 MHz / 14001 MHz` —— 省 4 个字符格。
        // 面板数值列按最长值定宽，把它写短才能让整块面板真正收窄。
        if (s.memClock > 0) v = WF(s.gpuClock, 0) + L"/" + WF(s.memClock, 0) + L" MHz";
        acc.row(L"核心/显存", v);
    }
    if (c.counters & NP_C_FAN) {
        if (s.gpuFanPct > -1.0f) acc.row(L"风扇转速", PctV(s.gpuFanPct));
        else if (s.gpuFanRpm > -1.0f) acc.row(L"风扇转速", WF(s.gpuFanRpm, 0) + L" RPM");
    }
    if (c.counters & NP_C_GPU_FRAME) {
        // 优先用系统 PDH 按游戏进程算出来的「每帧 GPU 执行时间」——驱动报的数，
        // 与锁帧 / Reflex / 多线程提交无关。钩子推算的值只作为兜底。
        std::wstring v;
        if (s.gpuBusyMs >= 0.0f && active) v = Msv(s.gpuBusyMs);
        // 钩子的兜底值必须是**有效正数**才显示。原来是 0 也照显示，
        // 于是「注入没成功」时面板上会出现一个假的 0.00 ms。
        else if (active && t.gpuFrameMs > 0.01f) v = Msv(t.gpuFrameMs);
        else v = L"—";
        // 金属 HUD 的 "GPU" 是绿色 —— GPU 侧的时间类指标统一用绿
        acc.rowRange(L"GPU 帧时间", v, PL_GPU, active, h ? h->latGpu : nullptr, h, 2, -1e30f,
                     33.34f);
        if (showCharts && (c.counters & NP_C_CHART_LATENCY)) attach(acc.pending.back(), h->latGpu, 0);
    }
    // 光流加速器（OFA）：N 卡上 DLSS **帧生成**专用的硬件单元。
    // 系统计数器直接给，不用估算 —— 只要它在动，帧生成就在工作。
    if (s.engOfa >= 0.0f || s.engCompute >= 0.0f) {
        std::wstring v;
        if (s.engOfa > 0.0f) v += L"光流 " + PctV(s.engOfa);
        if (s.engCompute >= 0.0f) {
            if (!v.empty()) v += L" · ";
            v += L"Compute " + PctV(s.engCompute);
        }
        if (!v.empty()) acc.row(L"AI 引擎", v, PL_NORMAL);
    }
    if (c.counters & NP_C_VRAM) {
        std::wstring v = GBv(s.vramUsedGB);
        if (s.vramTotalGB > 0) v = WF(s.vramUsedGB, 1) + L"/" + WF(s.vramTotalGB, 1) + L" GB";
        acc.row(L"显存", v, s.vramPct > 92 ? PL_WARN : PL_NORMAL);
    }
    if (c.counters & NP_C_GPU_FB && s.domFb >= 0)
        acc.row(L"显存带宽", PctV(s.domFb), PL_DIM);
    if (c.counters & NP_C_GPU_VID && s.domVid >= 0)
        acc.row(L"视频引擎", PctV(s.domVid), PL_DIM);
    if (c.counters & NP_C_GPU_BUS && s.domBus >= 0)
        acc.row(L"PCIe 总线", PctV(s.domBus), PL_DIM);
    if (c.counters & NP_C_RT) {
        std::wstring v;
        int lv = PL_ACCENT;
        if (s.hwRtPct >= 0) {
            v = PctV(s.hwRtPct) + L" (硬件)";
        } else if (hooked && t.rtMeasured) {
            v = PctV(t.rtLoad) + L" (实测)";
            if (t.rtGpuMs > 0) v += L" " + Msv(t.rtGpuMs);
        } else if (hooked && t.rtDispatches) {
            v = L"启用 " + std::to_wstring(t.rtDispatches) + L" 次";
        } else if (hooked) {
            v = L"无 DXR";
            lv = PL_DIM;
        } else {
            v = L"需注入";
            lv = PL_DIM;
        }
        acc.row(L"RT Core", v, lv);
    }
    if (c.counters & NP_C_TENSOR) {
        std::wstring v;
        int lv = PL_ACCENT;
        if (s.hwTensorPct >= 0) {
            v = PctV(s.hwTensorPct) + L" (硬件)";
        } else if (hooked && t.tensorMeasured) {
            v = PctV(t.tensorLoad) + L" (实测)";
            if (t.aiGpuMs > 0) v += L" " + Msv(t.aiGpuMs);
        } else if (hooked && t.aiModules) {
            v = L"AI 已启用 · 估算中";
            lv = PL_DIM;
        } else if (hooked) {
            v = L"无 AI";
            lv = PL_DIM;
        } else {
            v = L"需注入";
            lv = PL_DIM;
        }
        acc.row(L"Tensor", v, lv);
    }
    if (c.counters & NP_C_RESOLUTION && hooked && t.renderW) {
        std::wstring v = std::to_wstring(t.renderW) + L"×" + std::to_wstring(t.renderH);
        if (t.windowW && (t.windowW != t.renderW || t.windowH != t.renderH)) {
            float sc = (float)t.renderW / (float)t.windowW;
            v += L" → " + std::to_wstring(t.windowW) + L"×" + std::to_wstring(t.windowH);
            v += L" (" + WF(sc * 100.0f, 0) + L"%)";
        }
        // 分辨率移到「系统」分组（用户要求）—— 这里只把值准备好，稍后再入行
        resText = v;
    }
    if (c.counters & NP_C_AI_MODULES && hooked) {
        acc.row(L"AI 模块", AiModuleText(t.aiModules), t.aiModules ? PL_ACCENT : PL_DIM);
    }
    acc.flush(L"GPU", false, 3.0f);      // CPU 组 -> GPU 组：3px

    // ---------------- 系统 ----------------
    //
    // 低延迟状态（用户要求放这里，显示成 On/Off，不用黄色）
    //
    // 判据：**CPU Wait < 1.6ms**。
    //   CPU Wait = 本帧卡在 Present 内部等垂直同步的时长。
    //   开了 Reflex 这类低延迟技术后，等待被**移出** Present（挪到 Present 之前），
    //   Wait 随之变小 —— 所以「Wait 小」是低延迟生效的**直证**，
    //   比原来的旁证（CPUBusy 偏大）可靠。
    //   阈值 1.6ms 按用户实测指定，想调直接改这个数。
    //   用平滑值 cpuWaitAvg 判定，避免临界值来回翻。
    if (c.counters & NP_C_CPU_FRAME) {
        // 判据有两条，**优先用驱动直证**：
        //   1) NP_HOOK_REFLEX_KNOWN 置位 = 我们问过 NvAPI_D3D_GetSleepStatus，
        //      此时 NP_HOOK_REFLEX 就是驱动给出的权威答案；
        //   2) 问不到才回退：CPU Wait < 1.6ms（等待被移出 Present 的旁证）。
        float waitRef = t.cpuWaitAvg > 0.0001f ? t.cpuWaitAvg : t.cpuWaitMs;
        bool known = (t.hookFlags & NP_HOOK_REFLEX_KNOWN) != 0;
        bool lowLatency = active && (known ? ((t.hookFlags & NP_HOOK_REFLEX) != 0)
                                          : (waitRef < 1.6f));
        acc.row(L"低延迟", lowLatency ? L"On" : L"Off",
                lowLatency ? PL_NORMAL : PL_DIM);
    }
    if (!resText.empty()) acc.row(L"分辨率", resText);   // 用户要求：归到系统分组
    if (c.counters & NP_C_RAM) {
        std::wstring v = GBv(s.ramUsedGB);
        if (s.ramTotalGB > 0) v = WF(s.ramUsedGB, 1) + L"/" + WF(s.ramTotalGB, 1) + L" GB";
        acc.row(L"内存", v, s.ramPct > 92 ? PL_WARN : PL_NORMAL);
    }
    if (c.counters & NP_C_API) acc.row(L"API", ApiName(t.gfxApi, t.presentMode), PL_DIM);
    if (c.counters & NP_C_DRAWS && hooked) {
        std::wstring v = std::to_wstring(t.drawCalls) + L" / " + std::to_wstring(t.dispatches);
        acc.row(L"Draw/Disp", v, PL_DIM);
    }
    if (c.counters & NP_C_SENSOR_SRC && s.sourceText[0]) {
        std::wstring st = FitCols(Utf8ToWide(s.sourceText), 14);
        acc.row(L"数据源", st, PL_DIM);
    }
    acc.flush(L"系统", true, 9.0f);      // GPU 组 -> 系统组：9px
}

}  // namespace np
