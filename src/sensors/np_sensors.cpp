#include "np_sensors.h"

#include <wbemidl.h>
#include <dxgi.h>
#include <pdhmsg.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>


namespace np {

// WMI 的 GUID 在 mingw 里没有 wbemuuid.lib，这里自己声明（值是公开固定的）
static const GUID kClsidWbemLocator = {
    0x4590F811, 0x1D3A, 0x11D0, {0x89, 0x1F, 0x00, 0xAA, 0x00, 0x4B, 0x2E, 0x24}};
static const GUID kIidIWbemLocator = {
    0xDC12A687, 0x737F, 0x11CF, {0x88, 0x4D, 0x00, 0xAA, 0x00, 0x4B, 0x2E, 0x24}};

// ================================================================== WMI 辅助
struct WmiRow {
    std::vector<std::pair<std::wstring, double>> nums;
    std::vector<std::pair<std::wstring, std::wstring>> strs;
};

static bool WmiRows(const wchar_t* ns, const wchar_t* wql,
                    const std::vector<std::wstring>& numProps,
                    const std::vector<std::wstring>& strProps,
                    std::vector<WmiRow>& out) {
    IWbemLocator* loc = nullptr;
    if (FAILED(CoCreateInstance(kClsidWbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                kIidIWbemLocator, reinterpret_cast<void**>(&loc))) || !loc)
        return false;

    IWbemServices* svc = nullptr;
    BSTR nsB = SysAllocString(ns);
    HRESULT hr = loc->ConnectServer(nsB, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc);
    SysFreeString(nsB);
    loc->Release();
    if (FAILED(hr) || !svc) return false;

    CoSetProxyBlanket(svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                      RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);

    BSTR lang = SysAllocString(L"WQL");
    BSTR q = SysAllocString(wql);
    IEnumWbemClassObject* en = nullptr;
    hr = svc->ExecQuery(lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en);
    SysFreeString(lang);
    SysFreeString(q);
    svc->Release();
    if (FAILED(hr) || !en) return false;

    while (true) {
        IWbemClassObject* obj = nullptr;
        ULONG n = 0;
        if (FAILED(en->Next(WBEM_INFINITE, 1, &obj, &n)) || !obj || n == 0) {
            if (obj) obj->Release();
            break;
        }
        WmiRow row;
        for (auto& p : numProps) {
            VARIANT v;
            VariantInit(&v);
            if (SUCCEEDED(obj->Get(p.c_str(), 0, &v, nullptr, nullptr))) {
                double d = 0;
                if (v.vt == VT_BSTR) d = _wtof(v.bstrVal);
                else if (v.vt == VT_R8) d = v.dblVal;
                else if (v.vt == VT_R4) d = v.fltVal;
                else if (v.vt == VT_I4) d = (double)v.lVal;
                else if (v.vt == VT_UI4) d = (double)v.ulVal;
                else if (v.vt == VT_I8) d = (double)v.llVal;
                else if (v.vt == VT_UI8) d = (double)v.ullVal;
                row.nums.emplace_back(p, d);
                VariantClear(&v);
            }
        }
        for (auto& p : strProps) {
            VARIANT v;
            VariantInit(&v);
            if (SUCCEEDED(obj->Get(p.c_str(), 0, &v, nullptr, nullptr))) {
                if (v.vt == VT_BSTR && v.bstrVal) row.strs.emplace_back(p, v.bstrVal);
                VariantClear(&v);
            }
        }
        out.push_back(std::move(row));
        obj->Release();
    }
    en->Release();
    return !out.empty();
}

bool SensorHub::WmiScalar(const wchar_t* ns, const wchar_t* wql, const wchar_t* prop,
                          double* out) const {
    std::vector<WmiRow> rows;
    if (!WmiRows(ns, wql, {prop}, {}, rows) || rows.empty()) return false;
    for (auto& kv : rows[0].nums) {
        if (kv.first == prop) { *out = kv.second; return true; }
    }
    return false;
}

// ================================================================== PDH
bool PdhQuery::Open() {
    if (opened_) return true;
    if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) return false;
    opened_ = true;
    return true;
}

void PdhQuery::Close() {
    if (query_) PdhCloseQuery(query_);
    query_ = nullptr;
    opened_ = false;
    counters_.clear();
    // ★ stars_ 必须一起清。PdhCloseQuery 之后这些 HCOUNTER 就是野句柄，
    //   而 AddStarCounter 见到同一个路径会直接返回 true（以为已经加过了）——
    //   于是下一次 Init 全程拿野句柄去 PdhGetFormattedCounterArrayW：
    //   轻则读不到任何数，重则访问已释放的内存。
    stars_.clear();
}

bool PdhQuery::AddWildcard(const std::wstring& wildcardPath) {
    if (!opened_) return false;
    DWORD bufLen = 0;
    DWORD st = PdhExpandCounterPathW(wildcardPath.c_str(), nullptr, &bufLen);
    if (st != PDH_MORE_DATA || bufLen == 0) return false;
    std::vector<wchar_t> buf((size_t)bufLen + 2);
    st = PdhExpandCounterPathW(wildcardPath.c_str(), buf.data(), &bufLen);
    if (st != ERROR_SUCCESS) return false;

    const wchar_t* p = buf.data();
    while (*p) {
        std::wstring path(p);
        p += path.size() + 1;
        if (path.empty()) continue;
        Counter c;
        c.path = path;
        // 取实例名：\\Object(Instance)\Counter
        size_t lp = path.find(L'(');
        size_t rp = path.rfind(L')');
        if (lp != std::wstring::npos && rp != std::wstring::npos && rp > lp)
            c.instance = path.substr(lp + 1, rp - lp - 1);
        if (PdhAddEnglishCounterW(query_, path.c_str(), 0, &c.handle) == ERROR_SUCCESS)
            counters_.push_back(c);
    }
    return !counters_.empty();
}

bool PdhQuery::RefreshWildcard(const std::wstring& wildcardPath) {
    if (!opened_) return false;
    DWORD bufLen = 0;
    DWORD st = PdhExpandCounterPathW(wildcardPath.c_str(), nullptr, &bufLen);
    if (st != PDH_MORE_DATA || bufLen == 0) return false;
    std::vector<wchar_t> buf((size_t)bufLen + 2);
    st = PdhExpandCounterPathW(wildcardPath.c_str(), buf.data(), &bufLen);
    if (st != ERROR_SUCCESS) return false;

    // 已加过的路径（不判重会把同一实例加两次，求和时算双份）
    std::vector<std::wstring> have;
    have.reserve(counters_.size());
    for (auto& c : counters_) have.push_back(c.path);

    int added = 0;
    const wchar_t* p = buf.data();
    while (*p) {
        std::wstring path(p);
        p += path.size() + 1;
        if (path.empty()) continue;
        bool dup = false;
        for (auto& h : have) if (h == path) { dup = true; break; }
        if (dup) continue;
        Counter c;
        c.path = path;
        size_t lp = path.find(L'(');
        size_t rp = path.rfind(L')');
        if (lp != std::wstring::npos && rp != std::wstring::npos && rp > lp)
            c.instance = path.substr(lp + 1, rp - lp - 1);
        if (PdhAddEnglishCounterW(query_, path.c_str(), 0, &c.handle) == ERROR_SUCCESS) {
            counters_.push_back(c);
            have.push_back(path);
            ++added;
        }
    }
    return added > 0;
}

bool PdhQuery::Collect() {
    if (!opened_) return false;
    if (PdhCollectQueryData(query_) != ERROR_SUCCESS) return false;
    for (auto& c : counters_) {
        PDH_FMT_COUNTERVALUE fv{};
        if (PdhGetFormattedCounterValue(c.handle, PDH_FMT_DOUBLE, nullptr, &fv) == ERROR_SUCCESS) {
            c.value = fv.doubleValue;
            c.valid = true;
        } else {
            c.valid = false;
        }
    }
    return true;
}

static bool PathHasObject(const std::wstring& path, const std::wstring& obj) {
    // path 形如 \\Computer\Object(Instance)\Counter
    size_t b = path.find(L"\\\\");
    if (b == std::wstring::npos) return false;
    size_t s = path.find(L'\\', b + 2);
    if (s == std::wstring::npos) return false;
    size_t e = path.find_first_of(L"(\\", s + 1);
    std::wstring o = path.substr(s + 1, (e == std::wstring::npos ? std::wstring::npos : e - s - 1));
    return o == obj;
}

