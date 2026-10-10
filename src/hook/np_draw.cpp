#include "np_draw.h"

#include <dwrite.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <vector>

#undef DrawText

// 由 np_hook.cpp 定义。画我们自己的面板时置上，让 DrawInstanced/DrawIndexedInstanced
// 的计数钩子不要把「叠加自己那一个全屏三角形」算成游戏的 draw call
// （面板每 50ms 重绘一次，统计里就会周期性多出 1 个 draw）。
// 与 np_hook.cpp 里那份定义保持一致：必须是 thread_local（见那边的说明）
extern "C" thread_local bool gNpCountingOverlayDraws;

namespace npg {

// mingw 的头文件里没有 d3dx12.h，这里补几个等价于 CD3DX12_* 的小工具
inline D3D12_HEAP_PROPERTIES NpHeapProps(D3D12_HEAP_TYPE t) {
    D3D12_HEAP_PROPERTIES p{};
    p.Type = t;
    p.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    p.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    p.CreationNodeMask = 1;
    p.VisibleNodeMask = 1;
    return p;
}
inline D3D12_RESOURCE_BARRIER NpTransition(ID3D12Resource* r, D3D12_RESOURCE_STATES before,
                                           D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}
inline D3D12_RASTERIZER_DESC NpDefaultRasterizer() {
    D3D12_RASTERIZER_DESC r{};
    r.FillMode = D3D12_FILL_MODE_SOLID;
    r.CullMode = D3D12_CULL_MODE_NONE;
    r.FrontCounterClockwise = FALSE;
    r.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    r.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    r.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    r.DepthClipEnable = FALSE;
    r.MultisampleEnable = FALSE;
    r.AntialiasedLineEnable = FALSE;
    r.ForcedSampleCount = 0;
    r.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    return r;
}

static HMODULE gCompiler = nullptr;
static HRESULT(WINAPI* gD3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
                                    LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**) = nullptr;

// 一个覆盖全屏的三角形，采样面板纹理，premultiplied alpha 混合
static const char kShaderSrc[] =
    "struct VSOut{float4 p:SV_Position;float2 uv:TEXCOORD0;};\n"
    "VSOut vsMain(uint vid:SV_VertexID){\n"
    "  VSOut o;\n"
    "  float2 xy = (vid==0)?float2(-1,-1):((vid==1)?float2(3,-1):float2(-1,3));\n"
    "  o.p = float4(xy,0,1);\n"
    "  o.uv = float2((xy.x+1)*0.5,(1-xy.y)*0.5);\n"
    "  return o;\n"
    "}\n"
    "Texture2D tex:register(t0);\n"
    "SamplerState samp:register(s0);\n"
    "float4 psMain(VSOut i):SV_Target{return tex.Sample(samp,i.uv);}\n";

bool GfxInit() {
    if (!npb::GfxInit()) return false;
    gCompiler = LoadLibraryW(L"d3dcompiler_47.dll");
    if (gCompiler)
        gD3DCompile = reinterpret_cast<decltype(gD3DCompile)>(
            GetProcAddress(gCompiler, "D3DCompile"));
    return true;
}

void GfxShutdown() {
    npb::GfxShutdown();
    if (gCompiler) { FreeLibrary(gCompiler); gCompiler = nullptr; gD3DCompile = nullptr; }
}

static bool CompileShader(const char* entry, const char* target, ID3DBlob** out) {
    if (!gD3DCompile) return false;
    ID3DBlob* err = nullptr;
    HRESULT hr = gD3DCompile(kShaderSrc, sizeof(kShaderSrc) - 1, "np_overlay", nullptr, nullptr,
                             entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &err);
    if (err) err->Release();
    return SUCCEEDED(hr) && *out;
}

// ================================================================ D3D11
bool Overlay11::Init(ID3D11Device* dev) {
    if (!dev) return false;
    ID3DBlob* vs = nullptr; ID3DBlob* ps = nullptr;
    if (!CompileShader("vsMain", "vs_5_0", &vs) || !CompileShader("psMain", "ps_5_0", &ps)) {
        if (vs) vs->Release();
        if (ps) ps->Release();
        return false;
    }
    HRESULT hr = dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vs_);
    if (FAILED(hr)) { vs->Release(); ps->Release(); return false; }
    hr = dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &ps_);
    vs->Release(); ps->Release();
    if (FAILED(hr)) return false;

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&sd, &samp_))) return false;

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;               // 预乘 alpha
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&bd, &blend_))) return false;

    // 必须显式建一个关掉背面剔除的光栅化状态并在绘制时绑定。
    // D3D11 的默认光栅化状态是 CullMode = BACK，而全屏三角形
    // (-1,-1)(3,-1)(-1,3) 正好是背面 —— 不绑这个的话 Draw 会「成功」返回，
    // 但屏幕上什么都没有（D3D12 那边 PSO 里本来就写了 CULL_NONE，所以没事）。
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = FALSE;
    if (FAILED(dev->CreateRasterizerState(&rd, &raster_))) return false;
    return true;
}

