// ============================================================================
//  NextPerf 模拟游戏进程 —— DX12 后端
// ============================================================================
//
//  完整的 DX12 帧循环：命令队列 / 交换链（flip 模型）/ RTV 描述符堆 /
//  命令分配器 + 命令列表 / fence 同步 / 每帧引用环。
//
//  GPU 帧时间的测法与 DX11 后端保持同一口径，这样两个后端的数据能横着比：
//    时间戳 A = 本帧命令列表开头
//    时间戳 B = 本帧命令列表结尾（Present 之前）
//    gpu_frame_ms = (B - A) / 频率 * 1000   —— 本帧的 GPU 工作量，不含 Present 阻塞
//  （Present 本身的耗时由 present_ms 覆盖：它是 CPU 侧的阻塞时长，
//    才是「这一帧到底等了多久」的答案。）
//
//  频率换算用 ID3D12CommandQueue::GetClockCalibration：它一次调用同时给出
//  GPU 时间戳和对应的 CPU QPC 计数，两次采样求差就是 GPU 时间戳频率。
//  这是 DX12 里**唯一保证可用**的换算办法（D3D12 没有暴露独立的
//  GetTimestampFrequency）。
//
//  【这个文件里没有的东西，如实说明】
//    * 没有 PS 常量/顶点缓冲：DX12 的 PSO 需要序列化好的 DXIL，本项目只有
//      zig 自带的 MinGW 头，没有 dxc/d3dcompiler 能产出 DXIL 的路径。
//      所以渲染用 ClearRenderTargetView（整屏）+ ClearView（矩形），
//      逐帧改变颜色与矩形位置。画面朴素，但 Present / 交换链重建 /
//      资源状态转换 / 时间戳这些**被测环节一个都不少**。
//    * 不支持独占全屏（WantsExclusiveFullscreen() 返回 false）：现代 DX12
//      游戏普遍用 borderless 模拟全屏，这里就按那条路走。
// ============================================================================

#include "sim.h"

#include <d3d12.h>
#include <dxgi1_4.h>

#include <cmath>
#include <string>
#include <vector>

// 逐帧颜色（与 DX11 后端同一份算法，实现在 sim_dx11.cpp；
// 这里只声明，不重复实现，保证两个后端画面口径一致）。
void SimFrameColors(int64_t frame, bool tint, float bg[3], float fg[3]);

// ---------------------------------------------------------------- COM 小工具

template <typename T>
struct Com {
    T* p = nullptr;
    Com() {}
    ~Com() { Reset(); }
    Com(const Com&) = delete;
    // 移动赋值：把所有权从临时对象搬过来（AddRef/Release 都省了）。
    // Com<IDXGIFactory4> fac = ...; factory_ = fac; 这种写法很自然，
    // 没有它就得写 factory_ = Com<...>(fac.Get()) 之类的绕路。
    Com& operator=(const Com& other) {
        if (this != &other) {
            if (other.p) other.p->AddRef();
            Reset();
            p = other.p;
        }
        return *this;
    }
    void Reset() {
        if (p) { p->Release(); p = nullptr; }
    }
    T** Put() {
        Reset();
        return &p;
    }
    T* operator->() const { return p; }
    T* Get() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// ---------------------------------------------------------------- 后端

class Dx12Engine : public SimEngine {
public:
    bool Init(HWND hwnd, const SimConfig& cfg, std::string* err) override;
    void BeginFrame(int64_t frame_index) override;
    void EndFrame(SimFrameReport& r) override;
    bool Resize(int w, int h, std::string* err) override;
    void SetVsync(bool on) override { vsync_ = on; }
    bool OnWindowModeChanged(SimWindowMode mode) override {
        // DX12 后端不走 DXGI 独占全屏（WantsExclusiveFullscreen() = false），
        // 所以这里只需要记下模式；公共代码已经把窗口调成无边框全屏尺寸，
        // 交换链也跟着 ResizeBuffers 过了 —— 这就是「borderless 全屏」。
        mode_ = mode;
        return true;
    }
    void SetGpuLoadMs(double ms) override { gpu_load_ms_ = ms; }
    int SwapchainGeneration() const override { return generation_; }
    int BufferCount() const override { return buffer_count_; }
    SimWindowMode CurrentWindowMode() const override { return mode_; }
    const char* BackendName() const override { return "dx12"; }
    const char* Note() const override;    bool TakeLastGpuMs(double* out) override;
    double GpuTimestampFreqHz() const override { return gpu_freq_hz_; }
    const char* GpuTimestampFreqSource() const override { return gpu_freq_source_; }
    bool GpuTimeAvailable() const override { return gpu_available_; }
    // 现代 DX12 游戏用 borderless 模拟全屏，不走 DXGI 独占全屏。
    bool WantsExclusiveFullscreen() const override { return false; }

private:
    static const int kMaxBuffers = 4;   // 交换链 buffer 上限
    static const int kFrameRing = 3;    // 每帧资源环大小（也是时间戳回读的延迟帧数）
    static const int kQPerFrame = 2;    // 每帧 2 个时间戳：命令列表开头 / 结尾

