#include "np_etw.h"

#include <evntrace.h>
#include <evntcons.h>
#include <tdh.h>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace np {

// Microsoft-Windows-DxgKrnl {802EC45A-1E99-4B83-9920-87C98277BA9D}
static const GUID kDxgKrnl = {
    0x802ec45a, 0x1e99, 0x4b83, {0x99, 0x20, 0x87, 0xc9, 0x82, 0x77, 0xba, 0x9d}};

// ★ 事件 ID 过滤所需的两样东西（官方定义在 evntrace.h，但**只在 NTDDI_WIN8 以上**才声明，
//   而我们的交叉编译目标版本低于它 → 头文件里没有，于是自己补上，值取自 Windows SDK）。
//   见 MS Learn「EnableTraceEx2 function (evntrace.h)」的 EVENT_FILTER_DESCRIPTOR 说明。
#ifndef EVENT_FILTER_TYPE_EVENT_ID
#define EVENT_FILTER_TYPE_EVENT_ID 0x80000200
typedef struct _NP_EVENT_FILTER_EVENT_ID {
    BOOLEAN FilterIn;      // TRUE = 只保留列出的这些 ID；FALSE = 排除
    UCHAR   Reserved;
    USHORT  Count;
    USHORT  Events[1];     // 必须按**升序**排列（实现内部二分查找）
} NP_EVENT_FILTER_EVENT_ID;
#else
typedef EVENT_FILTER_EVENT_ID NP_EVENT_FILTER_EVENT_ID;
#endif
// 关键字 Present = 0x0000000008000000（本机 logman query providers 实测）
// Present = 0x8000000，Base = 0x1。
// 只启用 Present 时实测抓到的是 PresentQueuePacket / PresentHistory 这类
// **记账事件**（每帧两次），真正的 Present_Start 反而没进来 —— 它在 Base 下面。
static const ULONGLONG kKwPresent = 0x0000000008000000ull | 0x1ull;

static const wchar_t* kSessionName = L"NextPerfFrameTrace";

EtwMonitor& Etw() {
    static EtwMonitor inst;
    return inst;
}

EtwMonitor::EtwMonitor() {
    InitializeCriticalSection(&lock_);
    lockInit_ = true;
}

EtwMonitor::~EtwMonitor() {
    Stop();
    if (lockInit_) { DeleteCriticalSection(&lock_); lockInit_ = false; }
}

void EtwMonitor::SetStatus(const char* s) {
    EnterCriticalSection(&lock_);
    status_ = s ? s : "";
    LeaveCriticalSection(&lock_);
}

std::string EtwMonitor::status() const {
    auto* self = const_cast<EtwMonitor*>(this);
    EnterCriticalSection(&self->lock_);
    std::string s = status_;
    LeaveCriticalSection(&self->lock_);
    return s;
}

