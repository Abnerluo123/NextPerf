#include "np_hook.h"

#include <dxgi1_6.h>
#include <d3d11.h>
#include <d3d12.h>

#include <atomic>
#include <algorithm>
#include <csetjmp>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "common/np_common.h"
#include "common/np_stats.h"
#include "common/np_panel.h"
#include "common/np_build.h"
#include "np_draw.h"
#include "common/np_bitmap.h"

// ---------------------------------------------------------------------------
// vtable 下标：这些数字是从 d3d12.h / dxgi.h 的 Vtbl 定义里逐个核对出来的，
// 改错一个就会劫持到别的成员函数，后果是游戏直接崩。
//
// ⚠ 别凭记忆数。用 `python tests/vt_check.py` 从 mingw 头文件里算一遍再改。
//   历史上这里错过两次：CreateSwapChainForHwnd 写成 13（其实是 IsCurrent），
//   CreateSwapChainForComposition 写成 22（其实是 RegisterOcclusionStatusEvent）。
// ---------------------------------------------------------------------------
namespace Vt {
enum SwapChain {
    Present = 8,
    GetBuffer = 9,
    GetFullscreenState = 11,
    GetDesc = 12,
    GetHwnd = 20,                      // IDXGISwapChain1
    Present1 = 22,                     // IDXGISwapChain1
    GetCurrentBackBufferIndex = 36,    // IDXGISwapChain3
};
enum Factory {
    CreateSwapChain = 10,
};
enum Factory2 {
    // IDXGIObject(0..6) + IDXGIFactory(7..11) + IDXGIFactory1(12..13)
    // + IsWindowedStereoEnabled(14)
    CreateSwapChainForHwnd = 15,
    CreateSwapChainForComposition = 24,
};
enum CommandQueue {
    ExecuteCommandLists = 10,
    Signal = 14,
    GetTimestampFrequency = 16,
    QueueGetDesc = 19,   // 注意别和 SwapChain::GetDesc 重名（同一命名空间下会冲突）
};
enum CommandList {
    Close = 9,
    Reset = 10,
    DrawInstanced = 12,
    DrawIndexedInstanced = 13,
    Dispatch = 14,
    RSSetViewports = 21,
    EndQuery = 53,
    ResolveQueryData = 54,
    BuildRaytracingAccelerationStructure = 72,
    DispatchRays = 76,
};
}  // namespace Vt

// GPU 时间戳槽位布局
//
// 核心是 TS_BATCH_*：把**每一次 ExecuteCommandLists** 用一对时间戳夹住。
// GPU 只在真正执行那一批命令时被计时，跑完空转的时间不算 ——
// 所以「本帧 GPU 时间」= 把所有批次的耗时加起来，它和锁不锁帧无关。
// （老做法是「本帧第一次提交 → Present」一个跨度，那个必然等于帧周期：
//   GPU 早跑完了在空转，而结束时间戳要等到 Present 才提交。用户一眼就看出来了。）
#define TS_SLOTS        64
#define TS_BATCH_BASE   0
#define TS_BATCH_PAIRS  16     // 槽 0..31，最多夹 16 批
#define TS_RT_BASE      32
#define TS_RT_PAIRS     8      // 槽 32..47
#define TS_AI_START     48
#define TS_AI_END       49

typedef HRESULT(__stdcall* FnPresent)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(__stdcall* FnPresent1)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef void(__stdcall* FnECL)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
typedef void(__stdcall* FnDispatchRays)(ID3D12GraphicsCommandList4*, const D3D12_DISPATCH_RAYS_DESC*);
typedef void(__stdcall* FnBuildAS)(ID3D12GraphicsCommandList4*,
                                   const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC*, UINT,
                                   const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC*);
typedef void(__stdcall* FnDispatch)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
typedef void(__stdcall* FnDrawInst)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
typedef void(__stdcall* FnDrawIdx)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
typedef void(__stdcall* FnSetViewports)(ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*);
typedef HRESULT(__stdcall* FnCreateSwapChain)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                              IDXGISwapChain**);
typedef HRESULT(__stdcall* FnCreateSwapChainHwnd)(IDXGIFactory2*, IUnknown*, HWND,
                                                  const DXGI_SWAP_CHAIN_DESC1*,
                                                  const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,
                                                  IDXGIOutput*, IDXGISwapChain1**);
typedef HRESULT(__stdcall* FnCreateSwapChainComp)(IDXGIFactory2*, IUnknown*,
                                                  const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*,
                                                  IDXGISwapChain1**);

extern "C" {
HRESULT __stdcall NpPresent(IDXGISwapChain* sc, UINT sync, UINT flags);
HRESULT __stdcall NpPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags,
                             const DXGI_PRESENT_PARAMETERS* pp);
void __stdcall NpECL(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists);
void __stdcall NpDispatchRays(ID3D12GraphicsCommandList4* cl, const D3D12_DISPATCH_RAYS_DESC* d);
void __stdcall NpBuildAS(ID3D12GraphicsCommandList4* cl,
                         const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* d, UINT n,
                         const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* p);
void __stdcall NpDispatch(ID3D12GraphicsCommandList* cl, UINT x, UINT y, UINT z);
void __stdcall NpDrawInst(ID3D12GraphicsCommandList* cl, UINT a, UINT b, UINT c, UINT d);
void __stdcall NpDrawIdx(ID3D12GraphicsCommandList* cl, UINT a, UINT b, UINT c, INT d, UINT e);
void __stdcall NpSetViewports(ID3D12GraphicsCommandList* cl, UINT n, const D3D12_VIEWPORT* v);
HRESULT __stdcall NpCreateSwapChain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d,
                                    IDXGISwapChain** sc);
HRESULT __stdcall NpCreateSwapChainHwnd(IDXGIFactory2* f, IUnknown* dev, HWND hw,
                                        const DXGI_SWAP_CHAIN_DESC1* d1,
                                        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fd, IDXGIOutput* out,
                                        IDXGISwapChain1** sc);
HRESULT __stdcall NpCreateSwapChainComp(IDXGIFactory2* f, IUnknown* dev,
                                        const DXGI_SWAP_CHAIN_DESC1* d1, IDXGIOutput* out,
                                        IDXGISwapChain1** sc);
}

namespace {

// ---------------------------------------------------------------------------
// 异常保护：钩子跑在别人的进程里，任何一步出错都必须自己扛住
//
// 这两个状态必须**每线程一份**：Present / 命令列表钩子跑在游戏的渲染线程上，
// 注入探测跑在工作线程上。共用一个 jmp_buf 的话，一边出错会 longjmp 到
// 另一边的栈上——直接是游戏闪退。
// ---------------------------------------------------------------------------
thread_local std::jmp_buf gJmp;
thread_local bool gJmpArmed = false;
std::atomic<bool> gSehReady{false};
void Log(const char* fmt, ...);

// 只有这些才是我们想兜住的「真崩溃」。
//
// 踩过的坑：AddVectoredExceptionHandler 拿到的是**所有**异常，包括
// 0x40010006 DBG_PRINTEXCEPTION_C —— OutputDebugString 就会发这个，
// D3D11/DXGI 初始化时很常见。原来不筛就把探测整个 longjmp 掉了，
// 而且 C++ 异常（0xE06D7363）之类的也不能吞，否则会把宿主自己的逻辑搞坏。
bool IsFatalCode(DWORD c) {
    switch (c) {
        case EXCEPTION_ACCESS_VIOLATION:       // 0xC0000005
        case EXCEPTION_IN_PAGE_ERROR:          // 0xC0000006
        case EXCEPTION_ILLEGAL_INSTRUCTION:    // 0xC000001D
        case EXCEPTION_PRIV_INSTRUCTION:       // 0xC0000096
        case EXCEPTION_INT_DIVIDE_BY_ZERO:     // 0xC0000094
        case EXCEPTION_INT_OVERFLOW:           // 0xC0000095
        case EXCEPTION_STACK_OVERFLOW:         // 0xC00000FD
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:  // 0xC000008C
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:     // 0xC000008E
        case EXCEPTION_FLT_INVALID_OPERATION:  // 0xC0000090
            return true;
        default:
            return false;
    }
}

LONG CALLBACK NpSeh(PEXCEPTION_POINTERS ei) {
    if (!gJmpArmed || !ei || !ei->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    DWORD code = ei->ExceptionRecord->ExceptionCode;
    if (!IsFatalCode(code)) return EXCEPTION_CONTINUE_SEARCH;   // 调试打印 / C++ 异常：放行
    gJmpArmed = false;
    // 报出出错地址落在哪个模块 —— 钩子跑在别人进程里，没有这个只能瞎猜
    void* addr = ei->ExceptionRecord->ExceptionAddress;
    char where[MAX_PATH]{};
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &mod) &&
        mod) {
        char path[MAX_PATH]{};
        GetModuleFileNameA(mod, path, MAX_PATH);
        const char* base = strrchr(path, '\\');
        snprintf(where, sizeof(where), "%s+0x%llx", base ? base + 1 : path,
                 (unsigned long long)((uintptr_t)addr - (uintptr_t)mod));
    } else {
        snprintf(where, sizeof(where), "?+%p", addr);
    }
    Log("SEH: code=0x%08lx at %s", (unsigned long)code, where);
    std::longjmp(gJmp, 1);
    return EXCEPTION_CONTINUE_SEARCH;   // 不会走到这里
}
inline bool SehReady() {
    if (gSehReady.load(std::memory_order_acquire)) return true;
    if (AddVectoredExceptionHandler(1, NpSeh)) {
        gSehReady.store(true, std::memory_order_release);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 全局状态
// ---------------------------------------------------------------------------
HMODULE gSelf = nullptr;
HANDLE  gMapCfg = nullptr, gMapSens = nullptr, gMapTel = nullptr;
NPConfig*    gCfg = nullptr;
NPSensors*   gSens = nullptr;
NPTelemetry* gTel = nullptr;

CRITICAL_SECTION gCs;
bool             gInPresent = false;
std::atomic<bool> gReady{false};
std::atomic<uint32_t> gSeq{0};

int gApi = NP_API_UNKNOWN;
ID3D11Device*        gDev11 = nullptr;
ID3D11DeviceContext* gCtx11 = nullptr;
ID3D12Device*        gDev12 = nullptr;
ID3D12CommandQueue*  gQueue12 = nullptr;

// 被补丁过的交换链 vtable。
//
// dxgi 里同一个交换链类的 vtable 是**全进程共享**的：只要拿到一个实例，
// 改它的 vtable 就等于改了这个进程里所有同类交换链 —— 这正是「游戏已经在跑、
// 工厂钩子不会再被调用」时唯一能把 Present 挂上去的办法。
// blt / flip / composition 可能是不同的类（vtable 不同），所以存成一张小表。
struct SwapVtEntry {
    void**     vt = nullptr;
    FnPresent  origPresent = nullptr;
    FnPresent1 origPresent1 = nullptr;
};
#define NP_MAX_SWAPVT 4
SwapVtEntry gSwapVts[NP_MAX_SWAPVT];
int         gSwapVtCount = 0;

SwapVtEntry* SwapVtFor(void** vt) {
    for (int i = 0; i < gSwapVtCount; ++i)
        if (gSwapVts[i].vt == vt) return &gSwapVts[i];
    return nullptr;
}
inline bool PresentHooked() { return gSwapVtCount > 0; }

// DXGI 工厂 vtable（注入时不再创建任何设备，改为拦截 CreateSwapChain*，
// 等游戏自己创建交换链时再补 Present 补丁）
void** gFactoryVt = nullptr;
FnCreateSwapChain gOrigCreateSC = nullptr;
void** gFactory2Vt = nullptr;
FnCreateSwapChainHwnd gOrigCreateSCHwnd = nullptr;
FnCreateSwapChainComp gOrigCreateSCComp = nullptr;

void** gQueueVt = nullptr;
FnECL gOrigECL = nullptr;
void** gClVt = nullptr;
FnDispatchRays gOrigDR = nullptr;
FnBuildAS gOrigBAS = nullptr;
FnDispatch gOrigDispatch = nullptr;
FnDrawInst gOrigDrawInst = nullptr;
FnDrawIdx  gOrigDrawIdx = nullptr;
FnSetViewports gOrigVP = nullptr;
bool gClHooked = false;

// 主程序退出时置位：所有跳板直通原函数，随后 vtable 还原、DLL 自卸载
std::atomic<bool> gUnloading{false};

// 我们那条时间戳/叠加命令列表（gList / gAlloc / gFenceVal）是**全局唯一**的，
// 而真实游戏是多线程的：加载线程在调 ExecuteCommandLists，渲染线程同时在
// Present 里录制。两个线程同时 Reset()/Close() 同一条命令列表就是非法调用，
// 设备会被设成 removed —— 游戏直接弹 DXGI_ERROR_INVALID_CALL 退出。
// 症状：注入后数据正常，一两秒后（加载线程开始提交时）闪退。
//
// 用 try_lock 而不是 lock：抢不到就**放弃这一帧**，绝不阻塞游戏线程。
// 少一帧叠加/时间戳无所谓，卡住或搞崩游戏才是大事。
std::mutex gD12Lock;
thread_local bool gD12Locked = false;

// D3D12 侧出过不可恢复的错（Close/Reset 失败）就永久停手。
// 注入器的第一原则是**绝不能把宿主搞崩**：宁可丢掉叠加和 RT/Tensor 数据，
// 也不能让游戏弹 DXGI_ERROR_INVALID_CALL 然后退出。
std::atomic<bool> gD12Broken{false};

// 宿主自己（不含我们静态导入的）用的是什么图形 API。
// 主要用于一件事：**绝不在一个 Vulkan 进程里创建 D3D 设备** ——
// 那会把 Vulkan 游戏带崩（实测某款 Vulkan 游戏：注入成功，一秒后闪退且无任何报错）。
int gHostApi = NP_API_UNKNOWN;

uint64_t gQpcFreq = 0;
// 渲染线程 = 调 Present 的那个线程。只有它自己的第一次提交才算「模拟阶段结束」；
// 后台流式线程随时都在提交，用全局「第一次 ECL」会把两段彻底搅乱。
DWORD    gRenderTid = 0;
uint64_t gPresentRetQpc = 0;   // 上一帧 Present 返回的时刻（= 下一帧模拟阶段开始）
uint64_t gSimEndQpc = 0;       // 本帧渲染线程第一次提交的时刻

uint64_t gLastPresentQpc = 0;
uint64_t gCpuStartQpc = 0;
// 上一帧卡在原始 Present 调用里的时长（主要是等垂直同步）
float    gLastInPresentMs = 0.0f;
// 「本帧的起点」标记必须是原子的：真实游戏会在**多个线程**上提交命令列表，
// 普通 bool 会让两个线程同时读到 false、同时去重设我们那条全局命令列表 ——
// 非法调用、设备 removed、游戏弹 DXGI_ERROR_INVALID_CALL 退出。
std::atomic<bool> gFrameStarted{false};
np::FrameStats gStats;

// 原来这几个是普通 uint32_t。但 D3D12 的命令列表钩子（Dispatch/Draw/BuildAS）
// 会被**游戏的多个提交线程**同时调用，而 Present 线程同时在读并清零它们：
// 普通变量的「读-改-写」既会丢计数，也是 C++ 层面的数据竞争（未定义行为）。
// 换成原子变量（relaxed 就够，只是计数，不需要排序语义）。
std::atomic<uint32_t> gDraws{0}, gDispatches{0}, gRtDispatches{0}, gAsBuilds{0};
uint32_t gVpW = 0, gVpH = 0;   // 只由写 vtable 钩子的提交线程写、Present 线程读，属 32 位标量竞态，影响可忽略
bool     gAiSpanOpen = false;

// D3D12 时间戳
ID3D12QueryHeap* gHeap = nullptr;
ID3D12Resource*  gReadback[4]{};
ID3D12Fence*     gFence = nullptr;
UINT64           gFenceVal = 0;
uint64_t         gTsFreq = 0;
// 分配器环形缓冲。
// 一帧里的用量：每批 GPU 时间戳要 2 次（起止各一次），再加 Present 里的 resolve
// 和叠加录制。批次多的时候十几个很正常，所以给足 —— 只给 3 或 8 会让 GPU 稍微
// 落后就全部「busy」，然后 BeginList 一路返回 nullptr，叠加**永久停画**。
#define NP_ALLOC_RING 32
ID3D12CommandAllocator*  gAlloc[NP_ALLOC_RING]{};
ID3D12GraphicsCommandList* gList = nullptr;
UINT64           gAllocFence[NP_ALLOC_RING]{};
int              gAllocCur = 0;
uint32_t         gBeginListFails = 0;   // 连续拿不到分配器的次数

struct Pending {
    bool    busy = false;
    UINT64  fenceVal = 0;
    uint32_t rtCount = 0;
    uint32_t batchCount = 0;
    bool    aiUsed = false;
};
Pending gPending[4];

// D3D11 时间戳
struct Ts11 {
    ID3D11Query* disjoint = nullptr;
    ID3D11Query* a = nullptr;
    ID3D11Query* b = nullptr;
    bool started = false, pendingRead = false;
};
Ts11 gTs11[3];

// 绘制
npb::PanelBitmap gBmp;
npg::Overlay11   gOv11;
npg::Overlay12   gOv12;
np::PanelRenderer gPanel;
np::PanelData     gPd;
uint64_t gLastPanelMs = 0;

uint32_t gAiModules = 0;

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------
// 本帧被夹住的批次数（可能来自多个提交线程，用原子计数）
std::atomic<uint32_t> gBatchCount{0};

uint64_t NowQpc() {
    LARGE_INTEGER li{};
    QueryPerformanceCounter(&li);
    return (uint64_t)li.QuadPart;
}
double QpcMs(uint64_t d) { return gQpcFreq ? (double)d * 1000.0 / (double)gQpcFreq : 0.0; }

bool Patch(void** vt, int idx, void* hook, void** orig) {
    if (!vt || idx < 0 || !vt[idx]) return false;
    DWORD old = 0;
    void* slot = &vt[idx];
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
    if (orig) *orig = vt[idx];   // orig 可以为空（还原时不需要回读旧值）
    vt[idx] = hook;
    DWORD dummy = 0;
    VirtualProtect(slot, sizeof(void*), old, &dummy);
    return true;
}

// ⚠ 这个函数现在**没有人用了**，留着是为了记住一个教训：
// 曾经用它做「槽位必须落在 dxgi.dll / d3d12.dll 里」的健全性检查，
// 结果在真实 Windows 上全数失败 —— D3D12 的实现其实在 **D3D12Core.dll** 里，
// d3d12.dll 只是个薄壳。判定依据错一个模块名，光追和 DLSS 的钩子就一个都装不上。
// 现在改用 PlausibleCodePtr + PtrOwner（只查可执行 + 槽位互不相同，并把归属模块写进日志）。
bool InModule(HMODULE mod, void* p) {
    if (!mod || !p) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    return mbi.AllocationBase == mod;
}

// 本 DLL 自己静态导入了哪些模块（读自己的 PE 导入表，只算一次）。
//
// 为什么要这个：判断「宿主是不是已经初始化过图形」时，直觉写法是
// `GetModuleHandleW(L"d3d12.dll") != nullptr`。但这个 DLL 为了拿接口和
// D3D11CreateDevice / D3D12CreateDevice，本身就静态链接了 dxgi/d3d11/d3d12，
// 那三个模块从 LoadLibrary 那一刻起就是「已加载」—— 判断永远为真，
// 于是所有「让宿主的工厂钩子先跑」的保护统统失效（踩过）。
bool SelfImports(const char* modName) {
    static char names[16][32];
    static int  count = -1;
    if (count < 0) {
        count = 0;
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(gSelf);
        if (dos && dos->e_magic == IMAGE_DOS_SIGNATURE) {
            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
                reinterpret_cast<BYTE*>(gSelf) + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE) {
                DWORD rva = nt->OptionalHeader
                                .DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]
                                .VirtualAddress;
                if (rva) {
                    auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
                        reinterpret_cast<BYTE*>(gSelf) + rva);
                    for (; imp->Name && count < 16; ++imp) {
                        const char* n = reinterpret_cast<const char*>(
                            reinterpret_cast<BYTE*>(gSelf) + imp->Name);
                        NPCopyStr(names[count], sizeof(names[0]), n);
                        ++count;
                    }
                }
            }
        }
    }
    for (int i = 0; i < count; ++i)
        if (_stricmp(names[i], modName) == 0) return true;
    return false;
}