    struct FrameRes {
        Com<ID3D12CommandAllocator> alloc;
        // PostPresent 用**另一个**分配器：同一帧里 Present 之后还要再开一条
        // 命令列表打时间戳 + Resolve，而 D3D12 不允许在同一个分配器上存在
        // 两条**同时活着**的列表（第二条 Reset 会打断第一条）。
        Com<ID3D12CommandAllocator> alloc_post;
        Com<ID3D12GraphicsCommandList> list_post;  // 复用同一对象，避免每帧重建
        Com<ID3D12Resource> readback;        // 时间戳回读缓冲（READBACK 堆）
        int64_t gpu_frame = -1;              // 这一槽记录的是哪一帧
        UINT64 fence_value = 0;              // 该帧两条列表都提交后的 fence 值（0=未提交）
    };

    bool CreateDeviceAndQueue(std::string* err);
    bool CreateSwapChain(std::string* err);
    bool CreateFrameResources(std::string* err);
    void ReleaseBackBuffers();
    bool WaitForFrameRing(int slot, DWORD timeout_ms);
    void ReadSlotTimestamps(int slot);
    void DrawFrame(int64_t frame_index, ID3D12GraphicsCommandList* list);

    HWND hwnd_ = nullptr;
    int width_ = 1280, height_ = 720;
    int buffer_count_ = 3;
    bool vsync_ = true;
    bool hidden_ = false;
    SimWindowMode mode_ = SimWindowMode::Windowed;
    double gpu_load_ms_ = 0.0;

    Com<IDXGIFactory4> factory_;
    Com<IDXGISwapChain3> swap_;
    Com<ID3D12Device> dev_;
    Com<ID3D12CommandQueue> queue_;
    Com<ID3D12DescriptorHeap> rtv_heap_;
    Com<ID3D12GraphicsCommandList> list_;
    Com<ID3D12Fence> fence_;
    Com<ID3D12QueryHeap> ts_heap_;
    Com<ID3D12Resource> ts_resolve_;    // ResolveQueryData 的目标（DEFAULT 堆）
    Com<ID3D12Resource> bb_[kMaxBuffers];

    FrameRes ring_[kFrameRing];
    UINT rtv_inc_ = 0;
    UINT64 fence_value_ = 0;
    HANDLE fence_event_ = nullptr;
    int slot_ = -1;

    bool gpu_available_ = false;
    double gpu_freq_hz_ = 0.0;
    const char* gpu_freq_source_ = "none";
    struct GpuSample { int64_t frame; double render_ms; bool taken; };
    std::vector<GpuSample> gpu_samples_;

