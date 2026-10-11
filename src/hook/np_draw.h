// 注入钩子里的叠加绘制：CPU 侧用 D2D 把面板画到一张 BGRA 位图，
// 再把这张位图作为纹理贴到游戏画面上。
//
// 之所以不直接在设备上下文里用 D2D 互操作，是因为同一套位图还能给
//   * D3D11（UpdateSubresource）
//   * D3D12（上传堆 + CopyTextureRegion）
//   * 桌面分层窗口（UpdateLayeredWindow，见 src/app）
// 三种目标复用，代码量小、行为一致，也不会和游戏自己的状态打架。
#pragma once

#include <windows.h>
#include <d2d1.h>
#include <d3d11.h>
#include <d3d12.h>
#include <cstdint>

#include "common/np_bitmap.h"

namespace npg {

bool GfxInit();
void GfxShutdown();

// D3D11 / D3D12 游戏内叠加（位图由 npb::PanelBitmap 提供）

// ---------------------------------------------------------------- D3D11 叠加
class Overlay11 {
public:
    bool Init(ID3D11Device* dev);
    void Release();
    // 把位图以 (x,y) 为左上角贴到后台缓冲上
    bool Draw(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* backbuffer,
              const unsigned char* px, int w, int h, int stride, float x, float y);

private:
    ID3D11VertexShader* vs_ = nullptr;
    ID3D11PixelShader*  ps_ = nullptr;
    ID3D11Texture2D*    tex_ = nullptr;
    ID3D11ShaderResourceView* srv_ = nullptr;
    ID3D11BlendState*   blend_ = nullptr;
    ID3D11SamplerState* samp_ = nullptr;
    ID3D11RasterizerState* raster_ = nullptr;
    ID3D11RenderTargetView* rtv_ = nullptr;
    ID3D11Texture2D*    rtvSrc_ = nullptr;
    int texW_ = 0, texH_ = 0;
};

// ---------------------------------------------------------------- D3D12 叠加
class Overlay12 {
public:
    bool Init(ID3D12Device* dev);
    void Release();
    // 在给定的命令列表里录制「上传 + 过渡 + 画一个全屏三角形」。
    // bbStateBefore/Before：调用方告诉我们后台缓冲当前处于什么状态。
    bool Record(ID3D12Device* dev, ID3D12GraphicsCommandList* list, ID3D12Resource* backbuffer,
                D3D12_RESOURCE_STATES bbBefore, const unsigned char* px, int w, int h, int stride,
                float x, float y, float bbW, float bbH);
    ID3D12Fence* fence() const { return fence_; }
    UINT64*      fenceValuePtr() { return &fenceValue_; }
    void         OnFrameCompleted();  // 回收上传缓冲
    // 诊断用：描述符堆被换过几次（交换链重建 / 全屏转换会触发）
    int          HeapSwaps() const { return rtvHeapSwaps_; }
    // 交换链刚被重建过、还需要静默几帧（见 np_hook.cpp 里的使用处）：
    // 重建窗口期往游戏的命令列表里录屏障/绘制会踩到驱动的崩溃点。
    bool         settlePending() const { return settle_ > 0; }
    void         tickSettle() { if (settle_ > 0) --settle_; }

private:
    bool EnsurePso(ID3D12Device* dev, DXGI_FORMAT rtvFormat);
    // 交给 fence 回收：绝不能在 GPU 可能还在用的时候立刻 Release
    void Retire(IUnknown* r);

    ID3D12RootSignature* root_ = nullptr;
    ID3D12PipelineState* pso_ = nullptr;
    ID3DBlob*  vsBlob_ = nullptr;
    ID3DBlob*  psBlob_ = nullptr;
    DXGI_FORMAT psoFormat_ = DXGI_FORMAT_UNKNOWN;
    ID3D12Resource*      tex_ = nullptr;
    ID3D12DescriptorHeap* srvHeap_ = nullptr;
    ID3D12DescriptorHeap* rtvHeap_ = nullptr;
    // 每种后台缓冲各自占一个 RTV 描述符槽。
    // 原来只有一个槽、每帧按当前缓冲重写 —— 上一帧的命令列表可能还在执行，
    // 而 GPU 是**执行时**才去读描述符的，等于让它画到别的缓冲上。
    struct RtvSlot { ID3D12Resource* res; int slot; };
    RtvSlot rtvSlots_[8]{};
    int     rtvSlotsN_ = 0;
    // 描述符堆被换掉过几次（交换链重建 = 全屏转换 / ResizeBuffers 会导致）。
    // 每次换堆都要把旧堆 Retire 掉，用来诊断「全屏后闪烁/消失」这类问题。
    int     rtvHeapSwaps_ = 0;
    // 交换链重建后的静默帧数（换堆时置位，逐帧递减）
    int     settle_ = 0;
    // 绘制被跳过的帧数（供诊断：闪烁是不是因为我们没画上去）
    int     drawSkips_ = 0;
    UINT rtvHeapSize_ = 0;
    int  texW_ = 0, texH_ = 0;

    ID3D12Fence* fence_ = nullptr;
    UINT64       fenceValue_ = 0;
    struct Pending { IUnknown* res; UINT64 value; };
    Pending pending_[16]{};
    int     pendingN_ = 0;

    // ---------------------------------------------------------------- 上传缓冲环
    //
    // 原来**每帧** CreateCommittedResource 一个约 300KB 的 UPLOAD 资源。两个理由不能留：
    //   1) 纯浪费：60fps × 300KB ≈ 18MB/s 的分配/释放抖动；
    //   2) 更要紧：它在崩溃二分的嫌疑名单上 —— 历史二分显示崩溃需要「绑定我们的 RTV」
    //      与「上传那一半」**同时**存在，而把 CopyTextureRegion 去掉**仍然崩**；
    //      于是"每帧新建上传资源"就是上传那一半里剩下的唯一动作。改成复用常驻缓冲，
    //      等于把这个动作从每帧路径上彻底删掉。
    // 复用前必须等 fence：GPU 还在读上一版内容时 memcpy 进去会让面板撕裂。
    enum { kUploadRing = 4 };
    ID3D12Resource* uploadRing_[kUploadRing] = {};
    UINT64          uploadRingSize_[kUploadRing] = {};    // 已创建大小（0 = 还没建）
    UINT64          uploadRingFence_[kUploadRing] = {};   // 用它的那一帧会 Signal 到这个值
    int             uploadRingNext_ = 0;
};

}  // namespace npg