void Overlay11::Release() {
    if (rtv_) { rtv_->Release(); rtv_ = nullptr; }
    if (rtvSrc_) { rtvSrc_->Release(); rtvSrc_ = nullptr; }
    if (tex_) { tex_->Release(); tex_ = nullptr; }
    if (srv_) { srv_->Release(); srv_ = nullptr; }
    if (raster_) { raster_->Release(); raster_ = nullptr; }
    if (blend_) { blend_->Release(); blend_ = nullptr; }
    if (samp_) { samp_->Release(); samp_ = nullptr; }
    if (vs_) { vs_->Release(); vs_ = nullptr; }
    if (ps_) { ps_->Release(); ps_ = nullptr; }
}

bool Overlay11::Draw(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* backbuffer,
                     const unsigned char* px, int w, int h, int stride, float x, float y) {
    if (!dev || !ctx || !backbuffer || !px || w <= 0 || h <= 0 || !vs_) return false;

    // 面板纹理（尺寸变化时重建）
    if (!tex_ || texW_ != w || texH_ != h) {
        if (tex_) { tex_->Release(); tex_ = nullptr; }
        if (srv_) { srv_->Release(); srv_ = nullptr; }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = (UINT)w; td.Height = (UINT)h;
        td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &tex_))) { tex_ = nullptr; return false; }
        // ★ texW_/texH_ 必须**在确认纹理和 SRV 都建好之后**才写。
        //   原来先写 texW_=w, texH_=h 再建 SRV，一旦 CreateShaderResourceView 失败，
        //   函数返回 false 但 texW_/texH_ 已经等于目标尺寸 —— 下一次调用
        //   `if (!tex_ || texW_ != w || texH_ != h)` 直接为假，永远不再重建，
        //   于是永久带着一个 NULL SRV 去 PSSetShaderResources，面板再也画不出来。
        if (FAILED(dev->CreateShaderResourceView(tex_, nullptr, &srv_))) {
            tex_->Release(); tex_ = nullptr;
            return false;
        }
        texW_ = w; texH_ = h;
    }

    D3D11_MAPPED_SUBRESOURCE ms{};
    if (FAILED(ctx->Map(tex_, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return false;
    for (int r = 0; r < h; ++r)
        memcpy(reinterpret_cast<unsigned char*>(ms.pData) + (size_t)r * ms.RowPitch,
               px + (size_t)r * stride, (size_t)stride);
    ctx->Unmap(tex_, 0);

    // ---- 后台缓冲 RTV：**每帧新建，本帧结束就放掉**
    //
    // ★★ 原来这里是「缓存 RTV + 对后台缓冲 AddRef」：
    //       rtvSrc_ = backbuffer;  rtvSrc_->AddRef();
    //    这是对 DXGI 规则的**严重违反**：应用调用 ResizeBuffers / 切换独占全屏之前，
    //    必须释放**所有**对后台缓冲的引用。我们长期握着引用、又握着从它创建的 RTV，
    //    游戏切全屏时 ResizeBuffers 就会失败（DXGI_ERROR_INVALID_CALL），
    //    而不少游戏不检查这个返回值 —— 直接闪退。
    //    用户实测完全吻合：**不绘制叠加层不闪退，一绘制就闪退**
    //    （AddRef 只发生在绘制路径里）。
    //    代价只是每帧多建一个 RTV，可忽略；换来的是不再干涉游戏的全屏转换。
    if (rtv_) { rtv_->Release(); rtv_ = nullptr; }
    if (FAILED(dev->CreateRenderTargetView(backbuffer, nullptr, &rtv_))) return false;

    D3D11_TEXTURE2D_DESC bbd{};
    backbuffer->GetDesc(&bbd);

    D3D11_VIEWPORT vp{};
    vp.Width = (float)bbd.Width;
    vp.Height = (float)bbd.Height;
    vp.MinDepth = 0; vp.MaxDepth = 1;
    vp.TopLeftX = 0; vp.TopLeftY = 0;

    ctx->OMSetRenderTargets(1, &rtv_, nullptr);
    ctx->RSSetState(raster_);          // 关掉背面剔除，否则全屏三角形会被剔掉
    ctx->RSSetViewports(1, &vp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(vs_, nullptr, 0);
    ctx->PSSetShader(ps_, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &srv_);
    ctx->PSSetSamplers(0, 1, &samp_);
    float blendFactor[4] = {1, 1, 1, 1};
    ctx->OMSetBlendState(blend_, blendFactor, 0xffffffff);

    // 用视口偏移实现面板位置：把三角形缩放到面板大小并平移
    // 这里改用一个更简单的办法：直接按屏幕像素换算到 NDC
    //   ndcX = 2*x/w - 1, ndcY = 1 - 2*y/h
    // 由于着色器里写死了全屏三角形，我们改成用视口裁剪到面板区域：
    //   视口设为 {x, y, w, h}，全屏三角形就会被拉伸到该矩形。
    D3D11_VIEWPORT panel{};
    panel.TopLeftX = x; panel.TopLeftY = y;
    panel.Width = (float)w; panel.Height = (float)h;
    panel.MinDepth = 0; panel.MaxDepth = 1;
    ctx->RSSetViewports(1, &panel);
    // 这个 draw 会走进我们自己的 DrawInstanced 钩子，标一下别让它进游戏统计
    gNpCountingOverlayDraws = true;
    ctx->Draw(3, 0);
    gNpCountingOverlayDraws = false;

    // 还原视口，尽量不打扰游戏
    ctx->RSSetViewports(1, &vp);
    // ★ 立刻解除绑定并释放后台缓冲的 RTV —— 绝不让这个引用活过本帧。
    //   游戏随后调用 ResizeBuffers / 切独占全屏时，要求后台缓冲引用为 0，
    //   留着它就会让那次调用失败（进而可能让游戏闪退）。见上面的长注释。
    if (rtv_) {
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        rtv_->Release();
        rtv_ = nullptr;
    }
    return true;
}

// ================================================================ D3D12
bool Overlay12::Init(ID3D12Device* dev) {
    if (!dev) return false;

    ID3DBlob* vs = nullptr; ID3DBlob* ps = nullptr;
    if (!CompileShader("vsMain", "vs_5_0", &vs) || !CompileShader("psMain", "ps_5_0", &ps)) {
        if (vs) vs->Release();
        if (ps) ps->Release();
        return false;
    }

    // 根签名：一个 SRV 描述符表 + 一个静态采样器
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.RegisterSpace = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rp{};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp.DescriptorTable.NumDescriptorRanges = 1;
    rp.DescriptorTable.pDescriptorRanges = &range;

    D3D12_STATIC_SAMPLER_DESC ss{};
    ss.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    ss.MaxLOD = D3D12_FLOAT32_MAX;

    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 1;
    rs.pParameters = &rp;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &ss;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob* sig = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (err) err->Release();
    if (FAILED(hr) || !sig) { vs->Release(); ps->Release(); return false; }
    hr = dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                  IID_PPV_ARGS(&root_));
    sig->Release();
    if (FAILED(hr)) { vs->Release(); ps->Release(); return false; }

    // PSO 依赖后台缓冲格式，等真正绘制时再建（见 EnsurePso）
    vsBlob_ = vs;
    psBlob_ = ps;

    D3D12_DESCRIPTOR_HEAP_DESC sh{};
    sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    sh.NumDescriptors = 1;
    sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&srvHeap_)))) return false;

    D3D12_DESCRIPTOR_HEAP_DESC rh{};
    rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rh.NumDescriptors = 8;
    if (FAILED(dev->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&rtvHeap_)))) return false;
    rtvHeapSize_ = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) return false;
    fenceValue_ = 0;
    return true;
}

