// NVIDIA / AMD 厂商接口的「最小可用声明」。
//
// 之所以自己声明而不是包含官方 SDK 头文件，是为了让这个项目零外部依赖、
// 开箱即编：所有库都用 LoadLibrary + GetProcAddress 动态加载，
// 机器上没有 NVIDIA / AMD 驱动也不会报错，只是相应数据源不可用。
//
// 关于 RT Core / Tensor Core 硬件占用率：
//   * NVIDIA 公开的 NVAPI 只提供 4 个利用率域（图形引擎 / 显存控制器 / 视频引擎 / PCIe），
//     并没有公开 RT / Tensor 域。NVML 同理（GeForce 上不暴露）。
//   * 因此本项目采用「两条腿走路」：
//       1) 持续探测 NVAPI utilization[4..7] 这些未公开的域
//          （NVAPI_MAX_GPU_UTILIZATIONS = 8，官方只用了前 4 个），
//          一旦厂商哪天填上了就直接用；
//       2) 在注入的游戏进程里用 DXR / compute pass 的 GPU 时间戳做实测推断
//          （见 src/hook/engine_probe.cpp）。
//   这比「假装有一个 API」要诚实得多。

#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

// ================================================================ NVML
typedef int nvmlReturn_t;                 // 0 = NVML_SUCCESS
typedef void* nvmlDevice_t;

typedef struct {
    unsigned int gpu;
    unsigned int memory;
} nvmlUtilization_t;

typedef struct {
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
} nvmlMemory_t;

enum nvmlTemperatureSensors_t { NVML_TEMPERATURE_GPU = 0 };
enum nvmlClockType_t {
    NVML_CLOCK_GRAPHICS = 0,
    NVML_CLOCK_SM       = 1,
    NVML_CLOCK_MEM      = 2,
    NVML_CLOCK_VIDEO    = 3
};
enum nvmlTemperatureThresholds_t {
    NVML_TEMPERATURE_THRESHOLD_SHUTDOWN = 0,
    NVML_TEMPERATURE_THRESHOLD_SLOWDOWN = 1,
    NVML_TEMPERATURE_THRESHOLD_MEM_MAX  = 2,
    NVML_TEMPERATURE_THRESHOLD_GPU_MAX  = 3
};

struct NvmlApi {
    bool loaded = false;
    HMODULE dll = nullptr;

    nvmlReturn_t (*Init)(void) = nullptr;
    nvmlReturn_t (*Shutdown)(void) = nullptr;
    nvmlReturn_t (*DeviceGetCount)(unsigned int*) = nullptr;
    nvmlReturn_t (*DeviceGetHandleByIndex)(unsigned int, nvmlDevice_t*) = nullptr;
    nvmlReturn_t (*DeviceGetName)(nvmlDevice_t, char*, unsigned int) = nullptr;
    nvmlReturn_t (*DeviceGetUtilizationRates)(nvmlDevice_t, nvmlUtilization_t*) = nullptr;
    nvmlReturn_t (*DeviceGetMemoryInfo)(nvmlDevice_t, nvmlMemory_t*) = nullptr;
    nvmlReturn_t (*DeviceGetTemperature)(nvmlDevice_t, int, unsigned int*) = nullptr;
    nvmlReturn_t (*DeviceGetPowerUsage)(nvmlDevice_t, unsigned int*) = nullptr;
    nvmlReturn_t (*DeviceGetEnforcedPowerLimit)(nvmlDevice_t, unsigned int*) = nullptr;
    nvmlReturn_t (*DeviceGetClockInfo)(nvmlDevice_t, int, unsigned int*) = nullptr;
    nvmlReturn_t (*DeviceGetFanSpeed)(nvmlDevice_t, unsigned int*) = nullptr;
    nvmlReturn_t (*DeviceGetPerformanceState)(nvmlDevice_t, unsigned int*) = nullptr;
    nvmlReturn_t (*DeviceGetTemperatureThreshold)(nvmlDevice_t, int, unsigned int*) = nullptr;
    nvmlReturn_t (*DeviceGetEncoderUtilization)(nvmlDevice_t, unsigned int*, unsigned int*) = nullptr;
    nvmlReturn_t (*DeviceGetDecoderUtilization)(nvmlDevice_t, unsigned int*, unsigned int*) = nullptr;

    bool Load();
    void Unload();
};

// ================================================================ NVAPI
typedef unsigned int NvU32;
typedef int NvS32;
typedef unsigned int NvAPI_Status;

struct NvPhysicalGpuHandleRec { NvU32 unused; };
typedef NvPhysicalGpuHandleRec* NvPhysicalGpuHandle;

#define NVAPI_MAX_PHYSICAL_GPUS              64
#define NVAPI_MAX_THERMAL_SENSORS_PER_GPU    3
#define NVAPI_MAX_GPU_UTILIZATIONS           8
#define NVAPI_MAX_GPU_USAGES_PER_GPU         34
#define NVAPI_SHORT_STRING_MAX               64

#define NP_MAKE_NVAPI_VERSION(typeName, ver) \
    (NvU32)((sizeof(typeName) & 0xffff) << 16 | ((ver) & 0xffff))

typedef struct {
    NvU32 version;
    NvS32 count;
    struct {
        NvU32 controller;
        NvS32 defaultMinTemp;
        NvS32 defaultMaxTemp;
        NvS32 currentTemp;
        NvU32 target;
    } sensor[NVAPI_MAX_THERMAL_SENSORS_PER_GPU];
} NV_GPU_THERMAL_SETTINGS_V2;
#define NV_GPU_THERMAL_SETTINGS_VER_2 NP_MAKE_NVAPI_VERSION(NV_GPU_THERMAL_SETTINGS_V2, 2)