    int generation_ = 0;
    std::string note_;
    // 时间戳回读的计数（诊断用，退出时打进汇总的 note）
    int ts_read_attempt_ = 0, ts_read_ok_ = 0, ts_read_bad_ = 0, ts_read_map_fail_ = 0,
        ts_read_fence_wait_ = 0;
};

// ---------------------------------------------------------------- 初始化

bool Dx12Engine::CreateDeviceAndQueue(std::string* err) {
    Com<IDXGIFactory4> fac;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(fac.Put()));
    if (FAILED(hr) || !fac) {
        *err = "CreateDXGIFactory1 失败 hr=" + std::to_string((unsigned long)hr);
        return false;
    }
    // 挑第一个能建出 D3D12 设备的适配器：混显笔记本上第一个不一定是能用的那块。
    Com<ID3D12Device> dev;
    Com<IDXGIAdapter1> chosen;
    for (UINT i = 0; fac->EnumAdapters1(i, chosen.Put()) == S_OK; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        chosen->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;  // 跳过 WARP，真跑硬件
        if (SUCCEEDED(D3D12CreateDevice(chosen.Get(), D3D_FEATURE_LEVEL_11_0,
                                        IID_PPV_ARGS(dev.Put())))) {
            char name[160] = {0};
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name) - 1, nullptr,
                                nullptr);
            SimLog("D3D12 适配器：%s（显存 %llu MB）", name,
                   (unsigned long long)(desc.DedicatedVideoMemory / (1024 * 1024)));
            break;
        }
        dev.Reset();
    }
    if (!dev) {
        // 退路：WARP。CI / 无独显环境要能跑通功能，帧率不具参考性。
        Com<IDXGIAdapter1> warp;
        if (SUCCEEDED(fac->EnumWarpAdapter(IID_PPV_ARGS(warp.Put())))) {
            if (SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0,
                                            IID_PPV_ARGS(dev.Put())))) {
                note_ += "warp-device;";
                SimLog("D3D12 退回 WARP 软件设备");
            }
        }
    }
    if (!dev) {
        *err = "D3D12CreateDevice 失败（硬件与 WARP 都没成功）";
        return false;
    }
    factory_ = fac;
    dev_ = dev;

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    hr = dev_->CreateCommandQueue(&qd, IID_PPV_ARGS(queue_.Put()));
    if (FAILED(hr)) {
        *err = "CreateCommandQueue 失败 hr=" + std::to_string((unsigned long)hr);
        return false;
    }
    hr = dev_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence_.Put()));
    if (FAILED(hr)) {
        *err = "CreateFence 失败";
        return false;
    }
    fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event_) {
        *err = "CreateEvent 失败";
        return false;
    }
    // 命令列表在 D3D12 里允许先建一个「空」的，之后每帧 Reset 复用，
    // 避免每帧 Create/Release 的开销。
    //
    // 关于 pCommandAllocator=nullptr：D3D12 文档说可以传 nullptr 表示
    // 「还没有分配器，第一次 Reset 时必须给一个」，但实测在 NVIDIA 驱动
    // （RTX 5080 Laptop，Blackwell）上这个写法会返回 E_INVALIDARG。
    // 所以这里先造一个临时分配器把列表建出来 —— 效果一样，兼容性更好。
    // 必须成对记住：Close 之后才能 Reset，Reset 之前必须已经 Close。
    {
        Com<ID3D12CommandAllocator> tmp_alloc;
        hr = dev_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          IID_PPV_ARGS(tmp_alloc.Put()));
        if (FAILED(hr)) {
            *err = "CreateCommandAllocator(临时) 失败 hr=" + std::to_string((unsigned long)hr);
            return false;
        }
        hr = dev_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, tmp_alloc.Get(), nullptr,
                                     IID_PPV_ARGS(list_.Put()));
        if (FAILED(hr)) {
            *err = "CreateCommandList(主列表) 失败 hr=" + std::to_string((unsigned long)hr) + " (" +
                   SimHrName((long)hr) + ")";
            return false;
        }
        // tmp_alloc 在这里析构：列表已经建好，之后每帧 Reset 时会绑定别的分配器。
        // D3D12 允许命令列表换分配器（只要在 Reset 时给新的）。
    }
    hr = list_->Close();
    if (FAILED(hr)) {
        *err = "初始命令列表 Close 失败 hr=" + std::to_string((unsigned long)hr);
        return false;
    }
    SimLog("D3D12 队列 / fence / 命令列表就绪");
    return true;
}

bool Dx12Engine::CreateSwapChain(std::string* err) {
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = (UINT)width_;
    sd.Height = (UINT)height_;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;   // flip 模型下最通用
    sd.Stereo = FALSE;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = (UINT)buffer_count_;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    sd.Flags = 0;   // 不开 ALLOW_TEARING：撕裂行为会让 Present 语义变得难以对照

    Com<IDXGISwapChain1> sc1;
    HRESULT hr = factory_->CreateSwapChainForHwnd(queue_.Get(), hwnd_, &sd, nullptr, nullptr,
                                                  sc1.Put());
    if (FAILED(hr)) {
        *err = "CreateSwapChainForHwnd 失败 hr=" + std::to_string((unsigned long)hr) + " (" +
               SimHrName((long)hr) + ")";
        return false;
    }
    if (FAILED(sc1->QueryInterface(IID_PPV_ARGS(swap_.Put()))) || !swap_) {
        *err = "QueryInterface(IDXGISwapChain3) 失败";
        return false;
    }
    // 同样关掉 DXGI 自带的 Alt+Enter：它会绕过我们的窗口样式管理。
    factory_->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    generation_++;
    return true;
}

