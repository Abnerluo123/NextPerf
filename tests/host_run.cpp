// NextPerf 测试宿主：模拟一个「已经在跑」的游戏。
//
// 和 _host.cpp 的关键区别：**启动时立刻创建 D3D 设备和交换链**，然后一直 Present。
// 注入发生在这一切之后 —— 也就是真实场景（用户先进游戏，再点注入）。
// 旧的测试宿主等脚本发令才建交换链，正好把「工厂钩子不会再被调用」这个 bug 遮住了。
//
// 用法： host_run.exe [--d3d11] [--seconds N]
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstring>

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(h, m, w, l);
}

static HWND MakeWindow(HINSTANCE inst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"NpHostCls";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, L"NpHostCls", L"NpHost", WS_OVERLAPPEDWINDOW, 100, 100, 860,
                                640, nullptr, nullptr, inst, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    return hwnd;
}

// --delay N：开好窗口之后、建 D3D 设备/交换链之前等 N 秒。
// 给测试留出「在游戏初始化图形之前注入」的窗口，用来验证工厂钩子那条路。
static int gDelayMs = 0;

// 并发提交线程（实现在文件末尾，这里先声明）
static volatile LONG gSubmitRun = 0;
static DWORD WINAPI SubmitThread(LPVOID param);

// ---------------------------------------------------------------- D3D11
static int Run11(HINSTANCE inst, int seconds) {
    HWND hwnd = MakeWindow(inst);
    if (gDelayMs) {
        printf("host: d3d11 init delayed %d ms\n", gDelayMs);
        fflush(stdout);
        Sleep(gDelayMs);
    }
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferDesc.Width = 800;
    sd.BufferDesc.Height = 600;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.OutputWindow = hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_0};
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain* sc = nullptr;
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, fl, 1,
                                               D3D11_SDK_VERSION, &sd, &sc, &dev, &got, &ctx);
    if (FAILED(hr) || !sc) {
        MessageBoxW(nullptr, L"D3D11 init failed", L"NpHost", MB_ICONERROR);
        return 1;
    }
    ID3D11RenderTargetView* rtv = nullptr;
    {
        ID3D11Texture2D* bb = nullptr;
        sc->GetBuffer(0, IID_PPV_ARGS(&bb));
        if (bb) { dev->CreateRenderTargetView(bb, nullptr, &rtv); bb->Release(); }
    }
    printf("host: d3d11 ready, pid=%lu\n", (unsigned long)GetCurrentProcessId());
    fflush(stdout);

    ULONGLONG end = GetTickCount64() + (ULONGLONG)seconds * 1000;
    MSG msg{};
    while (GetTickCount64() < end && msg.message != WM_QUIT) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (rtv) {
            float c[4] = {0.10f, 0.14f, 0.30f, 1.0f};
            ctx->ClearRenderTargetView(rtv, c);
        }
        sc->Present(1, 0);
        Sleep(16);
    }
    return 0;
}