bool PdhQuery::SumWhere(const std::wstring& objKw, const std::wstring& instKw, double* out) const {
    double sum = 0;
    int n = 0;
    for (auto& c : counters_) {
        if (!c.valid) continue;
        if (!PathHasObject(c.path, objKw)) continue;
        if (!instKw.empty() && c.instance.find(instKw) == std::wstring::npos) continue;
        sum += c.value;
        ++n;
    }
    if (!n) return false;
    *out = sum;
    return true;
}

bool PdhQuery::SumWhere2(const std::wstring& objKw, const std::wstring& kw1,
                         const std::wstring& kw2, double* out) const {
    double sum = 0;
    int n = 0;
    for (auto& c : counters_) {
        if (!c.valid) continue;
        if (!PathHasObject(c.path, objKw)) continue;
        if (c.instance.find(kw1) == std::wstring::npos) continue;
        if (!kw2.empty() && c.instance.find(kw2) == std::wstring::npos) continue;
        sum += c.value;
        ++n;
    }
    if (!n) return false;
    *out = sum;
    return true;
}

bool PdhQuery::SumInstance(const std::wstring& counterKw, const std::wstring& kw1,
                           const std::wstring& kw2, double* out) const {
    double sum = 0;
    int n = 0;
    for (auto& c : counters_) {
        if (!c.valid) continue;
        if (c.path.find(counterKw) == std::wstring::npos) continue;
        if (c.instance.find(kw1) == std::wstring::npos) continue;
        if (!kw2.empty() && c.instance.find(kw2) == std::wstring::npos) continue;
        sum += c.value;
        ++n;
    }
    if (!n) return false;
    *out = sum;
    return true;
}

bool PdhQuery::AddStarCounter(const std::wstring& path) {
    if (!opened_) return false;
    for (auto& s : stars_)
        if (s.path == path) return s.handle != nullptr;
    StarCounter s;
    s.path = path;
    if (PdhAddEnglishCounterW(query_, path.c_str(), 0, &s.handle) == ERROR_SUCCESS) {
        stars_.push_back(s);
        return true;
    }
    return false;
}

bool PdhQuery::SumStarCounter(const std::wstring& path, const std::wstring& kw1,
                              const std::wstring& kw2, double* out, int* hitCount) const {
    HCOUNTER h = nullptr;
    for (auto& s : stars_)
        if (s.path == path) { h = s.handle; break; }
    if (!h) return false;

    DWORD sz = 0, cnt = 0;
    DWORD st = PdhGetFormattedCounterArrayW(h, PDH_FMT_DOUBLE, &sz, &cnt, nullptr);
    if (st != PDH_MORE_DATA || sz == 0) return false;
    std::vector<uint8_t> buf(sz);
    st = PdhGetFormattedCounterArrayW(
        h, PDH_FMT_DOUBLE, &sz, &cnt,
        reinterpret_cast<PPDH_FMT_COUNTERVALUE_ITEM_W>(buf.data()));
    if (st != ERROR_SUCCESS) return false;

    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
    double sum = 0;
    int n = 0;
    for (DWORD i = 0; i < cnt; ++i) {
        const wchar_t* nm = items[i].szName;
        if (!nm) continue;
        if (kw1.size() && wcsstr(nm, kw1.c_str()) == nullptr) continue;
        if (kw2.size() && wcsstr(nm, kw2.c_str()) == nullptr) continue;
        DWORD cs = items[i].FmtValue.CStatus;
        if (cs != ERROR_SUCCESS && cs != PDH_CSTATUS_VALID_DATA && cs != PDH_CSTATUS_NEW_DATA)
            continue;
        sum += items[i].FmtValue.doubleValue;
        ++n;
    }
    if (hitCount) *hitCount = n;
    if (!n) return false;
    *out = sum;
    return true;
}

bool PdhQuery::MaxStarCounter(const std::wstring& path, const std::wstring& kw1,
                              const std::wstring& kw2, double* out) const {
    HCOUNTER h = nullptr;
    for (auto& s : stars_)
        if (s.path == path) { h = s.handle; break; }
    if (!h) return false;

    DWORD sz = 0, cnt = 0;
    DWORD st = PdhGetFormattedCounterArrayW(h, PDH_FMT_DOUBLE, &sz, &cnt, nullptr);
    if (st != PDH_MORE_DATA || sz == 0) return false;
    std::vector<uint8_t> buf(sz);
    st = PdhGetFormattedCounterArrayW(
        h, PDH_FMT_DOUBLE, &sz, &cnt,
        reinterpret_cast<PPDH_FMT_COUNTERVALUE_ITEM_W>(buf.data()));
    if (st != ERROR_SUCCESS) return false;

    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
    bool found = false;
    double best = 0;
    for (DWORD i = 0; i < cnt; ++i) {
        const wchar_t* nm = items[i].szName;
        if (!nm) continue;
        if (kw1.size() && wcsstr(nm, kw1.c_str()) == nullptr) continue;
        if (kw2.size() && wcsstr(nm, kw2.c_str()) == nullptr) continue;
        DWORD cs = items[i].FmtValue.CStatus;
        if (cs != ERROR_SUCCESS && cs != PDH_CSTATUS_VALID_DATA && cs != PDH_CSTATUS_NEW_DATA)
            continue;
        double v = items[i].FmtValue.doubleValue;
        if (!found || v > best) { best = v; found = true; }
    }
    if (!found) return false;
    *out = best;
    return true;
}

bool PdhQuery::MaxWhere(const std::wstring& objKw, const std::wstring& instKw,
                        std::wstring* instOut, double* out) const {
    double best = -1e300;
    bool found = false;
    for (auto& c : counters_) {
        if (!c.valid) continue;
        if (!PathHasObject(c.path, objKw)) continue;
        if (!instKw.empty() && c.instance.find(instKw) == std::wstring::npos) continue;
        if (!found || c.value > best) { best = c.value; *instOut = c.instance; found = true; }
    }
    if (!found) return false;
    *out = best;
    return true;
}