void Dx12Engine::ReleaseBackBuffers() {
    // ResizeBuffers 之前必须释放所有 backbuffer 引用，否则返回
    // DXGI_ERROR_INVALID_CALL —— 这是交换链重建最经典的坑，也正是叠加层
    // ResizeBuffers 钩子必须处理的场景，这里如实复现。
    for (int i = 0; i < kMaxBuffers; ++i) bb_[i].Reset();
}

bool Dx12Engine::CreateFrameResources(std::string* err) {
    // RTV 描述符堆
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = (UINT)buffer_count_;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;  // RTV 堆不参与 shader 可见绑定
    HRESULT hr = dev_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(rtv_heap_.Put()));
    if (FAILED(hr)) {
        *err = "CreateDescriptorHeap(RTV) 失败";
        return false;
    }
    rtv_inc_ = dev_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    for (int i = 0; i < buffer_count_; ++i) {
        hr = swap_->GetBuffer((UINT)i, IID_PPV_ARGS(bb_[i].Put()));
        if (FAILED(hr) || !bb_[i]) {
            *err = "交换链 GetBuffer(" + std::to_string(i) + ") 失败";
            return false;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE rh = h;
        rh.ptr += (SIZE_T)i * rtv_inc_;
        dev_->CreateRenderTargetView(bb_[i].Get(), nullptr, rh);
    }

    // 每帧的命令分配器 + 时间戳回读缓冲
    for (int i = 0; i < kFrameRing; ++i) {
        FrameRes& f = ring_[i];
        HRESULT hr = dev_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(f.alloc.Put()));
        if (FAILED(hr)) {
            *err = "CreateCommandAllocator 失败（第 " + std::to_string(i) + " 个）";
            return false;
        }
        hr = dev_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          IID_PPV_ARGS(f.alloc_post.Put()));
        if (FAILED(hr)) {
            *err = "CreateCommandAllocator(post) 失败（第 " + std::to_string(i) + " 个）";
            return false;
        }
        hr = dev_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, f.alloc_post.Get(), nullptr,
                                     IID_PPV_ARGS(f.list_post.Put()));
        if (FAILED(hr)) {
            *err = "CreateCommandList(post) 失败（第 " + std::to_string(i) + " 个）";
            return false;
        }
        f.list_post->Close();
        f.gpu_frame = -1;
        f.fence_value = 0;
    }

    // 时间戳查询堆：kFrameRing 帧 × kQPerFrame 个时间戳
    {
        D3D12_QUERY_HEAP_DESC qhd{};
        qhd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qhd.Count = (UINT)(kFrameRing * kQPerFrame);
        qhd.NodeMask = 0;
        hr = dev_->CreateQueryHeap(&qhd, IID_PPV_ARGS(ts_heap_.Put()));
        if (FAILED(hr)) {
            // 极少数驱动 / WARP 上时间戳堆建不出来。如实标记不可用，
            // 其余功能（帧率、Present 阻塞、交换链重建）全都照跑。
            note_ += "timestamp-heap-unavailable;";
            SimLog("警告：CreateQueryHeap(TIMESTAMP) 失败 hr=%08X，GPU 帧时间标记为不可用",
                   (unsigned)hr);
            ts_heap_.Reset();
        } else {
            const UINT64 kBytes = (UINT64)(kFrameRing * kQPerFrame) * sizeof(UINT64);
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = kBytes;
            rd.Height = 1;
            rd.DepthOrArraySize = 1;
            rd.MipLevels = 1;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            hr = dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                               D3D12_RESOURCE_STATE_COMMON, nullptr,
                                               IID_PPV_ARGS(ts_resolve_.Put()));
            if (FAILED(hr)) {
                note_ += "timestamp-resolve-buffer-failed;";
                ts_heap_.Reset();
                ts_resolve_.Reset();
            } else {
                // 每帧一个 UPLOAD（readback）缓冲，映射后直接读时间戳
                D3D12_HEAP_PROPERTIES hp2{};
                hp2.Type = D3D12_HEAP_TYPE_READBACK;
                for (int i = 0; i < kFrameRing; ++i) {
                    hr = dev_->CreateCommittedResource(&hp2, D3D12_HEAP_FLAG_NONE, &rd,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(ring_[i].readback.Put()));
                    if (FAILED(hr)) {
                        note_ += "timestamp-readback-failed;";
                        ts_heap_.Reset();
                        ts_resolve_.Reset();
                        for (int j = 0; j < kFrameRing; ++j) ring_[j].readback.Reset();
                        break;
                    }
                }
            }
        }
    }

    // 用 GetClockCalibration 求 GPU 时间戳频率：它一次调用同时给出 GPU 时间戳
    // 与对应的 CPU QPC 计数，两者相除即得频率。
    // 做法：采样两次（中间隔几毫秒），用差值算，避免单次采样的量化误差。
    if (ts_heap_) {
        UINT64 g0 = 0, c0 = 0, g1 = 0, c1 = 0;
        if (SUCCEEDED(queue_->GetClockCalibration(&g0, &c0))) {
            Sleep(12);
            if (SUCCEEDED(queue_->GetClockCalibration(&g1, &c1)) && g1 > g0 && c1 > c0) {
                double gpu_hz = (double)(g1 - g0) / SimQpcToMs((int64_t)(c1 - c0)) * 1000.0;
                if (gpu_hz > 1.0 && gpu_hz < 1e12) {
                    gpu_freq_hz_ = gpu_hz;
                    gpu_freq_source_ = "clock-calibration";
                }
            }
        }
        if (gpu_freq_hz_ <= 0.0) {
            // 采样失败时不编数字：把频率标为 0，GPU 时间一律报不可用。
            note_ += "clock-calibration-failed;";
            SimLog("警告：GetClockCalibration 采样失败，GPU 时间戳无法换算成毫秒");
        } else {
            SimLog("GPU 时间戳频率 ≈ %.0f Hz（来源：GetClockCalibration）", gpu_freq_hz_);
        }
    }
    return true;
}