// ---------------------------------------------------------------- D3D12
static int Run12(HINSTANCE inst, int seconds) {
    HWND hwnd = MakeWindow(inst);
    if (gDelayMs) {
        printf("host: d3d12 init delayed %d ms\n", gDelayMs);
        fflush(stdout);
        Sleep(gDelayMs);
    }
    ID3D12Device* dev = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))) || !dev) {
        MessageBoxW(nullptr, L"D3D12CreateDevice failed", L"NpHost", MB_ICONERROR);
        return 1;
    }
    ID3D12CommandQueue* queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) return 1;

    IDXGIFactory4* factory = nullptr;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = 800; sd.Height = 600;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* sc1 = nullptr;
    if (FAILED(factory->CreateSwapChainForHwnd(queue, hwnd, &sd, nullptr, nullptr, &sc1)) || !sc1) {
        MessageBoxW(nullptr, L"CreateSwapChainForHwnd failed", L"NpHost", MB_ICONERROR);
        return 1;
    }
    IDXGISwapChain3* sc = nullptr;
    sc1->QueryInterface(IID_PPV_ARGS(&sc));

    ID3D12DescriptorHeap* rtvHeap = nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 2;
    dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap));
    UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ID3D12Resource* bb[2]{};
    sc->GetBuffer(0, IID_PPV_ARGS(&bb[0]));
    sc->GetBuffer(1, IID_PPV_ARGS(&bb[1]));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv0 = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(bb[0], nullptr, rtv0);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv1{rtv0.ptr + inc};
    dev->CreateRenderTargetView(bb[1], nullptr, rtv1);

    ID3D12CommandAllocator* alloc = nullptr;
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    ID3D12GraphicsCommandList* list = nullptr;
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list));
    ID3D12Fence* fence = nullptr;
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    UINT64 fv = 0;

    // ---------------------------------------------------------------
    // 下面是「真实游戏会做、而最小宿主不会做」的两件事。
    // 钩子原来在这两点上都会把设备搞成 removed（游戏弹 DXGI_ERROR_INVALID_CALL）：
    //   1) 往**复制队列**提交 —— 钩子会把它的 DIRECT 命令列表也丢给复制队列
    //   2) 用 **bundle** 录制 —— 钩子往 bundle 里插 EndQuery（bundle 上非法）
    // 加进来，测试才能真正覆盖真实场景。
    // ---------------------------------------------------------------
    ID3D12CommandQueue* copyQueue = nullptr;
    {
        D3D12_COMMAND_QUEUE_DESC cd{};
        cd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        dev->CreateCommandQueue(&cd, IID_PPV_ARGS(&copyQueue));
    }
    ID3D12CommandAllocator* copyAlloc = nullptr;
    ID3D12GraphicsCommandList* copyList = nullptr;
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&copyAlloc));
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, copyAlloc, nullptr,
                           IID_PPV_ARGS(&copyList));
    copyList->Close();
    ID3D12Resource* csrc = nullptr;
    ID3D12Resource* cdst = nullptr;
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = 4096; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                     D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&csrc));
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                     D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&cdst));
    }
    // bundle：里面放一个 Dispatch（钩子会想往它插时间戳）
    ID3D12CommandAllocator* bundleAlloc = nullptr;
    ID3D12GraphicsCommandList* bundle = nullptr;
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&bundleAlloc));
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, bundleAlloc, nullptr,
                           IID_PPV_ARGS(&bundle));
    bundle->Dispatch(1, 1, 1);
    bundle->Close();

    printf("host: d3d12 ready, pid=%lu  copyQueue=%p csrc=%p cdst=%p bundle=%p\n",
           (unsigned long)GetCurrentProcessId(), (void*)copyQueue, (void*)csrc, (void*)cdst,
           (void*)bundle);
    fflush(stdout);

    // 并发提交线程（复现多线程引擎的 ECL 竞争）
    InterlockedExchange(&gSubmitRun, 1);
    HANDLE submitTh = CreateThread(nullptr, 0, SubmitThread, queue, 0, nullptr);

    ULONGLONG end = GetTickCount64() + (ULONGLONG)seconds * 1000;
    MSG msg{};
    while (GetTickCount64() < end && msg.message != WM_QUIT) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        // 每帧先往复制队列塞一个拷贝（真实游戏的流式加载就是这样）
        if (copyQueue && csrc && cdst) {
            copyAlloc->Reset();
            copyList->Reset(copyAlloc, nullptr);
            copyList->CopyBufferRegion(cdst, 0, csrc, 0, 4096);
            copyList->Close();
            ID3D12CommandList* cl[1] = {copyList};
            copyQueue->ExecuteCommandLists(1, cl);
        }

        UINT idx = sc->GetCurrentBackBufferIndex();
        alloc->Reset();
        list->Reset(alloc, nullptr);
        // 用 bundle 执行一次 Dispatch（bundle 上不能插时间戳）
        if (bundle) list->ExecuteBundle(bundle);
        D3D12_RESOURCE_BARRIER bar{};
        bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bar.Transition.pResource = bb[idx];
        bar.Transition.Subresource = 0;
        bar.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        bar.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &bar);
        float c[4] = {0.10f, 0.14f, 0.30f, 1.0f};
        list->ClearRenderTargetView(idx ? rtv1 : rtv0, c, 0, nullptr);
        bar.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bar.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        list->ResourceBarrier(1, &bar);
        list->Close();
        ID3D12CommandList* lists[1] = {list};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence, ++fv);
        if (fence->GetCompletedValue() < fv) {
            HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            fence->SetEventOnCompletion(fv, ev);
            WaitForSingleObject(ev, 1000);
            CloseHandle(ev);
        }
        sc->Present(1, 0);
        Sleep(16);
    }

    InterlockedExchange(&gSubmitRun, 0);
    if (submitTh) {
        WaitForSingleObject(submitTh, 2000);
        CloseHandle(submitTh);
    }
    return 0;
}

