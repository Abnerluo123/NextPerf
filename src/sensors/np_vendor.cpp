#include "np_vendor.h"

#include <cstdio>
#include <cstring>
#include <algorithm>

// ==================================================================== NVML
bool NvmlApi::Load() {
    if (loaded) return true;
    const wchar_t* paths[] = {
        L"nvml.dll",
        L"C:\\Windows\\System32\\nvml.dll",
        L"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll",
    };
    for (auto p : paths) {
        dll = LoadLibraryW(p);
        if (dll) break;
    }
    if (!dll) return false;

#define NP_GET(name, var) var = reinterpret_cast<decltype(var)>(GetProcAddress(dll, name))
    NP_GET("nvmlInit_v2", Init);
    NP_GET("nvmlShutdown", Shutdown);
    NP_GET("nvmlDeviceGetCount_v2", DeviceGetCount);
    NP_GET("nvmlDeviceGetHandleByIndex_v2", DeviceGetHandleByIndex);
    NP_GET("nvmlDeviceGetName", DeviceGetName);
    NP_GET("nvmlDeviceGetUtilizationRates", DeviceGetUtilizationRates);
    NP_GET("nvmlDeviceGetMemoryInfo", DeviceGetMemoryInfo);
    NP_GET("nvmlDeviceGetTemperature", DeviceGetTemperature);
    NP_GET("nvmlDeviceGetPowerUsage", DeviceGetPowerUsage);
    NP_GET("nvmlDeviceGetEnforcedPowerLimit", DeviceGetEnforcedPowerLimit);
    NP_GET("nvmlDeviceGetClockInfo", DeviceGetClockInfo);
    NP_GET("nvmlDeviceGetFanSpeed", DeviceGetFanSpeed);
    NP_GET("nvmlDeviceGetPerformanceState", DeviceGetPerformanceState);
    NP_GET("nvmlDeviceGetTemperatureThreshold", DeviceGetTemperatureThreshold);
    NP_GET("nvmlDeviceGetEncoderUtilization", DeviceGetEncoderUtilization);
    NP_GET("nvmlDeviceGetDecoderUtilization", DeviceGetDecoderUtilization);
#undef NP_GET

    if (!Init || !DeviceGetHandleByIndex || !DeviceGetUtilizationRates) {
        FreeLibrary(dll); dll = nullptr; return false;
    }
    if (Init() != 0) {
        FreeLibrary(dll); dll = nullptr; return false;
    }
    loaded = true;
    return true;
}

void NvmlApi::Unload() {
    if (!loaded) return;
    if (Shutdown) Shutdown();
    FreeLibrary(dll);
    dll = nullptr;
    loaded = false;
}

// ==================================================================== NVAPI
// 这些 interfaceId 是 nvapi.h 里公开的，长期稳定。
static const NvU32 kIdInitialize           = 0x0150E828u;
static const NvU32 kIdUnload               = 0xD22BDD7Eu;
static const NvU32 kIdEnumPhysicalGPUs     = 0xE5AC921Fu;
static const NvU32 kIdGpuGetFullName       = 0xCEEE8E9Fu;
static const NvU32 kIdGpuThermalSettings   = 0xE3640A56u;
static const NvU32 kIdGpuDynamicPstates    = 0x60DED2EDu;
static const NvU32 kIdGpuGetUsages         = 0x189A1FDFu;
static const NvU32 kIdGpuGetTachReading    = 0x5F608315u;

bool NvapiApi::Load() {
    if (loaded) return true;
    dll = LoadLibraryW(L"nvapi64.dll");
    if (!dll) dll = LoadLibraryW(L"nvapi.dll");
    if (!dll) return false;

    QueryInterface = reinterpret_cast<NvAPI_QueryInterface_t>(GetProcAddress(dll, "nvapi_QueryInterface"));
    if (!QueryInterface) { FreeLibrary(dll); dll = nullptr; return false; }

    void* fn = nullptr;
    if (QueryInterface(kIdInitialize, &fn) == 0 && fn) Initialize = reinterpret_cast<decltype(Initialize)>(fn);
    fn = nullptr;
    if (QueryInterface(kIdUnload, &fn) == 0 && fn) NvAPI_UnloadFn = reinterpret_cast<decltype(NvAPI_UnloadFn)>(fn);
    fn = nullptr;
    if (QueryInterface(kIdEnumPhysicalGPUs, &fn) == 0 && fn) EnumPhysicalGPUs = reinterpret_cast<decltype(EnumPhysicalGPUs)>(fn);
    fn = nullptr;
    if (QueryInterface(kIdGpuGetFullName, &fn) == 0 && fn) GPU_GetFullName = reinterpret_cast<decltype(GPU_GetFullName)>(fn);
    fn = nullptr;
    if (QueryInterface(kIdGpuThermalSettings, &fn) == 0 && fn) GPU_GetThermalSettings = reinterpret_cast<decltype(GPU_GetThermalSettings)>(fn);
    fn = nullptr;
    if (QueryInterface(kIdGpuDynamicPstates, &fn) == 0 && fn) GPU_GetDynamicPstatesInfoEx = reinterpret_cast<decltype(GPU_GetDynamicPstatesInfoEx)>(fn);
    fn = nullptr;
    if (QueryInterface(kIdGpuGetUsages, &fn) == 0 && fn) GPU_GetUsages = reinterpret_cast<decltype(GPU_GetUsages)>(fn);
    fn = nullptr;
    if (QueryInterface(kIdGpuGetTachReading, &fn) == 0 && fn) GPU_GetTachReading = reinterpret_cast<decltype(GPU_GetTachReading)>(fn);

    if (!Initialize || !EnumPhysicalGPUs) { FreeLibrary(dll); dll = nullptr; return false; }
    if (Initialize() != 0) { FreeLibrary(dll); dll = nullptr; return false; }
    inited = true;

    if (EnumPhysicalGPUs(gpus, &gpuCount) != 0) gpuCount = 0;
    loaded = true;
    return true;
}

