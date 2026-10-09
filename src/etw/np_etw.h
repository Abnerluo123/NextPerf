// ETW 帧计时 —— 走 PresentMon 那条路：**不注入游戏**，直接订阅内核事件。
//
// 为什么要换成这个：
//   靠 Present 钩子推算帧时间，一路踩了太多坑（每帧两次 Present 导致翻倍、
//   多线程提交区间重叠、Reflex 改变 Present 行为、后台流式线程搅乱"第一次提交"）。
//   PresentMon / RTSS 这类成熟软件的做法是订阅 `Microsoft-Windows-DxgKrnl`
//   的 ETW 事件 —— 数据是内核报的，与锁帧、Reflex、多线程提交全都无关。
//
// 权限：创建 ETW 实时会话需要管理员。NextPerf 平时就是以管理员运行的，
//       所以这条路可行；万一不是管理员，Start() 会失败并给出原因，
//       调用方继续用钩子那套兜底，**绝不阻塞程序**。
//
// 本文件只负责「拿到逐帧的 present 时刻」这一步。事件结构的解析**不硬编码**
// （各 Windows 版本的事件版本号不一样，PresentMon 在这上面栽过），
// 而是用 TDH 动态取事件名再做匹配。

#pragma once

#include <windows.h>
#include <evntrace.h>     // PEVENT_TRACE_LOGFILEW / EVENT_TRACE_PROPERTIES
#include <evntcons.h>     // PEVENT_RECORD
#include <cstdint>
#include <string>

namespace np {

class EtwMonitor {
public:
    // ★ 构造函数里就把 CRITICAL_SECTION 初始化掉。
    //   原来是在 Start() 里懒初始化，可是 Snapshot()/SetTargetPid()/Stop() 都可能
    //   在 Start() 之前被调用（非管理员时 Start 失败；--uismoke 路径压根不调 Start，
    //   但主循环照样会 SetTargetPid）—— 在一个全零的 CRITICAL_SECTION 上
    //   EnterCriticalSection 是未定义行为，会去等一个 NULL 的信号量。
    EtwMonitor();
    ~EtwMonitor();

    // 启动采集。需要管理员；失败时 status() 里是原因。
    bool Start();
    void Stop();
    bool active() const { return active_; }

    // 指定要跟踪的游戏进程（pid 变了会自动切换）
    void SetTargetPid(uint32_t pid);

    // 取结果（线程安全）。返回 false 表示暂时还没有足够数据。
    //   frameMs  : 最近一帧的 present 间隔
    //   fps      : 最近 1 秒窗口内的帧率（窗口计数，锁 60 就是稳定 60.0）
    //   lowMs    : 最近 windowMs 窗口内最慢 1% 帧的平均帧时间换算的 FPS
    //   presentN : 累计观察到的 present 次数
    struct Result {
        float    frameMs = 0;
        float    fps = 0;
        float    low1Fps = 0;
        float    low01Fps = 0;
        uint32_t presentN = 0;
        uint32_t inWindow = 0;
        // 诊断：实际匹配上的事件名（去重，最多 6 个）。
        // 我原来靠「Task 名含 Present 且 opcode==1」判定，实测**每帧匹配到两次**
        // （120fps，真相是 60fps）—— 说明有第二种事件也被算进来了。
        // 不猜名字，把它打出来，照着改。
        wchar_t  evNames[6][96];
        int      evNameN = 0;
    };
    bool Snapshot(Result* out) const;

    // ★ 返回**拷贝**而不是引用：消费者线程也会写这句状态（OpenTrace 失败时），
    //   主线程同时读同一个 std::string 是未定义行为（SSO 缓冲/堆指针撕裂会崩）。
    std::string status() const;

private:
    static void __stdcall OnEvent(PEVENT_RECORD ev);
    static ULONG __stdcall OnBuffer(PEVENT_TRACE_LOGFILEW lf);
    void ConsumeLoop();
    void OnPresent(uint64_t qpc100ns);

    volatile bool active_ = false;
    volatile bool stop_ = false;
    uint64_t  session_ = 0;      // TRACEHANDLE
    uint64_t  consumer_ = 0;     // TRACEHANDLE (OpenTrace)
    void*     thread_ = nullptr;
    std::string status_;

    // 事件 → 是否 Present_Start 的判定缓存（避免每条事件都走一次 TDH）
    struct EvKey { uint16_t id; uint8_t ver; uint8_t op; };
    EvKey  cachedKey_[8]{};
    int8_t cachedVal_[8]{};      // 1 = 计为一次 present, 0 = 不是
    int    cacheN_ = 0;

    mutable CRITICAL_SECTION lock_{};
    bool   lockInit_ = false;

    // 诊断用：记录匹配上的事件名（去重）
    wchar_t evNames_[6][96]{};
    int     evNameN_ = 0;

    uint32_t targetPid_ = 0;
    uint64_t lastQpc_ = 0;
    uint32_t presentN_ = 0;

    // 最近 1 秒窗口（算 fps）
    uint64_t winStart_ = 0;
    uint32_t winN_ = 0;
    float    fps_ = 0;

    // 帧时间环形缓冲（算 Low 帧）
    static const int kCap = 2048;
    float    ring_[kCap]{};
    int      ringN_ = 0;
    int      ringW_ = 0;
    float    frameMs_ = 0;
    float    low1_ = 0, low01_ = 0;
    uint64_t lowCalcAt_ = 0;

    void RecalcLow();
    void SetStatus(const char* s);   // 线程安全地写 status_
};

// 全局实例（主程序持有）
EtwMonitor& Etw();

}  // namespace np
