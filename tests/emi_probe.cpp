// EMI / CPU 频率 探测程序
//
// 目的：在**不改主程序**的前提下，先实测这台机器上
//   1) Windows EMI（Energy Meter Interface）能不能读到 CPU 功耗
//   2) CallNtPowerInformation 能不能读到 CPU 当前频率
// 全部是纯用户态、免驱动、免管理员的路径。
//
// 编译（不碰 dist，避免和正在运行的实例撞车）：
//   zig c++ -target x86_64-windows-gnu -O2 -std=c++20 -DUNICODE -D_UNICODE \
//       -o tests\emi_probe.exe tests\emi_probe.cpp -lpdh -lpowrprof -ladvapi32
//
// 用法： tests\emi_probe.exe

#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <powrprof.h>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "powrprof.lib")

// MinGW 的头文件里没有这个结构体，按 MSDN 的定义补上。
// 注意字段顺序：Number / MaxMhz / CurrentMhz / MhzLimit / ...
#ifndef PROCESSOR_POWER_INFORMATION_DEFINED
#define PROCESSOR_POWER_INFORMATION_DEFINED
typedef struct _NP_PROCESSOR_POWER_INFORMATION {
    ULONG Number;
    ULONG MaxMhz;
    ULONG CurrentMhz;
    ULONG MhzLimit;
    ULONG MaxIdleState;
    ULONG CurrentIdleState;
} NP_PROCESSOR_POWER_INFORMATION;
#endif

// POWER_INFORMATION_LEVEL 里的 ProcessorInformation = 11
#ifndef ProcessorInformation
#define NP_ProcessorInformation ((POWER_INFORMATION_LEVEL)11)
#else
#define NP_ProcessorInformation ProcessorInformation
#endif

// ------------------------------------------------------------------ CPU 频率
static void ProbeCpuFrequency() {
    printf("=== 1. CPU 频率（CallNtPowerInformation）===\n");
    // ⚠ 不能靠 CallNtPowerInformation 返回的长度：失败时它返回的是 NTSTATUS
    //   （0xC0000023 = STATUS_BUFFER_TOO_SMALL），不是所需字节数。
    //   按逻辑处理器数自己算。
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    ULONG len = si.dwNumberOfProcessors * (ULONG)sizeof(NP_PROCESSOR_POWER_INFORMATION);
    std::vector<BYTE> buf(len);
    NTSTATUS st = CallNtPowerInformation(NP_ProcessorInformation, nullptr, 0, buf.data(), len);
    if (st != 0) {
        printf("  [失败] CallNtPowerInformation 返回 %ld\n", (long)st);
        return;
    }
    auto* pi = reinterpret_cast<NP_PROCESSOR_POWER_INFORMATION*>(buf.data());
    ULONG n = len / sizeof(NP_PROCESSOR_POWER_INFORMATION);
    printf("  逻辑处理器数 = %lu\n", (unsigned long)n);
    printf("  %-6s %-10s %-10s %-10s\n", "编号", "当前MHz", "最大MHz", "限制MHz");
    for (ULONG i = 0; i < n && i < 8; ++i)
        printf("  %-6lu %-10lu %-10lu %-10lu\n", (unsigned long)i,
               (unsigned long)pi[i].CurrentMhz, (unsigned long)pi[i].MaxMhz,
               (unsigned long)pi[i].MhzLimit);
    if (n > 8) printf("  ...（其余 %lu 个略）\n", (unsigned long)(n - 8));
    if (n) printf("  => 可用 ✅  （取 0 号核：%lu MHz / 最大 %lu MHz）\n",
                  (unsigned long)pi[0].CurrentMhz, (unsigned long)pi[0].MaxMhz);
}