bool Dx12Engine::Init(HWND hwnd, const SimConfig& cfg, std::string* err) {
    hwnd_ = hwnd;
    width_ = cfg.width;
    height_ = cfg.height;
    vsync_ = cfg.vsync;
    hidden_ = cfg.hidden;
    mode_ = cfg.window_mode;
    gpu_load_ms_ = cfg.gpu_load_ms;
    buffer_count_ = cfg.buffers;
    if (buffer_count_ < 2) buffer_count_ = 2;
    if (buffer_count_ > kMaxBuffers) buffer_count_ = kMaxBuffers;

    if (!CreateDeviceAndQueue(err)) return false;
    if (!CreateSwapChain(err)) return false;
    if (!CreateFrameResources(err)) return false;
    SimLog("D3D12 就绪 buffers=%d fmt=BGRA8 flip_discard", buffer_count_);
    return true;
}

// ---------------------------------------------------------------- 同步

bool Dx12Engine::WaitForFrameRing(int slot, DWORD timeout_ms) {
    FrameRes& f = ring_[slot];
    if (f.fence_value == 0) return true;  // 这一槽还没用过
    if (fence_->GetCompletedValue() >= f.fence_value) return true;
    if (FAILED(fence_->SetEventOnCompletion(f.fence_value, fence_event_))) return false;
    DWORD w = WaitForSingleObject(fence_event_, timeout_ms);
    if (w != WAIT_OBJECT_0) {
        SimLog("等待 fence 超时（slot=%d value=%llu）—— GPU 可能挂了", slot,
               (unsigned long long)f.fence_value);
        return false;
    }
    return true;
}