// ---------------------------------------------------------------
// 并发提交线程：真实游戏是重度多线程的 —— 加载/流式线程
// 会在渲染线程 Present 的同时往同一条队列提交命令列表。
// 钩子的时间戳/叠加用的是**全局唯一**的一条命令列表，没有同步就会两个线程
// 同时 Reset/Close 它 —— 那是非法调用，设备直接 removed。
// 症状就是「注入成功、数据正常，一两秒后闪退」。这里把那个条件造出来。
// ---------------------------------------------------------------
static DWORD WINAPI SubmitThread(LPVOID param) {
    ID3D12CommandQueue* q = reinterpret_cast<ID3D12CommandQueue*>(param);
    ID3D12Device* dev = nullptr;
    q->GetDevice(IID_PPV_ARGS(&dev));
    ID3D12CommandAllocator* a = nullptr;
    ID3D12GraphicsCommandList* l = nullptr;
    if (dev) {
        dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
        dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, nullptr, IID_PPV_ARGS(&l));
        if (l) l->Close();
    }
    while (InterlockedCompareExchange(&gSubmitRun, 1, 1)) {
        // 密集提交一小段再睡：真实游戏的加载线程就是这样突发的，
        // 只有这样才能真的和渲染线程的 Present 撞上钩子那条全局命令列表。
        for (int burst = 0; burst < 30 && InterlockedCompareExchange(&gSubmitRun, 1, 1); ++burst) {
            if (a && l) {
                a->Reset();
                if (SUCCEEDED(l->Reset(a, nullptr))) {
                    l->Close();
                    ID3D12CommandList* ls[1] = {l};
                    q->ExecuteCommandLists(1, ls);
                }
            }
        }
        Sleep(2);
    }
    if (l) l->Release();
    if (a) a->Release();
    if (dev) dev->Release();
    return 0;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR cmd, int) {
    int seconds = 40;
    if (const char* p = strstr(cmd, "--seconds")) {
        int v = atoi(p + 9);
        if (v > 0) seconds = v;
    }
    if (const char* p = strstr(cmd, "--delay")) {
        int v = atoi(p + 7);
        if (v > 0) gDelayMs = v * 1000;
    }
    // --nogfx：模拟 Vulkan / OpenGL 游戏 —— 只开窗口，加载 vulkan-1.dll，
    // 完全不碰 DXGI。用来验证「Present 挂不上时钩子也要说明原因」这条路。
    if (strstr(cmd, "--nogfx")) {
        HWND h = MakeWindow(inst);
        (void)h;
        LoadLibraryW(L"vulkan-1.dll");
        printf("host: nogfx (vulkan loaded, no dxgi), pid=%lu\n",
               (unsigned long)GetCurrentProcessId());
        fflush(stdout);
        MSG msg{};
        ULONGLONG end = GetTickCount64() + (ULONGLONG)seconds * 1000;
        while (GetTickCount64() < end && msg.message != WM_QUIT) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            Sleep(16);
        }
        return 0;
    }
    if (strstr(cmd, "--d3d11")) return Run11(inst, seconds);
    return Run12(inst, seconds);
}