// ================================================================== 游戏进程的 GPU 引擎数据
//
// 这是「优先读系统/驱动数据」那一路的核心。
// Windows 通过 PDH 的 `\GPU Engine(*)\Running Time` 按 (进程, 引擎) 暴露 GPU 累计
// 执行时间，实例名形如：
//     pid_1234_luid_0x00000000_0x0000ABCD_phys_0_eng_0_engtype_3D
// **非管理员可读**（实测枚举出 915 个实例）。
//
// 「本帧 GPU 忙时间」= 两次采样之间该进程 GPU 执行时间的增量 ÷ 这段时间的帧数。
// 它是驱动自己报的累计量，所以与锁帧、NVIDIA Reflex、多线程提交**都无关** ——
// 这正是靠 Present 钩子推算做不到的。
void SensorHub::PollGameGpu(uint32_t pid, uint32_t frameDelta, NPSensors& out) {
    // 先把**上一次的值**放回去。Poll() 每次都把 out 清成 -1，
    // 如果这里不填，界面大多数轮询拿到的就是 -1（用户反馈的「时有时无」）。
    out.gpuBusyMs = lastBusyMs_;
    out.engCompute = lastCompute_;
    out.engOfa = lastOfa_;
    gameGpuDiag_.clear();
    if (!gameGpuOk_) { gameGpuDiag_ = "Running Time 计数器在 Init 时没加上"; gameGpuPid_ = 0; lastGameGpuSec_ = -1.0; lastBusyMs_ = -1.0f; return; }
    if (!pid) { gameGpuDiag_ = "还没有目标进程"; gameGpuPid_ = 0; lastGameGpuSec_ = -1.0; lastBusyMs_ = -1.0f; return; }

    // 避免与 SensorHub::Poll() 在同一毫秒内重复 Collect 导致速率计数器样本塌缩为 0
    uint64_t collectNow = GetTickCount64();
    if (collectNow - lastPdhCollectMs_ >= 80) {
        if (!pdh_.Collect()) { gameGpuDiag_ = "PdhCollectQueryData 失败"; return; }
        lastPdhCollectMs_ = collectNow;
    }

    wchar_t kw[64];
    swprintf(kw, 64, L"pid_%lu_", (unsigned long)pid);

    // 分引擎占用（该进程自己在这类引擎上的百分比）
    // ⚠ 必须放在下面所有提前 return **之前**（原来它写在 Running Time 那段
    //   return 的后面）：目标进程刚建立基线的那一轮、以及 Running Time 暂时
    //   读不到的那一轮（比如游戏还在菜单里）都会提前 return，
    //   于是「AI 引擎」那一行在这些轮询里永远算不到、只能显示上一次的旧值。
    {
        double v = 0;
        if (pdh_.SumStarCounter(L"\\GPU Engine(*)\\Utilization Percentage", kw,
                                L"engtype_Compute", &v, nullptr)) {   // ⚠ 大小写敏感！真实实例名是大写 C
        // （原来写全小写 engtype_compute -> wcsstr 大小写敏感 -> 命中 0 个
        //   -> 永远显示 compute=-1.0%。子代理实测：engtype_Compute 有 5 个实例）
            out.engCompute = (float)v;
            lastCompute_ = (float)v;
        } else {
            out.engCompute = lastCompute_;
        }
        v = 0;
        // OFA = 光流加速器，N 卡上 DLSS **帧生成**专用的硬件单元。
        // 它只要在动，就说明帧生成在工作 —— 系统计数器直接给，不用估算。
        if (pdh_.SumStarCounter(L"\\GPU Engine(*)\\Utilization Percentage", kw,
                                L"engtype_OFA", &v, nullptr)) {       // ⚠ 同上：真实是 OFA 全大写
        // （原来写 engtype_ofa -> 命中 0 个；实测 engtype_OFA 有 38 个实例）
            out.engOfa = (float)v;
            lastOfa_ = (float)v;
        } else {
            out.engOfa = lastOfa_;
        }
    }

    // 该进程在所有引擎上的 GPU 执行时间之和。
    // ⚠ 单位是 **100 纳秒**（FILETIME 那种累计量），不是秒 —— PDH 对这类
    //   累积计数器不做换算，直接给原始值。忘了除 1e7 会得到天文数字。
    // ⚠ 必须用 SumStarCounter（动态实例列表）：进程的 GPU 引擎实例是游戏启动后
    //   才出现的，用「启动时展开好」的静态列表永远找不到它。
    double sec = 0;
    int hits = 0;
    if (!pdh_.SumStarCounter(L"\\GPU Engine(*)\\Running Time", kw, L"engtype_", &sec, &hits)) {
        char b[192];
        snprintf(b, sizeof(b),
                 "PDH 动态实例里没有 pid 对应的 Running Time 实例（pid 可能还没用到 GPU）");
        gameGpuDiag_ = b;
        lastGameGpuSec_ = -1.0;
        return;
    }
    sec /= 1.0e7;
    {
        char b[192];
        snprintf(b, sizeof(b), "命中 %d 个引擎实例，累计 %.3f 秒", hits, sec);
        gameGpuDiag_ = b;   // 正常路径也留一条，方便对照
    }

    uint64_t nowMs = GetTickCount64();
    if (pid != gameGpuPid_) {          // 换了目标进程：重建基线
        gameGpuPid_ = pid;
        lastGameGpuSec_ = sec;
        lastGameGpuMs_ = nowMs;
        lastPidFrameTotal_ = frameDelta;   // frameDelta 传的是累计帧数
        return;
    }

    // ★ 帧计数是单调累计值。它反而变小只有一种可能：钩子被重新注入 / 进程重启后
    //   复用了同一个 pid。此时直接相减会无符号下溢成天文数字，
    //   算出来的「每帧 GPU 时间」恒为 0（界面显示 0.00 ms）。重建基线即可。
    if (frameDelta < lastPidFrameTotal_) {
        lastGameGpuSec_ = sec;
        lastGameGpuMs_ = nowMs;
        lastPidFrameTotal_ = frameDelta;
        lastBusyMs_ = -1.0f;
        return;
    }

    double dsec = sec - lastGameGpuSec_;
    uint64_t dms = nowMs - lastGameGpuMs_;
    uint32_t dframes = frameDelta - lastPidFrameTotal_;
    if (dms >= 100) {
        if (dsec > 0 && dframes > 0) {
            // 算出新样本，**并且把它留存下来**
            lastBusyMs_ = (float)(dsec * 1000.0 / (double)dframes);
            lastGameGpuSec_ = sec;
            lastGameGpuMs_ = nowMs;
            lastPidFrameTotal_ = frameDelta;
        } else if (dsec < 0) {
            lastGameGpuSec_ = sec;
            lastGameGpuMs_ = nowMs;
            lastPidFrameTotal_ = frameDelta;
        }
    }
    // ★ 把**上一次算出来的值**给出去，而不是每次轮询都重置成 -1。
    //
    // 原来的写法是：Poll() 先把 out 清成 -1，而我这里间隔不足 100ms 就直接
    // return —— 界面每秒轮询十几次，于是绝大多数轮询拿到的都是 -1，
    // 只有偶尔那一次有值。用户看到的就是「时有时无，大部分时间没有，
    // 偶尔闪一下」。
    out.gpuBusyMs = lastBusyMs_;
    return;
}

// ================================================================== 初始化
bool SensorHub::Init() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    comInited_ = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;

    nvml_.Load();
    nvapi_.Load();
    adl_.Load();
    hwinfoTried_ = true;
    hwinfo_.Open();

    if (pdh_.Open()) {
        pdh_.AddWildcard(L"\\GPU Engine(*)\\Utilization Percentage");
        // 按 (进程, 引擎) 的累计 GPU 执行时间 —— 驱动报的数，非管理员可读。
        // ⚠ 必须用**带 `*` 的计数器**（动态实例列表），不能用 AddWildcard 展开：
        //   展开出来的实例在 AddCounter 那一刻就冻结了，游戏后启动就永远读不到
        //   （用户实测：先开游戏再开 NextPerf 有数据，反过来整场没数据）。
        if (pdh_.AddStarCounter(L"\\GPU Engine(*)\\Running Time")) gameGpuOk_ = true;
        pdh_.AddStarCounter(L"\\GPU Engine(*)\\Utilization Percentage");
        // CPU 包功耗：Windows 的 EMI（Energy Meter Interface）以 PDH 对象
        // `Energy Meter` 暴露 Intel RAPL 域。注意对象名不是 `Energy Meter Interface`。
        pdh_.AddStarCounter(L"\\Energy Meter(*)\\Power");
        pdh_.AddWildcard(L"\\GPU Adapter Memory(*)\\Dedicated Usage");
        pdh_.AddStarCounter(L"\\GPU Adapter Memory(*)\\Dedicated Usage");
        pdh_.AddWildcard(L"\\GPU Adapter Memory(*)\\Shared Usage");
        pdh_.AddWildcard(L"\\Processor Information(*)\\% Processor Performance");
        // ★ 同一路计数器**必须也进 stars_ 表**，否则 MaxStarCounter /
        //   SumStarCounter 查不到它（它们只遍历 stars_，见其实现）。
        //   CPU 频率就是靠这里读 % Processor Performance 的 ——
        //   只注册 Wildcard 的话 pct 恒为 0，会静默回落到 NT 路径并恒显示基准频率。
        pdh_.AddStarCounter(L"\\Processor Information(*)\\% Processor Performance");
        pdh_.AddWildcard(L"\\Processor Information(*)\\Processor Frequency");
        pdh_.AddStarCounter(L"\\Processor Information(*)\\Processor Frequency");
        if (pdh_.Collect()) lastPdhCollectMs_ = GetTickCount64();  // 第一次采集给速率型计数器做基线
    }

    // 显卡基础信息用 DXGI 拿，优先挑选非软件模拟且专用显存最大的适配器（兼容核显+独显双卡）
    {
        IDXGIFactory1* f = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f))) && f) {
            IDXGIAdapter1* a = nullptr;
            bool picked = false;
            for (UINT idx = 0; f->EnumAdapters1(idx, &a) != DXGI_ERROR_NOT_FOUND; ++idx) {
                if (!a) continue;
                DXGI_ADAPTER_DESC1 d{};
                if (SUCCEEDED(a->GetDesc1(&d))) {
                    if ((d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                        if (!picked || d.DedicatedVideoMemory > dxgiVramBytes_) {
                            char name[NP_NAME_LEN]{};
                            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name,
                                                NP_NAME_LEN - 1, nullptr, nullptr);
                            dxgiName_ = name;
                            dxgiVendorId_ = d.VendorId;
                            dxgiVramBytes_ = d.DedicatedVideoMemory;
                            picked = true;
                        }
                    }
                }
                a->Release();
                a = nullptr;
            }
            f->Release();
        }
    }

    // CPU 基准频率（注册表里是标称值，配合 PDH 的性能百分比换算实际频率）
    {
        HKEY hk = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                          L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ,
                          &hk) == ERROR_SUCCESS) {
            DWORD mhz = 0, sz = sizeof(mhz);
            if (RegQueryValueExW(hk, L"~MHz", nullptr, nullptr,
                                 reinterpret_cast<LPBYTE>(&mhz), &sz) == ERROR_SUCCESS)
                cpuBaseMHz_ = (double)mhz;
            RegCloseKey(hk);
        }
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        cpuThreads_ = (uint32_t)si.dwNumberOfProcessors;
        // 物理核数
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION buf = nullptr;
        DWORD len = 0;
        GetLogicalProcessorInformation(nullptr, &len);
        if (len) {
            buf = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION>(malloc(len));
            if (buf && GetLogicalProcessorInformation(buf, &len)) {
                DWORD n = len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
                for (DWORD i = 0; i < n; ++i)
                    if (buf[i].Relationship == RelationProcessorCore) cpuCores_++;
            }
            free(buf);
        }
        if (!cpuCores_) cpuCores_ = cpuThreads_;
    }

    ProbeSources();
    return true;
}