// 把某一槽上一次提交的时间戳读出来。
//
// 同步关系（这里必须严格，否则会读到还没写完的显存）：
//   这一槽的数据是「上一轮用它的那一帧」写进去的，对应的 fence 值存在
//   f.fence_value 里。BeginFrame 已经 WaitForFrameRing(slot) 等过它，
//   所以进到这里时 GPU 一定写完了 —— 前提是 fence_value 在此之前没有被
//   覆盖。这就是 EndFrame 里「fence_value 在读完之前不能改」的原因。
void Dx12Engine::ReadSlotTimestamps(int slot) {
    FrameRes& f = ring_[slot];
    if (!f.readback || !ts_heap_ || f.gpu_frame < 0) return;
    // 双保险：万一 WaitForFrameRing 超时了，这里再确认一次 fence 状态。
    if (f.fence_value != 0 && fence_->GetCompletedValue() < f.fence_value) {
        ts_read_fence_wait_++;
        return;
    }

    void* mapped = nullptr;
    // Map 的读取范围必须从**这一槽自己的字节偏移**开始。
    // 踩过的坑：回读缓冲是 kFrameRing * kQPerFrame 个 UINT64，每槽的数据在
    // 不同偏移上；如果写成 {0, 32}，槽 1/2 读到的永远是槽 0 那份数据
    // （表现为「只有 1/3 的帧有 GPU 时间戳」）。
    const UINT64 off = (UINT64)slot * kQPerFrame * sizeof(UINT64);
    D3D12_RANGE range{(SIZE_T)off, (SIZE_T)(off + kQPerFrame * sizeof(UINT64))};
    if (FAILED(f.readback->Map(0, &range, &mapped)) || !mapped) {
        ts_read_map_fail_++;
        return;
    }
    const UINT64* ts = (const UINT64*)((const unsigned char*)mapped + off);
    ts_read_attempt_++;
    // ts[0] = 命令列表开头，ts[1] = 命令列表结尾（Present 之前）
    if (gpu_freq_hz_ > 0.0 && ts[1] >= ts[0] && ts[0] != 0) {
        GpuSample s;
        s.frame = f.gpu_frame;
        s.render_ms = (double)(ts[1] - ts[0]) * 1000.0 / gpu_freq_hz_;
        s.taken = false;
        gpu_samples_.push_back(s);
        gpu_available_ = true;
        ts_read_ok_++;
    } else {
        ts_read_bad_++;
        if (ts_read_bad_ <= 3) {
            SimLog("时间戳回读异常：t0=%llu t1=%llu freq=%.0f", (unsigned long long)ts[0],
                   (unsigned long long)ts[1], gpu_freq_hz_);
        }
    }
    D3D12_RANGE empty{0, 0};
    f.readback->Unmap(0, &empty);
}

// ---------------------------------------------------------------- 渲染

void Dx12Engine::DrawFrame(int64_t frame_index, ID3D12GraphicsCommandList* list) {
    float bg[3], fg[3];
    SimFrameColors(frame_index, true, bg, fg);

    const UINT idx = swap_->GetCurrentBackBufferIndex();
    const UINT rtv_index = idx < (UINT)buffer_count_ ? idx : 0;

    // 资源屏障：flip 模型里 backbuffer 平时处于 PRESENT 状态。
    // 第一次拿到某块 buffer 时它还是 COMMON（刚创建 / 刚 ResizeBuffers），
    // 所以 StateBefore 要按实际情况给，给错了调试层会直接报错。
    D3D12_RESOURCE_STATES before = D3D12_RESOURCE_STATE_PRESENT;
    D3D12_RESOURCE_BARRIER bar{};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    bar.Transition.pResource = bb_[rtv_index].Get();
    bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bar.Transition.StateBefore = before;
    bar.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    list->ResourceBarrier(1, &bar);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)rtv_index * rtv_inc_;
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    const float clear[4] = {bg[0], bg[1], bg[2], 1.0f};
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);

    // 逐帧变化的矩形：4 个绕中心旋转的方块。颜色每帧也变。
    {
        const float fg4[4] = {fg[0], fg[1], fg[2], 1.0f};
        const float br4[4] = {fg[0] * 0.5f + 0.3f, fg[1] * 0.5f + 0.3f, fg[2] * 0.5f + 0.3f, 1.0f};
        const FLOAT fw = (FLOAT)width_, fh = (FLOAT)height_;
        float ang = (float)((double)(frame_index % 1000000) * 0.03141592653589793);
        for (int i = 0; i < 4; ++i) {
            double a = (double)ang + i * 1.5707963267948966;
            FLOAT cx = fw * (FLOAT)(0.5 + 0.22 * cos(a));
            FLOAT cy = fh * (FLOAT)(0.5 + 0.22 * sin(a));
            D3D12_RECT rc{};
            rc.left = (LONG)(cx - fw / 8);
            rc.top = (LONG)(cy - fh / 8);
            rc.right = (LONG)(cx + fw / 8);
            rc.bottom = (LONG)(cy + fh / 8);
            if (rc.right <= rc.left || rc.bottom <= rc.top) continue;
            list->ClearRenderTargetView(rtv, fg4, 1, &rc);

            D3D12_RECT rb{};
            rb.left = 0;
            rb.top = (LONG)(i * fh / 24);
            rb.right = (LONG)(fw / 6);
            rb.bottom = (LONG)((i + 1) * fh / 24);
            list->ClearRenderTargetView(rtv, br4, 1, &rb);
        }
    }

    // 模拟 GPU 压力：额外的整屏 Clear。次数按目标毫秒折算，只是「大约」——
    // 目的是让 GPU 时间戳有东西可测、能把 GPU 顶到接近满载，不是精确标定。
    if (gpu_load_ms_ > 0.0) {
        int passes = (int)(gpu_load_ms_ / 0.25);
        if (passes < 1) passes = 1;
        if (passes > 4000) passes = 4000;
        for (int i = 0; i < passes; ++i) {
            float c[4] = {bg[2], bg[0], bg[1], 1.0f};
            list->ClearRenderTargetView(rtv, c, 0, nullptr);
        }
    }

    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bar.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    list->ResourceBarrier(1, &bar);
}