// 宿主（游戏）自己是否已经把图形栈拉起来了 —— 排掉我们自己导入的那些。
bool HostGraphicsUp() {
    static const char* kMods[] = {"dxgi.dll", "d3d12.dll", "d3d11.dll",
                                  "d3d10.dll", "d3d9.dll", "opengl32.dll", "vulkan-1.dll"};
    for (const char* m : kMods) {
        if (SelfImports(m)) continue;          // 是我们带进来的，不算数
        if (GetModuleHandleA(m)) return true;  // 宿主自己加载的
    }
    return false;
}

// 宿主自己用的是哪套 API。
//
// ⚠ 不能拿 `vulkan-1.dll` 当「这是 Vulkan 游戏」的证据：实测**D3D12 与 Vulkan 游戏
//   都加载了它**（引擎/启动器的视频或探测模块）。上一版把 vulkan 排在最前，
//   于是 D3D12 游戏被判成 Vulkan → 探测被永久禁用 → 中途注入再也挂不上 Present。
//   判据按可靠性排序：d3d12 > d3d11 > d3d10 > d3d9 > vulkan > opengl。
int HostApiGuess() {
    struct Item { const char* mod; int api; };
    static const Item kItems[] = {
        {"d3d12.dll", NP_API_D3D12},     {"d3d11.dll", NP_API_D3D11},
        {"d3d10.dll", NP_API_D3D11},     {"d3d9.dll", NP_API_D3D9},
        {"vulkan-1.dll", NP_API_VULKAN}, {"opengl32.dll", NP_API_OPENGL},
    };
    for (const Item& it : kItems) {
        if (SelfImports(it.mod)) continue;
        if (GetModuleHandleA(it.mod)) return it.api;
    }
    return NP_API_UNKNOWN;
}

// 宿主自己加载了 vulkan-1.dll 吗（用于给探测次数限个更紧的上限）
bool HostHasVulkan() {
    return !SelfImports("vulkan-1.dll") && GetModuleHandleA("vulkan-1.dll") != nullptr;
}

// ---------------------------------------------------------------------------
// 诊断日志：钩子跑在别的进程里，出了问题只能靠这个文件定位。
// %TEMP%\NextPerfHook.log，超过 64KB 自动清空重写。
// ---------------------------------------------------------------------------
void Log(const char* fmt, ...) {
    wchar_t path[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"TEMP", path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    wcscat_s(path, MAX_PATH, L"\\NextPerfHook.log");
    HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD size = GetFileSize(h, nullptr);
    if (size > 64 * 1024) SetFilePointer(h, 0, nullptr, FILE_BEGIN);
    else SetFilePointer(h, 0, nullptr, FILE_END);

    char buf[768];
    unsigned long long ms = GetTickCount64();
    // pid 也要记：这个日志文件是**所有被注入进程共用**的，
    // 同时注入两个游戏时没有 pid 根本分不清哪一行是谁写的。
    int len = snprintf(buf, sizeof(buf), "[%llu.%03llu pid=%lu tid=%lu] ", ms / 1000ull,
                       ms % 1000ull, (unsigned long)GetCurrentProcessId(),
                       (unsigned long)GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    len += vsnprintf(buf + len, sizeof(buf) - (size_t)len, fmt, ap);
    va_end(ap);
    if (len > 0 && len < (int)sizeof(buf) - 1) {
        buf[len++] = '\n';
        DWORD wr = 0;
        WriteFile(h, buf, (DWORD)len, &wr, nullptr);
    }
    CloseHandle(h);
}

// ---------------------------------------------------------------------------
// IPC
// ---------------------------------------------------------------------------
bool OpenIpc() {
    gMapCfg = OpenFileMappingW(FILE_MAP_READ, FALSE, NP_SHM_CONFIG);
    if (gMapCfg) gCfg = (NPConfig*)MapViewOfFile(gMapCfg, FILE_MAP_READ, 0, 0, sizeof(NPConfig));
    gMapSens = OpenFileMappingW(FILE_MAP_READ, FALSE, NP_SHM_SENSORS);
    if (gMapSens) gSens = (NPSensors*)MapViewOfFile(gMapSens, FILE_MAP_READ, 0, 0, sizeof(NPSensors));
    // 遥测按 PID 分开建：同时注入两个游戏时，共用一个名字会互相覆盖
    wchar_t telName[64]{};
    NPTelemetryShmName(GetCurrentProcessId(), telName, 64);
    gMapTel = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                 sizeof(NPTelemetry), telName);
    if (!gMapTel) return false;
    gTel = (NPTelemetry*)MapViewOfFile(gMapTel, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(NPTelemetry));
    if (!gTel) return false;
    NPClearTelemetry(gTel);
    gTel->pid = GetCurrentProcessId();
    return true;
}

NPConfig Cfg() {
    NPConfig c{};
    NPDefaultConfig(&c);
    if (gCfg && gCfg->magic == NP_MAGIC) c = *gCfg;
    return c;
}
NPSensors Sens() {
    NPSensors s{};
    NPClearSensors(&s);
    if (gSens && gSens->magic == NP_MAGIC && gSens->valid) s = *gSens;
    return s;
}

// ---------------------------------------------------------------------------
// AI 模块识别
// ---------------------------------------------------------------------------
void DetectAi() {
    struct Item { const wchar_t* dll; uint32_t bit; };
    static const Item items[] = {
        {L"nvngx_dlss.dll", NP_AI_DLSS_SR},
        {L"nvngx_dlssd.dll", NP_AI_DLSS_RR},
        {L"nvngx_dlssg.dll", NP_AI_DLSS_FG},
        {L"libxess.dll", NP_AI_XESS},
        {L"libxess_dx11.dll", NP_AI_XESS},
        {L"igxess.dll", NP_AI_XESS},
        {L"amd_fidelityfx_dx12.dll", NP_AI_FSR},
        {L"amd_fidelityfx_vk.dll", NP_AI_FSR},
        {L"DirectML.dll", NP_AI_DIRECTML},
        {L"onnxruntime.dll", NP_AI_ORT},
    };
    uint32_t m = 0;
    for (auto& it : items)
        if (GetModuleHandleW(it.dll)) m |= it.bit;
    gAiModules = m;
}

// ---------------------------------------------------------------------------
// D3D12 时间戳 / 命令列表
// ---------------------------------------------------------------------------
bool InitTs12(ID3D12Device* dev, ID3D12CommandQueue* q) {
    D3D12_QUERY_HEAP_DESC qh{};
    qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qh.Count = TS_SLOTS;
    qh.NodeMask = 0;
    if (FAILED(dev->CreateQueryHeap(&qh, IID_PPV_ARGS(&gHeap)))) return false;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    for (int i = 0; i < 4; ++i) {
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = TS_SLOTS * 8;
        rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&gReadback[i]))))
            return false;
    }
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gFence)))) return false;
    for (int i = 0; i < NP_ALLOC_RING; ++i)
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(&gAlloc[i]))))
            return false;
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gAlloc[0], nullptr,
                                      IID_PPV_ARGS(&gList))))
        return false;
    gList->Close();
    if (q->GetTimestampFrequency(&gTsFreq) != S_OK || gTsFreq == 0) gTsFreq = 1000000;
    return true;
}