void SensorHub::Shutdown() {
    nvml_.Unload();
    nvapi_.Unload();
    adl_.Unload();
    hwinfo_.Close();
    pdh_.Close();
    pdhNominalMHz_ = 0.0;
    lastPdhCollectMs_ = 0;
    hwinfoNextTryMs_ = 0;
    hwinfoFailStreak_ = 0;
    lhmNextTryMs_ = 0;
    acpiNextTryMs_ = 0;
    lhmPresent_ = false;
    cachedLhmCpuTemp_ = -273.0f;
    cachedLhmRt_ = -1.0f;
    cachedLhmTensor_ = -1.0f;
    cachedAcpiTemp_ = -273.0f;
    cpuPowerEma_ = 0.0f;
    lastCpuPowerMs_ = 0;
    if (comInited_) { CoUninitialize(); comInited_ = false; }
}

void SensorHub::ProbeSources() {
    srcInfo_.clear();
    auto add = [&](uint32_t mask, bool present, const char* name, const char* note) {
        SourceInfo si{mask, present, false, name, note};
        srcInfo_.push_back(si);
        if (present) available_ |= mask;
    };

    add(NP_SRC_NVML, nvml_.loaded, "NVML (NVIDIA)",
        nvml_.loaded ? "占用率/显存/温度/功耗/频率/风扇" : "未检测到 NVIDIA 驱动或 nvml.dll");
    {
        char nvNote[96];
        if (nvapi_.loaded) {
            snprintf(nvNote, sizeof(nvNote), "温度/频率/风扇/利用率域 (gpu=%u)", (unsigned)nvapi_.gpuCount);
        } else {
            snprintf(nvNote, sizeof(nvNote), "未加载 (loadErr=%d)", nvapi_.loadErr);
        }
        add(NP_SRC_NVAPI, nvapi_.loaded, "NVAPI (NVIDIA)", nvNote);
    }
    add(NP_SRC_ADL, adl_.loaded, "ADL (AMD)",
        adl_.loaded ? "占用率/温度 (Overdrive5)" : "未检测到 atiadlxx.dll");
    add(NP_SRC_HWINFO, hwinfo_.loaded, "HWiNFO 共享内存",
        hwinfo_.loaded ? "温度/功耗/热点/主板传感器"
                       : "需在 HWiNFO 设置里开启 Shared Memory Support");
    add(NP_SRC_LHM, lhmPresent_, "LibreHardwareMonitor",
        lhmPresent_ ? "WMI CPU 温度 / 硬件传感器" : "未检测到 WMI root\\LibreHardwareMonitor");
    add(NP_SRC_PDH, pdh_.Count() > 0, "PDH 性能计数器",
        pdh_.Count() > 0 ? "进程 GPU 帧时间 / CPU 频率功耗 / 显存"
                         : "无可用 GPU 性能计数器");
    add(NP_SRC_WMI, comInited_, "WMI (ACPI)", "ACPI 热区温度兜底读数");
}

void SensorHub::UpdateSourceUsed(uint32_t mask, bool present, bool used, const char* note) {
    for (auto& s : srcInfo_) {
        if (s.mask == mask) {
            s.present = present;
            s.used = used;
            if (note) s.note = note;
            break;
        }
    }
}

void SensorHub::EnsureLhmPolled(uint64_t nowMs) {
    if (!comInited_ || nowMs < lhmNextTryMs_) return;
    std::vector<WmiRow> rows;
    if (WmiRows(L"root\\LibreHardwareMonitor", L"SELECT Name,Value FROM Sensor",
                {L"Value"}, {L"Name"}, rows)) {
        lhmPresent_ = true;
        available_ |= NP_SRC_LHM;
        lhmNextTryMs_ = nowMs + 1000;
        float tCpu = -273.0f, tRt = -1.0f, tTen = -1.0f;
        for (auto& r : rows) {
            if (r.strs.empty() || r.nums.empty()) continue;
            std::wstring n = r.strs[0].second;
            std::transform(n.begin(), n.end(), n.begin(), ::towlower);
            double v = r.nums[0].second;
            if (tCpu < -200.0f && (n.find(L"cpu package") != std::wstring::npos ||
                                   n.find(L"core (tctl") != std::wstring::npos)) {
                tCpu = (float)v;
            } else if (tTen < 0.0f && n.find(L"tensor") != std::wstring::npos) {
                tTen = (float)v;
            } else if (tRt < 0.0f && n.find(L"rt core") != std::wstring::npos) {
                tRt = (float)v;
            }
        }
        cachedLhmCpuTemp_ = tCpu;
        cachedLhmRt_ = tRt;
        cachedLhmTensor_ = tTen;
        UpdateSourceUsed(NP_SRC_LHM, true, tCpu > -200.0f || tRt >= 0.0f || tTen >= 0.0f,
                         "WMI CPU 温度 / 硬件传感器");
    } else {
        lhmPresent_ = false;
        available_ &= ~NP_SRC_LHM;
        cachedLhmCpuTemp_ = -273.0f;
        cachedLhmRt_ = -1.0f;
        cachedLhmTensor_ = -1.0f;
        lhmNextTryMs_ = nowMs + 30000;
        UpdateSourceUsed(NP_SRC_LHM, false, false, "未检测到 WMI root\\LibreHardwareMonitor");
    }
}

std::string SensorHub::Describe() const {
    std::string s;
    auto mark = [&](uint32_t m, const char* n) {
        if (available_ & m) { if (!s.empty()) s += "+"; s += n; }
    };
    mark(NP_SRC_NVML, "NVML");
    mark(NP_SRC_NVAPI, "NVAPI");
    mark(NP_SRC_ADL, "ADL");
    mark(NP_SRC_HWINFO, "HWiNFO");
    mark(NP_SRC_LHM, "LHM");
    mark(NP_SRC_PDH, "PDH");
    mark(NP_SRC_WMI, "WMI");
    if (s.empty()) s = "无";
    return s;
}