// ---------------------------------------------------------------- 每帧

void Dx12Engine::BeginFrame(int64_t frame_index) {
    // 复用槽位前先确认 GPU 已经把上一轮这一槽的两条命令列表都执行完 ——
    // D3D12 明确禁止在 GPU 还在用某个分配器时 Reset 它。
    // 这一步同时也是「时间戳回读安全」的前提。
    const int next_slot = (slot_ + 1) % kFrameRing;
    WaitForFrameRing(next_slot, 2000);
    slot_ = next_slot;
    FrameRes& f = ring_[slot_];

    // 上一轮这一槽写进去的时间戳现在可以安全读了。
    ReadSlotTimestamps(slot_);
    f.gpu_frame = -1;

    HRESULT hr = f.alloc->Reset();
    if (FAILED(hr)) SimLog("命令分配器 Reset 失败 hr=%08lX", (unsigned long)hr);
    hr = list_->Reset(f.alloc.Get(), nullptr);
    if (FAILED(hr)) {
        SimLog("命令列表 Reset 失败 hr=%08lX", (unsigned long)hr);
        return;
    }

    // 帧首时间戳
    if (ts_heap_) {
        const UINT base = (UINT)slot_ * kQPerFrame;
        list_->EndQuery(ts_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, base + 0);
    }
}

void Dx12Engine::EndFrame(SimFrameReport& r) {
    FrameRes& f = ring_[slot_];
    const UINT base = (UINT)slot_ * kQPerFrame;

    DrawFrame(r.frame_index, list_.Get());

    // 渲染结束时间戳
    if (ts_heap_) list_->EndQuery(ts_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, base + 1);
    list_->Close();

    ID3D12CommandList* lists[1] = {list_.Get()};
    queue_->ExecuteCommandLists(1, lists);

    // ---- Present ----
    // DX12 用 flip 模型 + sync interval 控制垂直同步：1=等垂直消隐，0=不等。
    UINT sync = vsync_ ? 1u : 0u;
    int64_t p0 = SimQpcNow();
    HRESULT hr = swap_->Present(sync, 0);
    int64_t p1 = SimQpcNow();

    r.present_ms = SimQpcToMs(p1 - p0);
    r.present_hr = (long)hr;
    r.present_failed = FAILED(hr);
    if (FAILED(hr)) SimLog("Present 失败 hr=%08lX(%s)", (unsigned long)hr, SimHrName((long)hr));

    // 把这一帧的 2 个时间戳 Resolve 到 DEFAULT 缓冲，再拷到 READBACK 缓冲。
    // 必须用一条**新的**命令列表（上一条已经 Close 并提交），并且让 Resolve
    // 和这 2 个时间戳在同一条列表里，才能保证 Resolve 一定发生在它们之后。
    //
    // 为什么放在 Present 之后而不是之前：Resolve 要读时间戳，而 Present 之前
    // 那条列表必须尽量"干净"（真实游戏的渲染列表就是这个样子，叠加层也是在
    // 这个位置插自己的时间戳）。把 Resolve/回读的开销挪到 Present 之后，
    // 我们测到的 CPU 帧时间就不会被自己的测量代码污染。
    if (ts_heap_ && f.list_post) {
        f.alloc_post->Reset();
        if (SUCCEEDED(f.list_post->Reset(f.alloc_post.Get(), nullptr))) {
            ID3D12GraphicsCommandList* l2 = f.list_post.Get();

            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = ts_resolve_.Get();
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            l2->ResourceBarrier(1, &b);
            l2->ResolveQueryData(ts_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, base, kQPerFrame,
                                 ts_resolve_.Get(), (UINT64)base * sizeof(UINT64));
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            l2->ResourceBarrier(1, &b);
            l2->CopyBufferRegion(f.readback.Get(), (UINT64)base * sizeof(UINT64),
                                 ts_resolve_.Get(), (UINT64)base * sizeof(UINT64),
                                 kQPerFrame * sizeof(UINT64));
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
            l2->ResourceBarrier(1, &b);
            l2->Close();
            ID3D12CommandList* l3[1] = {f.list_post.Get()};
            queue_->ExecuteCommandLists(1, l3);
        }
    }

    // 提交后打个信号：下一轮复用这一槽时要等这个值（它在两条列表都提交之后，
    // 所以等到它就等于两条列表都执行完了）。
    // 顺序很重要：先记 gpu_frame，再更新 fence_value —— 下一轮 BeginFrame
    // 会先 WaitForFrameRing（用新值）再 ReadSlotTimestamps。
    f.gpu_frame = r.frame_index;
    queue_->Signal(fence_.Get(), ++fence_value_);
    f.fence_value = fence_value_;
}