// ------------------------------------------------------------------ PDH 工具
static void ListPdhObjects(const wchar_t* filter) {
    DWORD size = 0;
    PDH_STATUS s = PdhEnumObjectsW(nullptr, nullptr, nullptr, &size, PERF_DETAIL_WIZARD, TRUE);
    if (s != PDH_MORE_DATA && s != ERROR_SUCCESS) {
        printf("  [失败] PdhEnumObjects 取长度失败 (0x%08lX)\n", (unsigned long)s);
        return;
    }
    // ⚠ pdh 的 pcchBufferLength 单位是**字符数**（不是字节数）
    std::vector<wchar_t> buf((size_t)size + 2);
    s = PdhEnumObjectsW(nullptr, nullptr, buf.data(), &size, PERF_DETAIL_WIZARD, TRUE);
    if (s != ERROR_SUCCESS) {
        printf("  [失败] PdhEnumObjects (0x%08lX)\n", (unsigned long)s);
        return;
    }
    int hit = 0;
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) {
        if (wcsstr(p, filter)) {
            printf("    %ls\n", p);
            ++hit;
        }
    }
    if (!hit) printf("    （没有名字含 \"%ls\" 的性能对象）\n", filter);
}

// 展开一条计数器路径，打印所有实例的格式化值
static void ReadCounterPath(const wchar_t* path) {
    printf("  路径: %ls\n", path);
    DWORD size = 0;
    PDH_STATUS s = PdhExpandWildCardPathW(nullptr, path, nullptr, &size, 0);
    if (s != PDH_MORE_DATA && s != ERROR_SUCCESS) {
        printf("    [无] 展开失败 (0x%08lX)\n", (unsigned long)s);
        return;
    }
    // ⚠ 同上：size 是字符数
    std::vector<wchar_t> buf((size_t)size + 2);
    s = PdhExpandWildCardPathW(nullptr, path, buf.data(), &size, 0);
    if (s != ERROR_SUCCESS) {
        printf("    [无] 展开失败 (0x%08lX)\n", (unsigned long)s);
        return;
    }
    int printed = 0;
    for (const wchar_t* p = buf.data(); *p && printed < 12; p += wcslen(p) + 1) {
        PDH_HQUERY q = nullptr;
        PDH_HCOUNTER c = nullptr;
        if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS) continue;
        if (PdhAddCounterW(q, p, 0, &c) != ERROR_SUCCESS) { PdhCloseQuery(q); continue; }
        PdhCollectQueryData(q);
        Sleep(120);
        PdhCollectQueryData(q);
        PDH_FMT_COUNTERVALUE v{};
        if (PdhGetFormattedCounterValue(c, PDH_FMT_DOUBLE, nullptr, &v) == ERROR_SUCCESS &&
            v.CStatus == PDH_CSTATUS_VALID_DATA) {
            printf("    %-58ls = %.3f\n", p, v.doubleValue);
            ++printed;
        }
        PdhCloseQuery(q);
    }
    if (!printed) printf("    [无] 没有可用实例（计数器存在但读不到数据）\n");
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    printf("NextPerf · EMI / CPU 频率 探测\n");
    printf("============================================================\n\n");

    ProbeCpuFrequency();

    printf("\n=== 2. 系统里跟「能耗 / 电源」有关的性能对象 ===\n");
    ListPdhObjects(L"Energy");
    ListPdhObjects(L"Power");
    ListPdhObjects(L"Processor");

    printf("\n=== 3. EMI（Energy Meter Interface）计数器 ===\n");
    ReadCounterPath(L"\\Energy Meter(*)\\Power");
    ReadCounterPath(L"\\Energy Meter(*)\\Energy");
    ReadCounterPath(L"\\Energy Meter(*)\\*");

    printf("\n=== 4. 对照：传统 CPU 计数器 ===\n");
    ReadCounterPath(L"\\Processor Information(_Total)\\% Processor Performance");
    ReadCounterPath(L"\\Processor Information(_Total)\\Processor Frequency");

    printf("\n============================================================\n");
    printf("怎么看结果：\n");
    printf("  * 第 1 节有数 -> CPU 频率可以直接读，无需驱动/管理员\n");
    printf("  * 第 3 节有非零数值 -> EMI 可用，CPU 功耗有希望\n");
    printf("  * 第 3 节全是 [无] -> 这台机器/固件没暴露 EMI，功耗只能走 HWiNFO\n");
    printf("\n按回车退出...\n");
    getchar();
    return 0;
}