// ================================================================== CPU / 内存
// ---------------------------------------------------------------------------
// CPU 当前频率：CallNtPowerInformation(ProcessorInformation)
//
// 为什么用它而不是 PDH 的「标称 × 性能百分比」：后者依赖一个猜测的标称频率。
// 实测本机 PDH `Processor Frequency` 报 2300MHz、`% Processor Performance` 报 141%
// （=> 3259MHz），而 CallNtPowerInformation 直接给出每核当前 MHz，没有这层误差。
//
// 用 GetProcAddress 动态取，避免给 build.bat 增加 -lpowrprof 依赖。
// MinGW 头文件缺 PROCESSOR_POWER_INFORMATION，这里按 MSDN 定义补齐。
// ---------------------------------------------------------------------------
// 前置声明：本文件属于传感器层，AppLog 定义在 app 层。
// 只做诊断输出用；本文件只被主程序链接（钩子不链它），所以符号可用。
namespace {
struct NpProcessorPowerInfo {
    unsigned long Number;
    unsigned long MaxMhz;
    unsigned long CurrentMhz;
    unsigned long MhzLimit;
    unsigned long MaxIdleState;
    unsigned long CurrentIdleState;
};
// POWER_INFORMATION_LEVEL 里的 ProcessorInformation = 11
constexpr int kNpProcessorInformation = 11;

bool CpuFreqFromPowerInfo(double* mhzOut) {
    using Fn = long (*)(int, void*, unsigned long, void*, unsigned long);
    static Fn fn = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        HMODULE m = LoadLibraryW(L"powrprof.dll");
        if (m) fn = reinterpret_cast<Fn>(GetProcAddress(m, "CallNtPowerInformation"));
    }
    if (!fn) return false;

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    unsigned long n = si.dwNumberOfProcessors;
    if (!n || n > 1024) return false;
    unsigned long bytes = n * (unsigned long)sizeof(NpProcessorPowerInfo);
    std::vector<NpProcessorPowerInfo> buf(n);
    // 注意：失败时返回的是 NTSTATUS（如 0xC0000023），**不是所需长度**
    long st = fn(kNpProcessorInformation, nullptr, 0, buf.data(), bytes);
    if (st != 0) return false;

    // 取所有核里最快的那个 —— 玩家关心的是"CPU 现在跑多快"，
    // 而任务管理器/各家工具展示的也是最高核心频率。
    unsigned long best = 0;
    double sum = 0;
    int cnt = 0;
    for (unsigned long i = 0; i < n; ++i) {
        if (buf[i].CurrentMhz > best) best = buf[i].CurrentMhz;
        if (buf[i].CurrentMhz > 0) { sum += buf[i].CurrentMhz; ++cnt; }
    }
    if (!best || !cnt) return false;
    // 同时给出平均值作参考（放日志里，面板只显示最高）
    *mhzOut = (double)best;
    return true;
}
}  // namespace

void SensorHub::PollCpu(NPSensors& out) {
    FILETIME idle{}, kern{}, user{};
    if (GetSystemTimes(&idle, &kern, &user)) {
        auto toU64 = [](const FILETIME& f) {
            return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime;
        };
        uint64_t i = toU64(idle), k = toU64(kern), u = toU64(user);
        uint64_t nowMs = GetTickCount64();
        if (cpuBaseValid_) {
            uint64_t di = i - lastIdle_;
            uint64_t dk = k - lastKern_;
            uint64_t du = u - lastUser_;
            uint64_t total = dk + du;
            if (total > 0) {
                double busy = (di < total) ? (double(total - di) / double(total) * 100.0) : 0.0;
                busy = std::clamp(busy, 0.0, 100.0);
                // ★ 1 秒级平滑（2026-10-11，用户反馈"蜘蛛侠能波动到 50%、看着乱跳"）。
                //
                //   原来画到面板上的是「两次轮询之间的瞬时差分」，而主循环节拍约 **120ms**
                //   → 窗口太短，数字跳得厉害。任务管理器那种"稳"的感觉来自它约 **1 秒**的窗口。
                //   这里用**按真实间隔算系数的指数平滑**（不是固定 alpha，节拍变了也不会失真）：
                //       alpha = 1 − exp(−dt / τ)，τ = 0.5s
                //   效果：约 1 秒内 86% 的权重落在最近这一秒；既不跳，也不会明显滞后。
                //   口径不变 —— 仍然是**整机**占用（所有进程、所有逻辑处理器），
                //   100% = 全部逻辑处理器跑满（本机 24 线程）。
                static const double kTauMs = 500.0;
                double dt = double(nowMs - lastCpuMs_);
                if (!cpuEmaValid_) {
                    cpuUsageEma_ = busy;
                    cpuEmaValid_ = true;
                } else if (dt > 0.0 && dt < 10000.0) {
                    double alpha = 1.0 - std::exp(-dt / kTauMs);
                    cpuUsageEma_ += alpha * (busy - cpuUsageEma_);
                } else {
                    cpuUsageEma_ = busy;      // 间隔异常（挂起/长时间不轮询）时直接重置
                }
                out.cpuUsage = (float)std::clamp(cpuUsageEma_, 0.0, 100.0);
            }
        }
        lastIdle_ = i; lastKern_ = k; lastUser_ = u;
        lastCpuMs_ = nowMs;
        cpuBaseValid_ = true;
    }
    out.cpuCores = cpuCores_;
    out.cpuThreads = cpuThreads_;

    double perfFreq = -1;
    // 频率：优先 CallNtPowerInformation（直接给每核当前 MHz，无需猜标称频率）
    double mhz = 0;
    // ★ 优先用 PDH：当前频率 = 标称 × %Processor Performance(_Total) / 100
    //
    // 为什么不用 CallNtPowerInformation：用户实测它在本机（Intel HWP/Speed Shift）
    // **恒定返回基准频率 2700MHz** —— 面板因此一直显示 2700，那不是当前频率。
    //
    // 用户给的 typeperf 实测（每秒一行，20 行）证明只有这个是活的：
    //     Processor Frequency     = 2300.000000  （恒定，死值，只能当标称用）
    //     % Processor Performance = 108.9 → 187.8（跟着负载变）
    //   2300 × 187.8% = 4320MHz，与任务管理器量级一致。
    //
    // `_Total` 实例是 **Windows 自己给的多核聚合结果**，不需要我们拟合。
    // ★ 顺序很重要：**PDH 优先，NT 兜底**。
    //
    // 原来这里是反的（NT 先跑），而 NT 在本机（Intel HWP/Speed Shift）**恒定返回
    // 基准频率 2700MHz**，于是后面的 PDH 判断因为「已经有值」永远进不去 ——
    // 面板因此一直显示 2700。**这条 PDH 路从来没被执行过。**
    // （用 --selftest 自查发现，不是推演出来的。）
    //
    // 用户 typeperf 实测证明只有这个是活的：
    //     Processor Frequency     = 2300（恒定，只能当标称）
    //     % Processor Performance = 108.9 → 187.8（跟着负载变）
    //   2300 x 187.8% = 4320MHz，与任务管理器量级一致。
    //
    // 注意 API：按「路径 + 实例」取数用 SumWhere；SumInstance 的第一个参数是
    // **关键字**（匹配计数器名用），拿完整路径喂它会命中不了。
    // ★ 诊断结论（留给下一轮，别再从零查）：
    //
    // 已确证的事实：
    //   1) CallNtPowerInformation 的 CurrentMhz 在本机（Intel HWP/Speed Shift）
    //      **恒定返回基准 2700MHz** —— 所以「一直显示 2700」是这个原因。它只能垫底。
    //   2) 用户 typeperf 实测：\\Processor Information(_Total)\\Processor Frequency
    //      恒为 2300（可当标称），% Processor Performance 在 108~187% 之间跟着负载变。
    //      => 2300 x 187.8% = 4320MHz，与任务管理器量级一致。
    //   3) 我试过 pdh_.SumWhere(L"Processor Information", L"_Total", &pct) 取百分比，
    //      但实测 pct 达到 **289%**（x2300 = 6659MHz，荒谬）—— 
    //      **说明 SumWhere 并不是只取 _Total 那一个实例，而是把多个实例累加了**。
    //      下一步要解决的就是这一点：只取 _Total 实例的单个值。
    // 因此本轮**先退回 NT 路径**（显示 2700，虽不对但不荒谬），
    // 把这个坑记在这里，避免下一轮又从头试一遍。
    // ★ 用 MaxStarCounter 取【单一实例】的百分比（不会像 SumWhere 那样累加多实例）。
    //   SumWhere 实测返回 289%（用户 typeperf 实测同一实例只有 108~187%）——
    //   它把多个实例累加了，x2300 会得到 6659MHz 这种荒谬值。
    //   MaxStarCounter 取「最忙的那个核」，也是任务管理器/各家工具的惯例。
    out.cpuClock = 0.0f;
    {
        if (pdhNominalMHz_ <= 0.0) {
            double nom = 0.0;
            if (pdh_.MaxStarCounter(L"\\Processor Information(*)\\Processor Frequency", L"", L"", &nom) &&
                nom > 100.0 && nom < 8000.0) {
                pdhNominalMHz_ = nom;
            }
        }
        double nominalMHz = (pdhNominalMHz_ > 100.0) ? pdhNominalMHz_
                                                    : ((cpuBaseMHz_ > 100.0) ? cpuBaseMHz_ : 2300.0);
        double pct = 0;
        if (pdh_.MaxStarCounter(L"\\Processor Information(*)\\% Processor Performance", L"", L"", &pct) && pct > 1.0) {
            double mhz2 = nominalMHz * pct / 100.0;
            if (mhz2 > 100.0 && mhz2 < 8000.0) {   // 夹紧：超过 8GHz 一律视为读错
                out.cpuClock = (float)mhz2;
                cpuFreqFromNt_ = false;
            }
        }
    }
    // 兜底：NT 路径（本机恒返回基准频率，所以只在前一条拿不到时用）
    if (out.cpuClock < 100.0f && CpuFreqFromPowerInfo(&mhz)) {
        out.cpuClock = (float)mhz;
        cpuFreqFromNt_ = true;
    }

    // 温度优先级：HWiNFO > LibreHardwareMonitor > ACPI 热区
    double t = 0;
    if (hwinfo_.loaded) {
        if (hwinfo_.Find(nullptr, L"CPU Package", &t) ||
            hwinfo_.Find(L"CPU", L"Package", &t) ||
            hwinfo_.FindAny(L"CPU (Tctl", &t) ||
            hwinfo_.FindAny(L"CPU Package", &t)) {
            out.cpuTemp = (float)t;
            out.sources |= NP_SRC_HWINFO;
        }
    }
    uint64_t wmiNow = GetTickCount64();
    if (out.cpuTemp < -200.0f && comInited_) {
        EnsureLhmPolled(wmiNow);
        if (cachedLhmCpuTemp_ > -200.0f) {
            out.cpuTemp = cachedLhmCpuTemp_;
            out.sources |= NP_SRC_LHM;
        }
    }
    if (out.cpuTemp < -200.0f && comInited_) {
        if (wmiNow >= acpiNextTryMs_) {
            double tk = 0;
            if (WmiScalar(L"root\\WMI", L"SELECT CurrentTemperature FROM MSAcpi_ThermalZoneTemperature",
                          L"CurrentTemperature", &tk) && tk > 1000.0 && tk < 5000.0) {
                cachedAcpiTemp_ = (float)(tk / 10.0 - 273.15);
                acpiNextTryMs_ = wmiNow + 1000;
            } else {
                cachedAcpiTemp_ = -273.0f;
                acpiNextTryMs_ = wmiNow + 30000;
            }
        }
        if (cachedAcpiTemp_ > -200.0f) {
            out.cpuTemp = cachedAcpiTemp_;
            out.sources |= NP_SRC_WMI;
        }
    }
    // 功耗：**EMI 优先**，HWiNFO 回退。
    //
    // 为什么优先 EMI：它是 Windows 自带的能量计量接口，走 Intel RAPL，免驱动、
    // 免管理员、不依赖用户装 HWiNFO。实测本机可用。
    //   * PDH 对象名是 `Energy Meter`（不是 `Energy Meter Interface`）
    //   * 必须按实例过滤出 `PKG`（整包）；`_Total` 恒为 0，PP0 只是核心
    //   * 单位实测推断为**毫瓦**（读数 12922 对应 12.9W），故 /1000
    double mw = 0;
    if (pdh_.MaxStarCounter(L"\\Energy Meter(*)\\Power", L"PKG", L"", &mw) && mw > 0) {
        // 按真实间隔 dt 做指数平滑（tau = 1000ms），避免不同 pollMs 节拍下响应漂移
        float inst = (float)(mw / 1000.0);
        uint64_t pnow = GetTickCount64();
        double dt = (lastCpuPowerMs_ && pnow > lastCpuPowerMs_)
                        ? (double)(pnow - lastCpuPowerMs_)
                        : 0.0;
        if (cpuPowerEma_ <= 0.0f || dt <= 0.0 || dt >= 10000.0) {
            cpuPowerEma_ = inst;
        } else {
            double alpha = 1.0 - std::exp(-dt / 1000.0);
            cpuPowerEma_ += (float)(alpha * (inst - cpuPowerEma_));
        }
        lastCpuPowerMs_ = pnow;
        out.cpuPower = cpuPowerEma_;
        out.sources |= NP_SRC_PDH;
    } else {
        double p = 0;
        if (hwinfo_.loaded && (hwinfo_.Find(L"CPU", L"Package Power", &p) ||
                               hwinfo_.FindAny(L"CPU Package Power", &p))) {
            out.cpuPower = (float)p;
            out.sources |= NP_SRC_HWINFO;
        }
    }
}