void NvapiApi::Unload() {
    if (!loaded) return;
    if (NvAPI_UnloadFn) NvAPI_UnloadFn();
    FreeLibrary(dll);
    dll = nullptr;
    loaded = false;
    inited = false;
}

// ==================================================================== AMD ADL
static void* __stdcall NpAdlMalloc(int size) { return malloc((size_t)size); }

bool AdlApi::Load() {
    if (loaded) return true;
    dll = LoadLibraryW(L"atiadlxx.dll");
    if (!dll) dll = LoadLibraryW(L"atiadlxy.dll");
    if (!dll) return false;

#define NP_GET(name, var) var = reinterpret_cast<decltype(var)>(GetProcAddress(dll, name))
    NP_GET("ADL_Main_Control_Create", Main_Control_Create);
    NP_GET("ADL_Main_Control_Destroy", Main_Control_Destroy);
    NP_GET("ADL_Adapter_NumberOfAdapters_Get", Adapter_NumberOfAdapters_Get);
    NP_GET("ADL_Adapter_AdapterInfo_Get", Adapter_AdapterInfo_Get);
    NP_GET("ADL_Overdrive5_CurrentActivity_Get", Overdrive5_CurrentActivity_Get);
    NP_GET("ADL_Overdrive5_Temperature_Get", Overdrive5_Temperature_Get);
#undef NP_GET

    if (!Main_Control_Create || !Overdrive5_CurrentActivity_Get) {
        FreeLibrary(dll); dll = nullptr; return false;
    }
    if (Main_Control_Create(NpAdlMalloc, 1) != NP_ADL_OK) {
        FreeLibrary(dll); dll = nullptr; return false;
    }
    if (Adapter_NumberOfAdapters_Get) Adapter_NumberOfAdapters_Get(&adapterCount);
    if (adapterCount < 0) adapterCount = 0;
    loaded = true;
    return true;
}

void AdlApi::Unload() {
    if (!loaded) return;
    if (Main_Control_Destroy) Main_Control_Destroy();
    FreeLibrary(dll);
    dll = nullptr;
    loaded = false;
}

// ================================================================ HWiNFO 共享内存
// 布局（v2，HWiNFO 官方文档 SharedMemoryLayout）：
//   Header: u32 signature 'HWiS', u32 version, u32 revision, i64 poll_time,
//           u32 offsetSensor, u32 offsetReading, u32 sizeSensor, u32 sizeReading
//   Sensor element : u32 sensorId, u32 sensorInst, char nameOrig[128], char nameUser[128]
//   Reading element: u32 type, u32 sensorIndex, u32 readingId,
//                    char labelOrig[128], char labelUser[128], char unit[16],
//                    double value, valueMin, valueMax, valueAvg, ... (后面还有更多字段)
struct NpHwinfoHeader {
    uint32_t signature;
    uint32_t version;
    uint32_t revision;
    int64_t  pollTime;
    uint32_t offSensor;
    uint32_t offReading;
    uint32_t sizeSensor;
    uint32_t sizeReading;
};

bool HwinfoCtx::Open() {
    if (loaded) return true;
    for (const wchar_t* name : {L"Global\\HWiNFO_SENS_SM2", L"Global\\HWiNFO_SENS_SM"}) {
        map = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
        if (map) break;
    }
    if (!map) return false;
    view = MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);
    if (!view) { CloseHandle(map); map = nullptr; return false; }
    loaded = true;
    return Refresh();
}

void HwinfoCtx::Close() {
    if (view) UnmapViewOfFile(view);
    if (map) CloseHandle(map);
    view = nullptr; map = nullptr; loaded = false;
    readings.clear();
}

static std::wstring NpHwStr(const char* s, size_t maxLen) {
    // HWiNFO 用的是 ANSI（当前代码页）字符串
    size_t n = 0;
    while (n < maxLen && s[n]) ++n;
    if (!n) return L"";
    int wlen = MultiByteToWideChar(CP_ACP, 0, s, (int)n, nullptr, 0);
    if (wlen <= 0) return L"";
    std::wstring w((size_t)wlen, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s, (int)n, &w[0], wlen);
    return w;
}