// --------------------------------------------------------------------------
// 事件判定：这条事件是不是「一次真正的 present 开始」。
//
// 不硬编码事件 ID / 结构体 —— 各 Windows 版本的事件版本不一样，
// PresentMon 在这上面栽过很多次。改成用 TDH 动态取事件名再做匹配。
//
// ⚠ nameOut 由**调用方**提供缓冲区：TDH 取到的名字指向 info 缓冲区，
//   那个缓冲区是这个函数的局部 std::vector，函数一返回就没了。
//   原来直接把指针传回调用方，OnEvent 随后拿它去 wcscpy / wcscmp ——
//   读的是已经析构的堆内存（use-after-free，堆一复用就会写坏别人的数据）。
// --------------------------------------------------------------------------
static int ClassifyEvent(PEVENT_RECORD ev, uint8_t* opOut, wchar_t* nameOut, size_t nameCap) {
    *opOut = ev->EventHeader.EventDescriptor.Opcode;
    if (nameOut && nameCap) nameOut[0] = 0;
    // ★★ 权威判定（2026-10-11）：按**清单里的事件 ID**，不再靠名字猜。
    //   来源：PresentMon 的 `PresentData/ETW/Microsoft_Windows_DxgKrnl.h`（由 `collect_etw_info.cmd`
    //   从 provider 清单生成）：
    //     EVENT_DESCRIPTOR_DECL(PresentHistory_Info , 0x00ac, ... opcode 0x00 ...);
    //     EVENT_DESCRIPTOR_DECL(PresentHistory_Start, 0x00ab, ... opcode 0x01 ...);
    //     EVENT_DESCRIPTOR_DECL(Present_Info       , 0x00b8, ... opcode 0x00 ...);
    //   → 171(0xab) = PresentHistory_Start（应用把一帧交给 DWM，每帧一次）
    //     172(0xac) = PresentHistory_Info （伴随信息，不是"一帧的开始"）
    //     184(0xb8) = Present_Info        （DxgKrnl 的 Present 事件）
    //   实测日志证实：id=171 op=1 的名字正是 [PresentHistory]，而我们的名字启发式
    //   （要求 "Present" 后跟 0 或 '_'）把它拒了 —— 名字猜测这条路就是不可靠的。
    const uint16_t id = ev->EventHeader.EventDescriptor.Id;
    if (id == 171 || id == 184) return 1;
    if (id == 172) return 0;
    DWORD need = 0;
    ULONG st = TdhGetEventInformation(ev, 0, nullptr, nullptr, &need);
    if (st != ERROR_INSUFFICIENT_BUFFER || need == 0) return -1;
    std::vector<uint8_t> buf(need);
    auto* info = reinterpret_cast<TRACE_EVENT_INFO*>(buf.data());
    if (TdhGetEventInformation(ev, 0, nullptr, info, &need) != ERROR_SUCCESS) return -1;

    // TRACE_EVENT_INFO 里**没有** EventNameOffset（我一开始就是这么写错的），
    // 事件名要靠 TaskName + OpcodeName 组合。
    const wchar_t* task = nullptr, *opname = nullptr;
    if (info->TaskNameOffset) task = (const wchar_t*)(buf.data() + info->TaskNameOffset);
    if (info->OpcodeNameOffset) opname = (const wchar_t*)(buf.data() + info->OpcodeNameOffset);
    const wchar_t* n = task ? task : opname;
    if (!n) return -1;
    if (nameOut && nameCap) {
        wcsncpy(nameOut, n, nameCap - 1);
        nameOut[nameCap - 1] = 0;
    }

    // ★ 名字必须是 "Present" 本体，或者以 "Present_" 开头（OpcodeName 兜底时是
    //   "Present_Start" 这种写法）。**不能**用 wcsstr 做子串匹配 —— 那样
    //   PresentHistory / PresentQueuePacket / PresentMultiPlaneOverlay 这些
    //   记账事件全都会被算成一次 present（每帧两次 → 帧率翻倍成 120）。
    //   真正的 Present_Start 在 Base(0x1) 关键字下，就是 Task == "Present" + opcode Start(1)。
    bool isPresent = (wcsncmp(n, L"Present", 7) == 0 || wcsncmp(n, L"present", 7) == 0) &&
                     (n[7] == 0 || n[7] == L'_');
    if (!isPresent) return 0;
    return (*opOut == 1) ? 1 : 0;
}

std::string EtwMonitor::diagText() const {
    char buf[700];
    int off = snprintf(buf, sizeof(buf),
                       "收到事件 %u（头部 pid 命中 %u / 不命中 %u）| 目标进程 Present 家族计数: "
                       "171(PresentHistory_Start)=%u 172(Info)=%u 184(Present_Info)=%u | ID 直方图: ",
                       diagRecv_, diagPidMatch_, diagOtherPid_, diagId171_, diagId172_, diagId184_);
    for (int i = 0; i < diagSeenN_ && off > 0 && off < (int)sizeof(buf) - 90; ++i) {
        char nm[128] = {0};
        if (diagSeen_[i].nm[0])
            WideCharToMultiByte(CP_UTF8, 0, diagSeen_[i].nm, -1, nm, sizeof(nm) - 1, nullptr, nullptr);
        off += snprintf(buf + off, sizeof(buf) - (size_t)off, "%sid=%u op=%u pid=%u x%u%s%s",
                        i ? " | " : "", (unsigned)diagSeen_[i].id, (unsigned)diagSeen_[i].op,
                        (unsigned)diagSeen_[i].pid, diagSeen_[i].n,
                        nm[0] ? " [" : "", nm[0] ? nm : "");
        if (nm[0] && off < (int)sizeof(buf) - 2) off += snprintf(buf + off, sizeof(buf) - (size_t)off, "]");
    }
    return std::string(buf);
}