ID3D12GraphicsCommandList* BeginList() {
    if (gD12Locked) return nullptr;   // 同一线程重入：直接放弃
    if (gD12Broken.load(std::memory_order_acquire)) return nullptr;
    if (!gD12Lock.try_lock()) {
        // 另一个线程正在用我们那条命令列表。绝不等待 —— 等就会卡住游戏，
        // 硬闯就会把设备搞崩。放弃这一帧。
        static uint32_t contended = 0;
        if (contended++ < 5)
            Log("BeginList: our command list is busy on another thread, skipping frame");
        return nullptr;
    }
    gD12Locked = true;

    gAllocCur = (gAllocCur + 1) % NP_ALLOC_RING;
    // **绝不在这里等。** 原来会 WaitForSingleObject 最多 50ms —— 那是在游戏
    // Present 的调用栈里睡觉，直接变成游戏的卡顿，还会污染我们自己的帧时间统计。
    if (gAllocFence[gAllocCur] && gFence->GetCompletedValue() < gAllocFence[gAllocCur]) {
        // 连续拿不到就说明 GPU 已经严重落后（或者围栏根本没在推进）。
        // 这里必须有个了断：一直失败会让叠加永久停画，而围栏不推进本身
        // 往往意味着我们手里的队列/围栏已经失效 —— 继续硬撑只会把游戏拖死。
        if (++gBeginListFails == 5)
            Log("BeginList: allocator starved (GPU behind or fence not advancing)");
        if (gBeginListFails > 600) {
            gD12Broken.store(true, std::memory_order_release);
            Log("BeginList: starved too long -> D3D12 instrumentation disabled "
                "(frame timing keeps working)");
        }
        gD12Locked = false;
        gD12Lock.unlock();
        return nullptr;
    }
    gBeginListFails = 0;
    if (FAILED(gAlloc[gAllocCur]->Reset())) {
        gD12Broken.store(true, std::memory_order_release);
        Log("BeginList: allocator Reset failed -> D3D12 instrumentation disabled");
        gD12Locked = false;
        gD12Lock.unlock();
        return nullptr;
    }
    if (FAILED(gList->Reset(gAlloc[gAllocCur], nullptr))) {
        gD12Broken.store(true, std::memory_order_release);
        Log("BeginList: command list Reset failed -> D3D12 instrumentation disabled");
        gD12Locked = false;
        gD12Lock.unlock();
        return nullptr;
    }
    return gList;
}

void EndList(ID3D12CommandQueue* q) {
    if (!gD12Locked) return;   // 没拿锁（BeginList 放弃过），不要瞎解锁
    if (FAILED(gList->Close())) {
        gD12Broken.store(true, std::memory_order_release);
        Log("EndList: Close failed -> D3D12 instrumentation disabled");
        gD12Locked = false;
        gD12Lock.unlock();
        return;
    }
    ID3D12CommandList* lists[1] = {gList};
    q->ExecuteCommandLists(1, lists);
    ++gFenceVal;
    q->Signal(gFence, gFenceVal);
    gAllocFence[gAllocCur] = gFenceVal;
    if (gOv12.fence()) {
        UINT64* p = gOv12.fenceValuePtr();
        *p = gFenceVal;
        q->Signal(gOv12.fence(), gFenceVal);
    }
    gD12Locked = false;
    gD12Lock.unlock();
}

void HarvestTimestamps() {
    if (!gFence || !gTel) return;
    UINT64 done = gFence->GetCompletedValue();
    for (int i = 0; i < 4; ++i) {
        Pending& p = gPending[i];
        if (!p.busy || p.fenceVal > done) continue;
        p.busy = false;
        void* data = nullptr;
        D3D12_RANGE range{0, TS_SLOTS * 8};
        if (FAILED(gReadback[i]->Map(0, &range, &data)) || !data) continue;
        const UINT64* ts = reinterpret_cast<const UINT64*>(data);
        double gpuMs = 0, rtMs = 0, aiMs = 0;
        // 本帧 GPU 忙时间 = 每一批 [开始, 结束] 的差之和。
        // 批次之间 GPU 空转的间隙不在任何一对区间里，天然被排除。
        for (uint32_t b = 0; b < p.batchCount && b < TS_BATCH_PAIRS; ++b) {
            UINT64 a = ts[TS_BATCH_BASE + b * 2];
            UINT64 z = ts[TS_BATCH_BASE + b * 2 + 1];
            if (z > a) {
                double d = (double)(z - a) * 1000.0 / (double)gTsFreq;
                // 单批超过 50ms 一定是配对出了问题（比如跨帧），丢掉别污染读数
                if (d < 50.0) gpuMs += d;
                else Log("gpu busy: implausible batch %.1f ms dropped", d);
            }
        }
        for (uint32_t k = 0; k < p.rtCount && k < TS_RT_PAIRS; ++k) {
            UINT64 a = ts[TS_RT_BASE + k * 2];
            UINT64 b = ts[TS_RT_BASE + k * 2 + 1];
            if (b > a) rtMs += (double)(b - a) * 1000.0 / (double)gTsFreq;
        }
        if (p.aiUsed && ts[TS_AI_END] > ts[TS_AI_START])
            aiMs = (double)(ts[TS_AI_END] - ts[TS_AI_START]) * 1000.0 / (double)gTsFreq;
        gReadback[i]->Unmap(0, nullptr);

        if (gpuMs > 0 && gpuMs < 1000.0) gTel->gpuFrameMs = (float)gpuMs;
        gTel->rtGpuMs = (float)rtMs;
        gTel->aiGpuMs = (float)aiMs;
        gTel->rtDispatches = p.rtCount;
        // RT / Tensor 的百分比统一在 UpdateTelemetryCommon 里按**帧周期**算
        // （见那里的说明：用 gpuFrameMs 当分母会严重低估）
        gTel->rtMeasured = p.rtCount ? 1 : 0;
        gTel->tensorMeasured = (p.aiUsed && aiMs > 0.0) ? 1 : 0;
    }
}

void SubmitPendingResolve(ID3D12CommandQueue* q, uint32_t rtCount, bool aiUsed,
                          uint32_t batchCount) {
    int idx = -1;
    for (int i = 0; i < 4; ++i)
        if (!gPending[i].busy) { idx = i; break; }
    if (idx < 0) return;
    ID3D12GraphicsCommandList* l = BeginList();
    if (!l) return;
    l->ResolveQueryData(gHeap, D3D12_QUERY_TYPE_TIMESTAMP, 0, TS_SLOTS, gReadback[idx], 0);
    EndList(q);
    gPending[idx].busy = true;
    gPending[idx].fenceVal = gFenceVal;
    gPending[idx].rtCount = rtCount;
    gPending[idx].batchCount = batchCount;
    gPending[idx].aiUsed = aiUsed;
}

// ---------------------------------------------------------------------------
// D3D11 时间戳
// ---------------------------------------------------------------------------
bool InitTs11(ID3D11Device* dev) {
    for (int i = 0; i < 3; ++i) {
        D3D11_QUERY_DESC qd{};
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        if (FAILED(dev->CreateQuery(&qd, &gTs11[i].disjoint))) return false;
        qd.Query = D3D11_QUERY_TIMESTAMP;
        if (FAILED(dev->CreateQuery(&qd, &gTs11[i].a))) return false;
        if (FAILED(dev->CreateQuery(&qd, &gTs11[i].b))) return false;
    }
    return true;
}

void Ts11Tick(uint32_t seq) {
    if (!gCtx11) return;
    int cur = (int)(seq % 3);
    int prev = (int)((seq + 2) % 3);
    int old = (int)((seq + 1) % 3);

    if (gTs11[prev].started) {
        gCtx11->End(gTs11[prev].b);
        gCtx11->End(gTs11[prev].disjoint);
        gTs11[prev].started = false;
        gTs11[prev].pendingRead = true;
    }
    gCtx11->Begin(gTs11[cur].disjoint);
    gCtx11->End(gTs11[cur].a);
    gTs11[cur].started = true;

    if (gTs11[old].pendingRead) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        if (gCtx11->GetData(gTs11[old].disjoint, &dj, sizeof(dj), 0) == S_OK) {
            UINT64 a = 0, b = 0;
            if (gCtx11->GetData(gTs11[old].a, &a, sizeof(a), 0) == S_OK &&
                gCtx11->GetData(gTs11[old].b, &b, sizeof(b), 0) == S_OK && !dj.Disjoint &&
                dj.Frequency && b > a) {
                double ms = (double)(b - a) * 1000.0 / (double)dj.Frequency;
                if (ms > 0 && ms < 1000.0 && gTel) gTel->gpuFrameMs = (float)ms;
            }
            gTs11[old].pendingRead = false;
        }
    }
}

// ---------------------------------------------------------------------------
// 叠加绘制
// ---------------------------------------------------------------------------
struct DrawCtx {
    np::PanelRenderer* r;
    np::PanelData* d;
    NPConfig c;
};
void DrawCb(ID2D1RenderTarget* rt, void* ud) {
    DrawCtx* dc = reinterpret_cast<DrawCtx*>(ud);
    dc->r->Render(rt, 0.0f, 0.0f, *dc->d, dc->c);
}

int gBw = 0, gBh = 0;   // 面板纹理的物理像素尺寸
static NPHistory gHist; // 游戏内图表历史（约每 8 帧采一个点，256 点 ≈ 数十秒）

bool RenderPanel(const NPConfig& cfg) {
    np::BuildPanelData(gPd, cfg, Sens(), *gTel, &gHist);
    float pw = 0, ph = 0;
    gPanel.Measure(gPd, cfg, &pw, &ph);
    int w = (int)(pw + 3.0f), h = (int)(ph + 3.0f);
    if (w < 8 || h < 8) return false;
    // 诊断：面板逻辑尺寸一变就记一条。
    // 「HUD 宽度随数值一直变化」这个问题就靠它客观判定 ——
    // 理想情况整局只出现一次（首帧建立缓存），之后不该再变。
    {
        static int lastW = 0, lastH = 0;
        if (w != lastW || h != lastH) {
            lastW = w; lastH = h;
            Log("panel logical size %dx%d | label=%.1f value=%.1f rng=%.1f head=%.1f "
                "char=%.2f content=%.1f | rowH=%.2f sep=%.2f",
                w, h, (double)gPanel.diag.labelW, (double)gPanel.diag.valueW,
                (double)gPanel.diag.rngW, (double)gPanel.diag.headW,
                (double)gPanel.diag.charW, (double)gPanel.diag.contentW,
                (double)gPanel.diag.rowH, (double)gPanel.diag.sepH);
        }
    }
    DrawCtx dc{&gPanel, &gPd, cfg};
    if (!gBmp.Render(w, h, DrawCb, &dc)) return false;
    gBw = gBmp.width();     // 物理像素（含光栅倍率）
    gBh = gBmp.height();
    return true;
}

// ---------------------------------------------------------------------------
// Present 处理
// 这两个函数要放在匿名命名空间之外，extern "C" 的跳板才能调用
// ---------------------------------------------------------------------------
}  // namespace

// 必须定义在匿名命名空间**之外**：np_draw.cpp 用的是 extern "C" 声明，
// 匿名命名空间里的名字没有外部链接，链接会直接失败。
//
// 用途：叠加自己画的那一个全屏三角形也会走进我们钩的 DrawInstanced/
// DrawIndexedInstanced，于是被算进「游戏本帧的 draw call 数」——面板每 50ms
// 重绘一次，统计里就会周期性地多出 1 个 draw。画自己的东西时置上这个标志，
// 计数钩子就不再累加。
// 叠加绘制期间抑制「游戏 draw call」统计。
// ⚠ 必须是 thread_local：叠加是自己人画的，只应该抑制**我们自己这一路**的计数。
//   原来是普通全局 bool —— 叠加在 Present 线程绘制时，游戏在别的提交线程
//   恰好也 draw 一笔，那笔就会被误抑制（少数一次）。
extern "C" thread_local bool gNpCountingOverlayDraws = false;

void UpdateTelemetryCommon(const NPConfig& cfg, uint64_t nowQpc, bool realPresent) {
    NPTelemetry& t = *gTel;
    t.attached = 1;
    t.tickMs = GetTickCount64();
    t.gfxApi = (uint32_t)gApi;
    t.aiModules = gAiModules;
    t.drawCalls = gDraws.load(std::memory_order_relaxed);
    t.dispatches = gDispatches.load(std::memory_order_relaxed);
    t.asBuilds = gAsBuilds.load(std::memory_order_relaxed);

    if (gVpW && gVpH) { t.renderW = gVpW; t.renderH = gVpH; }

    // 显示用的数值做平滑。
    //
    // 底层数据是对的（实测帧周期 avg=16.66ms = 精确 60fps），但逐帧间隔本身在
    // 9~24ms 抖动。**FPS 改用时间窗口计数**：锁 60 时窗口内就是 30 帧/0.5 秒
    // = 恰好 60.0，不会像逐帧换算那样 60↔100 乱跳。
    // 帧时间用指数平滑（图表也用它画，否则锯齿看着像剧烈波动）。
    static uint32_t fpsWinN = 0;
    static uint64_t fpsWinT0 = 0;
    static np::Ema msEma(0.10f);
    float instMs = t.frameMs;
    if (instMs > 0.0001f) t.frameMsAvg = msEma.update(instMs);
    else t.frameMsAvg = instMs;
    uint64_t nowMs = GetTickCount64();
    if (!fpsWinT0) fpsWinT0 = nowMs;
    // ⚠ 必须只数**真正呈现**的那些调用。
    //   这游戏每帧调两次 Present（一次 DXGI_PRESENT_TEST 探测 + 一次真的），
    //   不过滤的话窗口计数正好是真实帧率的两倍 —— 用户实测显示 120（实际 60）。
    if (realPresent) ++fpsWinN;
    if (nowMs - fpsWinT0 >= 500) {
        t.fps = (float)fpsWinN * 1000.0f / (float)(nowMs - fpsWinT0);
        fpsWinN = 0;
        fpsWinT0 = nowMs;
    }

    // RT / Tensor 占比的分母是**帧周期**（frameMs），不是 gpuFrameMs。
    //
    // 为什么：gpuFrameMs 是「本帧第一个 ExecuteCommandLists 到 Present」这段
    // GPU 时间轴跨度。GPU 一旦把本帧命令跑完就空转等下一帧，而 TS_END 要等
    // 我们到 Present 才提交 —— 于是这段跨度实际上约等于帧周期，是个**上界**。
    // 拿它当分母会把光追占比严重低估（用户实测：开了光追却看着像没开）。
    // 用帧周期当分母，含义是「光追 pass 吃掉了这一帧多少时间预算」，
    // 既准确又稳定，也是玩家真正关心的口径。绝对值（ms）同时给出来。
    if (t.frameMs > 0.0001f) {
        t.rtLoad = t.rtGpuMs / t.frameMs * 100.0f;
        t.tensorLoad = t.aiGpuMs / t.frameMs * 100.0f;
    }
    t.fpsAvg = gStats.avgFps(600);
    // ★ Low 帧口径：**百分位**（与 NVIDIA 驱动面板 / FrameView 一致）
    //   实测驱动面板 1% = 59、本程序 = 30，差异就来自口径：
    //     按帧数(P99)  : 掉垂直同步的帧若不足 1%，P99 落在正常帧上 -> 60
    //     按时间(累加) : 33.3ms 的 spike 消耗预算的速度是正常帧两倍，
    //                    6 帧正好用满 1% 预算 -> 恰好停在 spike 上 -> 30（刀刃效应）
    //   驱动面板显示的是前者，所以按前者来。
    t.fpsLow1 = gStats.lowPercentileFps(99.0f, 1200);
    t.fpsLow01 = gStats.lowPercentileFps(99.9f, 1200);
    t.p99Ms = gStats.percentileMs(99.0f);
    t.p999Ms = gStats.percentileMs(99.9f);

    // ★ GPU 帧时间：优先用**系统 PDH** 按游戏进程算出来的值，并在这里统一回填
    //   `t.gpuFrameMs`。
    //
    //   为什么必须在**源头**回填而不是逐个消费点打补丁：t.gpuFrameMs 是多个
    //   地方共同的数据源 —— 图表环形缓冲 graphGpu、逐帧环形缓冲 gpuFrames、
    //   以及面板的兜底显示。而钩子自己那套「逐批 GPU 时间戳夹取」已经整个拆掉了
    //   （它给每次 ExecuteCommandLists 前后各插时间戳，真实游戏一帧十几到几十批，
    //   一帧要 32+ 个分配器，直接导致 allocator starved 和叠加永久停画）。
    //   拆掉之后这个字段再没人写、**恒为 0** —— 于是面板数值是对的（走 s.gpuBusyMs），
    //   但所有以它为源的曲线都是**一条零线**（用户实测反馈："gpu 帧这个曲线全部为 0"）。
    {
        float sysBusy = Sens().gpuBusyMs;
        if (sysBusy >= 0.0f) t.gpuFrameMs = sysBusy;
    }

    // 图表环形缓冲
    uint32_t gi = t.graphWrite;
    t.graphFrame[gi] = t.frameMs;
    t.graphCpu[gi] = t.cpuFrameMs;
    t.graphGpu[gi] = t.gpuFrameMs;
    t.graphWrite = (gi + 1) % NP_GRAPH_CAP;
    if (t.graphCount < NP_GRAPH_CAP) ++t.graphCount;
    (void)nowQpc; (void)cfg;

    gDraws.store(0, std::memory_order_relaxed);
    gDispatches.store(0, std::memory_order_relaxed);
    gAsBuilds.store(0, std::memory_order_relaxed);
}