// clang 在 mingw 目标上不支持 MSVC 的 __try/__except，
// 这里用「向量化异常处理 + setjmp/longjmp」自己实现一个等价的保护，
// 保证读 HWiNFO 共享内存时即使对方正在改结构也不会把主程序带崩。
#include <csetjmp>
namespace {
thread_local std::jmp_buf gNpJmp;
thread_local bool gNpJmpArmed = false;
LONG CALLBACK NpSehHandler(PEXCEPTION_POINTERS) {
    if (gNpJmpArmed) { gNpJmpArmed = false; std::longjmp(gNpJmp, 1); }
    return EXCEPTION_CONTINUE_SEARCH;
}
bool NpSehInstall() {
    static bool installed = false;
    if (installed) return true;
    installed = AddVectoredExceptionHandler(1, NpSehHandler) != nullptr;
    return installed;
}
}

bool HwinfoCtx::Refresh() {
    if (!loaded || !view) return false;
    if (!NpSehInstall()) return false;
    if (setjmp(gNpJmp) != 0) {
        gNpJmpArmed = false;
        return false;
    }
    gNpJmpArmed = true;
    bool result = RefreshUnsafe();
    gNpJmpArmed = false;
    return result;
}

bool HwinfoCtx::RefreshUnsafe() {
    const NpHwinfoHeader* h = reinterpret_cast<const NpHwinfoHeader*>(view);
        if (h->signature != 0x53495748u) return false;  // 'HWiS' little-endian
        if (h->version < 2) return false;
        if (h->sizeSensor < 264 || h->sizeReading < 292) return false;
        if (h->offSensor == 0 || h->offReading == 0) return false;

        // 读数数量由共享内存大小推算，最多取 4096 条防止越界
        MEMORY_BASIC_INFORMATION mbi{};
        SIZE_T total = 0;
        if (VirtualQuery(view, &mbi, sizeof(mbi))) total = mbi.RegionSize;
        if (total == 0) total = 1 << 20;
        // ★ offReading 是对方（HWiNFO 进程）写进来的值，不能信：
        //   它一旦大于映射大小，(total - offReading) 会无符号下溢成天文数字，
        //   下面的循环就会去读映射之外的地址（现在靠异常处理器兜住，但那是最后一道防线，
        //   不该拿来当正常路径用）。
        if (h->offReading >= total) return false;
        uint32_t maxRead = (uint32_t)((total - h->offReading) / h->sizeReading);
        if (maxRead > 4096) maxRead = 4096;
        // 传感器分组元素同理，也要按映射大小算上界：
        // 原来只判 `sensorIndex < 4096`，读数里给一个超大的 sensorIndex
        // 就会越界读到映射外面去。
        uint32_t maxSensor =
            (h->offSensor < total) ? (uint32_t)((total - h->offSensor) / h->sizeSensor) : 0;

        const char* base = reinterpret_cast<const char*>(view);
        readings.clear();
        readings.reserve(maxRead);

        for (uint32_t i = 0; i < maxRead; ++i) {
            const char* r = base + h->offReading + (SIZE_T)i * h->sizeReading;
            uint32_t type = *reinterpret_cast<const uint32_t*>(r + 0);
            uint32_t sensorIndex = *reinterpret_cast<const uint32_t*>(r + 4);
            if (type == 0xFFFFFFFFu) break;

            Reading rd;
            rd.label = NpHwStr(r + 12, 128);
            rd.unit = NpHwStr(r + 12 + 128 + 128, 16);
            rd.value = *reinterpret_cast<const double*>(r + 12 + 128 + 128 + 16);
            (void)sensorIndex;

            // 传感器分组名
            if (sensorIndex < maxSensor) {
                const char* s = base + h->offSensor + (SIZE_T)sensorIndex * h->sizeSensor;
                rd.sensor = NpHwStr(s + 8, 128);
            }
            if (rd.label.empty()) continue;
            readings.push_back(std::move(rd));
        }
        return !readings.empty();
}

static bool NpContainsI(const std::wstring& hay, const wchar_t* needle) {
    if (!needle || !*needle) return true;
    std::wstring h(hay.size(), L'\0');
    std::transform(hay.begin(), hay.end(), h.begin(), ::towlower);
    std::wstring n(needle);
    std::transform(n.begin(), n.end(), n.begin(), ::towlower);
    return h.find(n) != std::wstring::npos;
}

bool HwinfoCtx::Find(const wchar_t* sensorKw, const wchar_t* labelKw, double* out) const {
    for (auto& r : readings) {
        if (NpContainsI(r.sensor, sensorKw) && NpContainsI(r.label, labelKw)) {
            *out = r.value;
            return true;
        }
    }
    return false;
}

bool HwinfoCtx::FindAny(const wchar_t* labelKw, double* out) const {
    for (auto& r : readings) {
        if (NpContainsI(r.label, labelKw)) { *out = r.value; return true; }
    }
    return false;
}