void SensorHub::PollRam(NPSensors& out) {
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        out.ramTotalGB = (float)(ms.ullTotalPhys / (1024.0 * 1024.0 * 1024.0));
        out.ramUsedGB = (float)((ms.ullTotalPhys - ms.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0));
        out.ramPct = out.ramTotalGB > 0 ? (out.ramUsedGB / out.ramTotalGB * 100.0f) : 0;
    }
    // 显存总量：DXGI 提供，占用量优先 NVML
    if (!dxgiName_.empty()) {
        NPCopyStr(out.gpuName, NP_NAME_LEN, dxgiName_.c_str());
        out.vramTotalGB = (float)(dxgiVramBytes_ / (1024.0 * 1024.0 * 1024.0));
    }
    if (dxgiVendorId_ == 0x10DE) out.gpuVendor = 1;
    else if (dxgiVendorId_ == 0x1002) out.gpuVendor = 2;
    else if (dxgiVendorId_ == 0x8086) out.gpuVendor = 3;
}

// ================================================================== NVIDIA
void SensorHub::PollGpuNvidia(NPSensors& out) {
    if (!nvml_.loaded && !nvapi_.loaded) return;
    encUtil_ = -1.0f;
    decUtil_ = -1.0f;

    if (nvml_.loaded) {
        unsigned int count = 0;
        if (nvml_.DeviceGetCount && nvml_.DeviceGetCount(&count) == 0 && count > 0) {
            // 每 32 次轮询重新挑一次「正在干活的那张卡」；平时沿用上次结果，减少开销
            if (nvmlIndex_ < 0 || (pollCount_ % 32) == 0 || nvmlIndex_ >= (int)count) {
                int best = 0;
                unsigned int bestUtil = 0;
                for (unsigned int i = 0; i < count; ++i) {
                    nvmlDevice_t dev = nullptr;
                    if (nvml_.DeviceGetHandleByIndex(i, &dev) != 0 || !dev) continue;
                    nvmlUtilization_t u{};
                    if (nvml_.DeviceGetUtilizationRates(dev, &u) == 0 && u.gpu > bestUtil) {
                        bestUtil = u.gpu;
                        best = (int)i;
                    }
                }
                nvmlIndex_ = best;
            }

            nvmlDevice_t dev = nullptr;
            if (nvml_.DeviceGetHandleByIndex((unsigned int)nvmlIndex_, &dev) == 0 && dev) {
                out.gpuVendor = 1;
                char name[NP_NAME_LEN]{};
                if (nvml_.DeviceGetName(dev, name, NP_NAME_LEN) == 0 && name[0])
                    NPCopyStr(out.gpuName, NP_NAME_LEN, name);

                nvmlUtilization_t util{};
                if (nvml_.DeviceGetUtilizationRates(dev, &util) == 0) {
                    out.gpuUsage = (float)util.gpu;
                    out.sources |= NP_SRC_NVML;
                }

                nvmlMemory_t mem{};
                if (nvml_.DeviceGetMemoryInfo(dev, &mem) == 0) {
                    out.vramTotalGB = (float)(mem.total / (1024.0 * 1024.0 * 1024.0));
                    out.vramUsedGB = (float)(mem.used / (1024.0 * 1024.0 * 1024.0));
                    out.vramPct = mem.total ? (float)((double)mem.used / (double)mem.total * 100.0) : 0;
                    out.sources |= NP_SRC_NVML;
                }

                unsigned int v = 0;
                if (nvml_.DeviceGetTemperature && nvml_.DeviceGetTemperature(dev, NVML_TEMPERATURE_GPU, &v) == 0) {
                    out.gpuTemp = (float)v;
                    out.sources |= NP_SRC_NVML;
                }
                if (nvml_.DeviceGetPowerUsage && nvml_.DeviceGetPowerUsage(dev, &v) == 0)
                    out.gpuPower = (float)(v / 1000.0);
                if (nvml_.DeviceGetEnforcedPowerLimit && nvml_.DeviceGetEnforcedPowerLimit(dev, &v) == 0)
                    out.gpuPowerLimit = (float)(v / 1000.0);
                if (nvml_.DeviceGetFanSpeed && nvml_.DeviceGetFanSpeed(dev, &v) == 0)
                    out.gpuFanPct = (float)v;
                if (nvml_.DeviceGetClockInfo) {
                    if (nvml_.DeviceGetClockInfo(dev, NVML_CLOCK_GRAPHICS, &v) == 0) out.gpuClock = (float)v;
                    if (nvml_.DeviceGetClockInfo(dev, NVML_CLOCK_MEM, &v) == 0) out.memClock = (float)v;
                }
                if (nvml_.DeviceGetTemperatureThreshold) {
                    if (nvml_.DeviceGetTemperatureThreshold(dev, NVML_TEMPERATURE_THRESHOLD_GPU_MAX, &v) == 0)
                        maxGpuTemp_ = (float)v;
                }
                if (nvml_.DeviceGetEncoderUtilization) {
                    unsigned int su = 0, sa = 0;
                    if (nvml_.DeviceGetEncoderUtilization(dev, &su, &sa) == 0) encUtil_ = (float)su;
                }
                if (nvml_.DeviceGetDecoderUtilization) {
                    unsigned int su = 0, sa = 0;
                    if (nvml_.DeviceGetDecoderUtilization(dev, &su, &sa) == 0) decUtil_ = (float)su;
                }
            }
        }
    }

    // ---- NVAPI 利用率域与回退通道（温度/频率/风扇）
    if (nvapi_.loaded && nvapi_.gpuCount > 0) {
        int idx = (nvmlIndex_ >= 0) ? std::min<int>(nvmlIndex_, (int)nvapi_.gpuCount - 1) : 0;
        NvPhysicalGpuHandle g = nvapi_.gpus[idx];
        if (g) {
            out.gpuVendor = 1;
            if (out.gpuName[0] == 0 && nvapi_.GPU_GetFullName) {
                char nm[NVAPI_SHORT_STRING_MAX]{};
                if (nvapi_.GPU_GetFullName(g, nm) == 0) {
                    NPCopyStr(out.gpuName, NP_NAME_LEN, nm);
                    out.sources |= NP_SRC_NVAPI;
                }
            }
            if (out.gpuTemp < -200.0f && nvapi_.GPU_GetThermalSettings) {
                NV_GPU_THERMAL_SETTINGS_V2 th{};
                th.version = NV_GPU_THERMAL_SETTINGS_VER_2;
                if (nvapi_.GPU_GetThermalSettings(g, 0, &th) == 0 && th.count > 0) {
                    out.gpuTemp = (float)th.sensor[0].currentTemp;
                    out.sources |= NP_SRC_NVAPI;
                }
            }
            if (nvapi_.GPU_GetDynamicPstatesInfoEx) {
                NV_GPU_DYNAMIC_PSTATES_INFO_EX ps{};
                ps.version = NV_GPU_DYNAMIC_PSTATES_INFO_EX_VER_1;
                if (nvapi_.GPU_GetDynamicPstatesInfoEx(g, &ps) == 0) {
                    auto dom = [&](int i) -> float {
                        if (i >= NVAPI_MAX_GPU_UTILIZATIONS) return -1.0f;
                        if (!(ps.utilization[i].presentFlags & 1u)) return -1.0f;
                        float p = (float)ps.utilization[i].percentage;
                        return (p >= 0.0f && p <= 100.0f) ? p : -1.0f;
                    };
                    out.domGpu = dom(NP_NVAPI_DOM_GPU);
                    out.domFb = dom(NP_NVAPI_DOM_FB);
                    out.domVid = dom(NP_NVAPI_DOM_VID);
                    out.domBus = dom(NP_NVAPI_DOM_BUS);
                    // 未公开域 4..7 —— 一直探测，若厂商将来填入即可直接呈现
                    for (int i = 4; i < NVAPI_MAX_GPU_UTILIZATIONS; ++i) {
                        float d = dom(i);
                        out.domExt[i - 4] = d;
                        if (d >= 0.0f) {
                            out.domExtPresent |= (1u << (i - 4));
                            nvapiExtSeen_ |= (1 << (i - 4));
                        }
                    }
                    out.sources |= NP_SRC_NVAPI;
                    if (out.gpuUsage < 0 && out.domGpu >= 0) out.gpuUsage = out.domGpu;
                }
            }
            if (out.gpuFanPct < 0 && nvapi_.GPU_GetTachReading) {
                NvU32 rpm = 0;
                if (nvapi_.GPU_GetTachReading(g, &rpm) == 0 && rpm > 0) {
                    out.gpuFanRpm = (float)rpm;
                    out.sources |= NP_SRC_NVAPI;
                }
            }

            // ---- 频率回退：NVAPI 直读（NvAPI_GPU_GetAllClockFrequencies 0xDCB616C3）
            if (out.gpuClock <= 0.0f || out.memClock <= 0.0f) {
                double cMhz = 0, mMhz = 0;
                if (NvapiReadGpuClocks(nvapi_, g, &cMhz, &mMhz)) {
                    if (out.gpuClock <= 0.0f && cMhz > 0) out.gpuClock = (float)cMhz;
                    if (out.memClock <= 0.0f && mMhz > 0) out.memClock = (float)mMhz;
                    out.sources |= NP_SRC_NVAPI;
                }
            }
        }
    }

    // NVAPI 未提供视频域时回退使用 NVML 编解码占用率的最大值
    if (out.domVid < 0.0f && (encUtil_ >= 0.0f || decUtil_ >= 0.0f)) {
        out.domVid = std::max(encUtil_, decUtil_);
    }
}