bool EnsureD3D12Hooks(ID3D12Device* dev);

// D2D / DirectWrite 的初始化必须发生在渲染线程上（单线程 D2D 工厂不能跨线程使用，
// 多线程渲染的游戏会因此闪退）。Worker 线程不做任何图形初始化。
bool gRtReady = false;
void EnsureRt() {
    if (gRtReady) return;
    gRtReady = true;
    npg::GfxInit();
    // 面板是先画进一张 DIB 位图再上传给游戏的，那条路走的是 npb（D2D DC 渲染目标）。
    // 少了这一句 PanelBitmap::Ensure() 里 `if (!gD2D) return false;` 会一直失败，
    // 表现就是「注入成功、数据也有，但游戏里看不到面板」。
    npb::GfxInit();
    gPanel.Init();
    Log("render-thread gfx init done (panel=%d d2d=%d)", gPanel.ok() ? 1 : 0,
        npb::Factory() ? 1 : 0);
}

HRESULT PresentCommon(IDXGISwapChain* sc, UINT sync, UINT flags,
                      const DXGI_PRESENT_PARAMETERS* pp, bool isP1) {
    // 原始函数按「这条交换链属于哪张 vtable」来取：blt / flip 可能是不同的类，
    // 原始 Present 也就不是同一个函数，全局只存一个会串线。
    void** scVt = sc ? *reinterpret_cast<void***>(sc) : nullptr;
    SwapVtEntry* entry = SwapVtFor(scVt);
    FnPresent  op  = entry ? entry->origPresent  : nullptr;
    FnPresent1 op1 = entry ? entry->origPresent1 : nullptr;

    auto CallOriginal = [&]() -> HRESULT {
        if (isP1 && op1) return op1((IDXGISwapChain1*)sc, sync, flags, pp);
        if (op) return op(sc, sync, flags);
        return E_FAIL;
    };

    // ★ gUnloading 必须放在 gTel 解引用**之前**。
    //   SelfUnloadNow() 会先置 gUnloading、Sleep 排空，然后 UnmapViewOfFile(gTel)
    //   并把 gTel 置空；如果渲染线程正好晚一步进来，「!gTel」检查可能通过、
    //   而下一行 *gTel 指向的视图已经被解除映射 —— 直接访问违例。
    //   先看 gUnloading 能把这个窗口收掉（这个变量在自卸载结束后也不会再变回去）。
    if (gUnloading) return CallOriginal();
    if (!gTel || !sc) return CallOriginal();
    if (gInPresent) return CallOriginal();
    gInPresent = true;
    if (!SehReady()) { gInPresent = false; return CallOriginal(); }
    if (setjmp(gJmp) != 0) {
        gJmpArmed = false;
        gInPresent = false;
        return CallOriginal();
    }
    gJmpArmed = true;
    EnsureRt();

    uint64_t now = NowQpc();
    uint32_t seq = ++gSeq;
    NPConfig cfg = Cfg();

    // ---- 首次接入：识别设备
    // 注：GetDevice / GetImmediateContext 本身就会 AddRef，设备与上下文是安全的。
    // 真正缺 AddRef 的是命令队列（见 NpECL 里的说明）。
    if (!gDev11 && !gDev12) {
        if (SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&gDev11))) && gDev11) {
            gApi = NP_API_D3D11;
            gDev11->GetImmediateContext(&gCtx11);
            gOv11.Init(gDev11);
            InitTs11(gDev11);
        } else if (SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&gDev12))) && gDev12) {
            gApi = NP_API_D3D12;
            EnsureD3D12Hooks(gDev12);
        } else {
            // 兜底判断：按已加载模块猜
            if (GetModuleHandleW(L"d3d12.dll")) gApi = NP_API_D3D12;
            else if (GetModuleHandleW(L"d3d11.dll")) gApi = NP_API_D3D11;
            else if (GetModuleHandleW(L"vulkan-1.dll")) gApi = NP_API_VULKAN;
            else if (GetModuleHandleW(L"opengl32.dll")) gApi = NP_API_OPENGL;
        }
        wchar_t selfPath[MAX_PATH];
        if (GetModuleFileNameW(nullptr, selfPath, MAX_PATH)) {
            char buf[NP_NAME_LEN]{};
            WideCharToMultiByte(CP_UTF8, 0, selfPath, -1, buf, NP_NAME_LEN - 1, nullptr, nullptr);
            const char* slash = strrchr(buf, '\\');
            NPCopyStr(gTel->processName, NP_NAME_LEN, slash ? slash + 1 : buf);
        }
    }

    // ---- 分辨率与全屏模式
    if (sc) {
        DXGI_SWAP_CHAIN_DESC sd{};
        if (SUCCEEDED(sc->GetDesc(&sd))) {
            gTel->windowW = sd.BufferDesc.Width;
            gTel->windowH = sd.BufferDesc.Height;
            if (!gTel->renderW) { gTel->renderW = sd.BufferDesc.Width; gTel->renderH = sd.BufferDesc.Height; }
        }
        BOOL fs = FALSE;
        if (SUCCEEDED(sc->GetFullscreenState(&fs, nullptr)) && fs)
            gTel->presentMode = NP_PM_EXCLUSIVE;
        else {
            IDXGISwapChain1* sc1 = nullptr;
            gTel->presentMode = NP_PM_WINDOWED;
            if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1))) && sc1) {
                HWND hw = nullptr;
                if (SUCCEEDED(sc1->GetHwnd(&hw)) && hw) {
                    LONG st = GetWindowLongW(hw, GWL_STYLE);
                    LONG ex = GetWindowLongW(hw, GWL_EXSTYLE);
                    if ((st & WS_POPUP) && !(ex & WS_EX_TOPMOST) && (st & WS_VISIBLE))
                        gTel->presentMode = NP_PM_BORDERLESS;
                }
                sc1->Release();
            }
        }
    }

    // ---- 帧时间
    //
    // 只统计**真正呈现**的那些调用。`DXGI_PRESENT_TEST` 只是问一句「能不能呈现」，
    // 不产生新帧；把它算进去会让帧数虚高、帧时间忽上忽下 —— 正是「游戏稳稳 60、
    // 我们却在 60~70 之间波动」的一个来源。
    bool realPresent = (flags & DXGI_PRESENT_TEST) == 0;
    if (realPresent && gLastPresentQpc && now > gLastPresentQpc) {
        float fm = (float)QpcMs(now - gLastPresentQpc);
        // 超过 1 秒的间隔不是「一帧」，是切出去/加载/挂起留下的空档。
        // 把它塞进统计会把 1% Low / 0.1% Low 直接拖到个位数 ——
        // 用户看到「稳定 60fps 却显示 1% Low = 10」就是这么来的。
        if (fm > 0.02f && fm < 1000.0f) {
            gTel->frameMs = fm;
            gStats.push(fm);
            gTel->frameTotal++;
            uint32_t wi = gTel->frameWrite;
            gTel->frames[wi] = fm;
            gTel->cpuFrames[wi] = gTel->cpuFrameMs;
            gTel->gpuFrames[wi] = gTel->gpuFrameMs;
            gTel->frameWrite = (wi + 1) % NP_FRAME_CAP;
        }
    }
    // CPU 帧时间 = 帧周期 − 卡在 Present 里等垂直同步的时间。
    //
    // 为什么不量「第一次提交 → Present」：这游戏有后台流式线程在不停提交，
    // 「本帧第一次提交」可能发生在**上一帧 Present 的阻塞当中**，量出来就会
    // 恒等于帧周期（16.66ms）—— 那是帧周期，不是 CPU 干的活。
    // 正确分解：帧周期 = CPU 自己干活的时间 + 卡在 Present 里等显示器的时间。
    // 这游戏用 Present(1,0)，确实会在 Present 里阻塞到垂直消隐。
    // 60fps 且 CPU 有余量时，这个数应该是几毫秒。
    // ---- CPU 帧拆成两段（Reflex 的口径）
    //
    // 用户明确要的是**模拟阶段**，不是两段之和 —— 因为「渲染提交」那一段
    // 本质上是在排队（录制命令列表 + 等提交），把它算进 CPU 帧会让这个数
    // 被帧率绑架（锁 60 就恒等于 16.66）。
    //
    //   模拟阶段 = 上一帧 Present 返回 → 本帧渲染线程第一次提交
    //   渲染提交 = 第一次提交 → 调 Present
    //
    // 时刻都来自我们自己的钩子点，不需要 Reflex 的 NvAPI 接口。
    if (gRenderTid == GetCurrentThreadId() && gPresentRetQpc && gSimEndQpc > gPresentRetQpc) {
        gTel->simMs = (float)QpcMs(gSimEndQpc - gPresentRetQpc);
        gTel->submitMs = (float)QpcMs(now - gSimEndQpc);
        gTel->cpuFrameMs = gTel->simMs;
    } else if (gApi == NP_API_D3D12 && gTel->frameMs > 0.0001f) {
        float cpu = gTel->frameMs - gLastInPresentMs;
        gTel->cpuFrameMs = cpu > 0.0f ? cpu : 0.0f;
    } else {
        gTel->cpuFrameMs = gTel->frameMs;
    }
    {
        static np::Ema cpuEma(0.10f);
        gTel->cpuFrameMsAvg = cpuEma.update(gTel->cpuFrameMs);
    }
    if (realPresent) gLastPresentQpc = now;

    // 诊断：每 5 秒把 Present 的节奏记一条。
    // 「我们的帧率为什么和游戏 OSD 对不上」这种问题，只能靠这个定位：
    // 记录呈现次数、其中有多少是 TEST、sync interval 分布、以及帧时间的极值。
    {
        static uint32_t diagN = 0, diagTest = 0, diagSync0 = 0, diagSync1 = 0;
        static uint32_t diagOther = 0;
        static uint64_t diagT0 = 0;
        static float diagMin = 1e9f, diagMax = 0.0f;
        static double diagSum = 0.0;
        ++diagN;
        if (!realPresent) ++diagTest;
        else if (sync == 0) ++diagSync0;
        else if (sync == 1) ++diagSync1;
        else ++diagOther;
        if (realPresent && gTel->frameMs > 0.01f) {
            if (gTel->frameMs < diagMin) diagMin = gTel->frameMs;
            if (gTel->frameMs > diagMax) diagMax = gTel->frameMs;
            diagSum += gTel->frameMs;
        }
        uint64_t ms = GetTickCount64();
        if (!diagT0) diagT0 = ms;
        if (ms - diagT0 >= 5000 && diagN > 0) {
            uint32_t real = diagN - diagTest;
            Log("present diag 5s: calls=%u real=%u test=%u sync0=%u sync1=%u other=%u "
                "frameMs min=%.2f max=%.2f avg=%.2f",
                diagN, real, diagTest, diagSync0, diagSync1, diagOther,
                diagMin > 1e8f ? 0.0f : diagMin, diagMax, real ? (float)(diagSum / real) : 0.0f);
            diagN = diagTest = diagSync0 = diagSync1 = diagOther = 0;
            diagMin = 1e9f; diagMax = 0.0f; diagSum = 0.0;
            diagT0 = ms;
        }
    }

    // 图表历史：每 8 帧采一个点，256 点 ≈ 144FPS 下 14 秒 / 60FPS 下 34 秒
    if ((gTel->frameTotal & 7u) == 0) {
        const NPSensors& ss = Sens();
        gHist.sampleMs = (uint32_t)std::max(8.0, 8000.0 / std::max(1.0f, gTel->fpsAvg));
        // ★ GPU 曲线必须用 **PDH 的值**（ss.gpuBusyMs），不能用 gTel->gpuFrameMs。
        //
        //   原因：gpuFrameMs 来自钩子的「逐批 GPU 时间戳夹取」，而那套东西已经
        //   整个拆掉了 —— 它给每一次 ExecuteCommandLists 前后各插一条时间戳，
        //   真实游戏一帧提交十几到几十批，一帧要 32+ 个分配器，直接导致
        //   `allocator starved` 和叠加永久停画。
        //   拆掉之后 gpuFrameMs 就再也没人写，**恒为 0** —— 于是面板上的
        //   数值是对的（走 s.gpuBusyMs），但曲线是**一条零线**（用户实测反馈）。
        //   这里改成优先用 PDH 的每帧 GPU 时间，钩子那个字段只作兜底。
        float gpuSeries = (ss.gpuBusyMs >= 0.0f) ? ss.gpuBusyMs : gTel->gpuFrameMs;
        NPHistoryPush(&gHist, gTel->fps, gTel->fpsAvg, gTel->fpsLow1, gTel->fpsLow01,
                      ss.cpuUsage, ss.gpuUsage, gTel->frameMsAvg, gTel->cpuFrameMsAvg,
                      gpuSeries);
    }

    // 设备移除检测：GPU 侧非法操作会导致 device removed，游戏随即自杀式退出。
    // 注意健康的设备返回 S_OK —— 拿到别的失败码说明设备指针本身有问题，也要记下来。
    if (gDev12 && (gTel->frameTotal & 127u) == 0) {
        HRESULT r = gDev12->GetDeviceRemovedReason();
        if (r == DXGI_ERROR_DEVICE_REMOVED || r == DXGI_ERROR_DEVICE_HUNG ||
            r == DXGI_ERROR_DEVICE_RESET || r == DXGI_ERROR_DRIVER_INTERNAL_ERROR)
            Log("D3D12 device lost: hr=0x%08lx", (unsigned long)r);
        else if (FAILED(r))
            Log("D3D12 GetDeviceRemovedReason hr=0x%08lx dev=%p (设备指针可疑)",
                (unsigned long)r, (void*)gDev12);
    }

    // ---- GPU 时间戳收尾
    // D3D12 侧一旦出过不可恢复的错就彻底停手（gD12Broken），
    // 宁可全程没有 RT/Tensor 数据和叠加，也不能把游戏搞崩。
    bool d12ok = !gD12Broken.load(std::memory_order_acquire);
    if (gApi == NP_API_D3D12 && gQueue12 && gHeap && d12ok) {
        HarvestTimestamps();
        SubmitPendingResolve(gQueue12, gRtDispatches.load(std::memory_order_relaxed), gAiSpanOpen,
                             gBatchCount.load(std::memory_order_relaxed));
        gRtDispatches.store(0, std::memory_order_relaxed);
        gAiSpanOpen = false;
    } else if (gApi == NP_API_D3D11) {
        Ts11Tick(seq);
    }

    UpdateTelemetryCommon(cfg, now, realPresent);

    // ---- 叠加
    bool wantOverlay = (cfg.overlayMode != 2) && (gDev11 || gDev12) &&
                       !(gDev12 && !d12ok);
    bool panelOk = false, drawnOk = false;
    // ★ 原来这里（后台缓冲属于别的设备时）是 `bb->Release(); return CallOriginal();`
    //   —— 从 PresentCommon 的**中间**直接返回，把函数尾部的一整套收尾全跳过了：
    //     gInPresent = false / gFrameStarted = false / gLastPresentQpc / gLastInPresentMs。
    //   于是 gInPresent 永远是 true，之后每一帧都在开头「已在 Present 中」分支里
    //   直通原函数 —— 叠加和 GPU 时间戳**永久失效**，帧时间也不再更新。
    //   现在改成置一个标志跳过本帧叠加，收尾照常执行。
    bool stateOk = true;
    if (wantOverlay) {
        uint64_t ms = GetTickCount64();
        uint32_t hz = cfg.updateHz ? cfg.updateHz : 20;
        if (ms - gLastPanelMs >= 1000u / hz) {
            gLastPanelMs = ms;
            panelOk = RenderPanel(cfg);
        } else {
            panelOk = (gBw > 0 && gBh > 0);
        }
        if (gBw > 0 && gBh > 0) {
            // 光栅倍率以 1080p 为基准随分辨率放大，面板在各分辨率下占屏比例一致
            float s = npb::RasterScale();
            float x = cfg.offsetX * s, y = cfg.offsetY * s;
            if (gApi == NP_API_D3D11 && gDev11 && gCtx11) {
                // flip 模型（FLIP_DISCARD / FLIP_SEQUENTIAL）下 GetBuffer(0) 不一定是
                // 当前正在显示的那张后台缓冲，必须问 GetCurrentBackBufferIndex()，
                // 否则面板只会画进某一张、交替闪烁甚至完全看不见。
                // 只有老式 blt 模型（DISCARD）才固定是 0。
                UINT bbIndex = 0;
                IDXGISwapChain3* sc3 = nullptr;
                if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3) {
                    bbIndex = sc3->GetCurrentBackBufferIndex();
                    sc3->Release();
                }
                ID3D11Texture2D* bb = nullptr;
                if (SUCCEEDED(sc->GetBuffer(bbIndex, IID_PPV_ARGS(&bb))) && bb) {
                    ID3D11Device* bdev = nullptr;
                    bb->GetDevice(&bdev);
                    bool sameDev = bdev == gDev11;
                    if (bdev) bdev->Release();
                    if (!sameDev) { stateOk = false; }
                    D3D11_TEXTURE2D_DESC bd{};
                    bb->GetDesc(&bd);
                    float ns = std::clamp((float)bd.Height / 1080.0f, 1.0f, 2.5f);
                    if (stateOk && fabsf(ns - s) > 0.05f) { npb::SetRasterScale(ns); RenderPanel(cfg); s = ns; }
                    float px = (float)bd.Width - gBw - x;
                    if (stateOk)
                        drawnOk = gOv11.Draw(gDev11, gCtx11, bb, gBmp.pixels(), gBw, gBh,
                                             gBmp.stride(), px, y);
                    bb->Release();
                }
            } else if (gApi == NP_API_D3D12 && gDev12 && gQueue12) {
                IDXGISwapChain3* sc3 = nullptr;
                UINT idx = 0;
                if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3) {
                    idx = sc3->GetCurrentBackBufferIndex();
                    sc3->Release();
                }
                ID3D12Resource* bb = nullptr;
                if (SUCCEEDED(sc->GetBuffer(idx, IID_PPV_ARGS(&bb))) && bb) {
                    ID3D12Device* bdev = nullptr;
                    bool sameDev = SUCCEEDED(bb->GetDevice(IID_PPV_ARGS(&bdev))) && bdev == gDev12;
                    if (bdev) bdev->Release();
                    if (!sameDev) { stateOk = false; }
                    D3D12_RESOURCE_DESC rd = bb->GetDesc();
                    float ns = std::clamp((float)rd.Height / 1080.0f, 1.0f, 2.5f);
                    if (stateOk && fabsf(ns - s) > 0.05f) { npb::SetRasterScale(ns); RenderPanel(cfg); s = ns; }
                    float px = (float)rd.Width - gBw - x;
                    ID3D12GraphicsCommandList* l = stateOk ? BeginList() : nullptr;
                    if (l) {
                        drawnOk = gOv12.Record(gDev12, l, bb, D3D12_RESOURCE_STATE_PRESENT,
                                               gBmp.pixels(), gBw, gBh, gBmp.stride(), px, y,
                                               (float)rd.Width, (float)rd.Height);
                        EndList(gQueue12);
                    }
                    bb->Release();
                }
            }
        }
    }

    // 「注入成功、数据也对，但游戏里看不到面板」是最难查的一类问题。
    // 每秒最多记一条状态：want=是否打算画 / panel=位图是否画成功 / rec=是否提交到交换链。
    // stateOk=false 只是「这条后台缓冲不属于我们接入的设备」，不是故障，不刷日志。
    if (wantOverlay && stateOk && !drawnOk) {
        static uint64_t lastOvLog = 0;
        uint64_t ms = GetTickCount64();
        if (ms - lastOvLog >= 1000) {
            lastOvLog = ms;
            Log("overlay NOT drawn: want=%d panel=%d bmp=%dx%d mode=%u dev11=%d dev12=%d queue=%d d2d=%d",
                wantOverlay ? 1 : 0, panelOk ? 1 : 0, gBw, gBh, cfg.overlayMode, gDev11 ? 1 : 0,
                gDev12 ? 1 : 0, gQueue12 ? 1 : 0, npb::Factory() ? 1 : 0);
        }
    } else if (wantOverlay && drawnOk) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            Log("overlay drawn: %dx%d scale=%.2f", gBw, gBh, (double)npb::RasterScale());
        }
    }

    gJmpArmed = false;

    // 原始 Present 的耗时（`Present(1,0)` 会阻塞到垂直消隐）。
    // 下一帧算 CPU 帧时间时把它扣掉 —— 那是等显示器，不是 CPU 在干活。
    uint64_t tPresent0 = NowQpc();
    HRESULT hr = CallOriginal();
    uint64_t tPresent1 = NowQpc();
    gLastInPresentMs = (float)QpcMs(tPresent1 - tPresent0);
    if (gTel) gTel->msInPresent = gLastInPresentMs;

    // 记下渲染线程和「本帧 Present 返回的时刻」——它就是下一帧模拟阶段的起点
    gRenderTid = GetCurrentThreadId();
    gPresentRetQpc = tPresent1;
    gSimEndQpc = 0;

    // 帧边界放在 Present **返回之后**：阻塞期间后台线程提交的命令算下一帧，
    // 不然它们会被算进本帧，让「本帧第一次提交」的时刻变得毫无意义。
    gFrameStarted = false;
    gInPresent = false;
    return hr;
}

