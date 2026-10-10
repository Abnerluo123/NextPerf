// ============================================================================
//  NextPerf 模拟游戏进程 —— DX11 后端
// ============================================================================
//
//  和旧宿主（tests/host_run.cpp）的区别，也就是这个文件存在的理由：
//    * 锁帧交给公共的 SimPacer（QPC），不是 Sleep(16) —— 后者实测只有 ~32fps
//    * Present(1,0) / Present(0,0) 可运行中切换
//    * ResizeBuffers 可在运行中调用（先释放 RTV，再重建，再重建 RTV）
//    * 每帧真的换颜色 / 转三角形，肉眼就能确认确实在出新帧
//    * 用 D3D11_QUERY_TIMESTAMP_DISJOINT + D3D11_QUERY_TIMESTAMP 测 GPU 帧时间
//
//  交换链形态刻意贴近真游戏：FLIP_DISCARD + BGRA8 + 关掉 DXGI 自动 Alt+Enter。
//  flip 模型是 PresentMon / 叠加层最常遇到、也最容易出问题的形态。
//
//  【关于着色器编译】本项目只有 zig 自带的 MinGW 头与库，没有 Windows SDK。
//  d3dcompiler_47.dll 在 Win10+ 上是系统自带的（D3D 游戏普遍动态加载它），
//  所以这里用 LoadLibrary + GetProcAddress 动态取 D3DCompile —— 这样即使某个
//  精简系统上没有这个 dll，也只是退化成「无着色器渲染路径」，而不是进程起不来。
//  两条路径都画东西，都不影响 Present / 交换链重建 / 时间戳这些被测目标。
// ============================================================================

#include "sim.h"

#include <d3d11.h>
#include <dxgi1_2.h>

#include <cmath>
#include <string>
#include <vector>

// ---------------------------------------------------------------- 逐帧颜色

void SimFrameColors(int64_t frame, bool tint, float bg[3], float fg[3]) {
    if (!tint) {
        bg[0] = 0.10f; bg[1] = 0.14f; bg[2] = 0.30f;
        fg[0] = 1.00f; fg[1] = 0.60f; fg[2] = 0.10f;
        return;
    }
    // 用黄金角推进色相：相邻帧颜色差异明显，且长时间不重复 ——
    // 截图 / 录屏时一眼就能看出「这一帧确实是新的」。
    const double kGolden = 2.39996322972865332;  // 黄金角（弧度）
    auto hsv2rgb = [](double h, float rgb[3]) {
        const double kTwoPi = 6.283185307179586;
        h = h - floor(h / kTwoPi) * kTwoPi;
        double x = h / 1.0471975511965976;  // / 60 度
        int i = (int)x;
        double f = x - i;
        double v = 0.95, s = 0.80;
        double p = v * (1 - s), q = v * (1 - s * f), u = v * (1 - s * (1 - f));
        double r, g, b;
        switch (i % 6) {
            case 0: r = v; g = u; b = p; break;
            case 1: r = q; g = v; b = p; break;
            case 2: r = p; g = v; b = u; break;
            case 3: r = p; g = q; b = v; break;
            case 4: r = u; g = p; b = v; break;
            default: r = v; g = p; b = q; break;
        }
        rgb[0] = (float)r; rgb[1] = (float)g; rgb[2] = (float)b;
    };
    hsv2rgb((double)frame * kGolden, bg);
    hsv2rgb((double)frame * kGolden + 2.6, fg);
    // 背景压暗到暗色：叠加层是半透明贴上去的，背景太亮会看不清数字。
    for (int i = 0; i < 3; ++i) bg[i] *= 0.45f;
}

// ---------------------------------------------------------------- COM 小工具