bool Overlay12::EnsurePso(ID3D12Device* dev, DXGI_FORMAT rtvFormat) {
    if (pso_ && psoFormat_ == rtvFormat) return true;
    if (!vsBlob_ || !psBlob_ || !root_) return false;
    // 旧 PSO 可能还绑在正在执行的命令列表上，同样交给 fence 回收
    if (pso_) { Retire(pso_); pso_ = nullptr; }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = root_;
    pso.VS.pShaderBytecode = vsBlob_->GetBufferPointer();
    pso.VS.BytecodeLength = vsBlob_->GetBufferSize();
    pso.PS.pShaderBytecode = psBlob_->GetBufferPointer();
    pso.PS.BytecodeLength = psBlob_->GetBufferSize();
    pso.BlendState.RenderTarget[0].BlendEnable = TRUE;
    pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;          // 预乘 alpha
    pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = 0xffffffff;
    pso.RasterizerState = NpDefaultRasterizer();
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = rtvFormat;
    pso.SampleDesc.Count = 1;
    HRESULT hr = dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pso_));
    if (FAILED(hr)) return false;
    psoFormat_ = rtvFormat;
    return true;
}

void Overlay12::Release() {
    for (int i = 0; i < pendingN_; ++i) if (pending_[i].res) pending_[i].res->Release();
    pendingN_ = 0;
    if (tex_) { tex_->Release(); tex_ = nullptr; }
    if (srvHeap_) { srvHeap_->Release(); srvHeap_ = nullptr; }
    if (rtvHeap_) { rtvHeap_->Release(); rtvHeap_ = nullptr; }
    rtvSlotsN_ = 0;
    if (pso_) { pso_->Release(); pso_ = nullptr; }
    if (root_) { root_->Release(); root_ = nullptr; }
    if (fence_) { fence_->Release(); fence_ = nullptr; }
    // 原来漏了这两个：vsBlob_/psBlob_ 一直持有到进程结束（自卸载时也不放），
    // 而且 psoFormat_ 不重置的话，若本对象在同一个进程里被重新 Init，
    // EnsurePso 会因为 psoFormat_ 恰好相等而认为旧 PSO 还能用（其实已经 Release 了）。
    if (vsBlob_) { vsBlob_->Release(); vsBlob_ = nullptr; }
    if (psBlob_) { psBlob_->Release(); psBlob_ = nullptr; }
    psoFormat_ = DXGI_FORMAT_UNKNOWN;
}