// ---------------------------------------------------------------------------
// 安装钩子
//
// 兼容性设计（DX12 游戏注入即闪退的教训）：
//   * 注入时先只挂 DXGI 工厂的 CreateSwapChain*，不去碰设备；
//   * 游戏自己创建交换链时，在它的交换链 vtable 上补 Present/Present1；
//   * **但游戏通常早就把交换链建好了**，工厂钩子不会再被调用 —— 这时
//     由 ProbeSwapChainVtable() 自己造一条临时交换链把 vtable 拿到手；
//   * D3D12 的队列/命令列表 vtable 在游戏首次 Present（拿到游戏 device）后惰性安装。
// ---------------------------------------------------------------------------

// 判断一个 vtable 槽位是不是「看起来正常」的函数指针：
// 落在已提交且可执行的内存里，且不等于同一张表里别的槽位。
bool PlausibleCodePtr(void* p, void* a, void* b) {
    if (!p || p == a || p == b) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    DWORD prot = mbi.Protect & 0xff;
    return prot == PAGE_EXECUTE || prot == PAGE_EXECUTE_READ ||
           prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
}

// 某个函数指针属于哪个模块（诊断用）。
//
// ⚠ 这里的教训值得写下来：原来的健全性检查要求这些槽位**必须落在 dxgi.dll /
// d3d12.dll 里**，那套假设只在我那个干净的自建宿主里成立。真实游戏里模块布局
// 完全不同（别的叠加层、厂商模块、D3D11 与 D3D12 走不同实现），
// 结果检查全部失败 → 命令列表钩子一个都没装 → **光追和 DLSS 数据全丢**，
// 表面现象就是「未检测到 DXR」「AI 已启用 · 估算中」。
// 所以现在只检查「是可执行代码、槽位互不相同」，并把实际归属模块打进日志。
std::string PtrOwner(void* p) {
    char buf[160];
    HMODULE mod = nullptr;
    if (p && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCSTR)p, &mod) &&
        mod) {
        char path[MAX_PATH]{};
        GetModuleFileNameA(mod, path, MAX_PATH);
        const char* base = strrchr(path, '\\');
        snprintf(buf, sizeof(buf), "%s+0x%llx", base ? base + 1 : path,
                 (unsigned long long)((uintptr_t)p - (uintptr_t)mod));
    } else {
        snprintf(buf, sizeof(buf), "%p", p);
    }
    return std::string(buf);
}

// 给一条交换链补 Present / Present1 补丁。返回是否（已经）挂上。
bool PatchSwapChainVtable(IDXGISwapChain* sc) {
    if (!sc) return false;
    void** vt = *reinterpret_cast<void***>(sc);
    if (!vt) return false;
    if (SwapVtFor(vt)) return true;                  // 这张表补过了
    if (gSwapVtCount >= NP_MAX_SWAPVT) return false;

    // 只要求「是可执行代码、几个槽位互不相同」。
    // Present 有可能已经被别的叠加层（Steam / Discord / RTSS / MSI Afterburner）
    // 换成它们的跳板 —— 那也能正常串接，所以不能因为「不在 dxgi 里」就拒掉。
    void* pPresent  = vt[Vt::SwapChain::Present];
    void* pPresent1 = vt[Vt::SwapChain::Present1];
    void* pGetBuf   = vt[Vt::SwapChain::GetBuffer];
    void* pGetDesc  = vt[Vt::SwapChain::GetDesc];
    if (!PlausibleCodePtr(pPresent, pPresent1, pGetBuf) ||
        !PlausibleCodePtr(pGetDesc, pPresent, pPresent1)) {
        Log("PatchSwapChainVtable: implausible vtable, skip. Present=%s GetBuffer=%s GetDesc=%s",
            PtrOwner(pPresent).c_str(), PtrOwner(pGetBuf).c_str(), PtrOwner(pGetDesc).c_str());
        return false;
    }

    SwapVtEntry& e = gSwapVts[gSwapVtCount];
    void* orig = nullptr;
    if (!Patch(vt, Vt::SwapChain::Present, (void*)&NpPresent, &orig) || !orig) return false;
    e.vt = vt;
    e.origPresent = (FnPresent)orig;

    // Present1 不一定存在（基类表可能就到不了这一格），失败也无所谓
    if (PlausibleCodePtr(pPresent1, pPresent, pGetBuf)) {
        void* o1 = nullptr;
        if (Patch(vt, Vt::SwapChain::Present1, (void*)&NpPresent1, &o1) && o1)
            e.origPresent1 = (FnPresent1)o1;
    }
    ++gSwapVtCount;
    Log("swapchain vtable patched: vt=%p present=%s present1=%p (now %d)", (void*)vt,
        PtrOwner((void*)e.origPresent).c_str(), (void*)e.origPresent1, gSwapVtCount);
    if (gTel) gTel->hookFlags |= NP_HOOK_PRESENT;
    return true;
}

