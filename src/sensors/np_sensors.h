// 传感器中枢：把「能拿到的最好数据」挑出来。
//
// 设计原则：
//   * 所有数据源都是可选的，任何一个失败都不影响其他源；
//   * 同一指标有多个源时按准确度排序（厂商 SDK > 第三方监控软件共享内存 > WMI > PDH）；
//   * UI 上会显示每个指标最终用的是哪个源，方便排查。

#pragma once

#include <windows.h>
#include <pdh.h>
#include <string>
#include <vector>
#include <cstdint>

#include "common/np_common.h"
#include "np_vendor.h"

namespace np {

// 一个 PDH 查询，支持通配符展开到实例
class PdhQuery {
public:
    struct Counter {
        std::wstring path;
        std::wstring instance;
        HCOUNTER handle = nullptr;
        double value = 0.0;
        bool valid = false;
    };

    bool Open();
    void Close();
    bool AddWildcard(const std::wstring& wildcardPath);
    // 重新展开通配符，把**新出现的实例**补进查询。
    //
    // 为什么必须重新展开：PDH 的通配实例是在 AddCounter 那一刻就固定下来的。
    // 主程序启动时游戏还没开，`\GPU Engine(*)` 里根本没有它的实例 ——
    // 之后就算游戏跑起来也永远读不到（实测卡了一轮：busy 一直是 -1）。
    // 每次目标进程变化时调一次即可。
    bool RefreshWildcard(const std::wstring& wildcardPath);
    bool Collect();
    // 求所有 instance 命中关键字的计数器之和
    bool SumWhere(const std::wstring& objKw, const std::wstring& instKw, double* out) const;
    // 实例名同时命中两个关键字才计入。
    // 需要它是因为 `\GPU Engine(*)` 的实例名很长：
    //   pid_1234_luid_0x..._phys_0_eng_0_engtype_3D
    // 要同时按 pid 和 engtype 过滤，一个关键字不够。
    bool SumWhere2(const std::wstring& objKw, const std::wstring& kw1, const std::wstring& kw2,
                   double* out) const;
    // 按「计数器名 + 实例关键字」求和。
    // ⚠ 必须按计数器名区分：`\GPU Engine(*)` 下 `Running Time`（累计秒数）和
    //   `Utilization Percentage`（百分比）的**实例名一模一样**，
    //   混在一起求和就是把秒和百分号加到一起。
    // 也不用 PathHasObject —— 本地展开出来的路径不带 `\\计算机名` 前缀，它会解析失败。
    bool SumInstance(const std::wstring& counterKw, const std::wstring& kw1,
                     const std::wstring& kw2, double* out) const;
    // ---- 实例名带 `*` 的计数器（读「按进程的 GPU 引擎数据」的正确姿势）★
    //
    // 为什么不能用 AddWildcard：它把通配符**在 AddCounter 那一刻展开成 N 个
    // 独立计数器**，实例列表从此冻结。实测后果 —— 游戏在 NextPerf 之后启动时，
    // 该进程的 GPU 引擎实例根本不在查询里，求和永远找不到，
    // 于是「GPU 帧时间」整场为空。
    // （用户实测：先开游戏再开 NextPerf 就有数据，先开 NextPerf 再进游戏就没有。）
    //
    // 带 `*` 的计数器相反：实例列表由 PDH **每次取值时**给出
    // （PdhGetFormattedCounterArrayW），新出现的进程自动就有，不需要重新展开。
    bool AddStarCounter(const std::wstring& path);
    bool SumStarCounter(const std::wstring& path, const std::wstring& kw1,
                        const std::wstring& kw2, double* out, int* hitCount = nullptr) const;
    // 同样读动态实例列表，但取**最忙的那个实例**而不是求和。
    // 占用率百分比不能跨引擎求和：`\GPU Engine(*)` 下每个引擎（3D/Copy/Video/…）
    // 各自报 0~100%，几十个实例加起来永远是几百，clamp 一下就恒等于 100%。
    // 任务管理器算「整体 GPU 占用」用的就是最忙引擎，这里对齐它。
    bool MaxStarCounter(const std::wstring& path, const std::wstring& kw1,
                        const std::wstring& kw2, double* out) const;
    bool MaxWhere(const std::wstring& objKw, const std::wstring& instKw, std::wstring* instOut,
                  double* out) const;
    size_t Count() const { return counters_.size(); }

private:
    PDH_HQUERY query_ = nullptr;
    std::vector<Counter> counters_;
    // 实例名带 `*` 的计数器（动态实例列表）
    struct StarCounter { std::wstring path; HCOUNTER handle = nullptr; };
    std::vector<StarCounter> stars_;
    bool opened_ = false;
};

class SensorHub {
public:
    bool Init();
    void Shutdown();
    void Poll(NPSensors& out);