typedef struct {
    NvU32 version;
    struct {
        NvU32 bIsPresent : 1;
        NvU32 percentage : 31;
    } utilization[NVAPI_MAX_GPU_UTILIZATIONS];
} NV_GPU_DYNAMIC_PSTATES_INFO_EX;
#define NV_GPU_DYNAMIC_PSTATES_INFO_EX_VER_1 NP_MAKE_NVAPI_VERSION(NV_GPU_DYNAMIC_PSTATES_INFO_EX, 1)

// NVAPI_GPU_GetUsages：usages[] 里下标 2 历史上是 3D 引擎占用（单位 1/100 %？实测为百分比）
typedef struct {
    NvU32 version;
    NvU32 usages[NVAPI_MAX_GPU_USAGES_PER_GPU];
} NV_GPU_USAGES;
#define NV_GPU_USAGES_VER_1 NP_MAKE_NVAPI_VERSION(NV_GPU_USAGES, 1)

typedef NvAPI_Status (*NvAPI_QueryInterface_t)(NvU32 interfaceId, void** pFunction);

struct NvapiApi {
    bool loaded = false;
    bool inited = false;
    HMODULE dll = nullptr;
    NvAPI_QueryInterface_t QueryInterface = nullptr;

    NvAPI_Status (*Initialize)(void) = nullptr;
    NvAPI_Status (*NvAPI_UnloadFn)(void) = nullptr;
    NvAPI_Status (*EnumPhysicalGPUs)(NvPhysicalGpuHandle[NVAPI_MAX_PHYSICAL_GPUS], NvU32*) = nullptr;
    NvAPI_Status (*GPU_GetFullName)(NvPhysicalGpuHandle, char*) = nullptr;
    NvAPI_Status (*GPU_GetThermalSettings)(NvPhysicalGpuHandle, NvU32, NV_GPU_THERMAL_SETTINGS_V2*) = nullptr;
    NvAPI_Status (*GPU_GetDynamicPstatesInfoEx)(NvPhysicalGpuHandle, NV_GPU_DYNAMIC_PSTATES_INFO_EX*) = nullptr;
    NvAPI_Status (*GPU_GetUsages)(NvPhysicalGpuHandle, NV_GPU_USAGES*) = nullptr;
    NvAPI_Status (*GPU_GetTachReading)(NvPhysicalGpuHandle, NvU32*) = nullptr;

    NvPhysicalGpuHandle gpus[NVAPI_MAX_PHYSICAL_GPUS]{};
    NvU32 gpuCount = 0;

    bool Load();
    void Unload();
};

// 公开的利用率域下标
#define NP_NVAPI_DOM_GPU 0
#define NP_NVAPI_DOM_FB  1
#define NP_NVAPI_DOM_VID 2
#define NP_NVAPI_DOM_BUS 3

// ================================================================ AMD ADL
#define NP_ADL_MAX_PATH 256
#define NP_ADL_OK 0

typedef struct {
    int iSize;
    int iAdapterIndex;
    char strUDID[NP_ADL_MAX_PATH];
    int iBusNumber;
    int iDeviceNumber;
    int iFunctionNumber;
    int iVendorID;
    char strAdapterName[NP_ADL_MAX_PATH];
    char strDisplayName[NP_ADL_MAX_PATH];
    int iPresent;
    int iExist;
    char strDriverPath[NP_ADL_MAX_PATH];
    char strDriverPathExt[NP_ADL_MAX_PATH];
    char strPNPString[NP_ADL_MAX_PATH];
    int iOSDisplayIndex;
} NPAdapterInfo;

typedef struct {
    int iSize;
    int iEngineClock;
    int iMemoryClock;
    int iVddc;
    int iActivityPercent;
    int iCurrentPerformanceLevel;
    int iCurrentBusSpeed;
    int iCurrentBusLanes;
    int iMaximumBusLanes;
    int iReserved;
} NPADLPMActivity;

typedef struct {
    int iSize;
    int iTemperature;
} NPADLTemperature;

typedef void* (*NPADL_MAIN_MALLOC_CALLBACK)(int);

struct AdlApi {
    bool loaded = false;
    HMODULE dll = nullptr;

    int (*Main_Control_Create)(NPADL_MAIN_MALLOC_CALLBACK, int) = nullptr;
    int (*Main_Control_Destroy)(void) = nullptr;
    int (*Adapter_NumberOfAdapters_Get)(int*) = nullptr;
    int (*Adapter_AdapterInfo_Get)(NPAdapterInfo*, int) = nullptr;
    int (*Overdrive5_CurrentActivity_Get)(int, NPADLPMActivity*) = nullptr;
    int (*Overdrive5_Temperature_Get)(int, int, NPADLTemperature*) = nullptr;

    int adapterCount = 0;
    int activeIndex = -1;

    bool Load();
    void Unload();
};

// ================================================================ HWiNFO 共享内存
// HWiNFO 需要在设置里打开 "Shared Memory Support" 才会创建这块共享内存。
struct HwinfoCtx {
    bool loaded = false;
    HANDLE map = nullptr;
    void*  view = nullptr;

    struct Reading {
        std::wstring sensor;   // 传感器分组名
        std::wstring label;    // 读数名
        std::wstring unit;
        double value = 0.0;
    };

    bool Open();
    void Close();
    bool Refresh();
    bool Find(const wchar_t* sensorKw, const wchar_t* labelKw, double* out) const;
    bool FindAny(const wchar_t* labelKw, double* out) const;

private:
    bool RefreshUnsafe();
    std::vector<Reading> readings;
    uint64_t lastStamp = 0;
};