// 注入时游戏往往已经在跑了，CreateSwapChain* 永远不会再被调用 ——
// 那就自己造一条最小交换链，把 vtable 拿到手，补丁打完立刻销毁。
// 补丁作用在共享 vtable 上，所以游戏那条**已经存在的**交换链马上就被接管了。
bool ProbeSwapChainVtable() {
    if (PresentHooked()) return true;
    if (!GetModuleHandleW(L"dxgi.dll")) return false;
    if (!SehReady()) { Log("ProbeSwapChainVtable: no SEH handler"); return false; }
    Log("ProbeSwapChainVtable: probing");

    // 探测过程里任何一步炸了都不能带走游戏：longjmp 回来直接放弃。
    // 注意 longjmp 之后不要再碰 setjmp 之后声明的对象（值不确定），
    // 所以这里宁可泄漏这一次的 COM 引用，也不去 Release。
    if (setjmp(gJmp) != 0) {
        gJmpArmed = false;
        Log("ProbeSwapChainVtable: SEH during probe, abandoned");
        return PresentHooked();
    }
    gJmpArmed = true;

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl{};
    // 只用最普通的硬件设备；拿不到就退 WARP（虚拟机上也有软件光栅化）
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &dev, &fl, &ctx);
    if (FAILED(hr) || !dev) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                               D3D11_SDK_VERSION, &dev, &fl, &ctx);
    }
    if (SUCCEEDED(hr) && dev) {
        // 一个 8x8 的隐藏窗口，够 CreateSwapChainForHwnd 用
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = gSelf;
        wc.lpszClassName = L"NextPerfProbeWnd";
        RegisterClassExW(&wc);   // 已注册过也没关系
        HWND hwnd = CreateWindowExW(0, L"NextPerfProbeWnd", L"", WS_POPUP, 0, 0, 8, 8,
                                    nullptr, nullptr, gSelf, nullptr);

        IDXGIFactory1* f1 = nullptr;
        IDXGIFactory2* f2 = nullptr;
        if (hwnd && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f1))) && f1)
            f1->QueryInterface(IID_PPV_ARGS(&f2));

        if (f2) {
            // blt 和 flip 可能是两个不同的类（vtable 不同），两种都探一遍
            const DXGI_SWAP_EFFECT effects[2] = {DXGI_SWAP_EFFECT_DISCARD,
                                                 DXGI_SWAP_EFFECT_FLIP_DISCARD};
            for (int i = 0; i < 2 && !PresentHooked(); ++i) {
                DXGI_SWAP_CHAIN_DESC1 sd{};
                sd.Width = 8;
                sd.Height = 8;
                sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                sd.SampleDesc.Count = 1;
                sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                sd.BufferCount = (effects[i] == DXGI_SWAP_EFFECT_DISCARD) ? 1 : 2;
                sd.SwapEffect = effects[i];
                IDXGISwapChain1* sc = nullptr;
                HRESULT cr = f2->CreateSwapChainForHwnd(dev, hwnd, &sd, nullptr, nullptr, &sc);
                if (SUCCEEDED(cr) && sc) {
                    PatchSwapChainVtable(sc);
                    sc->Release();
                } else {
                    Log("ProbeSwapChainVtable: effect=%d create failed hr=0x%08lx", (int)i,
                        (unsigned long)cr);
                }
            }
            f2->Release();
        }
        if (f1) f1->Release();
        if (hwnd) DestroyWindow(hwnd);
        if (ctx) ctx->Release();
        dev->Release();
    } else {
        Log("ProbeSwapChainVtable: D3D11CreateDevice failed hr=0x%08lx", (unsigned long)hr);
    }

    gJmpArmed = false;
    if (!PresentHooked()) Log("ProbeSwapChainVtable: no vtable obtained");
    return PresentHooked();
}

bool InstallDxgi() {
    IDXGIFactory1* factory1 = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory1))) || !factory1) {
        Log("InstallDxgi: CreateDXGIFactory1 failed");
        return false;
    }
    IDXGIFactory* factory = nullptr;
    if (FAILED(factory1->QueryInterface(IID_PPV_ARGS(&factory))) || !factory) {
        factory1->Release();
        Log("InstallDxgi: QI IDXGIFactory failed");
        return false;
    }
    gFactoryVt = *reinterpret_cast<void***>(factory);
    void* orig = nullptr;
    Patch(gFactoryVt, Vt::Factory::CreateSwapChain, (void*)&NpCreateSwapChain, &orig);
    if (orig) gOrigCreateSC = (FnCreateSwapChain)orig;
    factory->Release();

    IDXGIFactory2* factory2 = nullptr;
    if (SUCCEEDED(factory1->QueryInterface(IID_PPV_ARGS(&factory2))) && factory2) {
        gFactory2Vt = *reinterpret_cast<void***>(factory2);
        orig = nullptr;
        Patch(gFactory2Vt, Vt::Factory2::CreateSwapChainForHwnd, (void*)&NpCreateSwapChainHwnd,
              &orig);
        if (orig) gOrigCreateSCHwnd = (FnCreateSwapChainHwnd)orig;
        orig = nullptr;
        Patch(gFactory2Vt, Vt::Factory2::CreateSwapChainForComposition,
              (void*)&NpCreateSwapChainComp, &orig);
        if (orig) gOrigCreateSCComp = (FnCreateSwapChainComp)orig;
        factory2->Release();
    }
    factory1->Release();
    Log("InstallDxgi: factory hooks ok (sc=%d schwnd=%d sccomp=%d)", gOrigCreateSC != nullptr,
        gOrigCreateSCHwnd != nullptr, gOrigCreateSCComp != nullptr);
    return gOrigCreateSC != nullptr || gOrigCreateSCHwnd != nullptr;
}

// 首次在 D3D12 游戏内 Present 时调用：此时手里是**游戏自己的 device**，
// 所有查询堆/fence/叠加资源都建在它的上面，队列与命令列表 vtable 也在这里补。
// 借用临时队列/列表只为拿 vtable 指针，补丁打完立即释放。
bool EnsureD3D12Hooks(ID3D12Device* dev) {
    // 失败必须能重试：首次 Present 时游戏可能还没准备好，一次失败就永久放弃
    // 会让 RT / Tensor 这两项数据永远拿不到。但也不能每帧都去建队列，
    // 所以限制次数 + 2 秒冷却。
    //
    // ⚠ 必须防止**并发重入**。Present 可能被游戏的多个线程同时调用，而
    //   attempts / lastTryMs 原来是普通静态量：两个线程可以同时通过检查，
    //   各自建一套查询堆 / fence / 叠加资源 —— 先建的那套句柄被覆盖，
    //   既泄漏又状态错乱（时间戳换算的 gTsFreq 也会跟着错）。
    //   这里的策略是「谁先抢到谁装，其他人立刻返回 false」：装好后 gOrigECL
    //   就非空了，下一帧所有线程都会走最前面的快速路径。
    static std::atomic<uint32_t> attempts{0};
    static std::atomic<uint64_t> lastTryMs{0};
    static std::atomic<bool> installing{false};

    if (gOrigECL) return true;
    if (!dev || gD12Broken.load(std::memory_order_acquire)) return false;

    bool exp = false;
    if (!installing.compare_exchange_strong(exp, true)) return false;   // 别的线程正在装
    // 无论从哪条路径返回都要放开这个闸
    struct InstallGuard {
        std::atomic<bool>* f;
        ~InstallGuard() { f->store(false, std::memory_order_release); }
    } guard{&installing};

    uint64_t nowMs = GetTickCount64();
    uint32_t at = attempts.load(std::memory_order_relaxed);
    uint64_t lt = lastTryMs.load(std::memory_order_relaxed);
    if (at >= 5 || (lt && nowMs - lt < 2000)) return false;
    lastTryMs.store(nowMs, std::memory_order_relaxed);
    attempts.store(at + 1, std::memory_order_relaxed);
    Log("EnsureD3D12Hooks: attempt %u", at + 1);

    ID3D12CommandQueue* q = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) || !q) {
        Log("EnsureD3D12Hooks: CreateCommandQueue failed");
        return false;
    }
    gQueueVt = *reinterpret_cast<void***>(q);
    void* orig = nullptr;
    if (Patch(gQueueVt, Vt::CommandQueue::ExecuteCommandLists, (void*)&NpECL, &orig))
        gOrigECL = (FnECL)orig;
    if (q->GetTimestampFrequency(&gTsFreq) != S_OK || gTsFreq == 0) gTsFreq = 1000000;
    bool tsOk = InitTs12(dev, q);
    bool ovOk = gOv12.Init(dev);
    if (gTel && ovOk) gTel->hookFlags |= NP_HOOK_OVERLAY;
    q->Release();
    Log("EnsureD3D12Hooks: ecl=%d ts=%d overlay=%d", gOrigECL != nullptr, tsOk, ovOk);

    // 命令列表级钩子（DXR / compute 统计，RT/Tensor 数据源）
    if (Cfg().deepEngineHook) {
        ID3D12CommandAllocator* alloc = nullptr;
        ID3D12GraphicsCommandList* list = nullptr;
        HRESULT ha = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 IID_PPV_ARGS(&alloc));
        HRESULT hl = FAILED(ha) ? ha
                                : dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc,
                                                         nullptr, IID_PPV_ARGS(&list));
        if (FAILED(ha) || FAILED(hl) || !list) {
            Log("EnsureD3D12Hooks: cannot create temp list (alloc=0x%08lx list=0x%08lx) -> "
                "NO RT/Tensor data", (unsigned long)ha, (unsigned long)hl);
        } else {
            ID3D12GraphicsCommandList4* cl4 = nullptr;
            HRESULT hq = list->QueryInterface(IID_PPV_ARGS(&cl4));
            if (FAILED(hq) || !cl4) {
                Log("EnsureD3D12Hooks: QI ID3D12GraphicsCommandList4 failed hr=0x%08lx "
                    "-> NO RT/Tensor data", (unsigned long)hq);
            } else {
                void** vt = *reinterpret_cast<void***>(cl4);
                // 只看「是可执行代码 + 槽位互不相同」，**不再要求落在 d3d12.dll 里**
                // （见 PtrOwner 上面的说明：按模块名过滤会把真实游戏的钩子全拒掉）
                bool sane = PlausibleCodePtr(vt[Vt::CommandList::DispatchRays],
                                             vt[Vt::CommandList::BuildRaytracingAccelerationStructure],
                                             vt[Vt::CommandList::Dispatch]) &&
                            PlausibleCodePtr(vt[Vt::CommandList::BuildRaytracingAccelerationStructure],
                                             vt[Vt::CommandList::DispatchRays],
                                             vt[Vt::CommandList::Dispatch]) &&
                            PlausibleCodePtr(vt[Vt::CommandList::Dispatch],
                                             vt[Vt::CommandList::DispatchRays],
                                             vt[Vt::CommandList::BuildRaytracingAccelerationStructure]) &&
                            PlausibleCodePtr(vt[Vt::CommandList::DrawInstanced],
                                             vt[Vt::CommandList::DispatchRays],
                                             vt[Vt::CommandList::Dispatch]);
                if (!sane) {
                    Log("EnsureD3D12Hooks: cmdlist vtable implausible -> NO RT/Tensor data. "
                        "DispatchRays=%s BAS=%s Dispatch=%s Draw=%s",
                        PtrOwner(vt[Vt::CommandList::DispatchRays]).c_str(),
                        PtrOwner(vt[Vt::CommandList::BuildRaytracingAccelerationStructure]).c_str(),
                        PtrOwner(vt[Vt::CommandList::Dispatch]).c_str(),
                        PtrOwner(vt[Vt::CommandList::DrawInstanced]).c_str());
                } else {
                    Log("EnsureD3D12Hooks: cmdlist vtable ok. DispatchRays=%s Dispatch=%s",
                        PtrOwner(vt[Vt::CommandList::DispatchRays]).c_str(),
                        PtrOwner(vt[Vt::CommandList::Dispatch]).c_str());
                    gClVt = vt;
                    void* o = nullptr;
                    if (Patch(vt, Vt::CommandList::DispatchRays, (void*)&NpDispatchRays, &o))
                        gOrigDR = (FnDispatchRays)o;
                    o = nullptr;
                    if (Patch(vt, Vt::CommandList::BuildRaytracingAccelerationStructure,
                              (void*)&NpBuildAS, &o))
                        gOrigBAS = (FnBuildAS)o;
                    o = nullptr;
                    if (Patch(vt, Vt::CommandList::Dispatch, (void*)&NpDispatch, &o))
                        gOrigDispatch = (FnDispatch)o;
                    o = nullptr;
                    if (Patch(vt, Vt::CommandList::DrawInstanced, (void*)&NpDrawInst, &o))
                        gOrigDrawInst = (FnDrawInst)o;
                    o = nullptr;
                    if (Patch(vt, Vt::CommandList::DrawIndexedInstanced, (void*)&NpDrawIdx, &o))
                        gOrigDrawIdx = (FnDrawIdx)o;
                    // 视口能反映游戏真正在渲染多大（DLSS/FSR 超分时 < 输出分辨率），
                    // 用来填「渲染 / 输出分辨率与缩放比」。
                    o = nullptr;
                    if (Patch(vt, Vt::CommandList::RSSetViewports, (void*)&NpSetViewports, &o) && o)
                        gOrigVP = (FnSetViewports)o;
                    gClHooked = gOrigDR != nullptr || gOrigDispatch != nullptr;
                    Log("EnsureD3D12Hooks: command-list hooks installed (rays=%d bas=%d disp=%d "
                        "draw=%d vp=%d)", gOrigDR ? 1 : 0, gOrigBAS ? 1 : 0, gOrigDispatch ? 1 : 0,
                        gOrigDrawInst ? 1 : 0, gOrigVP ? 1 : 0);
                }
                cl4->Release();
            }
            list->Release();
        }
        if (alloc) alloc->Release();
    }
    return gOrigECL != nullptr;
}