void __stdcall EtwMonitor::OnEvent(PEVENT_RECORD ev) {
    EtwMonitor& m = Etw();
    if (!m.active_ || !ev) return;
    // ---- 诊断：在**任何过滤之前**先统计 + 记 ID 直方图（带事件名）（2026-10-11）
    {
        ++m.diagRecv_;
        if (m.targetPid_ && ev->EventHeader.ProcessId == m.targetPid_) ++m.diagPidMatch_;
        else ++m.diagOtherPid_;
        const auto& dd = ev->EventHeader.EventDescriptor;
        if (m.targetPid_ && ev->EventHeader.ProcessId == m.targetPid_) {
            if (dd.Id == 171) ++m.diagId171_;
            else if (dd.Id == 172) ++m.diagId172_;
            else if (dd.Id == 184) ++m.diagId184_;
        }
        int slot = -1;
        for (int i = 0; i < m.diagSeenN_; ++i) {
            if (m.diagSeen_[i].id == dd.Id && m.diagSeen_[i].op == dd.Opcode) { slot = i; break; }
        }
        if (slot < 0 && m.diagSeenN_ < 12) {
            slot = m.diagSeenN_++;
            m.diagSeen_[slot] = {dd.Id, dd.Opcode, ev->EventHeader.ProcessId, 0, {}};
            // 取一次事件名（TDH）。注意 ClassifyEvent 会把 task/opcode 名写进 nm。
            uint8_t opTmp = 0;
            ClassifyEvent(ev, &opTmp, m.diagSeen_[slot].nm, 40);
        }
        if (slot >= 0) ++m.diagSeen_[slot].n;
    }
    // ★ 没有目标进程时**一条都不算**。targetPid_ == 0 以前表示「全都算」，
    //   而主程序在还没注入任何游戏时正是这个状态 —— 于是全系统（桌面、浏览器、
    //   播放器）的 present 事件都被算进来，Low 帧被污染成别的程序的数字。
    if (!m.targetPid_ || ev->EventHeader.ProcessId != m.targetPid_) return;

    // ★ 审计修正 1（2026-10-11）：**事件优先级**。不能 171/184 都算 ——
    //   若某游戏每帧两个事件都发，就会每帧计两次（帧率翻倍 / 帧时间减半 / Low 全错）。
    //   171(PresentHistory_Start) 是"应用把一帧交给 DWM"，语义最准；只有该进程
    //   从不产生 171 时才退回 184(Present_Info)。
    //   （Spider-Man 实测：171=0、184=299/5s，正是走 184 的情形。）
    if (ev->EventHeader.EventDescriptor.Id == 171) m.saw171_ = true;
    else if (ev->EventHeader.EventDescriptor.Id == 184 && m.saw171_) return;

    uint8_t op = 0;
    wchar_t evName[96] = {0};
    const auto& d = ev->EventHeader.EventDescriptor;
    int verdict = -2;
    for (int i = 0; i < m.cacheN_; ++i) {
        if (m.cachedKey_[i].id == d.Id && m.cachedKey_[i].ver == d.Version &&
            m.cachedKey_[i].op == d.Opcode) {
            verdict = m.cachedVal_[i];
            break;
        }
    }
    if (verdict == -2) {
        verdict = ClassifyEvent(ev, &op, evName, 96);
        if (verdict >= 0 && m.cacheN_ < 8) {
            m.cachedKey_[m.cacheN_] = {d.Id, d.Version, d.Opcode};
            m.cachedVal_[m.cacheN_] = (int8_t)verdict;
            ++m.cacheN_;
        }
    }
    if (verdict != 1) return;

    // 诊断：把这个事件的名字记下来（去重，最多 6 个）。
    // 实测每帧匹配两次（120fps vs 真相 60fps），必须知道第二个是谁。
    // （命中缓存的那次不会重新取名，所以这里用 evName[0] 判空。）
    if (evName[0] && m.evNameN_ < 6) {
        bool dup = false;
        for (int i = 0; i < m.evNameN_; ++i)
            if (wcscmp(m.evNames_[i], evName) == 0) { dup = true; break; }
        if (!dup) {
            wcsncpy(m.evNames_[m.evNameN_], evName, 95);
            m.evNames_[m.evNameN_][95] = 0;
            ++m.evNameN_;
        }
    }

    // EVENT_HEADER.TimeStamp 已经是 100ns 单位的 QPC 对齐时间戳，
    // 和 QueryPerformanceCounter 同源，可以直接比较差值。
    m.OnPresent(static_cast<uint64_t>(ev->EventHeader.TimeStamp.QuadPart));
}

