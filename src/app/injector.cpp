// 进程注入：把 NextPerfHook.dll 塞进目标游戏进程。
//
// 流程：
//   1) 打开进程（尽量降权，失败再申请全部权限）
//   2) 远程分配内存写入 DLL 路径
//   3) CreateRemoteThread + LoadLibraryW
//   4) 用一个命名互斥量标记「已注入」，避免重复注入

#include "np_app.h"

#include <tlhelp32.h>
#include <shellapi.h>
#include <cstdarg>
#include <cstdio>
#include <algorithm>

namespace npa {

static HANDLE gWatchThread = nullptr;
static volatile bool gWatchRun = false;

// ---------------------------------------------------------------- 注入状态锁
// 见 np_app.h 里的说明：守护线程与主线程共享 gApp.injected / gApp.games / gApp.notice。
static CRITICAL_SECTION& AppMutex() {
    struct Holder {
        CRITICAL_SECTION cs;
        Holder() { InitializeCriticalSection(&cs); }
    };
    static Holder h;      // 函数局部静态：首次使用时构造，C++11 起线程安全
    return h.cs;
}

AppLock::AppLock() { EnterCriticalSection(&AppMutex()); }
AppLock::~AppLock() { LeaveCriticalSection(&AppMutex()); }

// ---------------------------------------------------------------- 诊断日志
//
// 为什么必须有这个：注入失败的原因原来只写进 gApp.statusText，
// 而主循环每 120ms 就把状态栏刷掉了 —— 用户只会看到「未注入」，
// 永远不知道是权限不够、DLL 载入失败、还是目标根本不是 64 位。
// 游戏进程里的钩子日志（NextPerfHook.log）又只有在 DLL 真载入之后才有。
// 中间这一段（注入本身）原来完全没有记录。
// ---------------------------------------------------------------- 构建指纹
std::wstring BuildStamp() {
    static std::wstring cached;
    if (!cached.empty()) return cached;
    wchar_t exe[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) { cached = L"unknown"; return cached; }
    HANDLE h = CreateFileW(exe, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { cached = L"unknown"; return cached; }
    FILETIME ft{};
    GetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
    FILETIME local{};
    FileTimeToLocalFileTime(&ft, &local);
    SYSTEMTIME st{};
    FileTimeToSystemTime(&local, &st);
    wchar_t buf[64];
    swprintf(buf, 64, L"%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    cached = buf;
    return cached;
}

std::wstring AppLogPath() {
    wchar_t dir[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"TEMP", dir, MAX_PATH);
    std::wstring p = (n && n < MAX_PATH) ? std::wstring(dir) : std::wstring(L".");
    return p + L"\\NextPerf.log";
}

void AppLog(const char* fmt, ...) {
    char body[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    SYSTEMTIME st{};
    GetLocalTime(&st);
    char line[1200];
    int len = snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d pid=%lu] %s\n", st.wHour, st.wMinute,
                       st.wSecond, st.wMilliseconds, (unsigned long)GetCurrentProcessId(), body);
    OutputDebugStringA(line);

    HANDLE h = CreateFileW(AppLogPath().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD sz = GetFileSize(h, nullptr);
    if (sz > 1024 * 1024) {
        // 超过 1MB 就截断重建。
        // ⚠ 不能像原来那样 SetFilePointer(h, 0, FILE_BEGIN) 就以为「从头覆盖」：
        //   这个句柄是用 FILE_APPEND_DATA（没有 FILE_WRITE_DATA）打开的，
        //   WriteFile 会**忽略文件指针**、永远写到文件末尾 —— 于是日志无限增长，
        //   那句「超过 1MB 从头覆盖」从来没生效过。正确做法是关掉重开、
        //   用 GENERIC_WRITE 截断，再继续按追加方式打开。
        CloseHandle(h);
        HANDLE t = CreateFileW(AppLogPath().c_str(), GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (t != INVALID_HANDLE_VALUE) { SetEndOfFile(t); CloseHandle(t); }
        h = CreateFileW(AppLogPath().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
    }
    DWORD wr = 0;
    WriteFile(h, line, (DWORD)len, &wr, nullptr);
    CloseHandle(h);
}

void SetNotice(const std::string& text, int level, uint32_t noticeMs) {
    // ★ 守护线程（注入看护）也会调 SetNotice，而主线程每 120ms 读/清 gApp.notice。
    //   两个线程同时碰一个 std::string 是 UB（SSO 缓冲与堆指针撕裂会崩），必须加锁。
    AppLock lk;
    gApp.notice = text;
    gApp.noticeLevel = level;
    gApp.noticeUntil = GetTickCount64() + noticeMs;
    AppLog("notice[%d]: %s", level, text.c_str());
}

bool AdminRelaunch() {
    wchar_t exe[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return false;
    // runas 会弹 UAC；用户拒绝就返回 <=32，此时不做任何事
    HINSTANCE r = ShellExecuteW(nullptr, L"runas", exe, nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) <= 32) {
        SetNotice("已被拒绝提升权限（或 UAC 取消），程序继续以普通权限运行", 1);
        return false;
    }
    AppLog("relaunch as admin requested");
    return true;
}

// ---------------------------------------------------------------- 路径
std::wstring DllPath() {
    if (!gApp.dllPath.empty()) return gApp.dllPath;
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring exe(buf);
    size_t p = exe.find_last_of(L"\\/");
    std::wstring dir = (p == std::wstring::npos) ? L"." : exe.substr(0, p);
    gApp.exeDir = dir;
    gApp.dllPath = dir + L"\\NextPerfHook.dll";
    return gApp.dllPath;
}

static std::wstring InjectMutexName(DWORD pid) {
    wchar_t n[64];
    swprintf(n, 64, L"Local\\NextPerf_Injected_%lu", (unsigned long)pid);
    return std::wstring(n);
}

bool IsInjected(DWORD pid) {
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, InjectMutexName(pid).c_str());
    if (m) { CloseHandle(m); return true; }
    return false;
}

// ---------------------------------------------------------------------------
// WoW64（跨位数）注入支持
//
// 我们是 64 位程序。往 32 位目标注入时**不能**用我们自己的 LoadLibraryW 地址 ——
// 那是 64 位 kernel32 里的地址，在 32 位进程的地址空间里不存在，
// 远端线程起不来（实测现象：目标进程里始终没有我们的模块，钩子日志也不生成）。
//
// 正解：
//   1) NtQueryInformationProcess(ProcessWow64Information=26) 直接给出目标的 32 位 PEB
//   2) 走 32 位 PEB 的 Ldr->InMemoryOrderModuleList 找到 kernel32.dll 的 DllBase
//   3) 解析它的导出表，取 **32 位 LoadLibraryW** 的地址
//   4) 用这个 32 位地址 CreateRemoteThread
//
// ⚠ 下面所有偏移都按 **32 位** 布局硬编码（64 位的指针宽度不同，偏移也不一样）。
// ---------------------------------------------------------------------------

typedef LONG(NTAPI* NpNtQueryInformationProcess_t)(HANDLE, ULONG, PVOID, ULONG, PULONG);

static bool NpReadRemote(HANDLE proc, uint64_t addr, void* buf, SIZE_T n) {
    SIZE_T got = 0;
    if (!ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(addr)), buf, n,
                           &got))
        return false;
    return got == n;
}

// 在 32 位模块里按导出名找函数，返回**绝对地址**（modBase + 导出 RVA）
static uint32_t NpExportAddr32(HANDLE proc, uint32_t modBase, const char* funcName) {
    IMAGE_DOS_HEADER dos{};
    if (!NpReadRemote(proc, modBase, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    IMAGE_NT_HEADERS32 nt{};
    if (!NpReadRemote(proc, (uint64_t)modBase + dos.e_lfanew, &nt, sizeof(nt))) return 0;
    if (nt.Signature != IMAGE_NT_SIGNATURE) return 0;

    const IMAGE_DATA_DIRECTORY& dir =
        nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress || !dir.Size) return 0;

    IMAGE_EXPORT_DIRECTORY exp{};
    if (!NpReadRemote(proc, (uint64_t)modBase + dir.VirtualAddress, &exp, sizeof(exp))) return 0;
    if (!exp.NumberOfNames || !exp.AddressOfNames || !exp.AddressOfFunctions ||
        !exp.AddressOfNameOrdinals)
        return 0;

    for (DWORD i = 0; i < exp.NumberOfNames; ++i) {
        uint32_t nameRva = 0;
        if (!NpReadRemote(proc, (uint64_t)modBase + exp.AddressOfNames + i * 4, &nameRva, 4))
            return 0;
        if (!nameRva) continue;
        char nm[64]{};
        if (!NpReadRemote(proc, (uint64_t)modBase + nameRva, nm, sizeof(nm) - 1)) continue;
        if (strcmp(nm, funcName) != 0) continue;
        uint16_t ord = 0;
        if (!NpReadRemote(proc, (uint64_t)modBase + exp.AddressOfNameOrdinals + i * 2, &ord, 2))
            return 0;
        uint32_t fnRva = 0;
        if (!NpReadRemote(proc, (uint64_t)modBase + exp.AddressOfFunctions + (uint32_t)ord * 4,
                          &fnRva, 4))
            return 0;
        if (!fnRva) return 0;
        return modBase + fnRva;
    }
    return 0;
}

// 在 32 位目标进程里解析 LoadLibraryW 的地址；失败返回 0
static uint32_t NpResolve32LoadLibraryW(HANDLE proc) {
    static NpNtQueryInformationProcess_t ntq = nullptr;
    if (!ntq) {
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        if (nt) ntq = reinterpret_cast<NpNtQueryInformationProcess_t>(
            GetProcAddress(nt, "NtQueryInformationProcess"));
    }
    if (!ntq) return 0;

    // 1) 32 位 PEB 地址（ProcessWow64Information = 26）
    ULONG_PTR peb32 = 0;
    if (ntq(proc, 26, &peb32, sizeof(peb32), nullptr) != 0 || !peb32) return 0;

    // 2) PEB->Ldr        —— 32 位 PEB 里 Ldr 在 +0x0C
    uint32_t ldr = 0;
    if (!NpReadRemote(proc, peb32 + 0x0C, &ldr, 4) || !ldr) return 0;

    // 3) Ldr->InMemoryOrderModuleList —— 32 位 PEB_LDR_DATA 里在 +0x14
    uint32_t head = 0;
    if (!NpReadRemote(proc, (uint64_t)ldr + 0x14, &head, 4) || !head) return 0;

    uint32_t cur = head;
    for (int i = 0; i < 256; ++i) {
        // LDR_DATA_TABLE_ENTRY：InMemoryOrderLinks 在 +0x08，DllBase 在 +0x18，
        // BaseDllName（UNICODE_STRING）在 +0x2C（Length@+0x2C, Buffer@+0x30）
        uint32_t entry = cur - 0x08;
        uint32_t base = 0, nameBuf = 0;
        uint16_t nameLen = 0;
        if (!NpReadRemote(proc, (uint64_t)entry + 0x18, &base, 4)) return 0;
        NpReadRemote(proc, (uint64_t)entry + 0x2C, &nameLen, 2);
        NpReadRemote(proc, (uint64_t)entry + 0x30, &nameBuf, 4);

        // "kernel32.dll" = 12 个字符 = 24 字节
        if (base && nameBuf && nameLen >= 24 && nameLen < 260) {
            wchar_t nm[130]{};
            if (NpReadRemote(proc, nameBuf, nm, nameLen)) {
                for (auto& ch : nm) ch = (wchar_t)towlower(ch);
                if (wcscmp(nm, L"kernel32.dll") == 0)
                    return NpExportAddr32(proc, base, "LoadLibraryW");
            }
        }
        uint32_t next = 0;
        if (!NpReadRemote(proc, cur, &next, 4) || !next || next == head) break;
        cur = next;
    }
    return 0;
}

// 把 Win32 错误码翻译成人话 —— 用户看到 "err=5" 没有意义
static const char* ExplainWin32(DWORD e) {
    switch (e) {
        case 5:    return "ERROR_ACCESS_DENIED（目标权限更高：试试「以管理员身份重启」）";
        case 87:   return "ERROR_INVALID_PARAMETER";
        case 126:  return "ERROR_MOD_NOT_FOUND（DLL 或其依赖在目标进程里找不到）";
        case 127:  return "ERROR_PROC_NOT_FOUND";
        case 299:  return "ERROR_PARTIAL_COPY（目标很可能是 32 位进程）";
        case 1008: return "ERROR_NO_TOKEN";
        default:   return "";
    }
}

bool ProcessIsWow64(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    BOOL wow = FALSE;
    bool ok = IsWow64Process(h, &wow) && wow;
    CloseHandle(h);
    return ok;
}

bool InjectInto(DWORD pid) {
    if (pid == 0 || pid == GetCurrentProcessId()) return false;
    if (IsInjected(pid)) return true;

    std::wstring dll = DllPath();
    std::wstring exeName = ProcessExeName(pid);

    std::string who = "pid " + std::to_string(pid);
    if (!exeName.empty()) {
        char buf[NP_NAME_LEN]{};
        WideCharToMultiByte(CP_UTF8, 0, exeName.c_str(), -1, buf, NP_NAME_LEN - 1, nullptr, nullptr);
        who += std::string(" (") + buf + ")";
    }

    // ---- 按目标进程位数选 DLL
    // 32 位进程**加载不了** 64 位 DLL（反之亦然），必须各用各的。
    // 原来的行为是检测到 32 位目标就直接拒绝注入，现在两边都编了，改用 32 位那份。
    if (ProcessIsWow64(pid)) {
        size_t dot = dll.find_last_of(L'.');
        std::wstring p32 = (dot == std::wstring::npos)
                               ? dll + L"32"
                               : dll.substr(0, dot) + L"32" + dll.substr(dot);
        if (GetFileAttributesW(p32.c_str()) != INVALID_FILE_ATTRIBUTES) {
            dll = p32;
            AppLog("inject %s: target is 32-bit, using 32-bit hook %ls", who.c_str(),
                   dll.c_str());
        } else {
            AppLog("inject %s: target is 32-bit but NextPerfHook32.dll is missing",
                   who.c_str());
            SetNotice("目标是 32 位进程，但程序目录下没有 NextPerfHook32.dll，无法注入：" + who,
                      1, 20000);
            return false;
        }
    }

    DWORD attr = GetFileAttributesW(dll.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        AppLog("inject %s: DLL not found at %ls", who.c_str(), dll.c_str());
        SetNotice("找不到 NextPerfHook.dll（应在 " + std::string("程序目录") + "），无法注入", 1);
        return false;
    }

    DWORD access = PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_CREATE_THREAD |
                   PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ;
    HANDLE proc = OpenProcess(access, FALSE, pid);
    DWORD err1 = GetLastError();
    if (!proc) {
        proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
        DWORD err2 = GetLastError();
        AppLog("inject %s: OpenProcess failed (limited err=%lu %s / all err=%lu %s)",
               who.c_str(), (unsigned long)err1, ExplainWin32(err1), (unsigned long)err2,
               ExplainWin32(err2));
        if (err1 == 5 || err2 == 5) {
            SetNotice("注入失败：打不开 " + who +
                      " —— 目标权限更高。请用托盘菜单「以管理员身份重启」后再试", 1, 20000);
        } else {
            SetNotice("注入失败：OpenProcess 被拒（err=" + std::to_string(err1) + " " +
                      ExplainWin32(err1) + "）：" + who, 1, 20000);
        }
        return false;
    }

    SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        DWORD e = GetLastError();
        AppLog("inject %s: VirtualAllocEx failed err=%lu %s", who.c_str(), (unsigned long)e,
               ExplainWin32(e));
        SetNotice("注入失败：目标进程里分配内存被拒（err=" + std::to_string(e) + "）：" + who, 1);
        CloseHandle(proc);
        return false;
    }

    BOOL written = WriteProcessMemory(proc, remote, dll.c_str(), bytes, nullptr);
    if (!written) {
        DWORD e = GetLastError();
        AppLog("inject %s: WriteProcessMemory failed err=%lu %s", who.c_str(), (unsigned long)e,
               ExplainWin32(e));
        SetNotice("注入失败：写目标进程内存被拒（err=" + std::to_string(e) + "）：" + who, 1);
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC loadLib = GetProcAddress(k32, "LoadLibraryW");

    // 32 位目标：我们自己的 LoadLibraryW 地址在它的地址空间里**不存在**，
    // 必须换成目标进程里那份 32 位的 LoadLibraryW（走 PEB 解析，见上面）。
    if (ProcessIsWow64(pid)) {
        uint32_t a32 = NpResolve32LoadLibraryW(proc);
        if (!a32) {
            AppLog("inject %s: 32-bit target but could not resolve its LoadLibraryW", who.c_str());
            SetNotice("注入失败：这是 32 位进程，但没能定位它内部的 LoadLibraryW（PEB 解析失败）：" +
                          who,
                      1, 20000);
            VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
            CloseHandle(proc);
            return false;
        }
        loadLib = reinterpret_cast<FARPROC>(static_cast<uintptr_t>(a32));
        AppLog("inject %s: 32-bit target, using its LoadLibraryW at %08X", who.c_str(),
               (unsigned)a32);
    }

    HANDLE th = CreateRemoteThread(proc, nullptr, 0,
                                   reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLib), remote, 0,
                                   nullptr);
    if (!th) {
        DWORD e = GetLastError();
        AppLog("inject %s: CreateRemoteThread failed err=%lu %s", who.c_str(), (unsigned long)e,
               ExplainWin32(e));
        SetNotice("注入失败：CreateRemoteThread 被拒（err=" + std::to_string(e) + " " +
                  ExplainWin32(e) + "）：" + who, 1, 20000);
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    DWORD wait = WaitForSingleObject(th, 10000);
    DWORD exitCode = 0;
    GetExitCodeThread(th, &exitCode);
    CloseHandle(th);
    // ★ 只有确认远端线程已经结束了，才能释放远程内存。
    //   超时（WAIT_TIMEOUT）说明 LoadLibraryW 可能**还在读**这段 DLL 路径字符串，
    //   这时候 VirtualFreeEx 等于让游戏进程去读已释放的页 —— 直接把它打崩。
    //   按铁律「宁可功能退化也不能崩游戏」：这一种情况故意不释放（只泄漏一页）。
    if (wait == WAIT_OBJECT_0) {
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    } else {
        AppLog("inject %s: 远端 LoadLibrary 10 秒未返回，保留远程内存不释放（泄漏 %llu 字节）",
               who.c_str(), (unsigned long long)bytes);
    }
    CloseHandle(proc);

    // 注意：exitCode == STILL_ACTIVE(259) 说明 LoadLibrary 还在跑（大进程加载慢），
    // 这不算失败；只有明确的 0 才是载入失败。
    AppLog("inject %s: remote LoadLibrary returned 0x%lX (wait=%lu)", who.c_str(),
           (unsigned long)exitCode, (unsigned long)wait);
    if (exitCode == 0) {
        SetNotice("注入失败：目标进程里 LoadLibrary 返回 NULL。"
                  "常见原因是依赖缺失或目标有反注入保护：" + who, 1, 20000);
        return false;
    }

    // ★ 下面要改 gApp.injected，而主线程每 120ms 就在遍历它 —— 必须加锁。
    //   （本函数最多会在这里等 10 秒的那一段已经过去了，锁只圈住登记动作。）
    AppLock lk;
    for (DWORD p : gApp.injected)
        if (p == pid) {
            SetNotice("已注入 " + who + "（钩子已载入，正在等它接管画面）", 0, 8000);
            return true;
        }
    gApp.injected.push_back(pid);
    SetNotice("已注入 " + who + "（钩子已载入，正在等它接管画面）", 0, 8000);
    return true;
}


static std::wstring ExeNameOf(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    std::wstring n = (p == std::wstring::npos) ? path : path.substr(p + 1);
    std::transform(n.begin(), n.end(), n.begin(), ::towlower);
    return n;
}

// 枚举进程，按 exe 名匹配
struct ProcEntry {
    DWORD pid;
    std::wstring name;
};

static std::vector<ProcEntry> EnumProcesses() {
    std::vector<ProcEntry> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            std::wstring n = pe.szExeFile;
            std::transform(n.begin(), n.end(), n.begin(), ::towlower);
            out.push_back({pe.th32ProcessID, n});
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

// 请求某个 pid 的钩子自卸载：置 cfg.detachPid，钩子在守卫线程（约 800ms 一轮）
// 看到后会走 SelfUnloadNow —— 那套会 RestoreAllHooks（还原 vtable 补丁）、
// 注销 VEH、FreeLibraryAndExitThread，正是「反复注入不闪退」用的干净卸载。
// detachPid 的请求时间戳：约 2.5 秒后无条件清零。
//
// 为什么不能只在「pid 已退出」时清：游戏还活着的话 detachPid 会一直留着，
// 下次再注入这个游戏，钩子一看 detachPid == 自己就**立刻又自卸载**。
// 2.5 秒足够钩子（200ms 一轮）完成 SelfUnloadNow。
static uint32_t gDetachAt = 0;

void RequestHookDetach(DWORD pid) {
    if (!pid) return;
    gApp.cfg.detachPid = pid;
    gDetachAt = GetTickCount() ? GetTickCount() : 1;
    AppLog("detach: 请求 pid=%lu 的钩子自卸载", (unsigned long)pid);
}

// 移除第 index 条游戏记录：**先卸载钩子再删条目**。
// 不卸载的话，学习逻辑会立刻从还活着的数据里把它重新学回来 ——
// 这正是用户报的「清空后又自己回来」。
void ForgetGameAt(int index) {
    if (index < 0 || index >= (int)gApp.games.size()) return;
    DWORD pid = gApp.games[index].pid;
    std::wstring nm = gApp.games[index].name.empty() ? ExeNameOf(gApp.games[index].path)
                                                     : gApp.games[index].name;

    // ★ pid 为 0 时**当场按名字查一遍进程**。
    //   g.pid 是由 InjectorScanNow 填的，而它在 !monitoring 时直接 return，
    //   所以监视关着、或条目刚学习完还没被扫过时 pid 都是 0 ——
    //   原来 `if (pid)` 不成立就**根本没请求卸载**（用户报的「清空没有正确退出 hook」）。
    if (!pid && !nm.empty()) {
        for (auto& p : EnumProcesses()) {
            if (p.pid == GetCurrentProcessId()) continue;
            if (p.name == nm) { pid = p.pid; break; }
        }
    }

    if (pid) {
        RequestHookDetach(pid);

        // ★ 摘掉「已注入」记录：否则 IsInjected 仍为真 -> 界面显示"已接管"、
        //   而且自动注入不会再处理它（用户报的「接管状态没清空」）。
        {
            AppLock lk;
            for (size_t i = gApp.injected.size(); i-- > 0;) {
                if (gApp.injected[i] == pid) gApp.injected.erase(gApp.injected.begin() + i);
            }
            // ★ 遥测属于这个 pid 就清掉：否则 attached 仍为真，面板还以为钩子活着。
            if (gApp.telemetry.pid == pid) {
                NPClearTelemetry(&gApp.telemetry);
                AppLog("forget: 已清空 pid=%lu 的遥测（接管状态）", (unsigned long)pid);
            }
            gApp.games.erase(gApp.games.begin() + index);
        }
    } else {
        AppLock lk;
        gApp.games.erase(gApp.games.begin() + index);
    }

    AppLog("forget: 已移除 %ls（pid=%lu，已请求卸载钩子）", nm.c_str(), (unsigned long)pid);
    SettingsSave();
}

// 清空「学习来的」条目（手动添加的保留）。逐条走 ForgetGameAt 同一入口。
int ForgetLearned() {
    int n = 0;
    for (size_t i = gApp.games.size(); i-- > 0;) {
        if (gApp.games[i].learned) {
            ForgetGameAt((int)i);
            ++n;
        }
    }
    return n;
}

void InjectorScanNow() {
    {
        AppLock lk;      // gApp.monitoring / gApp.games 都是跨线程共享的
        if (!gApp.monitoring || gApp.games.empty()) return;
    }

    auto procs = EnumProcesses();   // 纯本线程数据，锁外做，别占着锁

    // 本轮的注入目标先收集起来，**出了锁再注入**：
    // InjectInto 里那个 WaitForSingleObject 最长等 10 秒，如果在锁里做，
    // 主线程的界面刷新（AppPublish / AppPollSensors 都要拿这把锁）会一起卡死。
    std::vector<DWORD> toInject;

    {
        AppLock lk;
        // 每轮把「配置的游戏现在是什么状态」重新算一遍，UI 直接显示这个。
        // 用户看到的「待注入」原来没有任何解释，这一栏就是解释。
        for (auto& g : gApp.games) {
            // 学习来的条目没有可读路径（商店应用在 WindowsApps 下，普通用户读不了），
            // 所以优先用 name；手动添加的条目没有 name，回退到从 path 取。
            std::wstring want = g.name.empty() ? ExeNameOf(g.path) : g.name;
            if (want.empty()) continue;
            // 学习来的条目要用户主动打开「实验性自动注入」才动手 ——
            // 手动添加的条目不受此开关影响，一直自动注入。
            if (g.learned && !gApp.cfg.learnedAutoHook) continue;
            DWORD pid = 0;
            for (auto& p : procs) {
                if (p.pid == GetCurrentProcessId()) continue;
                if (p.name == want) { pid = p.pid; break; }
            }
            // detachPid 用完就清，两种情况都清：
            //   a) 目标 pid 已退出
            //   b) 请求发出超过 2.5 秒（卸载早就完成了）—— 这一条是必须的：
            //      只在退出时清的话，游戏还活着时 detachPid 会一直留着，
            //      下次再注入它会立刻又被卸载。
            //   pid 复用的问题也由此解决（清掉之后就不会误伤新进程）。
            if (gApp.cfg.detachPid != 0) {
                bool stillAlive = false;
                for (auto& p : procs) {
                    if (p.pid == gApp.cfg.detachPid) { stillAlive = true; break; }
                }
                uint32_t tnow = GetTickCount();
                bool timeout = gDetachAt && tnow - gDetachAt >= 2500;
                if (!stillAlive || timeout) {
                    AppLog("detach: 清除 detachPid（pid=%lu %s）",
                           (unsigned long)gApp.cfg.detachPid,
                           stillAlive ? "已超时 2.5s，卸载应已完成" : "已退出");
                    gApp.cfg.detachPid = 0;
                    gDetachAt = 0;
                }
            }
            if (pid != g.pid) {
                if (pid) AppLog("watch: %ls is running (pid=%lu)", want.c_str(), (unsigned long)pid);
                else if (g.pid) AppLog("watch: %ls exited", want.c_str());
            }
            g.pid = pid;
            g.injected = pid && IsInjected(pid);
        }

        for (auto& p : procs) {
            if (p.pid == GetCurrentProcessId()) continue;
            if (IsInjected(p.pid)) {
                bool known = false;
                for (DWORD k : gApp.injected) if (k == p.pid) known = true;
                if (!known) gApp.injected.push_back(p.pid);
                continue;
            }
            for (auto& g : gApp.games) {
                if (g.pid == p.pid) {      // 只注入这一轮匹配上的那个进程
                    toInject.push_back(p.pid);
                    break;
                }
            }
        }
    }

    for (DWORD pid : toInject) {
        InjectInto(pid);                   // 自己会加锁登记
        AppLock lk;
        for (auto& g : gApp.games)
            if (g.pid == pid) g.injected = IsInjected(pid);
    }
}

static DWORD WINAPI WatchThread(LPVOID) {
    while (gWatchRun) {
        Sleep(800);
        InjectorScanNow();
    }
    return 0;
}

bool InjectorInit() {
    gWatchRun = true;
    gWatchThread = CreateThread(nullptr, 0, WatchThread, nullptr, 0, nullptr);
    return gWatchThread != nullptr;
}

void InjectorShutdown() {
    gWatchRun = false;
    if (gWatchThread) {
        WaitForSingleObject(gWatchThread, 2000);
        CloseHandle(gWatchThread);
        gWatchThread = nullptr;
    }
}

void InjectorTick() {
    // 主循环里定期调用，清理已经退出的进程
    // ★ 整段都在锁里：它遍历/修改 gApp.injected，而这个 vector 同时被守护线程
    //   （InjectorScanNow）和主线程的 AppPublish 读写。
    AppLock lk;
    for (size_t i = 0; i < gApp.injected.size();) {
        if (!ProcessAlive(gApp.injected[i])) gApp.injected.erase(gApp.injected.begin() + i);
        else ++i;
    }
    // 已经退出的进程，遥测映射也一起收掉，免得读到一堆陈旧数据
    for (size_t i = 0; i < gApp.telSlots.size();) {
        DWORD pid = gApp.telSlots[i].pid;
        bool keep = false;
        for (DWORD k : gApp.injected) if (k == pid) keep = true;
        if (keep) { ++i; continue; }
        if (gApp.telSlots[i].view) UnmapViewOfFile(gApp.telSlots[i].view);
        if (gApp.telSlots[i].map) CloseHandle(gApp.telSlots[i].map);
        if (gApp.telemetryPid == pid) gApp.telemetryPid = 0;
        gApp.telSlots.erase(gApp.telSlots.begin() + i);
    }
}

void TelemetryCloseAll() {
    AppLock lk;
    for (auto& s : gApp.telSlots) {
        if (s.view) UnmapViewOfFile(s.view);
        if (s.map) CloseHandle(s.map);
    }
    gApp.telSlots.clear();
    gApp.telemetryPid = 0;
}

bool ProcessAlive(DWORD pid) {
    if (!pid) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    DWORD code = 0;
    bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
}

// 完整路径（拿不到就返回空）
std::wstring ProcessImagePath(DWORD pid) {
    if (!pid) return {};
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t buf[MAX_PATH * 2]{};
    DWORD n = (DWORD)(sizeof(buf) / sizeof(buf[0]));
    std::wstring out;
    if (QueryFullProcessImageNameW(h, 0, buf, &n)) out.assign(buf, n);
    CloseHandle(h);
    return out;
}

std::wstring ProcessExeName(DWORD pid) {
    std::wstring p = ProcessImagePath(pid);
    size_t i = p.find_last_of(L"\\/");
    return (i == std::wstring::npos) ? p : p.substr(i + 1);
}

}  // namespace npa