// ================================================================== AMD
void SensorHub::PollGpuAmd(NPSensors& out) {
    if (!adl_.loaded || adl_.adapterCount <= 0) return;
    out.gpuVendor = 2;

    // 挑一个活动量最高的适配器
    if (adl_.activeIndex < 0 || (pollCount_ % 32) == 0) {
        int best = 0, bestAct = -1;
        for (int i = 0; i < adl_.adapterCount; ++i) {
            NPADLPMActivity act{};
            act.iSize = sizeof(act);
            if (adl_.Overdrive5_CurrentActivity_Get(i, &act) == NP_ADL_OK && act.iActivityPercent > bestAct) {
                bestAct = act.iActivityPercent;
                best = i;
            }
        }
        adl_.activeIndex = best;
    }

    NPADLPMActivity act{};
    act.iSize = sizeof(act);
    if (adl_.Overdrive5_CurrentActivity_Get(adl_.activeIndex, &act) == NP_ADL_OK) {
        out.gpuUsage = (float)act.iActivityPercent;
        if (act.iEngineClock > 0) out.gpuClock = (float)(act.iEngineClock / 100.0);  // 10kHz -> MHz
        if (act.iMemoryClock > 0) out.memClock = (float)(act.iMemoryClock / 100.0);
        out.sources |= NP_SRC_ADL;
    }
    NPADLTemperature temp{};
    temp.iSize = sizeof(temp);
    if (adl_.Overdrive5_Temperature_Get && adl_.Overdrive5_Temperature_Get(adl_.activeIndex, 0, &temp) == NP_ADL_OK)
        out.gpuTemp = (float)(temp.iTemperature / 1000.0);
}

