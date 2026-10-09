// Reflex 延迟标记读取 —— 权威的 CPU 帧时间拆分
//
// 为什么做这个：我们原来是"猜"CPU 帧时间（本帧 Present 开始 − 上帧 Present 返回），
// 实测在流水线渲染的游戏里这个值会退化成接近 0，开 Reflex 又会被抬高 —— 只能定性，
// 不能定量。
//
// Reflex 在游戏里**主动打标记**（NvAPI_D3D_SetLatencyMarker）：模拟开始/结束、
// 渲染提交开始/结束、Present 开始/结束……这些都是**游戏自己报的权威时刻**，
// 拿到它就能把 CPU 帧时间精确拆成「模拟」和「提交」两段。
//
// 接口 ID 来自 Intel PresentMon 的 nvapi_interface_table.h（我们此前卡在缺这个 ID）：
//     NvAPI_D3D_GetLatency   0x1a587f9c
//
// ⚠ 只有游戏**自己在用 Reflex** 时才会有数据；没有就返回 false，调用方要保持回退路径。
#pragma once

#include <windows.h>
#include <cstdint>
#include <cstring>

namespace np {

// ---- 与 nvapi.h 逐字段对齐（不能改顺序/宽度，否则解析出来全是垃圾）
struct NVFrameReport {
    uint64_t frameID;
    uint64_t inputSampleTime;
    uint64_t simStartTime;
    uint64_t simEndTime;
    uint64_t renderSubmitStartTime;
    uint64_t renderSubmitEndTime;
    uint64_t presentStartTime;
    uint64_t presentEndTime;
    uint64_t driverStartTime;
    uint64_t driverEndTime;
    uint64_t osRenderQueueStartTime;
    uint64_t osRenderQueueEndTime;
    uint64_t gpuRenderStartTime;
    uint64_t gpuRenderEndTime;
    uint32_t gpuActiveRenderTimeUs;
    uint32_t gpuFrameTimeUs;
    uint8_t  rsvd[120];
};

struct NVLatencyParams {
    uint32_t      version;
    NVFrameReport frameReport[64];
    uint8_t       rsvd[32];
};

// MAKE_NVAPI_VERSION(struct, ver) = sizeof(struct) | (ver << 16)
#define NP_NVAPI_VERSION(s, v) ((uint32_t)(sizeof(s) | ((uint32_t)(v) << 16)))

class ReflexReader {
public:
    // 载入 nvapi64.dll（失败再试 nvapi32.dll）并取接口。可在任意时刻调用，幂等。
    bool Init() {
        if (getLatency_) return true;
        if (!dll_) {
            dll_ = LoadLibraryW(L"nvapi64.dll");
            if (!dll_) dll_ = LoadLibraryW(L"nvapi32.dll");
            if (!dll_) return false;
        }
        auto qi = reinterpret_cast<void*(*)(unsigned int)>(
            GetProcAddress(dll_, "nvapi_QueryInterface"));
        if (!qi) return false;
        auto init = reinterpret_cast<int(*)()>(qi(0x0150E828u));   // NvAPI_Initialize
        if (init && init() != 0) return false;                     // 0 = NVAPI_OK
        getLatency_ = reinterpret_cast<int(*)(void*, void*)>(qi(0x1A587F9Cu));  // GetLatency
        return getLatency_ != nullptr;
    }

    bool ok() const { return getLatency_ != nullptr; }

    // 取最近一帧的报告。dev 是 D3D 设备（ID3D11Device*/ID3D12Device*）。
    // 返回 false = 这次没拿到有效数据（游戏没用 Reflex / 调用失败）。
    bool Poll(void* dev, NVFrameReport* out) {
        if (!getLatency_ || !dev || !out) return false;
        NVLatencyParams p{};
        p.version = NP_NVAPI_VERSION(NVLatencyParams, 1);
        if (getLatency_(dev, &p) != 0) return false;   // 0 = NVAPI_OK

        // 从 64 帧里挑**最后一条有效**的（frameID 最大且各时刻非 0）。
        const NVFrameReport* best = nullptr;
        for (int i = 0; i < 64; ++i) {
            const NVFrameReport& f = p.frameReport[i];
            if (!f.frameID || !f.simStartTime || !f.simEndTime) continue;
            if (!best || f.frameID > best->frameID) best = &f;
        }
        if (!best) return false;
        // 明显不合理的（结束早于开始）也不要
        if (best->simEndTime < best->simStartTime) return false;
        if (best->renderSubmitEndTime && best->renderSubmitEndTime < best->renderSubmitStartTime)
            return false;
        std::memcpy(out, best, sizeof(NVFrameReport));
        return true;
    }

private:
    HMODULE dll_ = nullptr;
    int (*getLatency_)(void*, void*) = nullptr;
};

}  // namespace np