ULONG __stdcall EtwMonitor::OnBuffer(PEVENT_TRACE_LOGFILEW) { return Etw().stop_ ? FALSE : TRUE; }

// ★ 审计修正 2（2026-10-11）：时间戳换算**必须用 QueryPerformanceFrequency**。
//   `EVENT_TRACE_PROPERTIES.Wnode.ClientContext = 1` 时，事件时间戳是 **QPC 刻度**，
//   而 QPC 频率**不保证是 10MHz**（本机恰好是，所以之前"看着对"）。
//   原来写死 `/10000.0`（= 假定 100ns 一个刻度）→ 换台频率不同的机器，
//   所有 ETW 帧时间都会按比例错。
static double QpcToMs() {
    static double k = 0.0;
    if (k == 0.0) {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        k = f.QuadPart ? (1000.0 / (double)f.QuadPart) : 0.0001;   // 兜底：按 10MHz
    }
    return k;
}

void EtwMonitor::OnPresent(uint64_t qpc) {
    EnterCriticalSection(&lock_);
    ++presentN_;
    if (lastQpc_ && qpc > lastQpc_) {
        double ms = (double)(qpc - lastQpc_) * QpcToMs();
        // 和钩子那边同样的道理：超过 1 秒的间隔不是「一帧」，是切出去/加载
        if (ms > 0.02 && ms < 1000.0) {
            frameMs_ = (float)ms;
            ring_[ringW_] = (float)ms;
            ringW_ = (ringW_ + 1) % kCap;
            if (ringN_ < kCap) ++ringN_;
        }
    }
    lastQpc_ = qpc;

    // 1 秒窗口计数 → 帧率（锁 60 时就是稳定 60.0，不做逐帧换算）
    const uint64_t oneSec = (uint64_t)(1000.0 / QpcToMs());
    if (!winStart_) winStart_ = qpc;
    ++winN_;
    if (qpc > winStart_ && qpc - winStart_ >= oneSec) {
        fps_ = (float)((double)winN_ * (double)oneSec / (double)(qpc - winStart_));
        winN_ = 0;
        winStart_ = qpc;
    }
    LeaveCriticalSection(&lock_);
}