// 极简 COM 智能指针。`-fno-exceptions` 环境下用不了 shared_ptr 的花样，
// 这里只需要「离开作用域就 Release」，手写最省事也最不容易漏。
template <typename T>
struct Com {
    T* p = nullptr;
    Com() {}
    ~Com() { Reset(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    void Reset() {
        if (p) { p->Release(); p = nullptr; }
    }
    // QueryInterface / Create* 需要 T**：这里先释放旧对象，再交出地址，
    // 避免把已有对象写丢（那是内存泄漏的经典来源）。
    T** Put() {
        Reset();
        return &p;
    }
    T* operator->() const { return p; }
    T* Get() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// ---------------------------------------------------------------- 着色器

// 顶点：位置（NDC，z 用于让三角形叠在背景之上）+ 颜色。
struct Vtx {
    float x, y, z;
    float r, g, b, a;
};

// 常量缓冲：xform = {cos, sin, scale, unused}，tint = {r,g,b,unused}
// 平移放在顶点数据里（背景方块的四个角就靠它铺满屏幕）。
static const char* kVsSrc =
    "cbuffer CB : register(b0) { float4 xform; float4 tint; }\n"
    "struct VSIn  { float3 pos : POSITION; float4 col : COLOR0; };\n"
    "struct VSOut { float4 pos : SV_POSITION; float4 col : COLOR0; };\n"
    "VSOut main(VSIn i) {\n"
    "  VSOut o;\n"
    "  float c = xform.x, s = xform.y, k = xform.z;\n"
    "  float2 p = i.pos.xy * k;\n"
    "  o.pos = float4(p.x * c - p.y * s, p.x * s + p.y * c, i.pos.z, 1.0);\n"
    "  o.col = float4(i.col.rgb * tint.rgb, i.col.a);\n"
    "  return o;\n"
    "}\n";

static const char* kPsSrc =
    "struct VSOut { float4 pos : SV_POSITION; float4 col : COLOR0; };\n"
    "float4 main(VSOut i) : SV_TARGET { return i.col; }\n";

// d3dcompiler_47.dll 里的 D3DCompile。手写函数指针类型 + 动态取，
// 理由见文件头注释（不引入对 d3dcompiler 的硬导入依赖）。
typedef HRESULT(WINAPI* PFN_D3DCompile)(LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
                                        const void* pDefines, void* pInclude, LPCSTR pEntrypoint,
                                        LPCSTR pTarget, UINT Flags1, UINT Flags2, void** ppCode,
                                        void** ppErrorMsgs);

static PFN_D3DCompile LoadD3DCompile() {
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!m) m = LoadLibraryW(L"d3dcompiler.dll");
    if (!m) return nullptr;
    return (PFN_D3DCompile)GetProcAddress(m, "D3DCompile");
}

// ---------------------------------------------------------------- 后端

class Dx11Engine : public SimEngine {
public:
    bool Init(HWND hwnd, const SimConfig& cfg, std::string* err) override;
    void BeginFrame(int64_t frame_index) override;
    void EndFrame(SimFrameReport& r) override;
    bool Resize(int w, int h, std::string* err) override;
    void SetVsync(bool on) override { vsync_ = on; }
    void OnWindowModeChanged(SimWindowMode mode) override;
    void SetGpuLoadMs(double ms) override { gpu_load_ms_ = ms; }
    int SwapchainGeneration() const override { return generation_; }
    int BufferCount() const override { return buffer_count_; }
    const char* BackendName() const override { return "dx11"; }
    const char* Note() const override { return note_.c_str(); }
    bool TakeLastGpuMs(double* out) override;
    double GpuTimestampFreqHz() const override { return gpu_freq_hz_; }
    const char* GpuTimestampFreqSource() const override { return gpu_freq_source_; }
    bool GpuTimeAvailable() const override { return gpu_available_; }

private:
    bool CreateDeviceAndSwapChain(std::string* err);
    bool CreateSizeDependentResources(std::string* err);
    void DestroySizeDependentResources();
    void DrawFrame(int64_t frame_index);
    void DrawShaded(int64_t frame_index);
    void DrawFallback(int64_t frame_index);
    void PollGpuTimestamps(int64_t target_frame);

    HWND hwnd_ = nullptr;
    int width_ = 1280, height_ = 720;
    int buffer_count_ = 2;
    bool vsync_ = true;
    bool hidden_ = false;
    SimWindowMode mode_ = SimWindowMode::Windowed;
    double gpu_load_ms_ = 0.0;

    Com<IDXGISwapChain> swap_;
    Com<ID3D11Device> dev_;
    Com<ID3D11DeviceContext> ctx_;
    Com<ID3D11RenderTargetView> rtv_;
    Com<ID3D11VertexShader> vs_;
    Com<ID3D11PixelShader> ps_;
    Com<ID3D11InputLayout> layout_;
    Com<ID3D11Buffer> vb_;
    Com<ID3D11Buffer> cb_;
    bool shaded_ = false;   // 着色器路径可用？
    int fallback_ticks_ = 0;

    // ---------- GPU 时间戳 ----------
    // D3D11 的时间戳是异步的：End 之后要隔几帧才能 GetData 拿到结果
    // （立刻拿会返回 S_FALSE）。若把 S_FALSE 时的 0 当成结果，统计出来的
    // GPU 帧时间就永远是 0 —— 必须用环状缓冲延迟回读。
    static const int kQSlots = 4;
    static const int kLatency = 3;  // 延迟 3 帧回读，够绝大多数驱动算完
    struct Slot {
        Com<ID3D11Query> disjoint;
        Com<ID3D11Query> t0;
        Com<ID3D11Query> t1;
        int64_t frame = -1;
        bool begun = false;
        bool pending = false;
        bool consumed = false;
    };
    Slot slots_[kQSlots];
    int slot_ = -1;
    bool queries_ok_ = false;
    bool gpu_available_ = false;
    double gpu_freq_hz_ = 0.0;
    const char* gpu_freq_source_ = "none";
    struct GpuSample { int64_t frame; double ms; bool taken; };
    std::vector<GpuSample> gpu_samples_;

    int generation_ = 0;
    float angle_ = 0.0f;
    std::string note_;
};

// ---------------------------------------------------------------- 初始化

bool Dx11Engine::CreateDeviceAndSwapChain(std::string* err) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferDesc.Width = (UINT)width_;
    sd.BufferDesc.Height = (UINT)height_;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;  // flip 模型下最通用
    sd.BufferDesc.RefreshRate.Numerator = 0;             // 0/1 = 交给 DXGI 选
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.SampleDesc.Count = 1;  // flip 模型不支持 MSAA
    sd.SampleDesc.Quality = 0;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = (UINT)buffer_count_;
    sd.OutputWindow = hwnd_;
    sd.Windowed = (mode_ == SimWindowMode::Fullscreen) ? FALSE : TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = 0;

    D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                              D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got{};
    swap_.Reset();
    dev_.Reset();
    ctx_.Reset();

    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, fl,
                                                4, D3D11_SDK_VERSION, &sd, swap_.Put(), dev_.Put(),
                                                &got, ctx_.Put());
    if (FAILED(hr)) {
        // RDP / 虚拟机 / 无独显时拿不到 HARDWARE 设备。退 WARP 至少能让 CI 跑通
        // 功能（帧率当然不能和真显卡比，README 里已注明）。
        SimLog("D3D11 硬件设备创建失败 hr=%08lX(%s)，退回 WARP 软件设备",
               (unsigned long)hr, SimHrName((long)hr));
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, fl, 4,
                                           D3D11_SDK_VERSION, &sd, swap_.Put(), dev_.Put(), &got,
                                           ctx_.Put());
        if (FAILED(hr)) {
            *err = "D3D11CreateDeviceAndSwapChain 失败 hr=" + std::to_string((unsigned long)hr) +
                   " (" + SimHrName((long)hr) + ")";
            return false;
        }
        if (note_.find("warp") == std::string::npos) note_ += "warp-device;";
    }
    SimLog("D3D11 设备就绪 feature_level=0x%04X buffers=%d fmt=BGRA8 flip_discard", (unsigned)got,
           buffer_count_);

    // 关掉 DXGI 自带的 Alt+Enter 处理：它会自己调 SetFullscreenState 而窗口样式
    // 没跟着改，结果是尺寸错乱的全屏窗口，采到的时间没意义。
    {
        Com<IDXGIDevice> dxgiDev;
        if (SUCCEEDED(dev_->QueryInterface(IID_PPV_ARGS(dxgiDev.Put()))) && dxgiDev) {
            Com<IDXGIAdapter> adapter;
            if (SUCCEEDED(dxgiDev->GetAdapter(adapter.Put())) && adapter) {
                Com<IDXGIFactory> fac;
                if (SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(fac.Put()))) && fac) {
                    fac->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
                }
            }
        }
    }
    return true;
}