// 把资源挂到 fence 上延迟回收。
// D3D12 不会因为我们提交过命令列表就给资源加引用 —— 立刻 Release 一个
// 「已经录制进命令列表、但 GPU 还没执行完」的资源，就是释放正在使用的显存。
void Overlay12::Retire(IUnknown* r) {
    if (!r) return;
    if (pendingN_ >= 16) { r->Release(); return; }   // 极端情况，宁可立刻放也别无限涨
    pending_[pendingN_].res = r;
    pending_[pendingN_].value = fenceValue_ + 1;     // 下一次 Signal 之后才算安全
    ++pendingN_;
}

void Overlay12::OnFrameCompleted() {
    UINT64 done = fence_->GetCompletedValue();
    for (int i = 0; i < pendingN_;) {
        if (pending_[i].value <= done) {
            pending_[i].res->Release();
            pending_[i] = pending_[pendingN_ - 1];
            --pendingN_;
        } else {
            ++i;
        }
    }
}

bool Overlay12::Record(ID3D12Device* dev, ID3D12GraphicsCommandList* list,
                       ID3D12Resource* backbuffer, D3D12_RESOURCE_STATES bbBefore,
                       const unsigned char* px, int w, int h, int stride, float x, float y,
                       float bbW, float bbH) {
    if (!dev || !list || !backbuffer || !px || !root_) return false;
    OnFrameCompleted();
    DXGI_FORMAT bbFormat = backbuffer->GetDesc().Format;
    if (!EnsurePso(dev, bbFormat)) return false;

    // ---- 面板纹理（按需重建）
    if (!tex_ || texW_ != w || texH_ != h) {
        // 旧纹理可能还被上一帧的命令列表引用着，交给 fence 回收，不能立刻 Release
        if (tex_) { Retire(tex_); tex_ = nullptr; }
        texW_ = w; texH_ = h;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = (UINT64)w; td.Height = (UINT)h;
        td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        td.Flags = D3D12_RESOURCE_FLAG_NONE;
        D3D12_HEAP_PROPERTIES hp = NpHeapProps(D3D12_HEAP_TYPE_DEFAULT);
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&tex_))))
            return false;
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = td.Format;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(tex_, &sv, srvHeap_->GetCPUDescriptorHandleForHeapStart());
    }

    // ---- 上传：行距必须按 D3D12_TEXTURE_DATA_PITCH_ALIGNMENT(256) 对齐
    UINT64 rowPitch = ((UINT64)stride + (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)) /
                      D3D12_TEXTURE_DATA_PITCH_ALIGNMENT * D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
    UINT64 uploadSize = rowPitch * (UINT64)h;
    D3D12_RESOURCE_DESC ud{};
    ud.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    ud.Width = uploadSize; ud.Height = 1; ud.DepthOrArraySize = 1; ud.MipLevels = 1;
    ud.SampleDesc.Count = 1; ud.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES up = NpHeapProps(D3D12_HEAP_TYPE_UPLOAD);
    ID3D12Resource* upload = nullptr;
    if (FAILED(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &ud,
                                            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                            IID_PPV_ARGS(&upload))))
        return false;
    void* dst = nullptr;
    if (SUCCEEDED(upload->Map(0, nullptr, &dst))) {
        for (int r = 0; r < h; ++r)
            memcpy(reinterpret_cast<unsigned char*>(dst) + r * rowPitch, px + (size_t)r * stride,
                   (size_t)stride);
        upload->Unmap(0, nullptr);
    }
    if (pendingN_ < 15) {
        pending_[pendingN_].res = upload;
        pending_[pendingN_].value = fenceValue_ + 1;  // 调用方执行后会 Signal 到这个值
        ++pendingN_;
    } else {
        // 回收队列满了：先清掉已完成的，还满就放弃这一帧的叠加。
        // 绝不能在这里直接 Release —— 那个上传缓冲十有八九还在 GPU 手里。
        OnFrameCompleted();
        if (pendingN_ < 15) {
            pending_[pendingN_].res = upload;
            pending_[pendingN_].value = fenceValue_ + 1;
            ++pendingN_;
        } else {
            upload->Release();
            return false;
        }
    }

    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource = upload;
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint.Offset = 0;
    srcLoc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    srcLoc.PlacedFootprint.Footprint.Width = (UINT)w;
    srcLoc.PlacedFootprint.Footprint.Height = (UINT)h;
    srcLoc.PlacedFootprint.Footprint.Depth = 1;
    srcLoc.PlacedFootprint.Footprint.RowPitch = (UINT)rowPitch;

    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.pResource = tex_;
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    // 面板纹理进入 Record 时恒为 COPY_DEST（本函数末尾会转回去）
    list->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
    {
        D3D12_RESOURCE_BARRIER b = NpTransition(
            tex_, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &b);
    }

    // ---- 后台缓冲 RTV
    //
    // 每种后台缓冲占**各自**一个描述符槽。原来只有一个槽、每帧按当前缓冲重写：
    // 上一帧的命令列表可能还在执行，而 GPU 是执行时才去读描述符的，
    // 结果就是让它画到另一张缓冲上 —— 轻则画面错乱，重则设备 removed。
    int slot = -1;
    for (int i = 0; i < rtvSlotsN_; ++i)
        if (rtvSlots_[i].res == backbuffer) { slot = rtvSlots_[i].slot; break; }

    // ★ 槽位满了 => 交换链被重建过多次（全屏独占转换 / ResizeBuffers 都会换掉一整批
    //   后台缓冲对象，每个新对象占一个新槽）。
    //   旧槽里的描述符仍然引用着**已经作废的旧后台缓冲** —— 这既让游戏的
    //   ResizeBuffers 失败（就是刚修好的那个闪退），也让我们自己画到旧缓冲上
    //   （用户实测：全屏后换场景开始**闪烁**，闪一会就**看不见**了，正是槽位用尽后
    //   本函数返回 false 停止绘制）。
    //
    //   所以这里**重置整组槽位**，把对旧后台缓冲的引用放掉。
    //   ⚠ 必须确认没有在途命令列表：GPU 是**执行时**才去读描述符堆的，
    //     覆盖一个正在被读的槽会让它画到错的地方。pendingN_ == 0 表示我们记录过的
    //     命令全部已完成（OnFrameCompleted 每帧回收），此时覆盖是安全的。
    //     不满足就这一帧先不画，下一帧再试 —— 宁可不画，也不画错。
    if (slot < 0 && rtvSlotsN_ >= 8) {
        // ★ 换一个**全新的描述符堆**，旧堆交给 Retire() 延迟释放。
        //
        // 为什么必须换堆而不是覆盖旧槽位：
        //   GPU 是**执行时**才去读描述符堆的，而读它的是**游戏自己的命令列表**
        //   （我们只是往那个列表里录了 OMSetRenderTargets）。所以"没有在途命令列表"
        //   这件事我们根本无法判断 —— pending_ 只跟踪我们自己的上传缓冲。
        //   之前用 pendingN_ == 0 做判据是**不够的**，会覆盖正在被读的描述符，
        //   表现就是偶发闪退 + 闪烁。
        //   换堆则绕开了整个问题：旧堆原封不动留给还在执行的那几帧用，
        //   由 Retire -> OnFrameCompleted 在 fence 确认后才 Release。
        //
        // 顺带这也是正确的资源管理：旧堆里的描述符引用着**已经作废的旧后台缓冲**，
        // 只有把堆放掉才能真正释放它们（否则游戏的 ResizeBuffers 会失败 = 闪退）。
        ID3D12DescriptorHeap* fresh = nullptr;
        D3D12_DESCRIPTOR_HEAP_DESC rh2{};
        rh2.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rh2.NumDescriptors = 8;
        if (FAILED(dev->CreateDescriptorHeap(&rh2, IID_PPV_ARGS(&fresh))) || !fresh)
            return false;              // 建不出来就这一帧不画，别硬来
        Retire(rtvHeap_);              // 接管旧堆所有权，fence 后才 Release
        rtvHeap_ = fresh;
        rtvSlotsN_ = 0;
        ++rtvHeapSwaps_;
    }

    if (slot < 0) {
        if (rtvSlotsN_ >= 8) return false;   // 换了 8 张缓冲还没复用？放弃，别硬来
        slot = rtvSlotsN_;
        D3D12_RENDER_TARGET_VIEW_DESC rv{};
        rv.Format = bbFormat;
        rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        rv.Texture2D.MipSlice = 0;
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)slot * rtvHeapSize_;
        dev->CreateRenderTargetView(backbuffer, &rv, h);
        rtvSlots_[rtvSlotsN_].res = backbuffer;
        rtvSlots_[rtvSlotsN_].slot = slot;
        ++rtvSlotsN_;
    }

    // ---- 录制绘制
    D3D12_RESOURCE_BARRIER pre = NpTransition(
        backbuffer, bbBefore, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ResourceBarrier(1, &pre);

    list->SetPipelineState(pso_);
    list->SetGraphicsRootSignature(root_);
    ID3D12DescriptorHeap* heaps[1] = {srvHeap_};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootDescriptorTable(0, srvHeap_->GetGPUDescriptorHandleForHeapStart());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)slot * rtvHeapSize_;
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    // 用裁剪矩形把全屏三角形限制到面板区域
    D3D12_RECT scissor{};
    scissor.left = (LONG)x; scissor.top = (LONG)y;
    scissor.right = (LONG)(x + w); scissor.bottom = (LONG)(y + h);
    D3D12_VIEWPORT vp{};
    vp.TopLeftX = x; vp.TopLeftY = y;
    vp.Width = (float)w; vp.Height = (float)h;
    vp.MinDepth = 0; vp.MaxDepth = 1;
    list->RSSetViewports(1, &vp);
    list->RSSetScissorRects(1, &scissor);
    // 同 Overlay11：别让我们自己这个全屏三角形污染「游戏 draw call」统计
    gNpCountingOverlayDraws = true;
    list->DrawInstanced(3, 1, 0, 0);
    gNpCountingOverlayDraws = false;

    D3D12_RESOURCE_BARRIER post = NpTransition(
        backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, bbBefore);
    list->ResourceBarrier(1, &post);

    // 面板纹理转回 COPY_DEST，供下一帧上传
    D3D12_RESOURCE_BARRIER back = NpTransition(
        tex_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    list->ResourceBarrier(1, &back);

    (void)bbW; (void)bbH;
    return true;
}

}  // namespace npg