void EtwMonitor::RecalcLow() {
    // ★ 口径 = **Intel PresentMon 的动态查询**（2026-10-11 复核其源码后定的）：
    //   * 窗口：**滑动 1000ms**（`--window-size` 默认值），每个轮询重算
    //     （PresentMonMiddleware/DynamicQuery.cpp: newest = now − offset; oldest = newest − size）
    //   * 统计：在窗口内的样本上**直接取百分位**（**没有**"先求窗口平均"这一步）：
    //       h = p×(N−1)，i = floor(h)，g = h − i，q = x[i] + g·(x[i+1] − x[i])
    //     （Core/source/pmon/StatisticsTracker.cpp）
    //   * 倒数指标排名反转（PresentMonMiddleware/DynamicStat.cpp:323）：
    //       FPS 的 P1 ≡ 帧时间的 P99 → 1% Low = 1000 / P99(帧时间)
    //   ⚠ 旧实现是"2048 个样本（≈34 秒）里取最慢 1% 求平均" —— 不是 PresentMon 的口径，
    //     而且 34 秒回看会让一次卡顿钉住很久（实测 0.1%Low 掉到 20.5 后长时间不恢复）。
    if (ringN_ < 32) return;
    // 限流：Snapshot() 每 120ms 被调 3 次，而这里在**持锁**状态下排序 ——
    // 持锁另一边是 ETW 消费者线程（OnPresent），一卡就丢事件。5Hz 够用。
    uint64_t now = GetTickCount64();
    if (lowCalcAt_ && now - lowCalcAt_ < 200) return;
    lowCalcAt_ = now;

    // 窗口 = 帧数近似。**两行用不同窗口**（这是有意的，理由见下）：
    //   1%   Low：PresentMon 的默认窗口 1000ms（≈60 个样本）
    //   0.1% Low：**10 秒**（≈600 个样本）
    // ⚠ 为什么 0.1% 不能也用 1 秒：60 个样本下 P99 = 第 58.41 个、P99.9 = 第 58.94 个，
    //   两者插值用的就是**同样那两个最差样本** → 两行必然几乎相同（用户反复报过这个现象）。
    //   600 个样本下 P99 ≈ 第 6 差、P99.9 ≈ 最差那一帧 → 两行才真正不同。
    //   （PresentMon 的动态查询只有 1% 档、窗口也是可配的 --window-size，这里按同一套
    //     "窗口内取百分位" 的定义、只把 0.1% 的窗口放长，不引入新统计量。）
    auto percentileOverLast = [&](int n, double p) -> float {
        if (n < 20) n = 20;
        if (n > ringN_) n = ringN_;
        std::vector<float> v;
        v.reserve((size_t)n);
        for (int i = 0; i < n; ++i) {
            int idx = (int)((ringW_ + kCap - (size_t)n + i) % kCap);   // 按写入顺序取最近 n 个
            v.push_back(ring_[idx]);
        }
        std::sort(v.begin(), v.end());          // 升序
        if (v.empty()) return 0.0f;
        if (v.size() == 1) return v[0];
        double h = p * (double)(v.size() - 1);  // 与 PresentMon 逐字一致的线性插值
        size_t lo = (size_t)h;
        size_t hi = (lo + 1 < v.size()) ? lo + 1 : lo;
        double g = h - (double)lo;
        return (float)(v[lo] * (1.0 - g) + v[hi] * g);
    };
    int n1s = (int)(fps_ > 1.0f ? fps_ : 60.0f);
    int n10s = n1s * 10;
    float m1 = percentileOverLast(n1s, 0.99);
    float m01 = percentileOverLast(n10s, 0.999);
    low1_ = m1 > 0.0001f ? 1000.0f / m1 : 0.0f;
    low01_ = m01 > 0.0001f ? 1000.0f / m01 : 0.0f;
}