bool Dx11Engine::CreateSizeDependentResources(std::string* err) {
    Com<ID3D11Texture2D> bb;
    HRESULT hr = swap_->GetBuffer(0, IID_PPV_ARGS(bb.Put()));
    if (FAILED(hr) || !bb) {
        *err = "GetBuffer(0) 失败 hr=" + std::to_string((unsigned long)hr);
        return false;
    }
    hr = dev_->CreateRenderTargetView(bb.Get(), nullptr, rtv_.Put());
    if (FAILED(hr)) {
        *err = "CreateRenderTargetView 失败 hr=" + std::to_string((unsigned long)hr);
        return false;
    }
    D3D11_VIEWPORT vp{};
    vp.Width = (float)width_;
    vp.Height = (float)height_;
    vp.MaxDepth = 1.0f;
    ctx_->RSSetViewports(1, &vp);
    return true;
}

void Dx11Engine::DestroySizeDependentResources() {
    // ResizeBuffers 要求先释放所有对 backbuffer 的引用，否则返回
    // DXGI_ERROR_INVALID_CALL (0x887A0001)。这是交换链重建最经典的坑，
    // 也正是叠加层的 ResizeBuffers 钩子必须处理的事 —— 这里如实复现。
    rtv_.Reset();
    if (ctx_) {
        ctx_->OMSetRenderTargets(0, nullptr, nullptr);
        ctx_->Flush();  // 确保命令真的下发完，别让引用留在队列里
    }
}