// ---------------------------------------------------------------------------
// 跳板函数实现（extern "C"，供 vtable 直接调用）
// ---------------------------------------------------------------------------
extern "C" {

HRESULT __stdcall NpPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    return PresentCommon(sc, sync, flags, nullptr, false);
}

HRESULT __stdcall NpPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags,
                             const DXGI_PRESENT_PARAMETERS* pp) {
    return PresentCommon(sc, sync, flags, pp, true);
}

// 工厂钩子：游戏创建交换链后补 Present 补丁（异常一律放行，绝不影响创建）
//
// ★ 三个工厂钩子里都必须先判 gUnloading：自卸载进行时 vtable 正在被还原，
//   如果这期间游戏正好又建了一条交换链，我们就会往它（进程共享的）vtable 里
//   重新写进 NpPresent —— 而本 DLL 马上就要 FreeLibrary 了，那个槽位就成了
//   指向已卸载内存的野指针，宿主下一次 Present 必定崩。
HRESULT __stdcall NpCreateSwapChain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d,
                                    IDXGISwapChain** sc) {
    Log("factory: CreateSwapChain called");
    HRESULT hr = gOrigCreateSC ? gOrigCreateSC(f, dev, d, sc) : E_FAIL;
    if (SUCCEEDED(hr) && sc && *sc && !gUnloading) PatchSwapChainVtable(*sc);
    return hr;
}

HRESULT __stdcall NpCreateSwapChainHwnd(IDXGIFactory2* f, IUnknown* dev, HWND hw,
                                        const DXGI_SWAP_CHAIN_DESC1* d1,
                                        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fd, IDXGIOutput* out,
                                        IDXGISwapChain1** sc) {
    Log("factory: CreateSwapChainForHwnd called");
    HRESULT hr =
        gOrigCreateSCHwnd ? gOrigCreateSCHwnd(f, dev, hw, d1, fd, out, sc) : E_FAIL;
    if (SUCCEEDED(hr) && sc && *sc && !gUnloading) PatchSwapChainVtable(*sc);
    return hr;
}

HRESULT __stdcall NpCreateSwapChainComp(IDXGIFactory2* f, IUnknown* dev,
                                        const DXGI_SWAP_CHAIN_DESC1* d1, IDXGIOutput* out,
                                        IDXGISwapChain1** sc) {
    Log("factory: CreateSwapChainForComposition called");
    HRESULT hr = gOrigCreateSCComp ? gOrigCreateSCComp(f, dev, d1, out, sc) : E_FAIL;
    if (SUCCEEDED(hr) && sc && *sc && !gUnloading) PatchSwapChainVtable(*sc);
    return hr;
}

void __stdcall NpECL(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    if (gUnloading || !q) { if (gOrigECL) gOrigECL(q, n, lists); return; }

    // 只认 3D 直连队列。复制 / 计算队列拿去做叠加和 GPU 时间戳都是错的，
    // 而且游戏常常先提交复制队列，先到先得会把 gQueue12 记错。
    if (!gQueue12) {
        D3D12_COMMAND_QUEUE_DESC qd{};
        q->GetDesc(&qd);
        if (qd.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            // ★ 必须 AddRef。引擎在切换全屏/重建交换链/设备丢失恢复时会
            // **销毁并重建命令队列**（有时连设备一起换）。我们只存了个裸指针，
            // 之后往一条已销毁的队列上 ExecuteCommandLists / Signal：
            // 轻则围栏永远不推进（BeginList 从此一直「allocator busy」，
            // 叠加永久停画），重则在驱动里访问违例把游戏带崩。
            q->AddRef();
            gQueue12 = q;
            Log("ECL: latched DIRECT queue %p (addref)", (void*)q);
        } else {
            Log("ECL: non-direct queue (type=%d), passing through", (int)qd.Type);
        }
    }

    // ★ 关键修复：**只有我们选定的那条 DIRECT 队列**才插时间戳。
    //
    // 原来这里只判 `q` 非空，于是游戏往复制队列提交时，我们会把**自己的
    // DIRECT 命令列表**丢给复制队列去执行 —— 这是非法调用，直接把设备搞成
    // removed，游戏弹 `DXGI_ERROR_INVALID_CALL` 退出。
    // 自建测试宿主只有一条队列所以测不出来；真实游戏会大量用复制队列，
    // 症状就是「数据出来一秒后闪退」。
    if (q != gQueue12 || !gHeap || gD12Broken.load(std::memory_order_acquire)) {
        if (gOrigECL) gOrigECL(q, n, lists);
        return;
    }

    // ⚠ EndList() 内部会再调一次 q->ExecuteCommandLists，也就是再进一次本函数。
    // 所以「本帧已开始」标记必须在 EndList **之前**抢到手，再加一道本线程重入锁。
    // 用 compare_exchange 而不是「先读后写」：多线程提交时后者会让两个线程
    // 同时通过检查，同时去 Reset 同一条命令列表。
    thread_local bool busy = false;
    bool expected = false;
    bool firstThisFrame = !busy && gFrameStarted.compare_exchange_strong(expected, true);
    if (firstThisFrame) {
        gCpuStartQpc = NowQpc();
        gBatchCount.store(0, std::memory_order_relaxed);
    }
    // 模拟阶段结束 = **渲染线程**本帧第一次提交命令。
    // 只认渲染线程：后台流式线程随时都在提交，用它们的时刻会把
    // 「模拟」和「提交」两段彻底搅在一起（用户反馈过这个现象）。
    // ⚠ 还必须排除 gInPresent：我们自己的叠加和 GPU 时间戳命令列表
    //   也是从渲染线程提交的，不排除的话它们会被当成「本帧第一次提交」，
    //   算出来的模拟阶段就变成整个帧周期了。
    if (!busy && !gInPresent && gRenderTid == GetCurrentThreadId() && !gSimEndQpc)
        gSimEndQpc = NowQpc();

    // ★ 这里**故意不做**「逐批 GPU 时间戳夹取」了。
    //
    // 曾经为了测每帧真实 GPU 忙时间，给每一次 ExecuteCommandLists 前后各插一条
    // 时间戳（2 次 BeginList）。真实游戏一帧提交十几到几十批（实测
    // "more than 16 batches per frame"），一帧就要 32+ 个分配器 ——
    // 32 个的环瞬间被掏空，然后：
    //     BeginList: allocator starved   ← 刷屏
    //     overlay NOT drawn              ← 叠加永久停画
    // 这是「玩一会儿面板就不刷新」的真凶，代价远大于收益。
    //
    // 而且现在 **GPU 帧时间已经有更好的来源**：PDH 按游戏进程读
    // `\GPU Engine(pid_*)\Running Time`，那是驱动报的数，与锁帧/Reflex/
    // 多线程提交都无关，还不需要我们往游戏的队列里塞任何东西。
    // 所以这套夹取拆掉 —— 它的唯一用途已经被系统数据取代了。
    if (gOrigECL) gOrigECL(q, n, lists);
}

// 这条命令列表能不能插时间戳。
// `ID3D12GraphicsCommandList` 的 vtable 是**打包（bundle）和直连列表共用**的，
// 而 bundle 上 `EndQuery` 是非法操作 —— 真实游戏会用 bundle，
// 一插就把设备搞成 removed。复制列表同理不接受时间戳。
inline bool CanTimestamp(ID3D12GraphicsCommandList* cl) {
    if (!cl || !gHeap || gD12Broken.load(std::memory_order_acquire)) return false;
    return cl->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT;
}

void __stdcall NpDispatchRays(ID3D12GraphicsCommandList4* cl, const D3D12_DISPATCH_RAYS_DESC* d) {
    // ★ 这里原来有一个「光追被提交两次」的严重 bug：
    //   `if (CanTimestamp(...) && SehReady() && setjmp(gJmp) == 0) { ... EndQuery();
    //      gOrigDR(cl, d); EndQuery(); ... return; }`
    //   —— setjmp 返回非 0（即两次 EndQuery 之间出异常被 VEH longjmp 回来）时，
    //   条件整体为假，控制流掉到下面的兜底 `gOrigDR(cl, d)`，于是**同一次
    //   DispatchRays 被提交了两遍**，而且第一遍的 EndQuery 只插了一半
    //   （时间戳对不齐）。光追被算两遍 = 画面错乱、GPU 侧非法查询 =
    //   设备 removed、游戏退出。
    //   现在改成：只要探针期间出过异常，就认定我们的时间戳插桩已经不可信，
    //   永久关掉 D3D12 侧的插桩（gD12Broken，与 BeginList 的失败策略一致：
    //   宁可丢数据也绝不动游戏的命令流），并且**只**原样转发一次。
    bool faulted = false;
    if (SehReady()) {
        if (setjmp(gJmp) == 0) {
            gJmpArmed = true;
            // ⚠ 槽位分配必须用 fetch_add 的**返回值**，不能「先 load 判断、再
            //   fetch_add」。原来那样写，两个提交线程可以读到同一个 n，
            //   于是同时往同一个时间戳槽里写 —— RT 占用算出垃圾值，计数也会丢。
            //   改成先原子占号再用占到的号；号超出范围就不插桩（harvest 那边
            //   本来就用 `k < TS_RT_PAIRS` 夹住，不会读到没写的槽）。
            if (CanTimestamp(cl)) {
                uint32_t n = gRtDispatches.fetch_add(1, std::memory_order_relaxed);
                if (n < TS_RT_PAIRS) {
                    uint32_t slot = TS_RT_BASE + n * 2;
                    cl->EndQuery(gHeap, D3D12_QUERY_TYPE_TIMESTAMP, slot);
                    if (gOrigDR) gOrigDR(cl, d);
                    cl->EndQuery(gHeap, D3D12_QUERY_TYPE_TIMESTAMP, slot + 1);
                    gJmpArmed = false;
                    return;
                }
            }
            // 不在插桩范围（或列表类型不允许）→ 落到下面统一原样转发
            gJmpArmed = false;
            gJmpArmed = false;
        } else {
            faulted = true;
        }
    }
    if (faulted) {
        // 命令列表里已经留下了半对时间戳，之后 ResolveQueryData 读它同样危险。
        gD12Broken.store(true, std::memory_order_release);
        Log("NpDispatchRays: SEH during timestamp insert -> disable D3D12 instrumentation");
        gRtDispatches.store(TS_RT_PAIRS, std::memory_order_relaxed);   // 不再插桩
    }
    if (gOrigDR) gOrigDR(cl, d);
}

void __stdcall NpBuildAS(ID3D12GraphicsCommandList4* cl,
                         const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* d, UINT n,
                         const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* p) {
    gAsBuilds.fetch_add(1, std::memory_order_relaxed);
    if (gOrigBAS) gOrigBAS(cl, d, n, p);
}

void __stdcall NpDispatch(ID3D12GraphicsCommandList* cl, UINT x, UINT y, UINT z) {
    // AI 超分 / 帧生成通常发生在「所有 draw 之后」的 compute 阶段，
    // 用一对时间戳把这段区间夹住，作为 Tensor / AI 单元的负载来源。
    //
    // ★ 与 NpDispatchRays 同类的坑：原来这里的 `ts` 是 setjmp 的返回值，
    //   一旦探针中出异常，`ts` 变 false，后面的原函数调用会**再执行一遍**
    //   本就已经转发过的 Dispatch（下面的 gOrigDispatch 在 try 段之后就调过一次）。
    //   现在把「探针是否出异常」单独记下来，并且只转发一次。
    bool faulted = false;
    if (SehReady()) {
        if (setjmp(gJmp) == 0) {
            gJmpArmed = true;
            if (CanTimestamp(cl) && gDraws.load(std::memory_order_relaxed) > 0 && !gAiSpanOpen) {
                cl->EndQuery(gHeap, D3D12_QUERY_TYPE_TIMESTAMP, TS_AI_START);
                gAiSpanOpen = true;
            }
            gJmpArmed = false;
        } else {
            faulted = true;
        }
    }
    gDispatches.fetch_add(1, std::memory_order_relaxed);
    if (gOrigDispatch) gOrigDispatch(cl, x, y, z);

    if (faulted) {
        // 探针里出过异常，命令列表上可能已经留了半对时间戳。这里绝不能再去补写
        // 另一半（同一张坏列表上继续插桩只会更糟），也绝不能把这次 Dispatch 再
        // 转发一遍。直接按「插桩不可信」处理：永久停手，并把 AI 区间状态复位，
        // 否则 HarvestTimestamps 会把没写过的 TS_AI_END 当成有效结束点，
        // 报出一个荒唐的 Tensor 占用。
        gAiSpanOpen = false;
        gD12Broken.store(true, std::memory_order_release);
        Log("NpDispatch: SEH during AI timestamp -> disable D3D12 instrumentation");
        return;
    }

    // AI 区间结束时间戳
    if (gAiSpanOpen && cl) {
        if (SehReady() && setjmp(gJmp) == 0) {
            gJmpArmed = true;
            cl->EndQuery(gHeap, D3D12_QUERY_TYPE_TIMESTAMP, TS_AI_END);
            gAiSpanOpen = false;
            gJmpArmed = false;
        } else {
            gAiSpanOpen = false;   // 状态对齐，别让下一帧继续用这个坏区间
            gD12Broken.store(true, std::memory_order_release);
            Log("NpDispatch: SEH during AI timestamp -> disable D3D12 instrumentation");
        }
    }
}

void __stdcall NpDrawInst(ID3D12GraphicsCommandList* cl, UINT a, UINT b, UINT c, UINT d) {
    if (gNpCountingOverlayDraws) { if (gOrigDrawInst) gOrigDrawInst(cl, a, b, c, d); return; }
    gDraws.fetch_add(1, std::memory_order_relaxed);
    if (gOrigDrawInst) gOrigDrawInst(cl, a, b, c, d);
}