// --------------------------------------------------------------------------
bool EtwMonitor::Start() {
    if (active_) return true;
    // 锁在构造函数里就初始化好了（见 np_etw.h），这里不再懒初始化。

    const size_t nameLen = (wcslen(kSessionName) + 1) * sizeof(wchar_t);
    const size_t propsLen = sizeof(EVENT_TRACE_PROPERTIES) + nameLen;
    std::vector<uint8_t> buf(propsLen, 0);
    auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buf.data());
    props->Wnode.BufferSize = (ULONG)propsLen;
    props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    props->Wnode.ClientContext = 1;          // 1 = QPC 时间戳（我们要的就是它）
    props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    props->BufferSize = 64;
    props->MinimumBuffers = 4;
    props->MaximumBuffers = 24;
    props->FlushTimer = 1;                   // 秒级刷新，别让事件在缓冲里躺着
    memcpy(buf.data() + props->LoggerNameOffset, kSessionName, nameLen);

    TRACEHANDLE sh = 0;
    // 同名会话可能残留在上一次异常退出。先停掉再建。
    ULONG st = StartTraceW(&sh, kSessionName, props);
    if (st == ERROR_ALREADY_EXISTS) {
        ControlTraceW(0, kSessionName, props, EVENT_TRACE_CONTROL_STOP);
        st = StartTraceW(&sh, kSessionName, props);
    }
    if (st != ERROR_SUCCESS) {
        char msg[256];
        if (st == ERROR_ACCESS_DENIED)
            snprintf(msg, sizeof(msg),
                     "创建 ETW 会话被拒（需要管理员运行 NextPerf）");
        else
            snprintf(msg, sizeof(msg), "StartTrace 失败，Win32 错误 %lu", (unsigned long)st);
        SetStatus(msg);
        return false;
    }
    session_ = sh;

    // 只订阅 Present 关键字，级别 Information。
    //
    // ★ 再加一层**会话级事件 ID 过滤**（2026-10-11）：关键字 0x8000001(Present|Base) 是
    //   DxgKrnl 里一大批事件共用的（实测 5 秒收到 **1140 万条**，其中 GetDeviceState 一条就
    //   1200 万/5 秒）—— 这些事件要跨内核→用户态复制，是**游戏运行时真占开销**的。
    //   官方机制：EnableTraceEx2 的第 8 个参数传 EVENT_FILTER_DESCRIPTOR，
    //   类型 EVENT_FILTER_TYPE_EVENT_ID，数据是 EVENT_FILTER_EVENT_ID{FilterIn, Reserved,
    //   Count, Events[]}。见 MS Learn「EnableTraceEx2 function (evntrace.h)」。
    //   ⚠ 文档要求 Events[] **按升序排列**（内部二分查找）：171 < 172 < 184 ✓
    //
    //   ⚠ 传法要看**真实原型**（mingw 头文件 evntrace.h 里是 SDK 10 的版本）：
    //       EnableTraceEx2(TRACEHANDLE, LPCGUID, ULONG, UCHAR, ULONGLONG, ULONGLONG,
    //                      ULONG Timeout, PENABLE_TRACE_PARAMETERS EnableParameters)
    //     过滤器描述符是放在 ENABLE_TRACE_PARAMETERS.EnableFilterDesc 里，
    //     不是第 8 个参数直接传（那是老版本 EnableTraceEx 的形式）。
    NP_EVENT_FILTER_EVENT_ID filter{};
    filter.FilterIn = TRUE;          // TRUE = 只保留列出的这些 ID
    filter.Count = 3;
    filter.Events[0] = 171;          // PresentHistory_Start（每帧一次的应用提交）
    filter.Events[1] = 172;          // PresentHistory_Info
    filter.Events[2] = 184;          // Present_Info
    EVENT_FILTER_DESCRIPTOR fd{};
    fd.Ptr = (ULONGLONG)&filter;
    fd.Size = (ULONG)(sizeof(NP_EVENT_FILTER_EVENT_ID) + (filter.Count - 1) * sizeof(USHORT));
    fd.Type = EVENT_FILTER_TYPE_EVENT_ID;
    ENABLE_TRACE_PARAMETERS params{};
    params.Version = 2;              // ENABLE_TRACE_PARAMETERS_VERSION_2（v2 起才有 FilterDescCount）
    params.EnableFilterDesc = &fd;
    params.FilterDescCount = 1;
    ULONG en = EnableTraceEx2(sh, &kDxgKrnl, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                              TRACE_LEVEL_INFORMATION, kKwPresent, 0, 0, &params);
    if (en != ERROR_SUCCESS) {
        // 过滤失败就退回不过滤（宁可多收，也别收不到）。失败原因记在 status 里，
        // 由主程序每 5 秒那行 [ETW] 打出来。
        char msg[192];
        snprintf(msg, sizeof(msg),
                 "运行中（DxgKrnl Present 事件；⚠ 事件 ID 过滤不可用，Win32 %lu → 未过滤，事件量大）",
                 (unsigned long)en);
        SetStatus(msg);
        en = EnableTraceEx2(sh, &kDxgKrnl, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                            TRACE_LEVEL_INFORMATION, kKwPresent, 0, 0, nullptr);
    }
    if (en != ERROR_SUCCESS) {
        char msg[256];
        snprintf(msg, sizeof(msg), "EnableTraceEx2(DxgKrnl) 失败，Win32 错误 %lu",
                 (unsigned long)en);
        SetStatus(msg);
        Stop();
        return false;
    }

    stop_ = false;
    active_ = true;
    thread_ = CreateThread(nullptr, 0, [](void* p) -> DWORD {
        reinterpret_cast<EtwMonitor*>(p)->ConsumeLoop();
        return 0;
    }, this, 0, nullptr);
    if (!thread_) {
        SetStatus("创建消费者线程失败");
        Stop();
        return false;
    }
    SetStatus("运行中（DxgKrnl Present 事件）");
    return true;
}