bool Dx11Engine::Init(HWND hwnd, const SimConfig& cfg, std::string* err) {
    hwnd_ = hwnd;
    width_ = cfg.width;
    height_ = cfg.height;
    vsync_ = cfg.vsync;
    hidden_ = cfg.hidden;
    mode_ = cfg.window_mode;
    gpu_load_ms_ = cfg.gpu_load_ms;

    if (!CreateDeviceAndSwapChain(err)) return false;
    if (!CreateSizeDependentResources(err)) return false;

    // ---- 着色器（拿不到 d3dcompiler 就退化成 ClearView 渲染）----
    PFN_D3DCompile compile = LoadD3DCompile();
    if (!compile) {
        note_ += "no-d3dcompiler(clearview-fallback);";
        SimLog("未找到 d3dcompiler_47.dll，改用 ClearView 渲染路径（功能不受影响）");
    } else {
        Com<ID3DBlob> vsb, psb, errb;
        HRESULT hr = compile(kVsSrc, strlen(kVsSrc), "sim_vs", nullptr, nullptr, "main", "vs_4_0",
                             0, 0, (void**)vsb.Put(), (void**)errb.Put());
        if (FAILED(hr)) {
            SimLog("顶点着色器编译失败：%s",
                   errb ? (const char*)errb->GetBufferPointer() : "(无错误信息)");
        } else {
            errb.Reset();
            hr = compile(kPsSrc, strlen(kPsSrc), "sim_ps", nullptr, nullptr, "main", "ps_4_0", 0, 0,
                         (void**)psb.Put(), (void**)errb.Put());
            if (FAILED(hr)) {
                SimLog("像素着色器编译失败：%s",
                       errb ? (const char*)errb->GetBufferPointer() : "(无错误信息)");
            } else if (SUCCEEDED(dev_->CreateVertexShader(vsb->GetBufferPointer(),
                                                           vsb->GetBufferSize(), nullptr,
                                                           vs_.Put())) &&
                       SUCCEEDED(dev_->CreatePixelShader(psb->GetBufferPointer(),
                                                         psb->GetBufferSize(), nullptr, ps_.Put()))) {
                D3D11_INPUT_ELEMENT_DESC ied[] = {
                    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
                     D3D11_INPUT_PER_VERTEX_DATA, 0},
                    {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12,
                     D3D11_INPUT_PER_VERTEX_DATA, 0},
                };
                if (SUCCEEDED(dev_->CreateInputLayout(ied, 2, vsb->GetBufferPointer(),
                                                      vsb->GetBufferSize(), layout_.Put()))) {
                    // 顶点缓冲内容固定（颜色交给常量缓冲乘），一次建好永不更新。
                    Vtx tri[3] = {
                        {0.00f, 0.60f, 0.5f, 1, 1, 1, 1},
                        {0.52f, -0.38f, 0.5f, 1, 1, 1, 1},
                        {-0.52f, -0.38f, 0.5f, 1, 1, 1, 1},
                    };
                    D3D11_BUFFER_DESC bd{};
                    bd.Usage = D3D11_USAGE_IMMUTABLE;
                    bd.ByteWidth = sizeof(tri);
                    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
                    D3D11_SUBRESOURCE_DATA sub{};
                    sub.pSysMem = tri;
                    D3D11_BUFFER_DESC cbd{};
                    cbd.Usage = D3D11_USAGE_DYNAMIC;  // 每帧改角度，必须 DYNAMIC
                    cbd.ByteWidth = 32;               // 两个 float4
                    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                    if (SUCCEEDED(dev_->CreateBuffer(&bd, &sub, vb_.Put())) &&
                        SUCCEEDED(dev_->CreateBuffer(&cbd, nullptr, cb_.Put()))) {
                        shaded_ = true;
                    }
                }
            }
        }
        if (!shaded_) {
            // 部分驱动上 vs_4_0 可能不被支持，退到 vs_4_0_level_9_1 那种老模型
            // 又会让输入布局变复杂；这里直接退 ClearView 路径，简单可靠。
            note_ += "shader-path-unavailable(clearview-fallback);";
            SimLog("着色器路径不可用，改用 ClearView 渲染路径");
        }
    }

    // ---- 时间戳查询对象 ----
    {
        D3D11_QUERY_DESC qd{};
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        D3D11_QUERY_DESC td{};
        td.Query = D3D11_QUERY_TIMESTAMP;
        bool ok = true;
        for (int i = 0; i < kQSlots && ok; ++i) {
            if (FAILED(dev_->CreateQuery(&qd, slots_[i].disjoint.Put())) || !slots_[i].disjoint ||
                FAILED(dev_->CreateQuery(&td, slots_[i].t0.Put())) || !slots_[i].t0 ||
                FAILED(dev_->CreateQuery(&td, slots_[i].t1.Put())) || !slots_[i].t1) {
                ok = false;
            }
        }
        queries_ok_ = ok;
        if (!ok) {
            // 如实标记不可用，而不是编一个数字出来。
            note_ += "timestamp-query-unavailable;";
            SimLog("警告：D3D11 时间戳查询对象创建失败，GPU 帧时间将标记为不可用");
        }
    }
    generation_ = 1;
    return true;
}