void __stdcall NpDrawIdx(ID3D12GraphicsCommandList* cl, UINT a, UINT b, UINT c, INT d, UINT e) {
    if (gNpCountingOverlayDraws) { if (gOrigDrawIdx) gOrigDrawIdx(cl, a, b, c, d, e); return; }
    gDraws.fetch_add(1, std::memory_order_relaxed);
    if (gOrigDrawIdx) gOrigDrawIdx(cl, a, b, c, d, e);
}

void __stdcall NpSetViewports(ID3D12GraphicsCommandList* cl, UINT n, const D3D12_VIEWPORT* v) {
    // 「渲染分辨率」只在视口**长宽比和输出一致**时才认。
    // 原来取「见过的最大的视口」，结果被 shadow atlas / 后处理用的方目标骗到：
    // 用户 16:10 的屏幕上显示成 3072×3072 → 3840×2400（80%），完全是错的。
    if (v && n && v[0].Width > 1 && v[0].Height > 1) {
        uint32_t w = (uint32_t)(v[0].Width + 0.5f);
        uint32_t h = (uint32_t)(v[0].Height + 0.5f);
        if (gTel && gTel->windowW && gTel->windowH) {
            float want = (float)gTel->windowW / (float)gTel->windowH;
            float got = (float)w / (float)h;
            float rel = (got > want) ? (got / want) : (want / got);
            if (rel <= 1.05f && w * h > gVpW * gVpH) { gVpW = w; gVpH = h; }
        } else if (w * h > gVpW * gVpH) {
            gVpW = w; gVpH = h;
        }
    }
    if (gOrigVP) gOrigVP(cl, n, v);
}

}  // extern "C"

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------
static HANDLE gWorker = nullptr;
static HANDLE gInjectedMutex = nullptr;

// 主程序已退出：摘掉全部钩子、释放资源、从宿主进程卸载本 DLL。
// 顺序不能乱——先让跳板直通（gUnloading），再还原 vtable，
// 短暂等待在途调用排空后才允许 FreeLibrary。
static void SelfUnloadNow() {
    gUnloading = true;
    Sleep(150);

    // 还原交换链补丁（这张表里的每一条都要还，blt / flip 可能是不同的 vtable）
    for (int i = 0; i < gSwapVtCount; ++i) {
        SwapVtEntry& e = gSwapVts[i];
        if (!e.vt) continue;
        if (e.origPresent) Patch(e.vt, Vt::SwapChain::Present, (void*)e.origPresent, nullptr);
        if (e.origPresent1) Patch(e.vt, Vt::SwapChain::Present1, (void*)e.origPresent1, nullptr);
    }
    gSwapVtCount = 0;
    if (gFactoryVt && gOrigCreateSC)
        Patch(gFactoryVt, Vt::Factory::CreateSwapChain, (void*)gOrigCreateSC, nullptr);
    if (gFactory2Vt) {
        if (gOrigCreateSCHwnd)
            Patch(gFactory2Vt, Vt::Factory2::CreateSwapChainForHwnd, (void*)gOrigCreateSCHwnd, nullptr);
        if (gOrigCreateSCComp)
            Patch(gFactory2Vt, Vt::Factory2::CreateSwapChainForComposition,
                  (void*)gOrigCreateSCComp, nullptr);
    }
    if (gQueueVt && gOrigECL)
        Patch(gQueueVt, Vt::CommandQueue::ExecuteCommandLists, (void*)gOrigECL, nullptr);
    if (gClVt) {
        if (gOrigDR)       Patch(gClVt, Vt::CommandList::DispatchRays, (void*)gOrigDR, nullptr);
        if (gOrigBAS)
            Patch(gClVt, Vt::CommandList::BuildRaytracingAccelerationStructure, (void*)gOrigBAS,
                 nullptr);
        if (gOrigDispatch) Patch(gClVt, Vt::CommandList::Dispatch, (void*)gOrigDispatch, nullptr);
        if (gOrigDrawInst) Patch(gClVt, Vt::CommandList::DrawInstanced, (void*)gOrigDrawInst, nullptr);
        if (gOrigDrawIdx)
            Patch(gClVt, Vt::CommandList::DrawIndexedInstanced, (void*)gOrigDrawIdx, nullptr);
        if (gOrigVP)       Patch(gClVt, Vt::CommandList::RSSetViewports, (void*)gOrigVP, nullptr);
    }
    Sleep(250);   // 在途调用排空窗口

    // 图形与采集资源
    gOv11.Release();
    gOv12.Release();
    gBmp.Release();
    gPanel.Shutdown();
    npg::GfxShutdown();
    npb::GfxShutdown();
    for (auto& q : gTs11) {
        if (q.disjoint) { q.disjoint->Release(); q.disjoint = nullptr; }
        if (q.a) { q.a->Release(); q.a = nullptr; }
        if (q.b) { q.b->Release(); q.b = nullptr; }
    }
    for (auto& rb : gReadback) if (rb) { rb->Release(); rb = nullptr; }
    if (gList) { gList->Release(); gList = nullptr; }
    for (auto& a : gAlloc) if (a) { a->Release(); a = nullptr; }
    if (gHeap) { gHeap->Release(); gHeap = nullptr; }
    if (gFence) { gFence->Release(); gFence = nullptr; }
    if (gCtx11) { gCtx11->Release(); gCtx11 = nullptr; }
    if (gDev11) { gDev11->Release(); gDev11 = nullptr; }
    if (gQueue12) { gQueue12->Release(); gQueue12 = nullptr; }
    if (gDev12) { gDev12->Release(); gDev12 = nullptr; }

    // 互斥量、共享内存
    if (gInjectedMutex) { CloseHandle(gInjectedMutex); gInjectedMutex = nullptr; }
    if (gTel) { UnmapViewOfFile(gTel); gTel = nullptr; }
    if (gSens) { UnmapViewOfFile(gSens); gSens = nullptr; }
    if (gCfg) { UnmapViewOfFile(gCfg); gCfg = nullptr; }
    if (gMapTel) { CloseHandle(gMapTel); gMapTel = nullptr; }
    if (gMapSens) { CloseHandle(gMapSens); gMapSens = nullptr; }
    if (gMapCfg) { CloseHandle(gMapCfg); gMapCfg = nullptr; }

    FreeLibraryAndExitThread(gSelf, 0);
}

// 从进程里已加载的模块猜图形 API。
// Present 挂不上时（Vulkan / OpenGL 游戏）也要能告诉主程序「为什么没数据」，
// 否则界面上只能显示「已注入但没数据」，用户完全不知道为什么。
int GuessApiFromModules() {
    if (GetModuleHandleW(L"vulkan-1.dll")) return NP_API_VULKAN;
    if (GetModuleHandleW(L"opengl32.dll")) return NP_API_OPENGL;
    if (GetModuleHandleW(L"d3d12.dll")) return NP_API_D3D12;
    if (GetModuleHandleW(L"d3d11.dll")) return NP_API_D3D11;
    if (GetModuleHandleW(L"d3d9.dll")) return NP_API_D3D9;
    return NP_API_UNKNOWN;
}

// 钩子还活着、但 Present 没挂上时，周期性写一份「最小遥测」。
// 主程序靠它把「已注入但没数据」变成一句能看懂的话。
void PublishMinimalTelemetry() {
    if (!gTel) return;
    gTel->tickMs = GetTickCount64();
    if (gApi == NP_API_UNKNOWN)
        gApi = (gHostApi != NP_API_UNKNOWN) ? gHostApi : GuessApiFromModules();
    gTel->gfxApi = (uint32_t)gApi;
    if (!gTel->processName[0]) {
        wchar_t selfPath[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, selfPath, MAX_PATH)) {
            char buf[NP_NAME_LEN]{};
            WideCharToMultiByte(CP_UTF8, 0, selfPath, -1, buf, NP_NAME_LEN - 1, nullptr, nullptr);
            const char* slash = strrchr(buf, '\\');
            NPCopyStr(gTel->processName, NP_NAME_LEN, slash ? slash + 1 : buf);
        }
    }
    switch (gApi) {
        case NP_API_VULKAN:
            NPCopyStr(gTel->lastError, NP_NAME_LEN,
                      "Vulkan 渲染：钩不进它的呈现链（已跳过 D3D 探测，不会影响游戏）");
            break;
        case NP_API_OPENGL:
            NPCopyStr(gTel->lastError, NP_NAME_LEN,
                      "OpenGL 渲染：钩不进它的呈现链（已跳过 D3D 探测，不会影响游戏）");
            break;
        case NP_API_D3D12:
        case NP_API_D3D11:
            if (gD12Broken.load(std::memory_order_acquire))
                NPCopyStr(gTel->lastError, NP_NAME_LEN,
                          "D3D12 侧检测到非法调用，已自动停手保护游戏（叠加与 RT/Tensor 不可用）");
            else
                NPCopyStr(gTel->lastError, NP_NAME_LEN,
                          "D3D 已加载但 Present 未挂上（看 NextPerfHook.log）");
            break;
        default:
            NPCopyStr(gTel->lastError, NP_NAME_LEN, "没检测到图形 API：游戏可能还没开始渲染");
            break;
    }
}

static DWORD WINAPI Worker(LPVOID) {
    LARGE_INTEGER li{};
    QueryPerformanceFrequency(&li);
    gQpcFreq = (uint64_t)li.QuadPart;

    Log("attach: pid=%lu", (unsigned long)GetCurrentProcessId());
    if (!OpenIpc()) { Log("attach: OpenIpc failed"); return 0; }

    DetectAi();
    InstallDxgi();

    // 标记「已注入」，主程序靠这个互斥量判断，避免重复注入
    {
        wchar_t name[64];
        swprintf(name, 64, L"Local\\NextPerf_Injected_%lu", (unsigned long)GetCurrentProcessId());
        HANDLE m = CreateMutexW(nullptr, TRUE, name);
        if (m) gInjectedMutex = m;   // 进程存活期间保持持有
    }

    // 注入时游戏通常已经在渲染了，工厂钩子不会再被调用 —— 需要自己去拿 vtable。
    // 宿主已经加载过图形模块说明它早就建好交换链了，等一小会儿就探测；
    // 什么都没加载说明游戏还在初始化，先让工厂钩子干（更安全），超时再兜底。
    //
    // ⚠ 探测有副作用：它会在这个进程里**创建 D3D11 设备 + 交换链**。
    //   所以 1) 宿主是 Vulkan/OpenGL 时绝不能探（把一个 D3D 设备塞进 Vulkan 进程
    //   足以把游戏带崩）；2) 失败也不能无限重试 —— 原来每 2 秒一次、永不停止，
    //   一分钟创建 30 个设备，某款 Vulkan 游戏就是这么闪退的（无任何报错）。
    bool gfxUp = HostGraphicsUp();
    gHostApi = HostApiGuess();
    uint64_t probeAtMs = GetTickCount64() + (gfxUp ? 800u : 6000u);
    int probeTries = 0;
    const int kMaxProbeTries = 3;
    Log("attach: hostGraphicsUp=%d hostApi=%d probe at +%ums", gfxUp ? 1 : 0, gHostApi,
        gfxUp ? 800u : 6000u);

    if (gTel) {
        gTel->hookFlags = 0;
        if (PresentHooked()) gTel->hookFlags |= NP_HOOK_PRESENT;
        if (gOrigECL) gTel->hookFlags |= NP_HOOK_QUEUE;
        if (gClHooked) gTel->hookFlags |= NP_HOOK_CMDLIST;
    }
    gReady = true;

    while (gReady) {
        Sleep(200);
        // 主程序退出：完成自卸载（不再返回）
        if (gCfg && gCfg->magic == NP_MAGIC && gCfg->quit) SelfUnloadNow();
        DetectAi();

        if (!PresentHooked()) {
            // 每轮重新判断宿主 API：Vulkan 可能比我们晚加载
            int a = HostApiGuess();
            if (a != NP_API_UNKNOWN) gHostApi = a;
            bool hostIsD3D = (gHostApi != NP_API_VULKAN && gHostApi != NP_API_OPENGL);
            // 宿主同时挂着 vulkan-1.dll 又只有 D3D11（没有 D3D12）时，
            // 有可能其实是 Vulkan 游戏，探测次数收紧到 1 次 ——
            // 在一个 Vulkan 进程里创建 D3D 设备是有代价的（它就是这样被反复
            // 创建设备搞崩的）。D3D12 是强信号，照常给 3 次。
            int maxTries = hostIsD3D ? ((gHostApi == NP_API_D3D12 || !HostHasVulkan())
                                            ? kMaxProbeTries
                                            : 1)
                                     : 0;

            if (!hostIsD3D) {
                if (probeTries >= 0) {
                    probeTries = -1;   // 只提示一次
                    Log("probe disabled: host api=%d (non-D3D), refusing to create "
                        "a D3D device here", gHostApi);
                }
            } else if (Cfg().vtableProbe && probeTries < maxTries &&
                       GetTickCount64() >= probeAtMs && !gD12Broken.load(std::memory_order_acquire)) {
                ++probeTries;
                Log("probe attempt %d/%d (hostApi=%d)", probeTries, maxTries, gHostApi);
                ProbeSwapChainVtable();
                probeAtMs = GetTickCount64() + 2000;
            }
        }

        // 挂不上 Present 也要让主程序知道我们活着、以及卡在哪（Vulkan 之类）
        if (!PresentHooked()) PublishMinimalTelemetry();

        if (gTel) {
            gTel->aiModules = gAiModules;
            if (PresentHooked()) gTel->hookFlags |= NP_HOOK_PRESENT;
            if (gOrigECL) gTel->hookFlags |= NP_HOOK_QUEUE;
            if (gClHooked) gTel->hookFlags |= NP_HOOK_CMDLIST;
            if (gHeap) gTel->hookFlags |= NP_HOOK_TIMESTAMP;
        }
    }
    return 0;
}

void NpHookAttach(HMODULE self) {
    gSelf = self;
    InitializeCriticalSection(&gCs);
    gWorker = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
}

void NpHookDetach() {
    gReady = false;
    if (gWorker) {
        // 自卸载路径下 detach 发生在 worker 线程自己身上，不能等自己
        if (GetThreadId(gWorker) != GetCurrentThreadId())
            WaitForSingleObject(gWorker, 500);
        CloseHandle(gWorker);
        gWorker = nullptr;
    }
    DeleteCriticalSection(&gCs);
}