void EtwMonitor::ConsumeLoop() {
    EVENT_TRACE_LOGFILEW lf{};
    lf.LoggerName = const_cast<LPWSTR>(kSessionName);
    lf.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    lf.EventRecordCallback = &EtwMonitor::OnEvent;
    lf.BufferCallback = &EtwMonitor::OnBuffer;

    TRACEHANDLE h = OpenTraceW(&lf);
    if (h == INVALID_PROCESSTRACE_HANDLE) {
        // ⚠ 这里跑在**消费者线程**上，写 status_ 必须走加锁的 SetStatus。
        SetStatus("OpenTrace 失败");
        active_ = false;
        return;
    }
    consumer_ = h;
    ProcessTrace(&h, 1, nullptr, nullptr);   // 阻塞直到 CloseTrace
    CloseTrace(h);
    consumer_ = 0;
}

void EtwMonitor::Stop() {
    if (!active_ && !session_) return;
    stop_ = true;
    active_ = false;
    // 关掉消费者会让 ProcessTrace 返回
    if (consumer_) CloseTrace((TRACEHANDLE)consumer_);
    if (thread_) {
        WaitForSingleObject((HANDLE)thread_, 2000);
        CloseHandle((HANDLE)thread_);
        thread_ = nullptr;
    }
    if (session_) {
        const size_t nameLen = (wcslen(kSessionName) + 1) * sizeof(wchar_t);
        std::vector<uint8_t> buf(sizeof(EVENT_TRACE_PROPERTIES) + nameLen, 0);
        auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buf.data());
        props->Wnode.BufferSize = (ULONG)buf.size();
        props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        ControlTraceW((TRACEHANDLE)session_, kSessionName, props, EVENT_TRACE_CONTROL_STOP);
        session_ = 0;
    }
}

void EtwMonitor::SetTargetPid(uint32_t pid) {
    if (pid == targetPid_) return;
    EnterCriticalSection(&lock_);
    targetPid_ = pid;
    // 换目标就重建基线，否则会把两个进程的间隔算成一帧
    lastQpc_ = 0;
    winStart_ = 0;
    winN_ = 0;
    ringN_ = ringW_ = 0;
    frameMs_ = fps_ = low1_ = low01_ = 0;
    presentN_ = 0;
    LeaveCriticalSection(&lock_);
}

bool EtwMonitor::Snapshot(Result* out) const {
    if (!out) return false;
    auto* self = const_cast<EtwMonitor*>(this);
    EnterCriticalSection(&self->lock_);
    out->frameMs = frameMs_;
    out->fps = fps_;
    out->presentN = presentN_;
    out->inWindow = (uint32_t)ringN_;
    self->RecalcLow();
    out->low1Fps = low1_;
    out->low01Fps = low01_;
    out->evNameN = self->evNameN_;
    for (int i = 0; i < self->evNameN_ && i < 6; ++i)
        wcscpy(out->evNames[i], self->evNames_[i]);
    LeaveCriticalSection(&self->lock_);
    return out->presentN > 0;
}

}  // namespace np