// ---------------------------------------------------------------- 渲染

void Dx11Engine::DrawShaded(int64_t frame_index) {
    float bg[3], fg[3];
    SimFrameColors(frame_index, true, bg, fg);

    ID3D11RenderTargetView* rtvs[1] = {rtv_.Get()};
    ctx_->OMSetRenderTargets(1, rtvs, nullptr);
    const float clear[4] = {bg[0], bg[1], bg[2], 1.0f};
    ctx_->ClearRenderTargetView(rtv_.Get(), clear);

    UINT stride = sizeof(Vtx), off = 0;
    ID3D11Buffer* vbs[1] = {vb_.Get()};
    ctx_->IASetVertexBuffers(0, 1, vbs, &stride, &off);
    ctx_->IASetInputLayout(layout_.Get());
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->PSSetShader(ps_.Get(), nullptr, 0);
    ID3D11Buffer* cbs[1] = {cb_.Get()};
    ctx_->VSSetConstantBuffers(0, 1, cbs);

    // 本帧旋转角：按帧号推进（黄金角的小步长），每帧角度都不同。
    angle_ = (float)((double)(frame_index % 1000000) * 0.03141592653589793);

    auto update_cb = [&](float scale, const float col[3]) {
        D3D11_MAPPED_SUBRESOURCE ms{};
        if (FAILED(ctx_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return false;
        float data[8] = {cosf(angle_), sinf(angle_), scale, 0.0f, col[0], col[1], col[2], 1.0f};
        memcpy(ms.pData, data, sizeof(data));
        ctx_->Unmap(cb_.Get(), 0);
        return true;
    };

    if (update_cb(0.62f, fg)) ctx_->Draw(3, 0);

    // 模拟 GPU 压力：同一批几何多画几次。passes 按目标毫秒折算，
    // 只是「大约」——要精确的 GPU 负载得用真实 shader 循环，这里目的是
    // 让 GPU 时间戳有东西可测，而不是精确标定。
    if (gpu_load_ms_ > 0.0) {
        int passes = (int)(gpu_load_ms_ / 0.6);
        if (passes < 1) passes = 1;
        if (passes > 2000) passes = 2000;
        for (int i = 0; i < passes; ++i) {
            float s = 0.05f + 0.0005f * (float)(i % 64);
            if (!update_cb(s, fg)) break;
            ctx_->Draw(3, 0);
        }
    }
}

void Dx11Engine::DrawFallback(int64_t frame_index) {
    // 兜底渲染路径（正常情况下走不到这里）。
    //
    // 为什么这么朴素：zig 自带的 mingw d3d11.h 只声明了 D3D11.0 的
    //   ClearRenderTargetView(rtv, color)          —— 只能整屏清，没有 rect
    // 没有声明 D3D11.1 的
    //   ClearView(view, color, nrects, rects) / ClearRenderTargetView(..., rects)
    // 它们要先把上下文 QueryInterface 成 ID3D11DeviceContext1（这个接口在
    // 头文件里也没有），手写一整套 vtable 声明不值当。
    //
    // 所以这里退化成「整屏清 + 用 --gpu-load-ms 的次数做几次重复清屏」：
    // 画面仍然是**每帧不同颜色**（能确认在出帧），Present / 交换链重建 /
    // 时间戳这些真正被测的环节一个都没少。着色器路径可用时（绝大多数机器）
    // 走的是旋转三角形，画面更明显。
    float bg[3], fg[3];
    SimFrameColors(frame_index, true, bg, fg);

    ID3D11RenderTargetView* rtvs[1] = {rtv_.Get()};
    ctx_->OMSetRenderTargets(1, rtvs, nullptr);
    const float clear[4] = {bg[0], bg[1], bg[2], 1.0f};
    ctx_->ClearRenderTargetView(rtv_.Get(), clear);

    // 用前景色再叠几次（每帧的颜色都不一样），同时给 GPU 一点活干，
    // 免得 GPU 时间戳永远是 0。
    int passes = 1 + (int)(gpu_load_ms_ / 0.25);
    if (passes > 2000) passes = 2000;
    const float fg4[4] = {fg[0], fg[1], fg[2], 1.0f};
    for (int i = 1; i < passes; ++i) ctx_->ClearRenderTargetView(rtv_.Get(), fg4);
    fallback_ticks_++;
}

void Dx11Engine::DrawFrame(int64_t frame_index) {
    if (shaded_) {
        DrawShaded(frame_index);
    } else {
        DrawFallback(frame_index);
    }
}

// ---------------------------------------------------------------- 每帧

void Dx11Engine::BeginFrame(int64_t frame_index) {
    // 先回读 kLatency 帧之前那组时间戳（那时 GPU 应该已经算完了）。
    PollGpuTimestamps(frame_index - kLatency);

    // 本帧的时间戳对：t0 在「渲染前」，t1 在「渲染后、Present 前」。
    // 所以它测的是本帧的 GPU 工作量，不含 Present 阻塞 ——
    // 这一点和 DX12 后端保持同一口径，两个后端的数据才能横向比。
    if (queries_ok_) {
        slot_ = (slot_ + 1) % kQSlots;
        Slot& s = slots_[slot_];
        s.frame = frame_index;
        s.begun = false;
        s.pending = false;
        s.consumed = false;
        // 注意：ID3D11DeviceContext::Begin 返回 void（不是 HRESULT），
        // 所以这里不能用 SUCCEEDED 判断成败。
        ctx_->Begin(s.disjoint.Get());
        ctx_->End(s.disjoint.Get());  // 按 D3D11 的规定：Begin 后立刻 End 一次
        ctx_->End(s.t0.Get());
        s.begun = true;
    }
}

void Dx11Engine::PollGpuTimestamps(int64_t target_frame) {
    if (!queries_ok_ || target_frame < 0) return;
    for (int i = 0; i < kQSlots; ++i) {
        Slot& s = slots_[i];
        if (!s.pending || s.frame != target_frame) continue;
        const UINT kNoFlush = D3D11_ASYNC_GETDATA_DONOTFLUSH;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        HRESULT hr = ctx_->GetData(s.disjoint.Get(), &dj, sizeof(dj), kNoFlush);
        if (hr == S_FALSE) continue;  // 还没算完，下一帧再看
        s.pending = false;
        if (FAILED(hr)) continue;
        if (dj.Disjoint) {
            // Disjoint=TRUE 表示这段时间里 GPU 时钟被打断过（改频、切电源状态、
            // 进出独占全屏都会）。这一对时间戳不可信，如实丢掉，不参与任何平均。
            SimLog("帧 %lld 的 GPU 时间戳被标记为 Disjoint，丢弃该样本", (long long)s.frame);
            continue;
        }
        UINT64 t0 = 0, t1 = 0;
        if (FAILED(ctx_->GetData(s.t0.Get(), &t0, sizeof(t0), kNoFlush))) continue;
        if (FAILED(ctx_->GetData(s.t1.Get(), &t1, sizeof(t1), kNoFlush))) continue;
        gpu_available_ = true;
        gpu_freq_hz_ = (double)dj.Frequency;
        gpu_freq_source_ = "timestamp-disjoint";
        if (t1 > t0 && dj.Frequency > 0) {
            GpuSample gs;
            gs.frame = s.frame;
            gs.ms = (double)(t1 - t0) * 1000.0 / (double)dj.Frequency;
            gs.taken = false;
            gpu_samples_.push_back(gs);
        }
    }
    // 防止长时间运行下样本无限增长（正常情况下每帧都会被 TakeLastGpuMs 取走）
    if (gpu_samples_.size() > 4096) {
        gpu_samples_.erase(gpu_samples_.begin(), gpu_samples_.begin() + 2048);
    }
}

bool Dx11Engine::TakeLastGpuMs(double* out) {
    if (!out) return false;
    // 取最近一个尚未被别人拿走的样本。主循环每帧只调一次，所以这里
    // 「最新的未取走样本」就是本帧 GPU 时间的匹配项。
    for (size_t i = gpu_samples_.size(); i > 0; --i) {
        GpuSample& gs = gpu_samples_[i - 1];
        if (gs.taken) continue;
        gs.taken = true;
        *out = gs.ms;
        return true;
    }
    return false;
}

void Dx11Engine::EndFrame(SimFrameReport& r) {
    if (rtv_) {
        DrawFrame(r.frame_index);
    }

    // 结束时间戳对（放在 Present 之前）
    if (queries_ok_ && slot_ >= 0 && slots_[slot_].begun) {
        Slot& q = slots_[slot_];
        ctx_->End(q.t1.Get());
        q.pending = true;
    }

    // ---- Present ----
    // 刻意用最原始的 IDXGISwapChain::Present(sync, flags)：这正是 NextPerf
    // 挂钩子的函数，模拟进程必须给出和真游戏一模一样的调用序列与参数。
    UINT sync = vsync_ ? 1 : 0;
    int64_t p0 = SimQpcNow();
    HRESULT hr = swap_->Present(sync, 0);
    int64_t p1 = SimQpcNow();

    r.present_ms = SimQpcToMs(p1 - p0);
    r.present_hr = (long)hr;
    r.present_failed = FAILED(hr);
    if (FAILED(hr)) {
        SimLog("Present 失败 hr=%08lX(%s)", (unsigned long)hr, SimHrName((long)hr));
    } else if (hr == DXGI_STATUS_OCCLUDED) {
        // 窗口被完全遮挡 / 最小化时 DXGI 立刻返回这个值，Present 不阻塞，
        // 此时测出来的「帧率」会虚高 —— 脚本要能看出来。
        static bool warned = false;
        if (!warned) {
            SimLog("Present 返回 DXGI_STATUS_OCCLUDED（窗口被遮挡，帧率不可信）");
            warned = true;
        }
    }
}

bool Dx11Engine::Resize(int w, int h, std::string* err) {
    if (!swap_ || !dev_) {
        *err = "交换链不存在";
        return false;
    }
    DestroySizeDependentResources();

    HRESULT hr = swap_->ResizeBuffers((UINT)buffer_count_, (UINT)w, (UINT)h,
                                      DXGI_FORMAT_B8G8R8A8_UNORM, 0);
    if (hr == DXGI_ERROR_INVALID_CALL) {
        // 十有八九是有别的对象还持有 backbuffer 引用（叠加层 / 调试层 /
        // 忘了 Release 的 view）。真游戏里同样会遇到，如实报出来不静默重试。
        SimLog("ResizeBuffers 返回 DXGI_ERROR_INVALID_CALL —— 通常意味着还有对象持有"
               "backbuffer 引用（叠加层/调试层/未释放的 view）");
    }
    if (FAILED(hr)) {
        // 退路：整条交换链重建。代价大，但能让测试继续进行下去，
        // 而且 generation_ 会 +1，脚本能看出「这次是重建不是 ResizeBuffers」。
        SimLog("ResizeBuffers 失败 hr=%08lX(%s)，退回「重建整条交换链」", (unsigned long)hr,
               SimHrName((long)hr));
        if (!CreateDeviceAndSwapChain(err)) {
            width_ = w;
            height_ = h;
            return false;
        }
    }
    width_ = w;
    height_ = h;
    if (!CreateSizeDependentResources(err)) return false;
    ++generation_;
    SimLog("交换链已重设到 %dx%d（generation=%d）", w, h, generation_);
    return true;
}

void Dx11Engine::OnWindowModeChanged(SimWindowMode mode) {
    mode_ = mode;
    if (!swap_) return;
    if (mode == SimWindowMode::Fullscreen) {
        // 独占全屏：flip 模型要求先回到窗口化再切（调用方已经做了），
        // 且切换之后必须立刻 ResizeBuffers —— 调用方紧接着就会调 Resize()。
        HRESULT hr = swap_->SetFullscreenState(TRUE, nullptr);
        if (FAILED(hr)) {
            SimLog("SetFullscreenState(TRUE) 失败 hr=%08lX(%s) —— 保持窗口化",
                   (unsigned long)hr, SimHrName((long)hr));
        } else {
            SimLog("已进入 DXGI 独占全屏");
        }
    } else {
        HRESULT hr = swap_->SetFullscreenState(FALSE, nullptr);
        if (FAILED(hr)) {
            SimLog("SetFullscreenState(FALSE) 失败 hr=%08lX(%s)", (unsigned long)hr,
                   SimHrName((long)hr));
        }
    }
}

// ---------------------------------------------------------------- 工厂

SimEngine* SimCreateEngineDx11() { return new Dx11Engine(); }
