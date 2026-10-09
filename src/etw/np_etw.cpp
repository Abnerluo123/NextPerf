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

void __stdcall EtwMonitor::OnEvent(PEVENT_RECORD ev) {
    EtwMonitor& m = Etw();
    if (!m.active_ || !ev) return;
    // ★ 没有目标进程时**一条都不算**。targetPid_ == 0 以前表示「全都算」，
    //   而主程序在还没注入任何游戏时正是这个状态 —— 于是全系统（桌面、浏览器、
    //   播放器）的 present 事件都被算进来，Low 帧被污染成别的程序的数字。
    if (!m.targetPid_ || ev->EventHeader.ProcessId != m.targetPid_) return;

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

void EtwMonitor::OnPresent(uint64_t qpc) {
    EnterCriticalSection(&lock_);
    ++presentN_;
    if (lastQpc_ && qpc > lastQpc_) {
        double ms = (double)(qpc - lastQpc_) / 10000.0;   // 100ns → ms
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
    if (!winStart_) winStart_ = qpc;
    ++winN_;
    if (qpc > winStart_ && qpc - winStart_ >= 10000000ull) {   // 1 秒 = 1e7 * 100ns
        fps_ = (float)((double)winN_ * 10000000.0 / (double)(qpc - winStart_));
        winN_ = 0;
        winStart_ = qpc;
    }
    LeaveCriticalSection(&lock_);
}

void EtwMonitor::RecalcLow() {
    // 用户要的口径：「记录一段时间内所有帧数据，排序，取最慢的 1% 求平均」。
    // 之前 Low 不准不是因为算法，而是因为**喂进去的样本是脏的**（钩子量到的
    // 是两次真 Present 调用的间隔，而这游戏每帧调两次 Present，相位一直在变，
    // 单样本在 9.5~23.8ms 之间抖）。ETW 的事件在真正 flip 处触发，没有这个问题。
    if (ringN_ < 32) return;
    // ★ 限流：Snapshot() 每 120ms 会被主线程调 3 次，每次都全排序 2048 个样本。
    //   排序本身不贵，但它是在**持锁**状态下做的 —— 而持锁的另一边是 ETW 消费者
    //   线程（OnPresent），一卡就会丢事件。5Hz 对这个滚动统计足够。
    uint64_t now = GetTickCount64();
    if (lowCalcAt_ && now - lowCalcAt_ < 200) return;
    lowCalcAt_ = now;
    std::vector<float> v(ring_, ring_ + ringN_);
    std::sort(v.begin(), v.end());          // 升序
    auto avgSlowest = [&](double pct) {
        int k = (int)(v.size() * pct / 100.0);
        if (k < 1) k = 1;
        double s = 0;
        for (int i = 0; i < k; ++i) s += v[v.size() - 1 - i];   // 取最慢的 k 帧
        return (float)(s / k);
    };
    float m1 = avgSlowest(1.0), m01 = avgSlowest(0.1);
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

    // 只订阅 Present 关键字，级别 Information
    ULONG en = EnableTraceEx2(sh, &kDxgKrnl, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                              TRACE_LEVEL_INFORMATION, kKwPresent, 0, 0, nullptr);
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