// ================================================================== 通用兜底
void SensorHub::PollGpuGeneric(NPSensors& out) {
    if (out.gpuUsage < 0) {
        double best = 0;
        if (pdh_.MaxStarCounter(L"\\GPU Engine(*)\\Utilization Percentage", L"", L"", &best)) {
            out.gpuUsage = (float)std::clamp(best, 0.0, 100.0);
            out.sources |= NP_SRC_PDH;
        }
    }
    if (out.vramUsedGB < 0) {
        double bytes = 0;
        if (pdh_.MaxStarCounter(L"\\GPU Adapter Memory(*)\\Dedicated Usage", L"", L"", &bytes) && bytes > 0) {
            out.vramUsedGB = (float)(bytes / (1024.0 * 1024.0 * 1024.0));
            if (out.vramTotalGB > 0)
                out.vramPct = std::clamp(out.vramUsedGB / out.vramTotalGB * 100.0f, 0.0f, 100.0f);
            out.sources |= NP_SRC_PDH;
        }
    }
}

void SensorHub::PollHwinfoExtras(NPSensors& out) {
    if (!hwinfo_.loaded) return;
    double v = 0;
    if (out.gpuHotspot < -200.0f && (hwinfo_.Find(L"GPU", L"Hot Spot", &v) ||
                                     hwinfo_.FindAny(L"GPU Hot Spot", &v))) {
        out.gpuHotspot = (float)v;
        out.sources |= NP_SRC_HWINFO;
    }
    if (out.gpuMemTemp < -200.0f && (hwinfo_.Find(L"GPU", L"Memory Junction", &v) ||
                                     hwinfo_.FindAny(L"Memory Junction", &v))) {
        out.gpuMemTemp = (float)v;
        out.sources |= NP_SRC_HWINFO;
    }
    if (out.gpuPower < 0 && (hwinfo_.Find(L"GPU", L"Power", &v) || hwinfo_.FindAny(L"GPU Power", &v))) {
        out.gpuPower = (float)v;
        out.sources |= NP_SRC_HWINFO;
    }
    if (out.gpuFanPct < 0 && hwinfo_.FindAny(L"GPU Fan", &v)) {
        out.gpuFanPct = (float)v;
        out.sources |= NP_SRC_HWINFO;
    }
    if (out.vramUsedGB < 0 && hwinfo_.FindAny(L"GPU Memory Used", &v)) {
        out.vramUsedGB = (float)(v / 1024.0);  // MB -> GB
        out.sources |= NP_SRC_HWINFO;
    }
}

void SensorHub::ProbeRtTensorHardware(NPSensors& out) {
    double v = 0;
    if (hwinfo_.loaded) {
        if (hwinfo_.FindAny(L"Tensor", &v)) {
            out.hwTensorPct = (float)v;
            out.hwRtTensorSrc |= NP_SRC_HWINFO;
            out.sources |= NP_SRC_HWINFO;
        }
        if (hwinfo_.FindAny(L"RT Core", &v)) {
            out.hwRtPct = (float)v;
            out.hwRtTensorSrc |= NP_SRC_HWINFO;
            out.sources |= NP_SRC_HWINFO;
        }
    }
    if (comInited_ && (out.hwRtPct < 0 || out.hwTensorPct < 0)) {
        EnsureLhmPolled(GetTickCount64());
        if (out.hwTensorPct < 0 && cachedLhmTensor_ >= 0.0f) {
            out.hwTensorPct = cachedLhmTensor_;
            out.hwRtTensorSrc |= NP_SRC_LHM;
            out.sources |= NP_SRC_LHM;
        }
        if (out.hwRtPct < 0 && cachedLhmRt_ >= 0.0f) {
            out.hwRtPct = cachedLhmRt_;
            out.hwRtTensorSrc |= NP_SRC_LHM;
            out.sources |= NP_SRC_LHM;
        }
    }
}

// ================================================================== 主轮询
void SensorHub::Poll(NPSensors& out) {
    NPClearSensors(&out);
    out.valid = 1;
    uint64_t nowMs = GetTickCount64();
    out.tickMs = nowMs;
    ++pollCount_;

    // 统一在每轮 Poll 开头刷新一次 PDH 与 HWiNFO（避免同轮重复采样破坏速率计数器）
    if (pdh_.Collect()) lastPdhCollectMs_ = nowMs;
    if (!hwinfo_.loaded && nowMs >= hwinfoNextTryMs_) {
        hwinfoNextTryMs_ = nowMs + 10000;
        if (hwinfo_.Open()) {
            available_ |= NP_SRC_HWINFO;
            hwinfoFailStreak_ = 0;
            UpdateSourceUsed(NP_SRC_HWINFO, true, true, "温度/功耗/热点/主板传感器");
        }
    } else if (hwinfo_.loaded) {
        if (hwinfo_.Refresh()) {
            hwinfoFailStreak_ = 0;
            available_ |= NP_SRC_HWINFO;
        } else if (++hwinfoFailStreak_ >= 5) {
            hwinfo_.Close();
            hwinfoFailStreak_ = 0;
            available_ &= ~NP_SRC_HWINFO;
            hwinfoNextTryMs_ = nowMs + 10000;
            UpdateSourceUsed(NP_SRC_HWINFO, false, false, "共享内存已断开，自动重连中");
        }
    }

    PollCpu(out);
    PollRam(out);

    bool nvidia = (out.gpuVendor == 1) || ((out.gpuVendor != 2) && (nvml_.loaded || (nvapi_.loaded && nvapi_.gpuCount > 0)));
    bool amd = !nvidia && ((out.gpuVendor == 2) || adl_.loaded);

    if (nvidia) PollGpuNvidia(out);
    else if (amd) PollGpuAmd(out);

    PollGpuGeneric(out);
    PollHwinfoExtras(out);
    ProbeRtTensorHardware(out);

    uint32_t activeSrc = out.sources;
    UpdateSourceUsed(NP_SRC_NVML, nvml_.loaded, (activeSrc & NP_SRC_NVML) != 0);
    UpdateSourceUsed(NP_SRC_NVAPI, nvapi_.loaded, (activeSrc & NP_SRC_NVAPI) != 0);
    UpdateSourceUsed(NP_SRC_ADL, adl_.loaded, (activeSrc & NP_SRC_ADL) != 0);
    UpdateSourceUsed(NP_SRC_HWINFO, hwinfo_.loaded, (activeSrc & NP_SRC_HWINFO) != 0);
    UpdateSourceUsed(NP_SRC_LHM, lhmPresent_, (activeSrc & NP_SRC_LHM) != 0);
    UpdateSourceUsed(NP_SRC_PDH, pdh_.Count() > 0, (activeSrc & NP_SRC_PDH) != 0 || out.cpuClock > 0.0f);
    UpdateSourceUsed(NP_SRC_WMI, comInited_, (activeSrc & NP_SRC_WMI) != 0);

    // 数据源文字说明
    std::string gpuSrc;
    if (activeSrc & NP_SRC_NVML) gpuSrc = "NVML";
    else if (activeSrc & NP_SRC_ADL) gpuSrc = "ADL";
    else if (activeSrc & NP_SRC_NVAPI) gpuSrc = "NVAPI";
    else if (activeSrc & NP_SRC_PDH) gpuSrc = "PDH";
    else if (activeSrc & NP_SRC_HWINFO) gpuSrc = "HWiNFO";

    std::string txt = "GPU:" + (gpuSrc.empty() ? std::string("无") : gpuSrc);
    txt += " | CPU:系统计时器";
    if (activeSrc & NP_SRC_HWINFO) txt += " | HWiNFO";
    else if (activeSrc & NP_SRC_LHM) txt += " | LHM";
    else if (activeSrc & NP_SRC_WMI) txt += " | ACPI";
    if (nvapi_.loaded) txt += " | 域:NVAPI";
    if (nvapiExtSeen_) txt += " | 扩展域:有";
    out.sources = available_ | activeSrc;
    NPCopyStr(out.sourceText, NP_NAME_LEN, txt.c_str());
}

}  // namespace np