bool Dx12Engine::TakeLastGpuMs(double* out) {
    if (!out) return false;
    for (size_t i = gpu_samples_.size(); i > 0; --i) {
        GpuSample& s = gpu_samples_[i - 1];
        if (s.taken) continue;
        s.taken = true;
        *out = s.render_ms;
        return true;
    }
    return false;
}

// 让汇总 JSON 里能看到回读的成败计数：「GPU 时间戳可用但只有一半帧有样本」
// 这种半吊子状态必须能一眼看出来，不能含糊过去。
const char* Dx12Engine::Note() const {
    static char buf[256];
    snprintf(buf, sizeof(buf), "%sts-read(attempt=%d ok=%d bad=%d mapFail=%d fenceWait=%d);",
             note_.c_str(), ts_read_attempt_, ts_read_ok_, ts_read_bad_, ts_read_map_fail_,
             ts_read_fence_wait_);
    return buf;
}

bool Dx12Engine::Resize(int w, int h, std::string* err) {
    if (!swap_) {
        *err = "交换链不存在";
        return false;
    }
    // 等 GPU 把所有在用这条交换链的命令做完，否则 ResizeBuffers 会
    // 返回 DXGI_ERROR_INVALID_CALL 或者更糟：设备被移除。
    for (int i = 0; i < kFrameRing; ++i) WaitForFrameRing(i, 2000);
    // 清掉每槽记录的 fence 值：backbuffer 都换了，旧值没有意义了。
    for (int i = 0; i < kFrameRing; ++i) ring_[i].fence_value = 0;

    ReleaseBackBuffers();
    HRESULT hr = swap_->ResizeBuffers((UINT)buffer_count_, (UINT)w, (UINT)h,
                                      DXGI_FORMAT_B8G8R8A8_UNORM, 0);
    if (hr == DXGI_ERROR_INVALID_CALL) {
        SimLog("ResizeBuffers 返回 DXGI_ERROR_INVALID_CALL —— 通常意味着还有对象持有"
               "backbuffer 引用（叠加层/未释放的 view/未完成的命令列表）");
    }
    if (FAILED(hr)) {
        *err = "ResizeBuffers 失败 hr=" + std::to_string((unsigned long)hr) + " (" +
               SimHrName((long)hr) + ")";
        // 交换链尺寸没变，把记录的分辨率改回去，避免自报数据和实际不一致。
        DXGI_SWAP_CHAIN_DESC1 d{};
        if (SUCCEEDED(swap_->GetDesc1(&d))) {
            width_ = (int)d.Width;
            height_ = (int)d.Height;
        }
        return false;
    }
    width_ = w;
    height_ = h;
    if (!CreateFrameResources(err)) return false;
    ++generation_;
    SimLog("交换链已重设到 %dx%d（generation=%d）", w, h, generation_);
    return true;
}

// ---------------------------------------------------------------- 工厂

SimEngine* SimCreateEngineDx12() { return new Dx12Engine(); }