    // ---- 按游戏进程读 GPU 引擎数据（**不需要管理员**，走系统 PDH 计数器）
    //
    // 为什么加这个：靠 Present 钩子推算 GPU 忙时间，只在单线程、无 Reflex、
    // 提交模式简单的引擎上成立；真实游戏上会被多线程提交的区间重叠搞偏。
    // Windows 自己就按 (进程, 引擎) 暴露 GPU 执行时间，驱动报的数，
    // 与锁帧、Reflex、多线程全都无关。
    //
    //   gpuRunningSec : 该进程自开机以来累计的 GPU 执行时间（秒），-1 = 读不到
    //   busyMsPerFrame: 两次采样之间「每帧 GPU 忙时间」，-1 = 还测不出来
    //   engCompute/engOfa: 该进程在 compute / OFA(光流加速器) 引擎上的占用 %
    void PollGameGpu(uint32_t pid, uint32_t frameDelta, NPSensors& out);
    bool gameGpuAvailable() const { return gameGpuOk_; }
    // 诊断：上一次 PollGameGpu 为什么没出数（空 = 正常）
    const std::string& gameGpuDiag() const { return gameGpuDiag_; }

    uint32_t available() const { return available_; }
    std::string Describe() const;

    // 手动指定优先数据源（0 = 自动）
    void SetOverride(uint32_t srcMask) { override_ = srcMask; }
    uint32_t overrideMask() const { return override_; }

    // 供 UI 展示的探测详情
    struct SourceInfo {
        uint32_t mask;
        bool     present;
        bool     used;
        std::string name;
        std::string note;
    };
    const std::vector<SourceInfo>& sources() const { return srcInfo_; }

private:
    void ProbeSources();
    void PollCpu(NPSensors& out);
    void PollRam(NPSensors& out);
    void PollGpuNvidia(NPSensors& out);
    void PollGpuAmd(NPSensors& out);
    void PollGpuGeneric(NPSensors& out);
    void PollHwinfoExtras(NPSensors& out);
    void ProbeRtTensorHardware(NPSensors& out);

    bool WmiScalar(const wchar_t* ns, const wchar_t* wql, const wchar_t* prop, double* out) const;

    NvmlApi  nvml_;
    NvapiApi nvapi_;
    AdlApi   adl_;
    HwinfoCtx hwinfo_;
    PdhQuery pdh_;

    uint32_t available_ = 0;
    uint32_t override_ = 0;
    std::vector<SourceInfo> srcInfo_;

    // CPU 占用率差分
    uint64_t lastIdle_ = 0, lastKern_ = 0, lastUser_ = 0;
    bool     cpuBaseValid_ = false;
    double   cpuBaseMHz_ = 0;

    int      nvmlIndex_ = -1;
    bool     comInited_ = false;
    bool     hwinfoTried_ = false;
    uint32_t pollCount_ = 0;

    // 最近一次未公开 NVAPI 域的探测结果，用于诊断
    int      nvapiExtSeen_ = 0;

    // DXGI 探测到的显卡基础信息（跨厂商）
    std::string dxgiName_;
    uint32_t    dxgiVendorId_ = 0;
    uint64_t    dxgiVramBytes_ = 0;

    uint32_t cpuCores_ = 0;
    uint32_t cpuThreads_ = 0;

    float    maxGpuTemp_ = -1.0f;   // GPU 温度上限
    float    encUtil_ = -1.0f;      // 编码器占用
    float    decUtil_ = -1.0f;      // 解码器占用

    // 按游戏进程读 GPU 引擎数据的差分基线
    std::string gameGpuDiag_;
    bool     gameGpuOk_ = false;    // Running Time 计数器是否可用
    // 上一次算出来的值要**留着**：Poll() 每次都会把 NPSensors 清空，
    // 而这个采样最快也要 100ms 才有新样本，不留就会出现「时有时无」。
    float    lastBusyMs_ = -1.0f;
    float    lastCompute_ = -1.0f;
    float    lastOfa_ = -1.0f;
    uint32_t gameGpuPid_ = 0;
    double   lastGameGpuSec_ = -1.0;
    uint64_t lastGameGpuMs_ = 0;
    uint32_t lastFrameTotal_ = 0;
    uint32_t lastPidFrameTotal_ = 0;

public:
    float maxGpuTemp() const { return maxGpuTemp_; }
    float encoderUtil() const { return encUtil_; }
    float decoderUtil() const { return decUtil_; }
    bool  nvapiExtSeen() const { return nvapiExtSeen_ != 0; }
};

}  // namespace np
