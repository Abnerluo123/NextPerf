// ============================================================================
// NextPerf tests/simvk —— Vulkan 模拟游戏进程
// ============================================================================
//
// 存在的理由
// ----------
// NextPerf 目前只支持 D3D11 / D3D12，靠挂钩 DXGI 的 Present 和 D3D 的
// 命令队列来采集帧数据。Vulkan 是另一条完全独立的路径，而且**不能**照搬
// vtable 钩子那套 —— Vulkan 的 loader 是开源的、函数指针由
// vkGetInstanceProcAddr / vkGetDeviceProcAddr 动态派发，社区（RTSS、Steam
// Overlay、OBS）公认的正确做法是写一个 **Vulkan 隐式层**（VK_LAYER_*，
// 注册到 HKLM\SOFTWARE\Khronos\Vulkan\ImplicitLayers）。
//
// 那么在做那条路之前，先得有一个**真实的 Vulkan 靶子**，用来回答三个问题：
//   1) 隐式层能不能被 loader 自动挂到我的进程上？
//   2) 层能不能拿到 vkQueuePresentKHR，并且每帧都被调到？
//   3) 层从我这里采到的帧数据（帧序号、CPU/GPU 帧时间、Present 阻塞时长）
//      到底对不对？
// 这个程序就是那个靶子。它刻意做成"一个正常得不能再正常的 Vulkan 游戏"：
// 走标准分派路径、用标准同步、每帧 Present 一次、支持 resize/切模式/切 vsync。
//
// 为什么自己写 Vulkan 声明而不用 <vulkan/vulkan.h>
// -----------------------------------------------
// 本机没有 Vulkan SDK / MSVC / Windows SDK。声明全部手写在 vk_min.h 里，
// 并且用 44 个 static_assert 锁死了结构体布局（编译期就能发现抄错）。
// 详见 vk_min.h 顶部注释。
//
// 着色器怎么办？—— 没有 glslangValidator，所以 SPIR-V 是**手工汇编**的。
// 见下方 kVertSpv / kFragSpv，每条指令一行、带注释。为了不把拼错的字
// 交给驱动（驱动只会给你一个没头没脑的 VK_ERROR_INITIALIZATION_FAILED），
// 程序启动时会先跑一遍 SpirvSelfCheck()，逐条校验指令的 word-count 是否能
// 严丝合缝地走到数组末尾。
//
// 用法见 README.md。

#include "vk_min.h"

#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <functional>

// ============================================================================
// 0. 小工具：QPC 计时 / 线程安全输出 / 字符串
// ============================================================================

static int64_t g_qpcFreq = 1;

// 进程启动时刻，所有 t_ms 都相对它
static int64_t g_qpcOrigin = 0;

static void QpcInit() {
    LARGE_INTEGER f, n;
    ::QueryPerformanceFrequency(&f);
    ::QueryPerformanceCounter(&n);
    g_qpcFreq   = f.QuadPart ? f.QuadPart : 1;
    g_qpcOrigin = n.QuadPart;
}

// 高精度当前时间（毫秒，double）。QPC 在 Windows 上单调且分辨率 ~100ns，
// 是测帧时间唯一可用的时钟（GetTickCount64 只有 ~15.6ms 分辨率，
// 拿来测 16.6ms 的帧周期会直接丢精度）。
static inline double QpcMs() {
    LARGE_INTEGER n;
    ::QueryPerformanceCounter(&n);
    return (double)(n.QuadPart - g_qpcOrigin) * 1000.0 / (double)g_qpcFreq;
}

// stdout / stderr 会被渲染线程和 stdin 线程同时写，必须加锁。
// 用 CRITICAL_SECTION 而不是 std::mutex：这里不需要 RAII，
// 而且项目统一 -fno-exceptions。
static CRITICAL_SECTION g_outLock;
static bool g_jsonMode = false;

// 人类可读的信息：--json 时走 stderr，避免污染 stdout 的 JSON 流
// （Python 驱动方会逐行 json.loads(stdout)，混进横幅会直接解析失败）。
static void Info(const char* fmt, ...) {
    va_list ap;
    ::EnterCriticalSection(&g_outLock);
    FILE* f = g_jsonMode ? stderr : stdout;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fflush(f);
    ::LeaveCriticalSection(&g_outLock);
}

// 机器可读的 JSON 行：永远走 stdout，且立刻 flush。
// flush 很关键 —— 驱动方按行读，不 flush 的话管道里会攒一大坨才出来。
static void JsonRaw(const char* line) {
    ::EnterCriticalSection(&g_outLock);
    fputs(line, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    ::LeaveCriticalSection(&g_outLock);
}

// JSON 字符串转义（层名/设备名理论上不会有特殊字符，但不能赌）
static std::string JStr(const char* s) {
    std::string o;
    if (!s) return o;
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
        switch (*p) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (*p < 0x20) { char b[8]; sprintf(b, "\\u%04x", *p); o += b; }
                else o += (char)*p;
        }
    }
    return o;
}

static bool StrIContains(const char* hay, const char* needle) {
    if (!hay || !needle) return false;
    size_t nl = strlen(needle);
    if (!nl) return false;
    for (const char* p = hay; *p; ++p) {
        size_t i = 0;
        while (i < nl && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) ++i;
        if (i == nl) return true;
    }
    return false;
}

// UTF-16 → UTF-8（注册表/文件路径都是宽字符）
static std::string W2U(const wchar_t* w) {
    if (!w) return std::string();
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s((size_t)(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}
static std::string W2U(const std::wstring& w) { return W2U(w.c_str()); }

// ============================================================================
// 1. 手工汇编的 SPIR-V 着色器
// ============================================================================
//
// 等价 GLSL：
//
//   // vertex
//   #version 450
//   layout(location=0) in  vec2 inPos;
//   layout(location=1) in  vec3 inColor;
//   layout(location=0) out vec3 vColor;
//   layout(push_constant) uniform PC { vec4 rot; } pc;  // xy=cos,sin  zw=1/aspect,1
//   void main() {
//       vec2 p = vec2(inPos.x*pc.rot.x - inPos.y*pc.rot.y,
//                     inPos.x*pc.rot.y + inPos.y*pc.rot.x);
//       p *= pc.rot.zw;
//       gl_Position = vec4(p, 0.0, 1.0);
//       vColor = inColor;
//   }
//
//   // fragment
//   #version 450
//   layout(location=0) in  vec3 vColor;
//   layout(location=0) out vec4 outColor;
//   void main() { outColor = vec4(vColor, 1.0); }
//
// 为什么旋转放在着色器里、用 push constant 传？
// 因为 push constant 是 Vulkan 特有的、D3D 那边没有对应物的东西，
// 将来写层的时候是必须正确处理的一块（层要能改 pipeline layout 的
// push constant range）。让靶子用它，等于顺手把这个面也覆盖了。
//
// 为什么不用 sin/cos 指令（GLSL.std.450 ExtInst）？—— 少一个扩展指令集
// 就少一处手工汇编出错的机会，把 cos/sin 放 CPU 算好传进来更稳。

// 指令头：(wordCount << 16) | opcode
#define NP_SPV_OP(op, wc) (((uint32_t)(wc) << 16) | (uint32_t)(op))

static const uint32_t kVertSpv[] = {
    0x07230203, 0x00010000, 0x00000000, 45, 0x00000000,   // 魔数/版本1.0/生成器/bound/保留

    NP_SPV_OP(17, 2), 1,                                  // OpCapability Shader
    NP_SPV_OP(14, 3), 0, 1,                               // OpMemoryModel Logical GLSL450
    // OpEntryPoint Vertex %main "main" %inPos %inColor %vColor %gl_Position
    NP_SPV_OP(15, 9), 0, 23, 0x6E69616D, 0x00000000, 18, 19, 20, 21,
    NP_SPV_OP(3, 3), 2, 450,                              // OpSource GLSL 450

    NP_SPV_OP(5, 4), 23, 0x6E69616D, 0x00000000,          // OpName %main "main"
    NP_SPV_OP(5, 4), 18, 0x6F506E69, 0x00000073,          // OpName %inPos "inPos"
    NP_SPV_OP(5, 4), 19, 0x6F436E69, 0x00726F6C,          // OpName %inColor "inColor"
    NP_SPV_OP(5, 4), 20, 0x6C6F4376, 0x0000726F,          // OpName %vColor "vColor"
    NP_SPV_OP(5, 3), 11, 0x00004350,                      // OpName %PC "PC"
    NP_SPV_OP(6, 4), 11, 0, 0x00746F72,                   // OpMemberName %PC 0 "rot"
    NP_SPV_OP(5, 3), 22, 0x00006370,                      // OpName %pc "pc"
    // OpName %gl_Position "gl_Position"
    NP_SPV_OP(5, 5), 21, 0x505F6C67, 0x7469736F, 0x006E6F69,

    NP_SPV_OP(71, 4), 21, 11, 0,                          // OpDecorate %gl_Position BuiltIn Position
    NP_SPV_OP(71, 4), 18, 30, 0,                          // OpDecorate %inPos Location 0
    NP_SPV_OP(71, 4), 19, 30, 1,                          // OpDecorate %inColor Location 1
    NP_SPV_OP(71, 4), 20, 30, 0,                          // OpDecorate %vColor Location 0
    NP_SPV_OP(72, 5), 11, 0, 35, 0,                       // OpMemberDecorate %PC 0 Offset 0
    NP_SPV_OP(71, 3), 11, 2,                              // OpDecorate %PC Block

    NP_SPV_OP(19, 2), 1,                                  // %void    = OpTypeVoid
    NP_SPV_OP(33, 3), 2, 1,                               // %func    = OpTypeFunction %void
    NP_SPV_OP(22, 3), 3, 32,                              // %float   = OpTypeFloat 32
    NP_SPV_OP(23, 4), 4, 3, 2,                            // %v2float = OpTypeVector %float 2
    NP_SPV_OP(23, 4), 5, 3, 3,                            // %v3float = OpTypeVector %float 3
    NP_SPV_OP(23, 4), 6, 3, 4,                            // %v4float = OpTypeVector %float 4
    NP_SPV_OP(21, 4), 7, 32, 1,                           // %int     = OpTypeInt 32 1
    NP_SPV_OP(43, 4), 7, 8, 0,                            // %int_0   = OpConstant %int 0
    NP_SPV_OP(43, 4), 3, 9,  0x00000000,                  // %float_0 = OpConstant %float 0.0
    NP_SPV_OP(43, 4), 3, 10, 0x3F800000,                  // %float_1 = OpConstant %float 1.0
    NP_SPV_OP(30, 3), 11, 6,                              // %PC      = OpTypeStruct %v4float
    NP_SPV_OP(32, 4), 12, 9, 6,                           // %ptr_pc_v4float = OpTypePointer PushConstant %v4float
    NP_SPV_OP(32, 4), 13, 9, 11,                          // %ptr_pc_PC      = OpTypePointer PushConstant %PC
    NP_SPV_OP(32, 4), 14, 1, 4,                           // %ptr_in_v2float = OpTypePointer Input %v2float
    NP_SPV_OP(32, 4), 15, 1, 5,                           // %ptr_in_v3float = OpTypePointer Input %v3float
    NP_SPV_OP(32, 4), 16, 3, 5,                           // %ptr_out_v3float= OpTypePointer Output %v3float
    NP_SPV_OP(32, 4), 17, 3, 6,                           // %ptr_out_v4float= OpTypePointer Output %v4float
    NP_SPV_OP(59, 4), 14, 18, 1,                          // %inPos      = OpVariable Input
    NP_SPV_OP(59, 4), 15, 19, 1,                          // %inColor    = OpVariable Input
    NP_SPV_OP(59, 4), 16, 20, 3,                          // %vColor     = OpVariable Output
    NP_SPV_OP(59, 4), 17, 21, 3,                          // %gl_Position= OpVariable Output
    NP_SPV_OP(59, 4), 13, 22, 9,                          // %pc         = OpVariable PushConstant

    NP_SPV_OP(54, 5), 1, 23, 0, 2,                        // %main = OpFunction %void None %func
    NP_SPV_OP(248, 2), 24,                                // %label = OpLabel
    NP_SPV_OP(61, 4), 4, 25, 18,                          // %p_inPos = OpLoad %v2float %inPos
    NP_SPV_OP(65, 5), 12, 26, 22, 8,                      // %rotptr = OpAccessChain %ptr_pc_v4float %pc %int_0
    NP_SPV_OP(61, 4), 6, 27, 26,                          // %rotv = OpLoad %v4float %rotptr
    NP_SPV_OP(81, 5), 3, 28, 27, 0,                       // %rx = OpCompositeExtract %float %rotv 0
    NP_SPV_OP(81, 5), 3, 29, 27, 1,                       // %ry = OpCompositeExtract %float %rotv 1
    NP_SPV_OP(81, 5), 3, 30, 27, 2,                       // %rz = OpCompositeExtract %float %rotv 2
    NP_SPV_OP(81, 5), 3, 31, 27, 3,                       // %rw = OpCompositeExtract %float %rotv 3
    NP_SPV_OP(81, 5), 3, 32, 25, 0,                       // %px = OpCompositeExtract %float %p_inPos 0
    NP_SPV_OP(81, 5), 3, 33, 25, 1,                       // %py = OpCompositeExtract %float %p_inPos 1
    NP_SPV_OP(133, 5), 3, 34, 32, 28,                     // %t1 = OpFMul %float %px %rx
    NP_SPV_OP(133, 5), 3, 35, 33, 29,                     // %t2 = OpFMul %float %py %ry
    NP_SPV_OP(131, 5), 3, 36, 34, 35,                     // %nx = OpFSub %float %t1 %t2
    NP_SPV_OP(133, 5), 3, 37, 32, 29,                     // %t3 = OpFMul %float %px %ry
    NP_SPV_OP(133, 5), 3, 38, 33, 28,                     // %t4 = OpFMul %float %py %rx
    NP_SPV_OP(129, 5), 3, 39, 37, 38,                     // %ny = OpFAdd %float %t3 %t4
    NP_SPV_OP(133, 5), 3, 40, 36, 30,                     // %nx2 = OpFMul %float %nx %rz
    NP_SPV_OP(133, 5), 3, 41, 39, 31,                     // %ny2 = OpFMul %float %ny %rw
    NP_SPV_OP(80, 5), 4, 42, 40, 41,                      // %p2 = OpCompositeConstruct %v2float %nx2 %ny2
    NP_SPV_OP(80, 6), 6, 43, 42, 9, 10,                   // %pos4 = OpCompositeConstruct %v4float %p2 %float_0 %float_1
    NP_SPV_OP(62, 3), 21, 43,                             // OpStore %gl_Position %pos4
    NP_SPV_OP(61, 4), 5, 44, 19,                          // %col = OpLoad %v3float %inColor
    NP_SPV_OP(62, 3), 20, 44,                             // OpStore %vColor %col
    NP_SPV_OP(253, 1),                                    // OpReturn
    NP_SPV_OP(56, 1),                                     // OpFunctionEnd
};

static const uint32_t kFragSpv[] = {
    0x07230203, 0x00010000, 0x00000000, 15, 0x00000000,

    NP_SPV_OP(17, 2), 1,                                  // OpCapability Shader
    NP_SPV_OP(14, 3), 0, 1,                               // OpMemoryModel Logical GLSL450
    // OpEntryPoint Fragment %main "main" %vColor %outColor
    NP_SPV_OP(15, 7), 4, 11, 0x6E69616D, 0x00000000, 9, 10,
    NP_SPV_OP(16, 3), 11, 7,                              // OpExecutionMode %main OriginUpperLeft
    NP_SPV_OP(3, 3), 2, 450,                              // OpSource GLSL 450

    NP_SPV_OP(5, 4), 11, 0x6E69616D, 0x00000000,          // OpName %main "main"
    NP_SPV_OP(5, 4), 9, 0x6C6F4376, 0x0000726F,           // OpName %vColor "vColor"
    // OpName %outColor "outColor"
    NP_SPV_OP(5, 5), 10, 0x4374756F, 0x726F6C6F, 0x00000000,

    NP_SPV_OP(71, 4), 9, 30, 0,                           // OpDecorate %vColor Location 0
    NP_SPV_OP(71, 4), 10, 30, 0,                          // OpDecorate %outColor Location 0

    NP_SPV_OP(19, 2), 1,                                  // %void    = OpTypeVoid
    NP_SPV_OP(33, 3), 2, 1,                               // %func    = OpTypeFunction %void
    NP_SPV_OP(22, 3), 3, 32,                              // %float   = OpTypeFloat 32
    NP_SPV_OP(23, 4), 4, 3, 3,                            // %v3float = OpTypeVector %float 3
    NP_SPV_OP(23, 4), 5, 3, 4,                            // %v4float = OpTypeVector %float 4
    NP_SPV_OP(43, 4), 3, 6, 0x3F800000,                   // %float_1 = OpConstant %float 1.0
    NP_SPV_OP(32, 4), 7, 1, 4,                            // %ptr_in_v3float = OpTypePointer Input %v3float
    NP_SPV_OP(32, 4), 8, 3, 5,                            // %ptr_out_v4float= OpTypePointer Output %v4float
    NP_SPV_OP(59, 4), 7, 9, 1,                            // %vColor   = OpVariable Input
    NP_SPV_OP(59, 4), 8, 10, 3,                           // %outColor = OpVariable Output

    NP_SPV_OP(54, 5), 1, 11, 0, 2,                        // %main = OpFunction %void None %func
    NP_SPV_OP(248, 2), 12,                                // %label = OpLabel
    NP_SPV_OP(61, 4), 4, 13, 9,                           // %col = OpLoad %v3float %vColor
    NP_SPV_OP(80, 5), 5, 14, 13, 6,                       // %res = OpCompositeConstruct %v4float %col %float_1
    NP_SPV_OP(62, 3), 10, 14,                             // OpStore %outColor %res
    NP_SPV_OP(253, 1),                                    // OpReturn
    NP_SPV_OP(56, 1),                                     // OpFunctionEnd
};

#undef NP_SPV_OP

// 手工汇编的自检：逐条走指令，校验 wordCount 能否严丝合缝走到末尾。
// 这一步的价值：SPIR-V 的字数写错时，驱动只会回一个
// VK_ERROR_INITIALIZATION_FAILED，什么都不说；自己先查一遍能直接指出
// "第 N 条指令声明的字数越界了"。
static bool SpirvSelfCheck(const uint32_t* code, size_t words, const char* tag,
                           std::string* err) {
    if (words < 5) { *err = std::string(tag) + ": 长度不足 5 个字"; return false; }
    if (code[0] != 0x07230203) { *err = std::string(tag) + ": 魔数不对"; return false; }
    uint32_t bound = code[3];
    if (bound == 0) { *err = std::string(tag) + ": bound 为 0"; return false; }

    size_t pos = 5;
    uint32_t maxId = 0;
    int idx = 0;
    while (pos < words) {
        uint32_t head = code[pos];
        uint32_t wc = head >> 16;
        uint32_t op = head & 0xFFFFu;
        if (wc == 0) {
            char b[160]; sprintf(b, "%s: 第 %d 条指令 (偏移 %zu, opcode %u) 声明 wordCount=0",
                                 tag, idx, pos, op);
            *err = b; return false;
        }
        if (pos + wc > words) {
            char b[160]; sprintf(b, "%s: 第 %d 条指令 (偏移 %zu, opcode %u) 声明 %u 字，越界",
                                 tag, idx, pos, op, wc);
            *err = b; return false;
        }
        // 顺带记一下出现过的最大 id，和 bound 对一下（bound 必须 > 所有 id）
        if (op != 15 /*OpEntryPoint 的字符串字面量不是 id*/) {
            for (uint32_t i = 1; i < wc; ++i) {
                uint32_t v = code[pos + i];
                if (v > maxId && v < bound) maxId = v;
            }
        }
        pos += wc;
        ++idx;
    }
    if (pos != words) {
        char b[160]; sprintf(b, "%s: 指令总长度 %zu != 数组长度 %zu（字数写错了）",
                             tag, pos, words);
        *err = b; return false;
    }
    if (maxId + 1 > bound) {
        char b[160]; sprintf(b, "%s: bound=%u 小于最大 id+1=%u", tag, bound, maxId + 1);
        *err = b; return false;
    }
    return true;
}

// ============================================================================
// 2. 命令行参数
// ============================================================================

struct Options {
    double   seconds   = 0.0;      // >0 表示按时间退出
    uint32_t frames    = 0;        // >0 表示按帧数退出
    bool     vsync     = true;
    uint32_t fpsCap    = 0;        // 0 = 不锁
    uint32_t width     = 1280;
    uint32_t height    = 720;
    int      windowMode = 0;       // 0=windowed 1=borderless 2=fullscreen
    bool     json      = false;
    bool     listLayers = false;
    bool     showHelp  = false;
    bool     noStdin   = false;    // 不启动 stdin 命令线程（自动化跑的时候省心）
};

static const char* WindowModeName(int m) {
    switch (m) { case 0: return "windowed"; case 1: return "borderless"; case 2: return "fullscreen"; }
    return "unknown";
}

static const char* PresentModeName(VkPresentModeKHR m) {
    switch (m) {
        case VK_PRESENT_MODE_IMMEDIATE_KHR:    return "immediate";
        case VK_PRESENT_MODE_MAILBOX_KHR:      return "mailbox";
        case VK_PRESENT_MODE_FIFO_KHR:         return "fifo";
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "fifo_relaxed";
        default: return "unknown";
    }
}

static void PrintUsage() {
    printf(
"simvk —— NextPerf 的 Vulkan 模拟游戏进程（用于测试 Vulkan 隐式层注入）\n"
"\n"
"用法: simvk.exe [选项]\n"
"\n"
"  --seconds=N          运行 N 秒后退出（可带小数，如 --seconds=3.5）\n"
"  --frames=N           渲染 N 帧后退出\n"
"  --vsync=on|off       on=FIFO；off=优先 IMMEDIATE，其次 MAILBOX，都没有则回退 FIFO\n"
"  --fps-cap=N          锁帧（0=不锁，按 QPC 睡眠）\n"
"  --width=W --height=H 窗口/交换链尺寸\n"
"  --window-mode=M      windowed | borderless | fullscreen\n"
"  --json               每帧一行 JSON 输出到 stdout（人类可读信息改走 stderr）\n"
"  --no-stdin           不启动 stdin 命令线程（纯自动化跑，不需要运行时控制时用）\n"
"  --list-layers        列出系统上所有 Vulkan 层（隐式/显式），然后退出\n"
"  --help               显示本帮助\n"
"\n"
"运行时可从 stdin 逐行下发命令：\n"
"  resize W H | mode windowed|borderless|fullscreen | vsync on|off | fpscap N | quit\n"
"\n"
"环境变量 VK_INSTANCE_LAYERS 由 loader 按标准行为处理（自动启用其中列出的层）。\n");
}

static bool ParseArgs(int argc, char** argv, Options* o) {
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto val = [&](const char* name) -> const char* {
            size_t n = strlen(name);
            if (strncmp(a, name, n) == 0 && a[n] == '=') return a + n + 1;
            return nullptr;
        };
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) { o->showHelp = true; return true; }
        else if (strcmp(a, "--json") == 0)       o->json = true;
        else if (strcmp(a, "--no-stdin") == 0)   o->noStdin = true;
        else if (strcmp(a, "--list-layers") == 0) o->listLayers = true;
        else if (const char* v = val("--seconds"))   o->seconds = atof(v);
        else if (const char* v = val("--frames"))    o->frames = (uint32_t)strtoul(v, nullptr, 10);
        else if (const char* v = val("--fps-cap"))   o->fpsCap = (uint32_t)strtoul(v, nullptr, 10);
        else if (const char* v = val("--width"))     o->width = (uint32_t)strtoul(v, nullptr, 10);
        else if (const char* v = val("--height"))    o->height = (uint32_t)strtoul(v, nullptr, 10);
        else if (const char* v = val("--vsync")) {
            if (strcmp(v, "on") == 0) o->vsync = true;
            else if (strcmp(v, "off") == 0) o->vsync = false;
            else { fprintf(stderr, "[参数错误] --vsync 只接受 on|off，收到 '%s'\n", v); return false; }
        }
        else if (const char* v = val("--window-mode")) {
            if (strcmp(v, "windowed") == 0) o->windowMode = 0;
            else if (strcmp(v, "borderless") == 0) o->windowMode = 1;
            else if (strcmp(v, "fullscreen") == 0) o->windowMode = 2;
            else { fprintf(stderr, "[参数错误] --window-mode 只接受 windowed|borderless|fullscreen\n"); return false; }
        }
        else {
            fprintf(stderr, "[参数错误] 未知参数 '%s'（--help 看用法）\n", a);
            return false;
        }
    }
    if (o->width < 16 || o->height < 16 || o->width > 16384 || o->height > 16384) {
        fprintf(stderr, "[参数错误] 尺寸 %ux%u 不合理\n", o->width, o->height);
        return false;
    }
    return true;
}

// ============================================================================
// 3. 层枚举（--list-layers 的核心）
// ============================================================================
//
// 为什么要自己读注册表来分「隐式 / 显式」？
// 因为 Vulkan **没有任何 API** 能告诉你某个层是隐式还是显式。
// vkEnumerateInstanceLayerProperties 把两者混在一起返回。唯一的权威来源
// 就是 loader 自己读的那几个注册表键：
//     HKLM\SOFTWARE\Khronos\Vulkan\ImplicitLayers   ← 隐式（默认自动启用）
//     HKLM\SOFTWARE\Khronos\Vulkan\ExplicitLayers   ← 显式（必须显式点名）
//     （HKCU 同名键、以及 WOW6432Node 下的 32 位版本）
// 每个值名是一个 JSON 清单的路径，清单里的 "name" 才是层名。
//
// 这对 NextPerf 的意义：将来 NextPerf 的 Vulkan 层就是往 ImplicitLayers
// 里塞一个 VK_LAYER_NEXTPERF.json。所以 --list-layers 必须能一眼看出
// 「我的层注册上了没有、loader 认不认、DLL 在不在」。

struct LayerInfo {
    std::string name;
    std::string description;
    uint32_t    specVersion = 0;
    uint32_t    implVersion = 0;
    bool        implicit = false;
    bool        fromRegistry = false;
    bool        disabled = false;      // 注册表值数据为 1（显式禁用）
    bool        manifestRead = false;  // 清单 JSON 读得出来
    std::string manifestPath;
    std::string libraryPath;      // 清单里的 library_path（相对路径已按清单目录拼成绝对）
    bool        libraryExists = false;
    bool        libraryIs64 = false;  // 来自 64 位注册表视图还是 WOW6432Node
    std::vector<std::string> disableEnv;
    std::vector<std::string> enableEnv;
};

// 把 "1.3.296" 这种 specVersion 拆成人类可读形式
static std::string VersionStr(uint32_t v) {
    char b[64];
    sprintf(b, "%u.%u.%u", (v >> 22) & 0x3FFu, (v >> 12) & 0x3FFu, v & 0xFFFu);
    return b;
}

// 极简 JSON 取值：找 "key" 后第一个字符串。够用即可（清单格式是我们已知的）。
// 注意必须做转义还原：清单里写的是 "library_path": ".\\xxx.dll"，
// JSON 文本里是两个反斜杠字符，还原后才是真的单反斜杠路径。
// （不还原的话拼出来的路径会变成 "C:\dir\\xxx.dll" —— Windows 能容忍，
//   但输出很难看，而且拿去做字符串比较就会对不上。）
static void JsonUnescape(std::string* s) {
    std::string o;
    o.reserve(s->size());
    for (size_t i = 0; i < s->size(); ++i) {
        if ((*s)[i] == '\\' && i + 1 < s->size()) {
            char c = (*s)[++i];
            switch (c) {
                case 'n': o += '\n'; break;
                case 't': o += '\t'; break;
                case 'r': o += '\r'; break;
                case 'b': o += '\b'; break;
                case 'f': o += '\f'; break;
                case 'u': {                       // \uXXXX：只处理 ASCII 范围
                    if (i + 4 < s->size()) {
                        unsigned cp = 0;
                        bool ok = true;
                        for (int k = 1; k <= 4; ++k) {
                            char h = (*s)[i + k];
                            unsigned d;
                            if (h >= '0' && h <= '9') d = (unsigned)(h - '0');
                            else if (h >= 'a' && h <= 'f') d = (unsigned)(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') d = (unsigned)(h - 'A' + 10);
                            else { ok = false; break; }
                            cp = cp * 16 + d;
                        }
                        if (ok) { i += 4; if (cp < 0x80) o += (char)cp; else o += '?'; }
                        else o += c;
                    } else o += c;
                    break;
                }
                default: o += c; break;           // \\ \" \/ 等都归到"去掉反斜杠"
            }
        } else o += (*s)[i];
    }
    *s = o;
}

static bool JsonFindString(const std::string& text, const char* key, std::string* out) {
    std::string pat = std::string("\"") + key + "\"";
    size_t p = text.find(pat);
    if (p == std::string::npos) return false;
    p = text.find(':', p + pat.size());
    if (p == std::string::npos) return false;
    p = text.find('"', p);
    if (p == std::string::npos) return false;
    size_t e = text.find('"', p + 1);
    if (e == std::string::npos) return false;
    *out = text.substr(p + 1, e - p - 1);
    JsonUnescape(out);
    return true;
}

// 取出 "key": { ... } 这个对象的原文（花括号配对扫描），再从中抽出所有键名。
// disable_environment / enable_environment 都是这种 { "VAR": "1" } 形式。
static void JsonFindObjectKeys(const std::string& text, const char* key,
                               std::vector<std::string>* keys) {
    std::string pat = std::string("\"") + key + "\"";
    size_t p = text.find(pat);
    if (p == std::string::npos) return;
    p = text.find('{', p + pat.size());
    if (p == std::string::npos) return;
    int depth = 0;
    size_t e = p;
    for (; e < text.size(); ++e) {
        if (text[e] == '{') ++depth;
        else if (text[e] == '}') { if (--depth == 0) break; }
    }
    if (e >= text.size()) return;
    std::string obj = text.substr(p, e - p + 1);
    // 抽 "KEY" 形式的键名（跳过值字符串：值也有引号，但键总在 ':' 之前）
    size_t i = 0;
    while (i < obj.size()) {
        size_t q = obj.find('"', i);
        if (q == std::string::npos) break;
        size_t q2 = obj.find('"', q + 1);
        if (q2 == std::string::npos) break;
        std::string tok = obj.substr(q + 1, q2 - q - 1);
        size_t after = obj.find_first_not_of(" \t\r\n", q2 + 1);
        bool isKey = (after != std::string::npos && after < obj.size() && obj[after] == ':');
        if (isKey) keys->push_back(tok);
        i = q2 + 1;
    }
}

static bool ReadWholeFileW(const std::wstring& path, std::string* out) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!::GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (1 << 20)) {
        ::CloseHandle(h); return false;
    }
    out->resize((size_t)sz.QuadPart);
    DWORD got = 0;
    BOOL ok = ::ReadFile(h, &(*out)[0], (DWORD)out->size(), &got, nullptr);
    ::CloseHandle(h);
    if (!ok) return false;
    out->resize(got);
    return true;
}

static bool FileExistsW(const std::wstring& p) {
    DWORD a = ::GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// 读一个注册表清单键，把 {层名 -> 清单路径} 收进来
// 注册表里的一条层记录。
//
// ★ 关于注册表布局，这里有一个**极易搞错**的点，值得单独写清楚：
//   HKLM\SOFTWARE\Khronos\Vulkan\ImplicitLayers 的存储形式是
//       值名 (value NAME)  = 清单 JSON 的完整路径
//       值数据(value DATA) = REG_DWORD，0 = 启用，1 = 禁用
//   也就是说**路径在值名里，不在数据里**。
//   （第一版就是按"数据是路径"写的，结果注册了 4 个隐式层的机器上
//     一个都没解析出来，全被当成显式层 —— 这个 bug 值得留个记录。）
struct RegistryLayer {
    std::string name;          // 清单里的 "name"
    std::string manifestPath;  // 清单 JSON 路径（= 注册表值名）
    std::string description;
    std::string libraryPath;   // 拼成绝对路径后的层 DLL
    bool        implicit = false;
    bool        is64 = true;
    bool        disabled = false;    // 值数据为 1
    bool        manifestRead = false;// 清单能不能读出来
    std::vector<std::string> disableEnv;
    std::vector<std::string> enableEnv;
};

static void ReadLayerKey(HKEY root, const wchar_t* subkey, bool implicitWanted,
                         bool is64View, int* counter,
                         std::vector<RegistryLayer>* out) {
    HKEY k = nullptr;
    REGSAM sam = KEY_READ | (is64View ? KEY_WOW64_64KEY : KEY_WOW64_32KEY);
    if (::RegOpenKeyExW(root, subkey, 0, sam, &k) != ERROR_SUCCESS) return;

    for (DWORD i = 0;; ++i) {
        wchar_t valName[2048];
        DWORD   valLen = 2048;         // 单位是"字符数"，含结尾的 0
        DWORD   type = 0;
        BYTE    data[16];
        DWORD   dataLen = sizeof(data);
        LONG r = ::RegEnumValueW(k, i, valName, &valLen, nullptr, &type, data, &dataLen);
        if (r != ERROR_SUCCESS) break;   // ERROR_NO_MORE_ITEMS 也走这里

        RegistryLayer rl;
        rl.implicit = implicitWanted;
        rl.is64     = is64View;
        // 值数据是 DWORD：1 = 该层被禁用
        if (type == REG_DWORD && dataLen >= sizeof(DWORD))
            rl.disabled = (*(const DWORD*)data) != 0;
        rl.manifestPath = W2U(valName);
        if (rl.manifestPath.empty()) continue;

        std::wstring wmanifest(valName);
        std::string text;
        if (ReadWholeFileW(wmanifest, &text)) {
            rl.manifestRead = true;
            JsonFindString(text, "name", &rl.name);
            JsonFindString(text, "description", &rl.description);
            std::string lib;
            JsonFindString(text, "library_path", &lib);
            if (!lib.empty()) {
                // library_path 通常是 ".\\xxx.dll"，相对清单所在目录
                std::wstring wlib(lib.begin(), lib.end());
                size_t slash = wmanifest.find_last_of(L"\\/");
                std::wstring dir = (slash == std::wstring::npos) ? L"" : wmanifest.substr(0, slash + 1);
                if (wlib.size() >= 2 && wlib[0] == L'.' && (wlib[1] == L'\\' || wlib[1] == L'/'))
                    wlib = dir + wlib.substr(2);
                rl.libraryPath = W2U(wlib);
            }
            JsonFindObjectKeys(text, "disable_environment", &rl.disableEnv);
            JsonFindObjectKeys(text, "enable_environment", &rl.enableEnv);
        }
        if (rl.name.empty()) {
            // 清单读不出来（或没有 name 字段）时，至少用文件名占位，
            // 这样统计项数和"有 N 个注册项"能对上，不会凭空少几条。
            rl.name = "(清单无法解析) " + rl.manifestPath;
        }
        out->push_back(rl);
        ++(*counter);
    }
    ::RegCloseKey(k);
}

struct LayerRegistryStats {
    int hklm64Implicit = 0, hklm64Explicit = 0;
    int wow64Implicit  = 0, wow64Explicit  = 0;
    int hkcuImplicit   = 0, hkcuExplicit   = 0;
};

// 组装最终的层清单：以 vkEnumerateInstanceLayerProperties 为权威名单，
// 用注册表信息标注「隐式/显式」和「清单路径 / DLL 是否存在」。
static std::vector<LayerInfo> CollectLayers(VkApi& api, LayerRegistryStats* stats,
                                           std::vector<std::string>* registryNotes) {
    std::vector<LayerInfo> out;

    // (1) loader 报的可用层 —— 这是权威名单
    uint32_t count = 0;
    if (api.vkEnumerateInstanceLayerProperties &&
        api.vkEnumerateInstanceLayerProperties(&count, nullptr) == VK_SUCCESS && count) {
        std::vector<VkLayerProperties> props(count);
        if (api.vkEnumerateInstanceLayerProperties(&count, props.data()) == VK_SUCCESS) {
            for (uint32_t i = 0; i < count; ++i) {
                LayerInfo li;
                li.name        = props[i].layerName;
                li.description = props[i].description;
                li.specVersion = props[i].specVersion;
                li.implVersion = props[i].implementationVersion;
                out.push_back(li);
            }
        }
    }

    // (2) 注册表：谁在 ImplicitLayers / ExplicitLayers 里
    struct KeySpec { HKEY root; const wchar_t* sub; bool implicit; bool is64; int* counter;
                     const char* label; };
    std::vector<RegistryLayer> found;

    KeySpec keys[] = {
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers", true,  true,  &stats->hklm64Implicit, "HKLM\\...\\ImplicitLayers (64位视图)" },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\Vulkan\\ExplicitLayers", false, true,  &stats->hklm64Explicit, "HKLM\\...\\ExplicitLayers (64位视图)" },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Khronos\\Vulkan\\ImplicitLayers", true,  false, &stats->wow64Implicit, "HKLM\\...\\WOW6432Node\\...\\ImplicitLayers (32位)" },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Khronos\\Vulkan\\ExplicitLayers", false, false, &stats->wow64Explicit, "HKLM\\...\\WOW6432Node\\...\\ExplicitLayers (32位)" },
        { HKEY_CURRENT_USER,  L"SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers", true,  true,  &stats->hkcuImplicit,  "HKCU\\...\\ImplicitLayers" },
        { HKEY_CURRENT_USER,  L"SOFTWARE\\Khronos\\Vulkan\\ExplicitLayers", false, true,  &stats->hkcuExplicit,  "HKCU\\...\\ExplicitLayers" },
    };

    for (auto& k : keys) {
        ReadLayerKey(k.root, k.sub, k.implicit, k.is64, k.counter, &found);
        char line[512];
        sprintf(line, "%-54s : %d 项", k.label, *k.counter);
        registryNotes->push_back(line);
    }

    // (3) 合并：注册表里的每条记录，按层名并进 loader 报的名单
    for (auto& rl : found) {
        LayerInfo* hit = nullptr;
        for (auto& li : out) if (li.name == rl.name) { hit = &li; break; }

        if (hit) {
            hit->fromRegistry = true;
            // 同一个层名可能在 64 位和 32 位视图里各注册一次（同名不同 DLL）。
            // 只要有一次是隐式，就按隐式算 —— 那意味着它会被自动加载。
            if (rl.implicit) hit->implicit = true;
            if (hit->manifestPath.empty()) {
                hit->manifestPath  = rl.manifestPath;
                hit->libraryPath   = rl.libraryPath;
                hit->libraryIs64   = rl.is64;
                hit->disabled      = rl.disabled;
                hit->manifestRead  = rl.manifestRead;
                hit->disableEnv    = rl.disableEnv;
                hit->enableEnv     = rl.enableEnv;
                if (!rl.libraryPath.empty()) {
                    std::wstring w(rl.libraryPath.begin(), rl.libraryPath.end());
                    hit->libraryExists = FileExistsW(w);
                }
            }
        } else {
            // 注册表里有、但 vkEnumerateInstanceLayerProperties 没报 —— 说明 loader
            // 没认这个清单（JSON 格式错 / DLL 路径无效 / api_version 不兼容）。
            // 这正是最值得暴露的情况：以为层装好了，其实 loader 根本没加载它。
            LayerInfo li;
            li.name         = rl.name;
            li.description  = rl.description.empty()
                              ? "（注册表已注册，但 loader 未列出 —— 清单可能损坏）"
                              : rl.description + "（注册表已注册，但 loader 未列出 —— 清单可能损坏）";
            li.implicit     = rl.implicit;
            li.fromRegistry = true;
            li.manifestPath = rl.manifestPath;
            li.libraryPath  = rl.libraryPath;
            li.libraryIs64  = rl.is64;
            li.disabled     = rl.disabled;
            li.manifestRead = rl.manifestRead;
            li.disableEnv   = rl.disableEnv;
            li.enableEnv    = rl.enableEnv;
            if (!rl.libraryPath.empty()) {
                std::wstring w(rl.libraryPath.begin(), rl.libraryPath.end());
                li.libraryExists = FileExistsW(w);
            }
            out.push_back(li);
        }
    }
    return out;
}

static int DoListLayers(VkApi& api) {
    printf("================= Vulkan 层清单 =================\n");

    // loader 信息
    wchar_t sys[MAX_PATH] = L"";
    ::GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring loaderPath = std::wstring(sys) + L"\\vulkan-1.dll";
    printf("loader 路径     : %s\n", W2U(loaderPath).c_str());
    printf("loader 存在     : %s\n", FileExistsW(loaderPath) ? "是" : "否");
    if (api.dll) {
        wchar_t mod[MAX_PATH] = L"";
        ::GetModuleFileNameW(api.dll, mod, MAX_PATH);
        printf("实际加载的 DLL  : %s\n", W2U(mod).c_str());
    }
    uint32_t instVer = 0;
    if (api.vkEnumerateInstanceVersion && api.vkEnumerateInstanceVersion(&instVer) == VK_SUCCESS) {
        printf("loader 支持的实例版本: %s (0x%08X)\n", VersionStr(instVer).c_str(), instVer);
    } else {
        printf("loader 支持的实例版本: 无法查询（vkEnumerateInstanceVersion 不可用，说明是 1.0 loader）\n");
    }

    LayerRegistryStats stats{};
    std::vector<std::string> notes;
    std::vector<LayerInfo> layers = CollectLayers(api, &stats, &notes);

    printf("vkEnumerateInstanceLayerProperties 报告: %zu 个层\n", layers.size());
    printf("\n");

    auto printOne = [](const LayerInfo& li) {
        printf("  %s%s\n", li.name.c_str(), li.disabled ? "   [注册表标记为禁用]" : "");
        printf("      描述      : %s\n", li.description.c_str());
        printf("      规范版本  : %s (%u)   实现版本: %u\n",
               VersionStr(li.specVersion).c_str(), li.specVersion, li.implVersion);
        if (li.fromRegistry) {
            printf("      清单      : %s%s\n", li.manifestPath.c_str(),
                   li.manifestRead ? "" : "   [读取失败]");
            printf("      架构视图  : %s\n", li.libraryIs64 ? "64 位" : "32 位 (WOW6432Node)");
        } else {
            printf("      注册表    : 未在 ImplicitLayers/ExplicitLayers 中找到"
                   "（可能由 loader 内建或其他机制提供）\n");
        }
        if (!li.libraryPath.empty()) {
            printf("      层 DLL    : %s  [%s]\n", li.libraryPath.c_str(),
                   li.libraryExists ? "存在" : "缺失 → 该层会加载失败");
        }
        for (auto& d : li.disableEnv) printf("      禁用变量  : %s=1\n", d.c_str());
        for (auto& e : li.enableEnv)  printf("      启用变量  : %s=1\n", e.c_str());
    };

    printf("--- 隐式层 (implicit，loader 默认自动启用，NextPerf 将来要走的就是这条) ---\n");
    int nImp = 0;
    for (auto& li : layers) if (li.implicit) { printOne(li); ++nImp; }
    if (!nImp) printf("  （无）\n");

    printf("\n--- 显式层 (explicit，必须显式点名 / 靠 VK_INSTANCE_LAYERS 启用) ---\n");
    int nExp = 0;
    for (auto& li : layers) if (!li.implicit) { printOne(li); ++nExp; }
    if (!nExp) printf("  （无）\n");

    printf("\n--- 注册表来源统计（隐式/显式的判定依据）---\n");
    for (auto& n : notes) printf("  %s\n", n.c_str());

    printf("\n--- 环境变量 ---\n");
    const char* envNames[] = { "VK_INSTANCE_LAYERS", "VK_LOADER_LAYERS_ENABLE",
                               "VK_LOADER_LAYERS_DISABLE", "VK_LOADER_DEBUG",
                               "VK_ICD_FILENAMES", "VK_DRIVER_FILES", "VK_ADD_DRIVER_FILES" };
    for (const char* e : envNames) {
        char buf[1024];
        DWORD n = ::GetEnvironmentVariableA(e, buf, sizeof(buf));
        printf("  %-24s = %s\n", e, (n > 0 && n < sizeof(buf)) ? buf : "(未设置)");
    }

    // 这个统计是给「注入是否成功」这个问题用的
    int vkLayerCount = 0;
    for (auto& li : layers) if (li.name.rfind("VK_LAYER_", 0) == 0) ++vkLayerCount;
    printf("\n--- 结论 ---\n");
    printf("  名字以 VK_LAYER_ 开头的层: %d 个（其中隐式 %d 个）\n", vkLayerCount, nImp);
    int broken = 0;
    for (auto& li : layers) if (li.fromRegistry && !li.libraryPath.empty() && !li.libraryExists) ++broken;
    if (broken) printf("  ⚠ 有 %d 个已注册层的 DLL 不存在，loader 会报错但不会影响其他层。\n", broken);

    printf("\n注意：隐式层的**加载**发生在 vkCreateInstance 时，本命令不创建实例，\n");
    printf("      所以这里只能证明「注册了」，不能证明「挂上了」。\n");
    printf("      要确认某个层真的被加载，运行本程序时设 VK_LOADER_DEBUG=layer，\n");
    printf("      loader 会把每个层的加载/协商过程打到 stderr。\n");
    return 0;
}

// ============================================================================
// 3.5 「这个函数到底被层包装了没有」—— 进程内的硬证据
// ============================================================================
//
// 这是本工具对 NextPerf 最有价值的一个探针，所以单独讲清楚原理。
//
// 问题：Vulkan 没有任何 API 能告诉你「当前有哪些层处于激活状态」。
//       vkEnumerateInstanceLayerProperties 列的是"装了什么"，不是"挂了什么"。
//       对 NextPerf 来说，"我的层到底挂上了没"恰恰是最关键的问题。
//
// 办法：看**函数指针落在哪个模块里**。
//   * 没有任何层包装时，vkGetDeviceProcAddr(dev,"vkQueuePresentKHR")
//     返回的是 loader（vulkan-1.dll）里的终止跳板；
//   * 一旦某个层声明要拦截 vkQueuePresentKHR，loader 返回的就是
//     **层 DLL 里的那个函数**，模块随之变成 xxxLayer64.dll。
//   GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) 可以把一个
//   代码地址反查回它所属的模块，于是就有了确凿的、不依赖日志的结论。
//
// 局限（必须说明）：只能证明"被**某个**层包装了"，不能指出是哪个层 ——
//   不过模块名通常就写着是哪家的层。另外如果层选择不拦截该函数，
//   这里会显示 vulkan-1.dll，那是"没被这个函数拦截"，不等于"层没加载"。
struct ProcOrigin {
    std::string module;      // 函数所属模块的完整路径
    std::string moduleName;  // 只要文件名
    bool        fromLoader = false;
};

static ProcOrigin DescribeProc(const char* name, void* fn) {
    ProcOrigin o;
    if (!fn) { o.module = "(空指针)"; o.moduleName = o.module; return o; }
    HMODULE m = nullptr;
    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             (LPCWSTR)fn, &m) && m) {
        wchar_t p[MAX_PATH] = L"";
        ::GetModuleFileNameW(m, p, MAX_PATH);
        o.module = W2U(p);
        size_t s = o.module.find_last_of("\\/");
        o.moduleName = (s == std::string::npos) ? o.module : o.module.substr(s + 1);
        o.fromLoader = (_stricmp(o.moduleName.c_str(), "vulkan-1.dll") == 0);
    } else {
        o.module = "(不属于任何已加载模块)";
        o.moduleName = o.module;
    }
    (void)name;
    return o;
}

// 打印一份「分派归属」报告。层只要拦截了这几个函数中的任何一个，
// 对应的模块名就会从 vulkan-1.dll 变成层 DLL。
static void ReportDispatch(VkApi& api) {
    struct Item { const char* name; void* fn; };
    Item items[] = {
        { "vkQueuePresentKHR",          (void*)api.vkQueuePresentKHR },
        { "vkQueueSubmit",              (void*)api.vkQueueSubmit },
        { "vkAcquireNextImageKHR",      (void*)api.vkAcquireNextImageKHR },
        { "vkCreateSwapchainKHR",       (void*)api.vkCreateSwapchainKHR },
        { "vkCreateGraphicsPipelines",  (void*)api.vkCreateGraphicsPipelines },
        { "vkCmdDraw",                  (void*)api.vkCmdDraw },
    };
    int wrapped = 0;
    Info("[分派归属] 以下函数指针实际落在哪个模块里（层拦截了就会不是 vulkan-1.dll）：");
    for (auto& it : items) {
        ProcOrigin o = DescribeProc(it.name, it.fn);
        if (!o.fromLoader && it.fn) ++wrapped;
        Info("[分派归属]   %-26s -> %s%s", it.name, o.moduleName.c_str(),
             o.fromLoader ? "" : "   ← 被层拦截！");
    }
    if (wrapped == 0) {
        Info("[分派归属] 结论：这 6 个函数目前都由 loader(vulkan-1.dll) 直接分派，"
             "没有任何层拦截它们。");
        Info("[分派归属]       （注意：这不等于「没有层被加载」—— 层可能加载了但选择"
             "不拦截这些函数，或者只拦截了没在这里列出的函数。）");
    } else {
        Info("[分派归属] 结论：有 %d 个函数被层接管了（见上面标了「被层拦截」的行）。",
             wrapped);
    }
}

// ============================================================================
// 4. Win32 窗口
// ============================================================================

static HWND  g_hwnd = nullptr;
static bool  g_quit = false;
static volatile LONG g_pendingW = 0, g_pendingH = 0;   // WM_SIZE 攒下来的新尺寸
static bool  g_modeChangedDisplay = false;             // 是否改过显示模式（退出必须还原）

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) {
                ::InterlockedExchange(&g_pendingW, (LONG)LOWORD(lp));
                ::InterlockedExchange(&g_pendingH, (LONG)HIWORD(lp));
            } else {
                ::InterlockedExchange(&g_pendingW, 0);   // 0 = 最小化了
            }
            return 0;
        case WM_CLOSE:
            g_quit = true;
            return 0;
        case WM_DESTROY:
            g_quit = true;
            ::PostQuitMessage(0);
            return 0;
        case WM_ERASEBKGND:
            return 1;   // 我们整屏都会被 Vulkan 覆盖，别让 GDI 擦背景（会闪）
        case WM_SYSCOMMAND:
            // 屏蔽 Alt+Enter 之类的系统菜单行为，避免焦点跑掉
            if ((wp & 0xFFF0) == SC_KEYMENU) return 0;
            break;
    }
    return ::DefWindowProcW(h, msg, wp, lp);
}

// 还原显示模式。任何退出路径都必须走到这里 —— 用了 CDS_FULLSCREEN 改了
// 用户的分辨率却不还原，是绝对不能接受的。
static void RestoreDisplayMode() {
    if (g_modeChangedDisplay) {
        ::ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
        g_modeChangedDisplay = false;
    }
}

static BOOL WINAPI ConsoleCtrlHandler(DWORD) {
    g_quit = true;
    RestoreDisplayMode();
    return TRUE;
}

// 在全屏时找 W×H 的显示模式（挑刷新率最高的那个）
static bool FindDisplayMode(uint32_t w, uint32_t h, DEVMODEW* out) {
    DWORD best = 0; bool found = false;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; ::EnumDisplaySettingsExW(nullptr, i, &dm, 0); ++i) {
        if (dm.dmPelsWidth == w && dm.dmPelsHeight == h && dm.dmBitsPerPel >= 32) {
            if (!found || dm.dmDisplayFrequency > best) {
                best = dm.dmDisplayFrequency; *out = dm; found = true;
            }
        }
    }
    return found;
}

struct WindowSetup {
    bool ok = false;
    uint32_t clientW = 0, clientH = 0;
    std::string note;
};

// 按模式设置窗口样式 / 位置 / 显示模式
static WindowSetup ApplyWindowMode(HWND hwnd, int mode, uint32_t wantW, uint32_t wantH) {
    WindowSetup r;
    HMONITOR mon = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    ::GetMonitorInfoW(mon, &mi);
    const RECT& mr = mi.rcMonitor;
    uint32_t monW = (uint32_t)(mr.right - mr.left);
    uint32_t monH = (uint32_t)(mr.bottom - mr.top);

    DWORD style;
    RECT  rect;

    if (mode == 2) {   // ---- fullscreen
        DEVMODEW dm{};
        if (FindDisplayMode(wantW, wantH, &dm)) {
            LONG code = ::ChangeDisplaySettingsExW(nullptr, &dm, nullptr, CDS_FULLSCREEN, nullptr);
            if (code == DISP_CHANGE_SUCCESSFUL) {
                g_modeChangedDisplay = true;
                r.note = "已切换到独占显示模式（CDS_FULLSCREEN）";
                monW = wantW; monH = wantH;
                // 切了模式之后显示器信息可能变了，重新取一次
                ::GetMonitorInfoW(mon, &mi);
                monW = (uint32_t)(mi.rcMonitor.right - mi.rcMonitor.left);
                monH = (uint32_t)(mi.rcMonitor.bottom - mi.rcMonitor.top);
            } else {
                char b[128]; sprintf(b, "ChangeDisplaySettingsEx 失败 code=%ld，回退为无边框全屏", code);
                r.note = b;
            }
        } else {
            char b[128]; sprintf(b, "显示器上没有 %ux%u 的显示模式，回退为无边框全屏", wantW, wantH);
            r.note = b;
        }
        style = WS_POPUP | WS_VISIBLE;
        rect  = mi.rcMonitor;
    } else if (mode == 1) {   // ---- borderless
        RestoreDisplayMode();
        style = WS_POPUP | WS_VISIBLE;
        rect  = mi.rcMonitor;
        r.note = "无边框全屏（WS_POPUP 覆盖整个显示器，未改显示模式）";
    } else {                   // ---- windowed
        RestoreDisplayMode();
        style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
        rect.left = 0; rect.top = 0;
        rect.right = (LONG)wantW; rect.bottom = (LONG)wantH;
        ::AdjustWindowRectEx(&rect, style, FALSE, 0);
        LONG w = rect.right - rect.left, h = rect.bottom - rect.top;
        rect.left = mr.left + ((LONG)monW - w) / 2;
        rect.top  = mr.top + 80;
        rect.right = rect.left + w;
        rect.bottom = rect.top + h;
        r.note = "普通窗口";
    }

    ::SetWindowLongPtrW(hwnd, GWL_STYLE, (LONG_PTR)style);
    ::SetWindowPos(hwnd, HWND_TOP, rect.left, rect.top,
                   rect.right - rect.left, rect.bottom - rect.top,
                   SWP_FRAMECHANGED | SWP_SHOWWINDOW);

    RECT cr{};
    ::GetClientRect(hwnd, &cr);
    r.clientW = (uint32_t)(cr.right - cr.left);
    r.clientH = (uint32_t)(cr.bottom - cr.top);
    if (r.clientW < 1) r.clientW = 1;
    if (r.clientH < 1) r.clientH = 1;
    r.ok = true;
    return r;
}

static bool CreateSimWindow(const Options& o, const wchar_t* title, WindowSetup* setup) {
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = ::GetModuleHandleW(nullptr);
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"NextPerfSimVkWindow";
    if (!::RegisterClassExW(&wc)) {
        DWORD e = ::GetLastError();
        if (e != ERROR_CLASS_ALREADY_EXISTS) { Info("[错误] RegisterClassExW 失败: %lu", e); return false; }
    }

    // 先按 windowed 创建，再用 ApplyWindowMode 调整到目标模式。
    // 之所以不直接 CreateWindowEx 成目标样式：fullscreen 需要先有 HWND
    // 才能 MonitorFromWindow 找显示器。
    RECT rect{ 0, 0, (LONG)o.width, (LONG)o.height };
    ::AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0);

    g_hwnd = ::CreateWindowExW(0, L"NextPerfSimVkWindow", title,
                               WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               CW_USEDEFAULT, CW_USEDEFAULT,
                               rect.right - rect.left, rect.bottom - rect.top,
                               nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    if (!g_hwnd) {
        Info("[错误] CreateWindowExW 失败: %lu", ::GetLastError());
        return false;
    }
    *setup = ApplyWindowMode(g_hwnd, o.windowMode, o.width, o.height);
    return setup->ok;
}

// ============================================================================
// 5. 渲染器
// ============================================================================

static const uint32_t kFramesInFlight = 2;

struct Vertex { float x, y; float r, g, b; };   // stride = 20 字节

struct FrameSlot {
    VkCommandBuffer cmd        = VK_NULL_HANDLE;
    VkSemaphore     imageAvail = VK_NULL_HANDLE;   // acquire → submit
    VkFence         inFlight   = VK_NULL_HANDLE;
    VkBuffer        vbo        = VK_NULL_HANDLE;
    VkDeviceMemory  vboMem     = VK_NULL_HANDLE;
    Vertex*         vboMapped  = nullptr;          // 常驻映射（HOST_COHERENT，不用 flush）
};

// 每帧的原始测量结果。JSON 行不是当场吐出来的 —— 要等 GPU 时间戳可读，
// 也就是这一槽位被下一轮用到的时候（见 RenderLoop 里的说明）。
struct FrameRecord {
    bool     valid = false;
    uint32_t id = 0;
    double   dtMs = 0;          // 帧周期（上一帧起点 → 本帧起点）
    double   waitMs = 0;        // 等本槽位上一轮提交完成（vkWaitForFences）
    double   acquireMs = 0;     // vkAcquireNextImageKHR
    double   cpuMs = 0;         // 纯 CPU 干活时间（上传顶点 + 录制 + 提交）
    double   submitMs = 0;      // 其中 vkQueueSubmit 那一小段
    double   presentMs = 0;     // vkQueuePresentKHR 进到返回
    double   gpuMs = -1;        // 本帧 GPU 执行时间（时间戳查询）
    uint32_t imageIndex = 0;
    uint32_t w = 0, h = 0;
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    uint32_t fpsCap = 0;
    int      windowMode = 0;
    bool     swapRecreated = false;
    bool     warmup = false;    // 落在预热期，只输出不计入汇总统计
};

// 预热期帧数。为什么要丢弃开头这些帧：
//   驱动要编译管线、填 shader cache，交换链刚建好时 3 张图像都是空闲的
//   （垂直同步还没开始节流），头几帧的 dt 会低到 0.4ms —— 直接算进统计
//   会把 P99.9 和 1% Low 拉得一塌糊涂（实测 1% Low 从 58 掉到 30）。
//   所有正经的 benchmark 工具都会丢弃预热帧，这里也丢，并且**把丢了多少
//   明确报出来**，不偷偷摸摸。
static uint32_t WarmupFrameCount(uint32_t totalFrames) {
    uint32_t w = totalFrames / 5;      // 最多丢 20%
    if (w > 30) w = 30;                // 也最多丢 30 帧
    return w;
}
// 单帧 JSON 里的 warmup 标记用这个上界（跑完之前不知道总帧数，先按上界标）
static const uint32_t kMaxWarmupFrames = 30;

struct Stats {
    std::vector<double> frameMs;    // 帧间隔
    std::vector<double> fps;        // 1000/帧间隔
    std::vector<double> cpuMs;
    std::vector<double> gpuMs;
    std::vector<double> presentMs;
    std::vector<double> waitMs;
    std::vector<double> acquireMs;
    uint32_t frames = 0;
    double   wallSeconds = 0;
    static const size_t CAP = 400000;   // 约 2 小时 @60fps，够用且不会吃爆内存
};

// 取 [from, end) 的子区间（用于剔除预热帧）。用下标而不是拷贝迭代器，
// 是因为下面所有统计函数都吃 vector<double>。
static std::vector<double> TailFrom(const std::vector<double>& v, size_t from) {
    if (from >= v.size()) return std::vector<double>();
    return std::vector<double>(v.begin() + (ptrdiff_t)from, v.end());
}

struct Renderer {
    VkApi         api;
    VkInstance    instance   = VK_NULL_HANDLE;
    VkSurfaceKHR  surface    = VK_NULL_HANDLE;
    VkPhysicalDevice phys    = VK_NULL_HANDLE;
    VkDevice      device     = VK_NULL_HANDLE;
    VkQueue       queue      = VK_NULL_HANDLE;
    uint32_t      queueFamily = 0;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat       format    = VK_FORMAT_B8G8R8A8_UNORM;
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D     extent{};
    uint32_t       imageCount = 0;
    std::vector<VkImage>     images;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> framebuffers;
    // renderFinished 信号量按**交换链图像**分配，不是按 frames-in-flight。
    // 这是 Vulkan 里出了名的坑：present 引擎等待信号量是绑定到具体图像的，
    // 如果按 slot 分配，两个 slot 可能阻塞在同一张图像的 present 上。
    std::vector<VkSemaphore> renderFinished;

    VkRenderPass     renderPass     = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline       pipeline       = VK_NULL_HANDLE;
    VkShaderModule   vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkCommandPool    cmdPool  = VK_NULL_HANDLE;
    VkQueryPool      queryPool = VK_NULL_HANDLE;

    FrameSlot  slots[kFramesInFlight];
    FrameRecord pending[kFramesInFlight];

    bool     gpuTimingOk = false;
    float    timestampPeriod = 1.0f;    // 纳秒/tick
    uint32_t timestampValidBits = 0;
    std::string gpuTimingNote;

    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    std::vector<VkPresentModeKHR> presentModes;

    // resize 命令在无边框/全屏模式下没法改窗口大小（窗口占满显示器），
    // 只能改交换链图像尺寸。非 0 时优先用它，而不是 surface 报的 currentExtent。
    // Vulkan 允许交换链尺寸 != currentExtent，present 引擎会做拉伸。
    VkExtent2D requestedExtent{ 0, 0 };

    std::string deviceName;
    uint32_t    apiVersion = 0, driverVersion = 0, vendorID = 0;

    // ---------- 创建/销毁 ----------
    bool CreateInstanceAndDevice(const Options& o, bool vsync);
    bool CreateSurface(HWND hwnd);
    bool PickPhysicalDevice();
    bool FindQueueFamily();
    bool CreateLogicalDevice();
    bool CreateSwapchain(uint32_t w, uint32_t h, bool vsync, VkSwapchainKHR old);
    void DestroySwapchain();
    bool CreateRenderPass();
    bool CreatePipelineObjects();
    bool CreateCommandAndSync();
    bool CreateVertexBuffers();
    bool CreateQueryPool();
    void DestroyAll();

    // ---------- 每帧 ----------
    bool RecordFrame(uint32_t slot, uint32_t imageIndex, uint32_t frameId, float aspect);
    void WriteVertices(uint32_t slot, uint32_t frameId);
    double ReadGpuMs(uint32_t slot);
};

// ---------------------------------------------------------------- instance
static const char* kInstanceExts[] = { "VK_KHR_surface", "VK_KHR_win32_surface" };
static const char* kDeviceExts[]   = { "VK_KHR_swapchain" };

bool Renderer::CreateInstanceAndDevice(const Options& o, bool /*vsync*/) {
    (void)o;   // 目前不需要从参数里取东西，保留签名是为了以后加 --api-version 之类的开关
    if (!api.LoadDll()) {
        Info("[致命] 无法加载 vulkan-1.dll。本机没有 Vulkan 运行时。");
        return false;
    }

    // apiVersion 的选择：默认 1.1。原因有两个：
    //   * 本机装的几个第三方层（GamePP / MediaSDK）是 1.3 的，用 1.0 也不会报错，
    //     但用 1.4 会让 loader 打一堆 "layer uses API version 1.3 which is older
    //     than the application specified API version of 1.4" 的噪音；
    //   * 代码本身只用 1.0 核心 + VK_KHR_swapchain，1.1 是安全的下限。
    uint32_t wantVer = VK_API_VERSION_1_1;
    uint32_t loaderVer = 0;
    if (api.vkEnumerateInstanceVersion &&
        api.vkEnumerateInstanceVersion(&loaderVer) == VK_SUCCESS) {
        if (loaderVer < VK_API_VERSION_1_1) wantVer = VK_API_VERSION_1_0;
    } else {
        wantVer = VK_API_VERSION_1_0;   // 1.0 loader 不认 1.1
    }

    VkApplicationInfo ai{};
    ai.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName   = "NextPerfSimVk";
    ai.applicationVersion = 1;
    ai.pEngineName        = "simvk";
    ai.engineVersion      = 1;
    ai.apiVersion         = wantVer;

    VkInstanceCreateInfo ci{};
    ci.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo        = &ai;
    ci.enabledExtensionCount   = 2;
    ci.ppEnabledExtensionNames = kInstanceExts;
    // enabledLayerCount 故意留 0：让 loader 自己决定要启用哪些隐式层。
    // VK_INSTANCE_LAYERS 里列的层由 loader 追加，我们不用管 —— 这正是
    // 「模拟一个普通游戏」该有的样子（真实游戏不会去点名层）。

    VkResult r = api.vkCreateInstance(&ci, nullptr, &instance);
    if (r != VK_SUCCESS) {
        Info("[致命] vkCreateInstance 失败: VkResult=%d", (int)r);
        if (r == VK_ERROR_LAYER_NOT_PRESENT)
            Info("       → VK_INSTANCE_LAYERS 里点名的某个层不存在（这本身就说明环境变量生效了）。");
        else if (r == VK_ERROR_INCOMPATIBLE_DRIVER)
            Info("       → 没有可用的 Vulkan 驱动（ICD）。loader 在，但没有 GPU 驱动注册。");
        else if (r == VK_ERROR_EXTENSION_NOT_PRESENT)
            Info("       → 缺少 VK_KHR_surface / VK_KHR_win32_surface 实例扩展。");
        return false;
    }

    if (!api.LoadInstance(instance)) {
        Info("[致命] instance 级函数加载失败，第一个缺失: %s",
             api.firstMiss ? api.firstMiss : "(未知)");
        return false;
    }
    Info("[Vulkan] 实例创建成功，apiVersion=%s，启用层数=0（隐式层由 loader 自行挂载）",
         VersionStr(wantVer).c_str());
    return true;
}

bool Renderer::CreateSurface(HWND hwnd) {
    VkWin32SurfaceCreateInfoKHR sci{};
    sci.sType     = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    sci.hinstance = ::GetModuleHandleW(nullptr);
    sci.hwnd      = hwnd;
    VkResult r = api.vkCreateWin32SurfaceKHR(instance, &sci, nullptr, &surface);
    if (r != VK_SUCCESS) { Info("[致命] vkCreateWin32SurfaceKHR 失败: %d", (int)r); return false; }
    return true;
}

bool Renderer::PickPhysicalDevice() {
    uint32_t n = 0;
    VkResult r = api.vkEnumeratePhysicalDevices(instance, &n, nullptr);
    if (r != VK_SUCCESS || n == 0) {
        Info("[致命] vkEnumeratePhysicalDevices 返回 0 个设备。");
        Info("       → loader 在，但没有 ICD（GPU 驱动）注册到 HKLM\\SOFTWARE\\Khronos\\Vulkan\\Drivers。");
        return false;
    }
    std::vector<VkPhysicalDevice> devs(n);
    api.vkEnumeratePhysicalDevices(instance, &n, devs.data());

    Info("[Vulkan] 发现 %u 个物理设备：", n);
    std::vector<VkPhysicalDeviceProperties> props(n);
    int best = -1, bestScore = -1;
    for (uint32_t i = 0; i < n; ++i) {
        api.vkGetPhysicalDeviceProperties(devs[i], &props[i]);
        const VkPhysicalDeviceProperties& p = props[i];
        int score = 0;
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)   score = 300;
        else if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score = 200;
        else if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU)    score = 100;
        else score = 50;
        const char* tn = "其他";
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)   tn = "独显";
        else if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) tn = "核显";
        else if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU)    tn = "虚拟";
        else if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)            tn = "CPU";
        Info("   [%u] %-40s %s  api=%s driver=0x%08X vendor=0x%04X device=0x%04X",
             i, p.deviceName, tn, VersionStr(p.apiVersion).c_str(),
             p.driverVersion, p.vendorID, p.deviceID);
        if (score > bestScore) { bestScore = score; best = (int)i; }
    }

    phys = devs[(size_t)best];
    const VkPhysicalDeviceProperties& p = props[(size_t)best];
    deviceName    = p.deviceName;
    apiVersion    = p.apiVersion;
    driverVersion = p.driverVersion;
    vendorID      = p.vendorID;
    timestampPeriod = p.limits.timestampPeriod;
    Info("[Vulkan] 选中设备: %s", deviceName.c_str());
    Info("[Vulkan] timestampPeriod = %.6f ns/tick, timestampComputeAndGraphics = %u",
         timestampPeriod, p.limits.timestampComputeAndGraphics);
    return true;
}

bool Renderer::FindQueueFamily() {
    uint32_t n = 0;
    api.vkGetPhysicalDeviceQueueFamilyProperties(phys, &n, nullptr);
    if (!n) { Info("[致命] 没有队列族"); return false; }
    std::vector<VkQueueFamilyProperties> q(n);
    api.vkGetPhysicalDeviceQueueFamilyProperties(phys, &n, q.data());

    int chosen = -1;
    for (uint32_t i = 0; i < n; ++i) {
        VkBool32 present = VK_FALSE;
        api.vkGetPhysicalDeviceSurfaceSupportKHR(phys, i, surface, &present);
        if ((q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) { chosen = (int)i; break; }
    }
    if (chosen < 0) {
        for (uint32_t i = 0; i < n; ++i)
            if (q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { chosen = (int)i; break; }
    }
    if (chosen < 0) { Info("[致命] 找不到同时支持图形和 present 的队列族"); return false; }

    queueFamily = (uint32_t)chosen;
    timestampValidBits = q[queueFamily].timestampValidBits;
    Info("[Vulkan] 队列族 #%u: flags=0x%X queueCount=%u timestampValidBits=%u",
         queueFamily, q[queueFamily].queueFlags, q[queueFamily].queueCount, timestampValidBits);
    return true;
}

bool Renderer::CreateLogicalDevice() {
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = queueFamily;
    qci.queueCount       = 1;
    qci.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci{};
    dci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount    = 1;
    dci.pQueueCreateInfos       = &qci;
    dci.enabledExtensionCount   = 1;
    dci.ppEnabledExtensionNames = kDeviceExts;
    dci.pEnabledFeatures        = nullptr;   // 不需要任何可选特性

    VkResult r = api.vkCreateDevice(phys, &dci, nullptr, &device);
    if (r != VK_SUCCESS) { Info("[致命] vkCreateDevice 失败: %d（缺 VK_KHR_swapchain？）", (int)r); return false; }
    if (!api.LoadDevice(device)) {
        Info("[致命] device 级函数加载失败，第一个缺失: %s", api.firstMiss ? api.firstMiss : "(未知)");
        return false;
    }
    api.vkGetDeviceQueue(device, queueFamily, 0, &queue);
    Info("[Vulkan] 逻辑设备创建成功");

    // 「我的层挂上了没」—— 这是本工具存在的首要理由，所以每次启动都打一份
    ReportDispatch(api);
    return true;
}

// ---------------------------------------------------------------- swapchain
bool Renderer::CreateSwapchain(uint32_t w, uint32_t h, bool vsync, VkSwapchainKHR old) {
    VkSurfaceCapabilitiesKHR caps{};
    VkResult r = api.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps);
    if (r != VK_SUCCESS) { Info("[错误] 取 surface capabilities 失败: %d", (int)r); return false; }

    // 格式：优先 B8G8R8A8（Windows 桌面原生），其次 R8G8B8A8；SRGB 和非 SRGB 都可接受
    uint32_t fn = 0;
    api.vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fn, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fn ? fn : 1);
    if (fn) api.vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fn, fmts.data());
    else    fmts[0] = { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };

    VkSurfaceFormatKHR pick = fmts[0];
    if (fn == 1 && fmts[0].format == VK_FORMAT_UNDEFINED) {
        pick.format     = VK_FORMAT_B8G8R8A8_UNORM;
        pick.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    } else {
        bool got = false;
        for (auto& f : fmts)
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { pick = f; got = true; break; }
        if (!got) for (auto& f : fmts) if (f.format == VK_FORMAT_B8G8R8A8_UNORM) { pick = f; got = true; break; }
        if (!got) for (auto& f : fmts) if (f.format == VK_FORMAT_R8G8B8A8_UNORM) { pick = f; got = true; break; }
        if (!got) pick = fmts[0];
    }
    format     = pick.format;
    colorSpace = pick.colorSpace;

    // 尺寸：currentExtent == 0xFFFFFFFF 表示 surface 自己决定（Wayland 之类的场景），
    // Windows 上一般会给出真实客户区大小，我们就用它。
    // 例外：resize 命令显式要求了尺寸（requestedExtent），此时优先用它并夹到合法区间。
    VkExtent2D ext{};
    auto clampExtent = [&](uint32_t w, uint32_t h) -> VkExtent2D {
        VkExtent2D e{};
        e.width  = w < caps.minImageExtent.width  ? caps.minImageExtent.width
                 : (w > caps.maxImageExtent.width  ? caps.maxImageExtent.width  : w);
        e.height = h < caps.minImageExtent.height ? caps.minImageExtent.height
                 : (h > caps.maxImageExtent.height ? caps.maxImageExtent.height : h);
        return e;
    };
    if (requestedExtent.width && requestedExtent.height) {
        VkExtent2D c = clampExtent(requestedExtent.width, requestedExtent.height);
        if (c.width != requestedExtent.width || c.height != requestedExtent.height)
            Info("[交换链] 请求尺寸 %ux%u 超出可支持范围，夹到 %ux%u",
                 requestedExtent.width, requestedExtent.height, c.width, c.height);
        ext = c;
    } else if (caps.currentExtent.width != 0xFFFFFFFFu) {
        ext = caps.currentExtent;
    } else {
        ext = clampExtent(w, h);
    }
    if (ext.width == 0 || ext.height == 0) {
        // 最小化时 currentExtent 可能是 0，此时不该建交换链
        Info("[交换链] 当前客户区为 0（窗口最小化？），跳过本次创建");
        return false;
    }
    extent = ext;

    // present mode
    uint32_t pn = 0;
    api.vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &pn, nullptr);
    presentModes.assign(pn ? pn : 1, VK_PRESENT_MODE_FIFO_KHR);
    if (pn) api.vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &pn, presentModes.data());

    VkPresentModeKHR want = VK_PRESENT_MODE_FIFO_KHR;
    if (!vsync) {
        // 关垂直同步时优先 IMMEDIATE（真正不排队），退而求其次 MAILBOX（三缓冲不阻塞）
        bool hasImm = false, hasMb = false;
        for (auto m : presentModes) { if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) hasImm = true;
                                      if (m == VK_PRESENT_MODE_MAILBOX_KHR)   hasMb  = true; }
        if (hasImm)      want = VK_PRESENT_MODE_IMMEDIATE_KHR;
        else if (hasMb)  want = VK_PRESENT_MODE_MAILBOX_KHR;
        else { want = VK_PRESENT_MODE_FIFO_KHR;
               Info("[交换链] ⚠ 驱动既没有 IMMEDIATE 也没有 MAILBOX，只能回退 FIFO（等于开垂直同步）"); }
    }
    presentMode = want;

    uint32_t minImages = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && minImages > caps.maxImageCount) minImages = caps.maxImageCount;

    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)) {
        if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)
            alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
        else if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR)
            alpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
        else alpha = VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
    }

    VkSwapchainCreateInfoKHR sci{};
    sci.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface          = surface;
    sci.minImageCount    = minImages;
    sci.imageFormat      = format;
    sci.imageColorSpace  = colorSpace;
    sci.imageExtent      = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform     = (caps.currentTransform & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                           ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
    sci.compositeAlpha   = alpha;
    sci.presentMode      = presentMode;
    sci.clipped          = VK_TRUE;
    sci.oldSwapchain     = old;

    VkSwapchainKHR sc = VK_NULL_HANDLE;
    r = api.vkCreateSwapchainKHR(device, &sci, nullptr, &sc);
    if (r != VK_SUCCESS) { Info("[错误] vkCreateSwapchainKHR 失败: %d", (int)r); return false; }

    if (old != VK_NULL_HANDLE) api.vkDestroySwapchainKHR(device, old, nullptr);
    swapchain = sc;

    api.vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr);
    images.resize(imageCount);
    api.vkGetSwapchainImagesKHR(device, swapchain, &imageCount, images.data());

    views.resize(imageCount);
    framebuffers.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; ++i) {
        VkImageViewCreateInfo vci{};
        vci.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image                           = images[i];
        vci.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        vci.format                          = format;
        vci.components.r = vci.components.g = vci.components.b = vci.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        vci.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.baseMipLevel   = 0;
        vci.subresourceRange.levelCount     = 1;
        vci.subresourceRange.baseArrayLayer = 0;
        vci.subresourceRange.layerCount     = 1;
        if (api.vkCreateImageView(device, &vci, nullptr, &views[i]) != VK_SUCCESS) {
            Info("[错误] vkCreateImageView[%u] 失败", i); return false;
        }
        VkFramebufferCreateInfo fci{};
        fci.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass      = renderPass;
        fci.attachmentCount = 1;
        fci.pAttachments    = &views[i];
        fci.width           = extent.width;
        fci.height          = extent.height;
        fci.layers          = 1;
        if (api.vkCreateFramebuffer(device, &fci, nullptr, &framebuffers[i]) != VK_SUCCESS) {
            Info("[错误] vkCreateFramebuffer[%u] 失败", i); return false;
        }
    }

    // 每个交换链图像一个 renderFinished 信号量（见结构体里的说明）
    renderFinished.assign(imageCount, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < imageCount; ++i) {
        VkSemaphoreCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (api.vkCreateSemaphore(device, &si, nullptr, &renderFinished[i]) != VK_SUCCESS) {
            Info("[错误] vkCreateSemaphore(renderFinished[%u]) 失败", i); return false;
        }
    }

    Info("[交换链] %ux%u  format=%d  images=%u  presentMode=%s  可用的 present modes=%zu",
         extent.width, extent.height, (int)format, imageCount, PresentModeName(presentMode),
         presentModes.size());
    return true;
}

void Renderer::DestroySwapchain() {
    if (device == VK_NULL_HANDLE) return;
    for (auto s : renderFinished) if (s) api.vkDestroySemaphore(device, s, nullptr);
    renderFinished.clear();
    for (auto f : framebuffers) if (f) api.vkDestroyFramebuffer(device, f, nullptr);
    framebuffers.clear();
    for (auto v : views) if (v) api.vkDestroyImageView(device, v, nullptr);
    views.clear();
    images.clear();
    if (swapchain) { api.vkDestroySwapchainKHR(device, swapchain, nullptr); swapchain = VK_NULL_HANDLE; }
    imageCount = 0;
}

// ---------------------------------------------------------------- render pass
bool Renderer::CreateRenderPass() {
    VkAttachmentDescription color{};
    color.format         = format;
    color.samples        = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    // UNDEFINED → PRESENT_SRC_KHR：这样就不用自己做 layout 转换 barrier 了，
    // 渲染通道会隐式完成转换。缺点是丢掉了这一帧之前的内容 —— 我们本来就要清屏。
    color.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

    VkSubpassDescription sub{};
    sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments    = &ref;

    // 外部依赖：保证图像布局转换不会早于 COLOR_ATTACHMENT_OUTPUT 阶段开始，
    // 否则会和上一帧的 present 抢同一张图像。
    VkSubpassDependency dep{};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp{};
    rp.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = 1;
    rp.pAttachments    = &color;
    rp.subpassCount    = 1;
    rp.pSubpasses      = &sub;
    rp.dependencyCount = 1;
    rp.pDependencies   = &dep;

    VkResult r = api.vkCreateRenderPass(device, &rp, nullptr, &renderPass);
    if (r != VK_SUCCESS) { Info("[错误] vkCreateRenderPass 失败: %d", (int)r); return false; }
    return true;
}

bool Renderer::CreatePipelineObjects() {
    // ---- 着色器模块（SPIR-V 已经过自检）
    std::string err;
    if (!SpirvSelfCheck(kVertSpv, sizeof(kVertSpv) / 4, "vertex", &err)) {
        Info("[致命] 顶点着色器 SPIR-V 自检失败: %s", err.c_str()); return false;
    }
    if (!SpirvSelfCheck(kFragSpv, sizeof(kFragSpv) / 4, "fragment", &err)) {
        Info("[致命] 片元着色器 SPIR-V 自检失败: %s", err.c_str()); return false;
    }
    Info("[着色器] SPIR-V 自检通过（vertex %zu 字，fragment %zu 字）",
         sizeof(kVertSpv) / 4, sizeof(kFragSpv) / 4);

    VkShaderModuleCreateInfo smi{};
    smi.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = sizeof(kVertSpv);
    smi.pCode    = kVertSpv;
    if (api.vkCreateShaderModule(device, &smi, nullptr, &vs) != VK_SUCCESS) {
        Info("[致命] vkCreateShaderModule(vertex) 失败 —— SPIR-V 没通过驱动校验"); return false;
    }
    smi.codeSize = sizeof(kFragSpv);
    smi.pCode    = kFragSpv;
    if (api.vkCreateShaderModule(device, &smi, nullptr, &fs) != VK_SUCCESS) {
        Info("[致命] vkCreateShaderModule(fragment) 失败 —— SPIR-V 没通过驱动校验"); return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName  = "main";

    // ---- 顶点输入：location0 = vec2 位置，location1 = vec3 颜色
    VkVertexInputBindingDescription bind{ 0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attr[2]{};
    attr[0].location = 0; attr[0].binding = 0;
    attr[0].format   = VK_FORMAT_R32G32_SFLOAT;    attr[0].offset = 0;
    attr[1].location = 1; attr[1].binding = 0;
    attr[1].format   = VK_FORMAT_R32G32B32_SFLOAT; attr[1].offset = 8;

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount   = 1;
    vi.pVertexBindingDescriptions      = &bind;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions    = attr;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;      // 双面都画，旋转时不会突然消失
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable    = VK_FALSE;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments    = &cba;

    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds{};
    ds.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    ds.dynamicStateCount = 2;
    ds.pDynamicStates    = dyn;

    // ---- push constant：vec4 rot
    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pcr.offset     = 0;
    pcr.size       = 16;   // vec4

    VkPipelineLayoutCreateInfo pli{};
    pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount         = 0;      // 不用描述符集，靶子越简单越好
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges    = &pcr;
    if (api.vkCreatePipelineLayout(device, &pli, nullptr, &pipelineLayout) != VK_SUCCESS) {
        Info("[致命] vkCreatePipelineLayout 失败"); return false;
    }

    VkGraphicsPipelineCreateInfo gp{};
    gp.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount          = 2;
    gp.pStages             = stages;
    gp.pVertexInputState   = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState      = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState   = &ms;
    gp.pColorBlendState    = &cb;
    gp.pDynamicState       = &ds;
    gp.layout              = pipelineLayout;
    gp.renderPass          = renderPass;
    gp.subpass             = 0;
    gp.basePipelineIndex   = -1;

    VkResult r = api.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline);
    if (r != VK_SUCCESS) {
        Info("[致命] vkCreateGraphicsPipelines 失败: %d", (int)r);
        Info("       → 如果是 -3 (INITIALIZATION_FAILED)，多半是手工汇编的 SPIR-V 被驱动拒了。");
        return false;
    }
    Info("[管线] 图形管线创建成功（这就是「驱动认了我们的 SPIR-V」的证明）");
    return true;
}

bool Renderer::CreateCommandAndSync() {
    VkCommandPoolCreateInfo pci{};
    pci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queueFamily;
    if (api.vkCreateCommandPool(device, &pci, nullptr, &cmdPool) != VK_SUCCESS) {
        Info("[致命] vkCreateCommandPool 失败"); return false;
    }

    VkCommandBufferAllocateInfo ai{};
    ai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool        = cmdPool;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = kFramesInFlight;
    VkCommandBuffer cmds[kFramesInFlight]{};
    if (api.vkAllocateCommandBuffers(device, &ai, cmds) != VK_SUCCESS) {
        Info("[致命] vkAllocateCommandBuffers 失败"); return false;
    }

    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        slots[i].cmd = cmds[i];

        VkSemaphoreCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (api.vkCreateSemaphore(device, &si, nullptr, &slots[i].imageAvail) != VK_SUCCESS) {
            Info("[致命] vkCreateSemaphore 失败"); return false;
        }

        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        // 初始就给 SIGNALED：这样第一帧的 vkWaitForFences 会立刻返回，
        // 不用为「首次使用」写特例。
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (api.vkCreateFence(device, &fi, nullptr, &slots[i].inFlight) != VK_SUCCESS) {
            Info("[致命] vkCreateFence 失败"); return false;
        }
    }
    return true;
}

bool Renderer::CreateVertexBuffers() {
    // 找一个 HOST_VISIBLE | HOST_COHERENT 的显存类型。
    // 用常驻映射（persistent map）而不是每帧 map/unmap：省掉每帧一次
    // 内核态往返，这是 Vulkan 里上传动态顶点数据的常规做法。
    // 因为是 COHERENT，写完不需要 vkFlushMappedMemoryRanges。
    VkPhysicalDeviceMemoryProperties mp{};
    api.vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    int typeIdx = -1;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        VkFlags f = mp.memoryTypes[i].propertyFlags;
        if ((f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { typeIdx = (int)i; break; }
    }
    if (typeIdx < 0) { Info("[错误] 找不到 HOST_VISIBLE|HOST_COHERENT 的显存类型"); return false; }
    Info("[显存] 使用 memoryType #%d (flags=0x%X)", typeIdx, mp.memoryTypes[typeIdx].propertyFlags);

    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        VkBufferCreateInfo bci{};
        bci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size        = sizeof(Vertex) * 3;
        bci.usage       = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (api.vkCreateBuffer(device, &bci, nullptr, &slots[i].vbo) != VK_SUCCESS) {
            Info("[错误] vkCreateBuffer[%u] 失败", i); return false;
        }
        VkMemoryRequirements mr{};
        api.vkGetBufferMemoryRequirements(device, slots[i].vbo, &mr);

        VkMemoryAllocateInfo mai{};
        mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize  = mr.size;
        mai.memoryTypeIndex = (uint32_t)typeIdx;
        if (api.vkAllocateMemory(device, &mai, nullptr, &slots[i].vboMem) != VK_SUCCESS) {
            Info("[错误] vkAllocateMemory[%u] 失败", i); return false;
        }
        if (api.vkBindBufferMemory(device, slots[i].vbo, slots[i].vboMem, 0) != VK_SUCCESS) {
            Info("[错误] vkBindBufferMemory[%u] 失败", i); return false;
        }
        void* p = nullptr;
        if (api.vkMapMemory(device, slots[i].vboMem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS || !p) {
            Info("[错误] vkMapMemory[%u] 失败", i); return false;
        }
        slots[i].vboMapped = (Vertex*)p;
    }
    return true;
}

bool Renderer::CreateQueryPool() {
    if (timestampValidBits == 0) {
        gpuTimingOk = false;
        gpuTimingNote = "该队列族的 timestampValidBits == 0，驱动不支持时间戳查询";
        Info("[GPU 计时] %s —— gpu_ms 将恒为 -1", gpuTimingNote.c_str());
        return true;   // 不是致命错误
    }
    VkQueryPoolCreateInfo qp{};
    qp.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    qp.queryType  = VK_QUERY_TYPE_TIMESTAMP;
    qp.queryCount = kFramesInFlight * 2;    // 每个槽位两个：帧首 + 帧尾
    VkResult r = api.vkCreateQueryPool(device, &qp, nullptr, &queryPool);
    if (r != VK_SUCCESS) {
        gpuTimingOk = false;
        gpuTimingNote = "vkCreateQueryPool(TIMESTAMP) 失败";
        Info("[GPU 计时] %s (VkResult=%d) —— gpu_ms 将恒为 -1", gpuTimingNote.c_str(), (int)r);
        return true;
    }
    gpuTimingOk = true;
    return true;
}

void Renderer::DestroyAll() {
    if (device) {
        api.vkDeviceWaitIdle(device);
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            if (slots[i].vboMapped && slots[i].vboMem)
                api.vkUnmapMemory(device, slots[i].vboMem);
            slots[i].vboMapped = nullptr;
            if (slots[i].vbo)     api.vkDestroyBuffer(device, slots[i].vbo, nullptr);
            if (slots[i].vboMem)  api.vkFreeMemory(device, slots[i].vboMem, nullptr);
            if (slots[i].imageAvail) api.vkDestroySemaphore(device, slots[i].imageAvail, nullptr);
            if (slots[i].inFlight)   api.vkDestroyFence(device, slots[i].inFlight, nullptr);
        }
        if (queryPool)      api.vkDestroyQueryPool(device, queryPool, nullptr);
        if (cmdPool)        api.vkDestroyCommandPool(device, cmdPool, nullptr);
        if (pipeline)       api.vkDestroyPipeline(device, pipeline, nullptr);
        if (pipelineLayout) api.vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        if (vs)             api.vkDestroyShaderModule(device, vs, nullptr);
        if (fs)             api.vkDestroyShaderModule(device, fs, nullptr);
        if (renderPass)     api.vkDestroyRenderPass(device, renderPass, nullptr);
    }
    DestroySwapchain();
    if (device)   { api.vkDestroyDevice(device, nullptr); device = VK_NULL_HANDLE; }
    if (surface)  { api.vkDestroySurfaceKHR(instance, surface, nullptr); surface = VK_NULL_HANDLE; }
    if (instance) { api.vkDestroyInstance(instance, nullptr); instance = VK_NULL_HANDLE; }
    api.Unload();
}

// ---------------------------------------------------------------- 每帧数据
// 顶点数据每帧由 CPU 重新写一遍 —— 这是故意的：真实游戏每帧都会更新
// 常量缓冲/顶点数据，让这个靶子也这么干，才能测出「层在 Present 时
// 抓到的 CPU 帧时间」到底包不包含上传开销。
// 三个顶点绕原点脉动，颜色随时间轮转，保证画面肉眼可见地在变。
void Renderer::WriteVertices(uint32_t slot, uint32_t frameId) {
    Vertex* v = slots[slot].vboMapped;
    if (!v) return;
    float pulse = 0.62f + 0.22f * (float)sin((double)frameId * 0.045);
    float hue   = (float)((double)frameId * 0.010);
    // 基三角形（一个朝上的等腰三角形）
    const float base[3][2] = { { 0.00f,  0.70f }, { -0.62f, -0.42f }, { 0.62f, -0.42f } };
    const float col[3][3]  = { { 1.0f, 0.25f, 0.25f }, { 0.25f, 1.0f, 0.35f }, { 0.30f, 0.45f, 1.0f } };
    for (int i = 0; i < 3; ++i) {
        v[i].x = base[i][0] * pulse;
        v[i].y = base[i][1] * pulse;
        // 让颜色缓慢呼吸，避免画面看起来是静止的
        float m = 0.75f + 0.25f * (float)sin((double)frameId * 0.03 + (double)i + (double)hue * 6.0);
        v[i].r = col[i][0] * m;
        v[i].g = col[i][1] * m;
        v[i].b = col[i][2] * m;
    }
}

// 读取某个槽位的 GPU 帧时间。
// 必须在这一槽位的 fence 已经被等到（= 上一轮提交已完成）之后调用，
// 否则 vkGetQueryPoolResults 会阻塞或返回无意义的值。
double Renderer::ReadGpuMs(uint32_t slot) {
    if (!gpuTimingOk || queryPool == VK_NULL_HANDLE) return -1.0;
    uint64_t data[2] = { 0, 0 };
    // WAIT_BIT：fence 已经保证完成，这里是立刻返回的（加它是为了防万一）
    VkResult r = api.vkGetQueryPoolResults(
        device, queryPool, slot * 2, 2, sizeof(data), data, sizeof(uint64_t),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    if (r != VK_SUCCESS) return -1.0;

    uint64_t t0 = data[0], t1 = data[1];
    // timestampValidBits < 64 的队列族只保证低位有效，高位是垃圾，必须掩掉。
    // （mask 计算要小心：bits==64 时 1ull<<64 是 UB）
    if (timestampValidBits < 64) {
        uint64_t mask = (timestampValidBits == 0) ? 0ull : ((1ull << timestampValidBits) - 1ull);
        t0 &= mask; t1 &= mask;
    }
    // 时间戳计数器回绕的极端情况：t1 < t0 说明跨了回绕点，这一帧读数作废
    if (t1 < t0) return -1.0;
    return (double)(t1 - t0) * (double)timestampPeriod / 1e6;   // ns → ms
}

// ---------------------------------------------------------------- 录制
bool Renderer::RecordFrame(uint32_t slot, uint32_t imageIndex, uint32_t frameId, float aspect) {
    VkCommandBuffer cmd = slots[slot].cmd;
    api.vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (api.vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) return false;

    // ---- 时间戳 0：帧首（TOP_OF_PIPE = 命令刚进管线）
    // 注意：vkCmdResetQueryPool / vkCmdWriteTimestamp 都必须**在 render pass 之外**调用
    // （Vulkan 1.0~1.2 的硬性规定），所以两个时间戳都放在渲染通道前后。
    if (gpuTimingOk && queryPool) {
        api.vkCmdResetQueryPool(cmd, queryPool, slot * 2, 2);
        api.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool, slot * 2);
    }

    VkClearValue clear{};
    // 背景色也跟着帧号缓慢变化，这样即使三角形出问题，也能一眼看出还在出帧
    clear.float32[0] = 0.06f + 0.05f * (float)sin((double)frameId * 0.013);
    clear.float32[1] = 0.07f + 0.05f * (float)sin((double)frameId * 0.017 + 2.0);
    clear.float32[2] = 0.11f + 0.06f * (float)sin((double)frameId * 0.011 + 4.0);
    clear.float32[3] = 1.0f;

    VkRenderPassBeginInfo rbi{};
    rbi.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass      = renderPass;
    rbi.framebuffer     = framebuffers[imageIndex];
    rbi.renderArea.offset.x = 0;
    rbi.renderArea.offset.y = 0;
    rbi.renderArea.extent   = extent;
    rbi.clearValueCount = 1;
    rbi.pClearValues    = &clear;

    api.vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{ 0.0f, 0.0f, (float)extent.width, (float)extent.height, 0.0f, 1.0f };
    VkRect2D   sc{ { 0, 0 }, extent };
    api.vkCmdSetViewport(cmd, 0, 1, &vp);
    api.vkCmdSetScissor(cmd, 0, 1, &sc);

    api.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    VkDeviceSize off = 0;
    api.vkCmdBindVertexBuffers(cmd, 0, 1, &slots[slot].vbo, &off);

    // push constant：xy = cos/sin(旋转角)，zw = 1/aspect 与 1（防止宽屏拉伸）
    float pc[4];
    double ang = (double)frameId * 0.02;
    pc[0] = (float)cos(ang);
    pc[1] = (float)sin(ang);
    pc[2] = (aspect > 0.0f) ? (1.0f / aspect) : 1.0f;
    pc[3] = 1.0f;
    api.vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, pc);

    api.vkCmdDraw(cmd, 3, 1, 0, 0);
    api.vkCmdEndRenderPass(cmd);

    // ---- 时间戳 1：帧尾（BOTTOM_OF_PIPE = 前面所有工作都完成）
    if (gpuTimingOk && queryPool) {
        api.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, slot * 2 + 1);
    }

    return api.vkEndCommandBuffer(cmd) == VK_SUCCESS;
}

// ============================================================================
// 6. 统计
// ============================================================================

// 线性插值百分位。定义写清楚，免得和 PresentMon 的算法对不上时互相怀疑：
//   idx = p * (n-1)，在 floor(idx) 和 ceil(idx) 之间线性插值。
static double Percentile(const std::vector<double>& sortedAsc, double p) {
    size_t n = sortedAsc.size();
    if (!n) return 0.0;
    if (n == 1) return sortedAsc[0];
    double idx = p * (double)(n - 1);
    size_t i0 = (size_t)idx;
    if (i0 >= n - 1) return sortedAsc[n - 1];
    double frac = idx - (double)i0;
    return sortedAsc[i0] * (1.0 - frac) + sortedAsc[i0 + 1] * frac;
}

// PresentMon 口径的 low：把最慢的 p 比例帧的**帧时间**取平均，再换算成 FPS。
// 和上面「直接对 FPS 取百分位」不是一回事 —— 两个都报出来，方便和
// 别的工具对数据时判断差异来自算法还是来自采集。
static double LowFpsAvgWorst(const std::vector<double>& frameMsDesc, double p) {
    size_t n = frameMsDesc.size();
    if (!n) return 0.0;
    size_t k = (size_t)ceil(p * (double)n);
    if (k < 1) k = 1;
    if (k > n) k = n;
    double sum = 0;
    for (size_t i = 0; i < k; ++i) sum += frameMsDesc[i];
    double avg = sum / (double)k;
    return avg > 0 ? 1000.0 / avg : 0.0;
}

// ============================================================================
// 7. stdin 命令
// ============================================================================

enum CmdKind { CMD_NONE, CMD_RESIZE, CMD_MODE, CMD_VSYNC, CMD_FPSCAP, CMD_QUIT };

struct Command {
    CmdKind  kind = CMD_NONE;
    uint32_t a = 0, b = 0;
    bool     flag = false;
};

static CRITICAL_SECTION g_cmdLock;
static std::vector<Command> g_cmdQueue;

static DWORD WINAPI StdinThread(LPVOID) {
    HANDLE h = ::GetStdHandle(STD_INPUT_HANDLE);
    if (!h || h == INVALID_HANDLE_VALUE) return 0;

    // stdin 的类型决定了「读到 EOF」该怎么理解 —— 这一步不做的话会踩坑：
    //   * 管道 (FILE_TYPE_PIPE)：驱动方 process.stdin.close() 会让我们读到 EOF，
    //     这时候**应该**退出，否则管道关了程序还在傻跑。
    //   * 磁盘文件 (FILE_TYPE_DISK)：`simvk.exe < cmds.txt` 读完就是 EOF，
    //     但那是"命令读完了"，不是"让我退出"，不应该退出。
    //   * 控制台 (FILE_TYPE_CHAR)：ReadFile 会一直阻塞等输入，不会立刻 EOF。
    //   * 无效/未知：干脆不启动命令处理。
    // （最初的版本把任何 EOF 都当 quit，结果从某些启动器运行时 stdin 天生就是
    //   EOF，程序 0 帧就退出了。）
    DWORD ft = ::GetFileType(h);
    bool eofMeansQuit = (ft == FILE_TYPE_PIPE);

    std::string buf;
    char chunk[512];
    for (;;) {
        DWORD got = 0;
        // 用 ReadFile 而不是 std::getline：管道输入时 std::cin 的缓冲行为和
        // 控制台不一样，而我们要的就是「来一行处理一行」，ReadFile 最直观。
        if (!::ReadFile(h, chunk, sizeof(chunk), &got, nullptr) || got == 0) {
            if (eofMeansQuit) {
                ::EnterCriticalSection(&g_cmdLock);
                Command c; c.kind = CMD_QUIT;
                g_cmdQueue.push_back(c);
                ::LeaveCriticalSection(&g_cmdLock);
            }
            return 0;
        }
        buf.append(chunk, got);

        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.empty()) continue;

            Command c;
            char kw[32] = "";
            unsigned x = 0, y = 0;
            char val[32] = "";
            if (sscanf(line.c_str(), "%31s", kw) != 1) continue;

            if (_stricmp(kw, "quit") == 0 || _stricmp(kw, "exit") == 0) {
                c.kind = CMD_QUIT;
            } else if (_stricmp(kw, "resize") == 0 && sscanf(line.c_str(), "%*s %u %u", &x, &y) == 2) {
                c.kind = CMD_RESIZE; c.a = x; c.b = y;
            } else if (_stricmp(kw, "mode") == 0 && sscanf(line.c_str(), "%*s %31s", val) == 1) {
                c.kind = CMD_MODE;
                if      (_stricmp(val, "windowed") == 0)   c.a = 0;
                else if (_stricmp(val, "borderless") == 0) c.a = 1;
                else if (_stricmp(val, "fullscreen") == 0) c.a = 2;
                else { Info("[stdin] 未知窗口模式 '%s'（可选 windowed|borderless|fullscreen）", val); continue; }
            } else if (_stricmp(kw, "vsync") == 0 && sscanf(line.c_str(), "%*s %31s", val) == 1) {
                c.kind = CMD_VSYNC;
                if      (_stricmp(val, "on") == 0)  c.flag = true;
                else if (_stricmp(val, "off") == 0) c.flag = false;
                else { Info("[stdin] vsync 只接受 on|off"); continue; }
            } else if ((_stricmp(kw, "fpscap") == 0 || _stricmp(kw, "fps-cap") == 0) &&
                       sscanf(line.c_str(), "%*s %u", &x) == 1) {
                c.kind = CMD_FPSCAP; c.a = x;
            } else {
                Info("[stdin] 无法识别的命令: %s", line.c_str());
                continue;
            }
            ::EnterCriticalSection(&g_cmdLock);
            g_cmdQueue.push_back(c);
            ::LeaveCriticalSection(&g_cmdLock);
        }
    }
}

// ============================================================================
// 8. 主循环
// ============================================================================

// 锁帧：忙等 + 短睡眠混合。
//
// 为什么不用 Sleep(1) 循环到底？—— Windows 的 Sleep 粒度默认是 ~15.6ms
// （虽然我们用 timeBeginPeriod(1) 把它压到 ~1ms），光靠 Sleep 会持续超调。
// 为什么不用纯忙等？—— 会把一个核跑满，测出来的 CPU 帧时间会被自己的
// 忙等污染。所以：还剩 >2ms 时 Sleep(1)，最后 2ms 用 YieldProcessor 空转。
static void PaceUntil(double targetMs) {
    for (;;) {
        double now = QpcMs();
        double left = targetMs - now;
        if (left <= 0.0) break;
        if (left > 2.0) ::Sleep(1);
        else { ::SwitchToThread(); ::YieldProcessor(); }
    }
}

struct Emitter {
    // 把一帧的完整数据打成一行 JSON。
    // 注意：gpu_ms 是这一帧真正在 GPU 上跑的时间。为了拿到它，我们必须等
    // 这一槽位的 fence 被 signal（= 提交完成），而那要等到 kFramesInFlight
    // 帧之后。所以 JSON 行虽然**顺序正确、一帧不落**，但**滞后
    // kFramesInFlight 帧**才吐出来。这一点在 README 里写明了。
    //
    // 时间口径（这是本工具最该讲清楚的东西）：
    //   dt_ms = 帧周期 = wait + acquire + cpu + present + 锁帧睡眠
    //   cpu_ms 只算**真正在 CPU 上干活**的那一段（上传顶点 + 录制 + 提交），
    //         绝不包含等待。这是踩过坑才定死的：如果从帧首开始计时，
    //         开垂直同步时它会恒等于 16.67ms —— 那是帧周期，不是 CPU 的活。
    //   wait_ms 是卡在 vkWaitForFences 里的时间。**实测发现垂直同步的等待
    //         主要落在这里**（现代 WDDM 翻转模型下 vkQueuePresentKHR 几乎
    //         不阻塞），所以这一段必须单独报出来，否则 CPU 帧时间会被算错。
    static void EmitFrame(const FrameRecord& r, uint32_t framesInFlight) {
        char buf[1200];
        // dt 极小（交换链刚建好那几帧）时不要算出 10000000 这种离谱 FPS
        double fps = (r.dtMs > 0.01) ? (1000.0 / r.dtMs) : 0.0;
        char gpuField[32];
        if (r.gpuMs >= 0.0) sprintf(gpuField, "%.4f", r.gpuMs);
        else                sprintf(gpuField, "-1");
        sprintf(buf,
            "{\"type\":\"frame\",\"frame\":%u,\"lag_frames\":%u,\"warmup\":%d,"
            "\"dt_ms\":%.3f,\"fps\":%.2f,"
            "\"wait_ms\":%.3f,\"acquire_ms\":%.3f,\"cpu_ms\":%.3f,"
            "\"submit_ms\":%.3f,\"present_ms\":%.3f,\"gpu_ms\":%s,"
            "\"image\":%u,\"w\":%u,\"h\":%u,\"present_mode\":\"%s\","
            "\"fps_cap\":%u,\"window\":\"%s\",\"swap_recreated\":%d}",
            r.id, framesInFlight, r.warmup ? 1 : 0,
            r.dtMs, fps,
            r.waitMs, r.acquireMs, r.cpuMs, r.submitMs, r.presentMs, gpuField,
            r.imageIndex, r.w, r.h, PresentModeName(r.presentMode),
            r.fpsCap, WindowModeName(r.windowMode), r.swapRecreated ? 1 : 0);
        JsonRaw(buf);
    }

    static void EmitSummary(const Stats& s, const Renderer& R, const Options& o,
                            const std::vector<LayerInfo>& layers, const char* stopReason) {
        // 剔除预热帧后再统计
        uint32_t warmup = WarmupFrameCount(s.frames);
        std::vector<double> frameMs = TailFrom(s.frameMs, warmup);
        std::vector<double> fpsV    = TailFrom(s.fps, warmup);
        std::vector<double> cpuV    = TailFrom(s.cpuMs, warmup);
        std::vector<double> gpuV    = TailFrom(s.gpuMs, warmup);
        std::vector<double> presV   = TailFrom(s.presentMs, warmup);
        std::vector<double> waitV   = TailFrom(s.waitMs, warmup);

        std::vector<double> ftAsc = frameMs, ftDesc = frameMs;
        std::sort(ftAsc.begin(), ftAsc.end());
        std::sort(fpsV.begin(), fpsV.end());
        std::sort(ftDesc.begin(), ftDesc.end(), std::greater<double>());

        double avgFps = 0, avgFt = 0, p50 = 0, p99 = 0, p999 = 0;
        double low1 = 0, low01 = 0, low1avg = 0, low01avg = 0;
        if (!frameMs.empty()) {
            double sum = 0;
            for (double v : frameMs) sum += v;
            avgFt = sum / (double)frameMs.size();
            avgFps = avgFt > 0 ? 1000.0 / avgFt : 0.0;
            p50   = Percentile(ftAsc, 0.50);
            p99   = Percentile(ftAsc, 0.99);
            p999  = Percentile(ftAsc, 0.999);
            low1   = Percentile(fpsV, 0.01);
            low01  = Percentile(fpsV, 0.001);
            low1avg  = LowFpsAvgWorst(ftDesc, 0.01);
            low01avg = LowFpsAvgWorst(ftDesc, 0.001);
        }
        auto avgOf = [](const std::vector<double>& v) {
            if (v.empty()) return -1.0;
            double s2 = 0; for (double x : v) s2 += x;
            return s2 / (double)v.size();
        };
        double avgCpu = avgOf(cpuV), avgGpu = avgOf(gpuV),
               avgPres = avgOf(presV), avgWait = avgOf(waitV);

        // 隐式层名（NextPerf 将来的层会出现在这个列表里）
        std::string layerJson = "[";
        int nImp = 0;
        for (size_t i = 0; i < layers.size(); ++i) {
            if (layers[i].implicit) {
                if (nImp) layerJson += ",";
                layerJson += "\"" + JStr(layers[i].name.c_str()) + "\"";
                ++nImp;
            }
        }
        layerJson += "]";

        char buf[4096];
        sprintf(buf,
            "{\"type\":\"summary\",\"stop_reason\":\"%s\","
            "\"frames_total\":%u,\"warmup_excluded\":%u,\"frames_counted\":%zu,"
            "\"seconds\":%.3f,"
            "\"avg_fps\":%.2f,\"avg_frame_ms\":%.3f,"
            "\"ft_p50_ms\":%.3f,\"ft_p99_ms\":%.3f,\"ft_p999_ms\":%.3f,"
            "\"low1_fps\":%.2f,\"low01_fps\":%.2f,\"low1_avg_fps\":%.2f,\"low01_avg_fps\":%.2f,"
            "\"avg_wait_ms\":%.3f,\"avg_cpu_ms\":%.3f,\"avg_gpu_ms\":%.4f,\"avg_present_ms\":%.3f,"
            "\"gpu_timing\":%s,\"device\":\"%s\",\"api_version\":\"%s\","
            "\"w\":%u,\"h\":%u,\"present_mode\":\"%s\",\"fps_cap\":%u,\"window\":\"%s\","
            "\"vsync\":%s,\"layers_available\":%zu,\"implicit_layers\":%s}",
            stopReason,
            s.frames, warmup, frameMs.size(), s.wallSeconds,
            avgFps, avgFt,
            p50, p99, p999, low1, low01, low1avg, low01avg,
            avgWait, avgCpu, avgGpu, avgPres,
            R.gpuTimingOk ? "true" : "false",
            JStr(R.deviceName.c_str()).c_str(), VersionStr(R.apiVersion).c_str(),
            R.extent.width, R.extent.height, PresentModeName(R.presentMode), o.fpsCap,
            WindowModeName(o.windowMode), o.vsync ? "true" : "false",
            layers.size(), layerJson.c_str());
        JsonRaw(buf);
    }
};

static int RunSim(const Options& o, const std::vector<LayerInfo>& layers) {
    Renderer R;
    Options opt = o;   // 运行时可改，所以用副本

    if (!R.CreateInstanceAndDevice(opt, opt.vsync)) { R.DestroyAll(); return 2; }

    wchar_t title[256];
    swprintf(title, 256, L"NextPerf simvk — Vulkan 模拟游戏进程 [启动中]");
    WindowSetup ws{};
    if (!CreateSimWindow(opt, title, &ws)) { R.DestroyAll(); return 2; }
    Info("[窗口] %s  客户区 %ux%u  %s", WindowModeName(opt.windowMode),
         ws.clientW, ws.clientH, ws.note.c_str());

    if (!R.CreateSurface(g_hwnd))        { R.DestroyAll(); return 2; }
    if (!R.PickPhysicalDevice())         { R.DestroyAll(); return 2; }
    if (!R.FindQueueFamily())            { R.DestroyAll(); return 2; }
    if (!R.CreateLogicalDevice())        { R.DestroyAll(); return 2; }
    if (!R.CreateRenderPass())           { R.DestroyAll(); return 2; }
    if (!R.CreateSwapchain(ws.clientW, ws.clientH, opt.vsync, VK_NULL_HANDLE)) {
        Info("[致命] 首次创建交换链失败"); R.DestroyAll(); return 2;
    }
    if (!R.CreatePipelineObjects())      { R.DestroyAll(); return 2; }
    if (!R.CreateCommandAndSync())       { R.DestroyAll(); return 2; }
    if (!R.CreateVertexBuffers())        { R.DestroyAll(); return 2; }
    if (!R.CreateQueryPool())            { R.DestroyAll(); return 2; }

    // ---- 开跑
    Stats  stats;
    double wallStart = QpcMs();
    double prevFrameStart = wallStart;
    uint32_t frameId = 0;            // 已提交的帧数
    uint32_t emittedFrames = 0;      // 已输出 JSON 的帧数
    uint32_t swapRecreatedCount = 0;
    bool     needRecreate = false;
    bool     swapRecreatedThisFrame = false;

    // 为了让「帧刚开始就被最小化」这种情况不至于空转烧 CPU
    Info("[运行] 开始渲染。stdin 可用命令: resize W H | mode ... | vsync on|off | fpscap N | quit");

    // stdin 必须在**独立线程**上读：渲染循环不能阻塞在 ReadFile 上。
    // 命令通过 g_cmdQueue 交给渲染线程执行 —— 所有 Vulkan 调用都留在主线程，
    // 避免多线程访问 device 带来的同步问题。
    HANDLE stdinThread = nullptr;
    if (opt.noStdin) {
        Info("[运行] --no-stdin：不启动 stdin 命令线程");
    } else {
        HANDLE hIn = ::GetStdHandle(STD_INPUT_HANDLE);
        DWORD  ft  = (hIn && hIn != INVALID_HANDLE_VALUE) ? ::GetFileType(hIn) : FILE_TYPE_UNKNOWN;
        const char* tn = (ft == FILE_TYPE_PIPE) ? "管道（EOF 视为 quit）"
                       : (ft == FILE_TYPE_CHAR) ? "控制台"
                       : (ft == FILE_TYPE_DISK) ? "重定向文件（EOF 不退出）"
                       : "未知/无效（命令不可用）";
        Info("[运行] stdin 类型: %s", tn);
        if (ft == FILE_TYPE_UNKNOWN) {
            Info("[运行] 没有可用的 stdin，跳过命令线程（这不影响渲染）");
        } else {
            stdinThread = ::CreateThread(nullptr, 0, StdinThread, nullptr, 0, nullptr);
            if (!stdinThread) Info("[警告] 无法创建 stdin 线程，运行时命令不可用（GetLastError=%lu）", ::GetLastError());
            else Info("[运行] stdin 命令线程已启动");
        }
    }

    while (!g_quit) {
        // ---- 消息泵（必须每帧来一次，否则窗口会「无响应」）
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_quit = true; break; }
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        if (g_quit) break;

        // ---- 处理 stdin 命令
        bool wantRecreate = needRecreate;
        needRecreate = false;
        {
            std::vector<Command> cmds;
            ::EnterCriticalSection(&g_cmdLock);
            cmds.swap(g_cmdQueue);
            ::LeaveCriticalSection(&g_cmdLock);
            for (auto& c : cmds) {
                switch (c.kind) {
                    case CMD_QUIT: g_quit = true; break;
                    case CMD_RESIZE:
                        if (c.a < 16 || c.b < 16 || c.a > 16384 || c.b > 16384) {
                            Info("[stdin] resize 尺寸不合理，忽略");
                        } else if (opt.windowMode == 0) {
                            // 窗口模式下改窗口客户区，交换链随后跟着 WM_SIZE 走
                            RECT rc{ 0, 0, (LONG)c.a, (LONG)c.b };
                            ::AdjustWindowRectEx(&rc, (DWORD)::GetWindowLongPtrW(g_hwnd, GWL_STYLE), FALSE, 0);
                            ::SetWindowPos(g_hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                                           SWP_NOMOVE | SWP_NOZORDER);
                            Info("[stdin] resize → 窗口客户区 %ux%u", c.a, c.b);
                        } else {
                            // 无边框/全屏模式下窗口占满显示器，只改交换链图像尺寸（会被拉伸）
                            R.requestedExtent.width  = c.a;
                            R.requestedExtent.height = c.b;
                            wantRecreate = true;
                            Info("[stdin] resize → 仅交换链图像 %ux%u（窗口仍占满显示器，会拉伸）", c.a, c.b);
                        }
                        opt.width = c.a; opt.height = c.b;
                        break;
                    case CMD_MODE:
                        if (c.a != opt.windowMode) {
                            opt.windowMode = (int)c.a;
                            WindowSetup s2 = ApplyWindowMode(g_hwnd, opt.windowMode, opt.width, opt.height);
                            Info("[stdin] mode → %s（%s）", WindowModeName(opt.windowMode), s2.note.c_str());
                            wantRecreate = true;
                        }
                        break;
                    case CMD_VSYNC:
                        if (c.flag != opt.vsync) {
                            opt.vsync = c.flag;
                            Info("[stdin] vsync → %s", opt.vsync ? "on (FIFO)" : "off (IMMEDIATE/MAILBOX)");
                            wantRecreate = true;
                        }
                        break;
                    case CMD_FPSCAP:
                        opt.fpsCap = c.a;
                        Info("[stdin] fpscap → %u", opt.fpsCap);
                        break;
                    default: break;
                }
            }
        }
        if (g_quit) break;

        // ---- WM_SIZE 攒下来的尺寸变化。
        //      只有当尺寸真的变了才重建交换链：窗口刚创建时 ApplyWindowMode
        //      会 SetWindowPos，从而触发一次 WM_SIZE，尺寸和刚建好的交换链
        //      完全一样 —— 那次重建纯属浪费（而且会把第一帧的 dt 打成 0）。
        LONG pw = ::InterlockedExchange(&g_pendingW, 0);
        LONG ph = ::InterlockedExchange(&g_pendingH, 0);
        if (pw > 0 && ph > 0) {
            opt.width  = (uint32_t)pw;
            opt.height = (uint32_t)ph;
            if ((uint32_t)pw != R.extent.width || (uint32_t)ph != R.extent.height)
                wantRecreate = true;
        }

        // ---- 最小化：不渲染，也不计帧。真实游戏也是这么 idle 的。
        if (::IsIconic(g_hwnd)) {
            ::Sleep(30);
            prevFrameStart = QpcMs();
            continue;
        }

        // ---- 交换链重建
        if (wantRecreate && R.device != VK_NULL_HANDLE) {
            // 必须先等设备空闲：renderFinished 信号量可能还被 present 引擎持有，
            // 而且 per-image 的信号量在图像数变化时要重新分配。
            R.api.vkDeviceWaitIdle(R.device);
            R.DestroySwapchain();
            RECT cr{};
            ::GetClientRect(g_hwnd, &cr);
            uint32_t cw = (uint32_t)(cr.right - cr.left), ch = (uint32_t)(cr.bottom - cr.top);
            if (cw < 1 || ch < 1) { cw = 1; ch = 1; }
            if (R.CreateSwapchain(cw, ch, opt.vsync, VK_NULL_HANDLE)) {
                ++swapRecreatedCount;
                swapRecreatedThisFrame = true;
                // requestedExtent 是「一次性」请求：这次重建用完就清掉，
                // 免得后面 WM_SIZE 或切 vsync 触发的重建还沿用旧的目标尺寸。
                R.requestedExtent.width = R.requestedExtent.height = 0;
                prevFrameStart = QpcMs();
            } else {
                Info("[交换链] 重建失败，稍后重试");
                ::Sleep(50);
                continue;
            }
        }

        double tFrameStart = QpcMs();
        double dt = tFrameStart - prevFrameStart;
        prevFrameStart = tFrameStart;

        uint32_t slot = frameId % kFramesInFlight;
        ++frameId;

        // 1) 等这个槽位上一轮的提交完成。
        //    ★ 实测发现：开垂直同步时的等待**主要发生在这里**，而不是
        //      vkQueuePresentKHR。现代 WDDM 的翻转模型下 Present 很快返回，
        //      节流体现为「这一槽位的 fence 迟迟不 signal」。所以这一段必须
        //      单独计时，否则会被误算进 CPU 帧时间。
        double tw0 = QpcMs();
        VkResult wr = R.api.vkWaitForFences(R.device, 1, &R.slots[slot].inFlight, VK_TRUE, 1000000000ull);
        double waitMs = QpcMs() - tw0;
        if (wr == VK_TIMEOUT) {
            Info("[警告] vkWaitForFences 超时（1s），跳过本帧");
            --frameId;
            continue;
        }
        if (wr != VK_SUCCESS) {
            Info("[致命] vkWaitForFences 失败: %d（设备丢失？）", (int)wr);
            break;
        }

        // 2) 这个槽位已经空闲 —— 现在可以安全读它的 GPU 时间戳了。
        //    读到的是 (frameId - kFramesInFlight) 那一帧的数据。
        //
        //    ⚠ 必须用 pending[slot].valid 做闸门：查询池刚创建时里面的查询
        //    从未被执行过，此时带 VK_QUERY_RESULT_WAIT_BIT 调用
        //    vkGetQueryPoolResults 可能永远等不到结果（挂死）。
        //    只有在「这个槽位确实提交过一帧」之后才去读。
        double gpuMs = -1.0;
        if (R.pending[slot].valid) gpuMs = R.ReadGpuMs(slot);
        {
            FrameRecord& done = R.pending[slot];
            if (done.valid) {
                done.gpuMs = gpuMs;
                if (o.json) Emitter::EmitFrame(done, kFramesInFlight);
                else if ((done.id % 60) == 0) {
                    char g[32];
                    if (done.gpuMs >= 0) sprintf(g, "%.4f", done.gpuMs); else sprintf(g, "n/a");
                    Info("[帧 %u] dt=%.2fms fps=%.1f | 等待=%.3f 取图=%.3f CPU=%.3f 提交=%.3f Present=%.3f GPU=%s ms | %ux%u %s",
                         done.id, done.dtMs, (done.dtMs > 0.01 ? 1000.0 / done.dtMs : 0.0),
                         done.waitMs, done.acquireMs, done.cpuMs, done.submitMs, done.presentMs,
                         g, done.w, done.h, PresentModeName(done.presentMode));
                }
                // 统计里补上 GPU 时间
                if (done.gpuMs >= 0 && stats.gpuMs.size() < Stats::CAP) stats.gpuMs.push_back(done.gpuMs);
                done.valid = false;
                ++emittedFrames;
            }
        }

        // 3) 取图像
        uint32_t imageIndex = 0;
        double ta = QpcMs();
        VkResult ar = R.api.vkAcquireNextImageKHR(R.device, R.swapchain, 1000000000ull,
                                                  R.slots[slot].imageAvail, VK_NULL_HANDLE, &imageIndex);
        double acquireMs = QpcMs() - ta;
        if (ar == VK_ERROR_OUT_OF_DATE_KHR || ar == VK_SUBOPTIMAL_KHR) {
            needRecreate = true;
            --frameId;                        // 这一帧没画成，帧号不推进
            prevFrameStart = QpcMs();
            R.api.vkQueueWaitIdle(R.queue);
            continue;
        }
        if (ar == VK_TIMEOUT || ar == VK_NOT_READY) {
            --frameId; prevFrameStart = QpcMs(); continue;
        }
        if (ar != VK_SUCCESS) {
            Info("[致命] vkAcquireNextImageKHR 失败: %d（设备丢失？）", (int)ar);
            break;
        }

        // 4) 上传本帧顶点数据（此时该槽位的 fence 已被等到，GPU 不会读它）
        //    ── 从这里开始才是「CPU 真正在干活」，cpu_ms 就从这里量起 ──
        double tWork = QpcMs();
        R.WriteVertices(slot, frameId);

        // 5) 录制
        float aspect = (float)R.extent.width / (float)(R.extent.height ? R.extent.height : 1);
        R.api.vkResetFences(R.device, 1, &R.slots[slot].inFlight);
        if (!R.RecordFrame(slot, imageIndex, frameId, aspect)) {
            Info("[致命] 命令缓冲录制失败");
            break;
        }

        // 6) 提交
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{};
        si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount   = 1;
        si.pWaitSemaphores      = &R.slots[slot].imageAvail;
        si.pWaitDstStageMask    = &waitStage;
        si.commandBufferCount   = 1;
        si.pCommandBuffers      = &R.slots[slot].cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores    = &R.renderFinished[imageIndex];

        double ts = QpcMs();
        VkResult sr = R.api.vkQueueSubmit(R.queue, 1, &si, R.slots[slot].inFlight);
        double submitMs = QpcMs() - ts;
        if (sr != VK_SUCCESS) {
            Info("[致命] vkQueueSubmit 失败: %d", (int)sr);
            break;
        }
        double cpuMs = QpcMs() - tWork;   // 纯 CPU 干活时间（不含任何等待）

        // 7) Present —— 这一步就是将来 NextPerf 的 Vulkan 层要挂钩的地方。
        //    单独计时的原因：垂直同步开着的时候，卡在 Present 里的时间
        //    不是 CPU 的活，不该算进 CPU 帧时间（这一点和 D3D 那边一样）。
        VkPresentInfoKHR pi{};
        pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores    = &R.renderFinished[imageIndex];
        pi.swapchainCount     = 1;
        pi.pSwapchains        = &R.swapchain;
        pi.pImageIndices      = &imageIndex;

        double tp = QpcMs();
        VkResult pr = R.api.vkQueuePresentKHR(R.queue, &pi);
        double presentMs = QpcMs() - tp;

        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
            needRecreate = true;
        } else if (pr != VK_SUCCESS) {
            Info("[致命] vkQueuePresentKHR 失败: %d（设备丢失？）", (int)pr);
            break;
        }

        // 8) 攒下本帧的记录（等 kFramesInFlight 帧后再输出）
        {
            FrameRecord& rec = R.pending[slot];
            rec.valid        = true;
            rec.id           = frameId;
            rec.dtMs         = (dt > 0.0) ? dt : 0.0;
            rec.waitMs       = waitMs;
            rec.cpuMs        = cpuMs;
            rec.acquireMs    = acquireMs;
            rec.submitMs     = submitMs;
            rec.presentMs    = presentMs;
            rec.gpuMs        = -1.0;
            rec.imageIndex   = imageIndex;
            rec.w           = R.extent.width;
            rec.h           = R.extent.height;
            rec.presentMode  = R.presentMode;
            rec.fpsCap       = opt.fpsCap;
            rec.windowMode   = opt.windowMode;
            rec.swapRecreated = swapRecreatedThisFrame;
            // 前 kMaxWarmupFrames 帧在单帧输出里标 warmup=1（供下游自行取舍）；
            // 汇总统计里实际剔除的是 WarmupFrameCount(总帧数)，见 EmitSummary。
            rec.warmup       = (frameId <= kMaxWarmupFrames);
        }
        swapRecreatedThisFrame = false;

        // 9) 统计（GPU 时间在上面读到时补进去）。
        //    所有帧都进统计，预热帧在出汇总时再按 WarmupFrameCount 剔除 ——
        //    这样最后才知道总共跑了多少帧，也就能按比例决定丢多少。
        ++stats.frames;
        if (stats.frameMs.size() < Stats::CAP) {
            stats.frameMs.push_back(dt);
            stats.fps.push_back(dt > 0 ? 1000.0 / dt : 0.0);
            stats.cpuMs.push_back(cpuMs);
            stats.presentMs.push_back(presentMs);
            stats.waitMs.push_back(waitMs);
            stats.acquireMs.push_back(acquireMs);
        }

        // 10) 窗口标题上滚动显示帧号 —— 肉眼确认「真的在出帧」的最快方式，
        //     也是给人工测试者看的：帧号不动就说明卡住了。
        if ((frameId % 30) == 0) {
            double elapsed = (QpcMs() - wallStart) / 1000.0;
            double fps = elapsed > 0 ? (double)frameId / elapsed : 0.0;
            char nb[256];
            sprintf(nb, "NextPerf simvk | %s | %ux%u %s | %.1f FPS | frame %u",
                    R.deviceName.c_str(), R.extent.width, R.extent.height,
                    PresentModeName(R.presentMode), fps, frameId);
            std::wstring wt(nb, nb + strlen(nb));
            ::SetWindowTextW(g_hwnd, wt.c_str());
        }

        // 11) 退出条件
        if (opt.frames > 0 && frameId >= opt.frames) break;
        if (opt.seconds > 0.0 && (QpcMs() - wallStart) / 1000.0 >= opt.seconds) break;

        // 12) 锁帧：把下一帧的起点推到 tFrameStart + 预算
        if (opt.fpsCap > 0) {
            double budget = 1000.0 / (double)opt.fpsCap;
            PaceUntil(tFrameStart + budget);
        }
    }

    const char* stopReason = g_quit ? "quit" : (opt.frames > 0 && frameId >= opt.frames ? "frames" : "seconds");
    stats.wallSeconds = (QpcMs() - wallStart) / 1000.0;

    // ---- 收尾：把还在流水线里的帧读完，一帧都不丢
    if (R.device) {
        R.api.vkDeviceWaitIdle(R.device);
        for (uint32_t s = 0; s < kFramesInFlight; ++s) {
            FrameRecord& done = R.pending[s];
            if (!done.valid) continue;    // 没提交过就绝不能去读（见主循环里的说明）
            done.gpuMs = R.ReadGpuMs(s);
            if (o.json) Emitter::EmitFrame(done, kFramesInFlight);
            if (done.gpuMs >= 0 && stats.gpuMs.size() < Stats::CAP) stats.gpuMs.push_back(done.gpuMs);
            done.valid = false;
            ++emittedFrames;
        }
    }

    Info("");
    Info("================== 汇总 ==================");
    Info("退出原因        : %s", stopReason);
    Info("提交帧数        : %u（JSON 输出 %u 行）", stats.frames, emittedFrames);

    // 和 EmitSummary 用同一套口径：剔除预热帧再统计
    uint32_t warmup = WarmupFrameCount(stats.frames);
    std::vector<double> sFt   = TailFrom(stats.frameMs, warmup);
    std::vector<double> sFps  = TailFrom(stats.fps, warmup);
    std::vector<double> sCpu  = TailFrom(stats.cpuMs, warmup);
    std::vector<double> sGpu  = TailFrom(stats.gpuMs, warmup);
    std::vector<double> sPres = TailFrom(stats.presentMs, warmup);
    std::vector<double> sWait = TailFrom(stats.waitMs, warmup);
    auto avgOf = [](const std::vector<double>& v) {
        if (v.empty()) return -1.0;
        double s2 = 0; for (double x : v) s2 += x;
        return s2 / (double)v.size();
    };

    Info("运行时长        : %.3f s", stats.wallSeconds);
    Info("统计样本        : %zu 帧（已剔除开头 %u 帧预热期）", sFt.size(), warmup);
    if (!sFt.empty()) {
        std::vector<double> ft = sFt, fps = sFps, ftd = sFt;
        std::sort(ft.begin(), ft.end());
        std::sort(fps.begin(), fps.end());
        std::sort(ftd.begin(), ftd.end(), std::greater<double>());
        double avgFt = avgOf(sFt);
        Info("平均帧率        : %.2f FPS（平均帧时间 %.3f ms）",
             avgFt > 0 ? 1000.0 / avgFt : 0.0, avgFt);
        Info("帧时间 P50      : %.3f ms", Percentile(ft, 0.50));
        Info("帧时间 P99      : %.3f ms", Percentile(ft, 0.99));
        Info("帧时间 P99.9     : %.3f ms", Percentile(ft, 0.999));
        Info("1%% Low (FPS P1)  : %.2f FPS", Percentile(fps, 0.01));
        Info("0.1%% Low(FPS P0.1): %.2f FPS", Percentile(fps, 0.001));
        Info("1%% Low (PresentMon 口径: 最慢 1%% 帧平均)   : %.2f FPS", LowFpsAvgWorst(ftd, 0.01));
        Info("0.1%% Low(PresentMon 口径)                  : %.2f FPS", LowFpsAvgWorst(ftd, 0.001));
    } else {
        Info("（样本不足，跳过百分位统计）");
    }
    {
        double a = avgOf(sCpu), b = avgOf(sGpu), c = avgOf(sPres), d = avgOf(sWait);
        double e = avgOf(TailFrom(stats.acquireMs, warmup));
        Info("平均 等待(WaitForFences): %.3f ms   ← 开 vsync 时等待主要在这里", d);
        Info("平均 取图(AcquireNextImg): %.3f ms", e);
        Info("平均 CPU 帧时间         : %.3f ms   （只算真正干活，不含等待）", a);
        Info("平均 GPU 帧时间         : %s", b >= 0 ? (std::to_string(b) + " ms").c_str()
                                                   : "n/a（时间戳查询不可用）");
        Info("平均 Present 阻塞       : %.3f ms", c);
    }
    Info("交换链重建次数  : %u", swapRecreatedCount);
    Info("最终 present mode: %s   分辨率: %ux%u", PresentModeName(R.presentMode),
         R.extent.width, R.extent.height);
    if (!R.gpuTimingOk) Info("GPU 计时不可用原因: %s", R.gpuTimingNote.c_str());
    Info("==========================================");

    if (o.json) Emitter::EmitSummary(stats, R, opt, layers, stopReason);

    R.DestroyAll();
    return 0;
}

// ============================================================================
// 9. main
// ============================================================================

int main(int argc, char** argv) {
    ::InitializeCriticalSection(&g_outLock);
    ::InitializeCriticalSection(&g_cmdLock);
    QpcInit();

    Options o;
    if (!ParseArgs(argc, argv, &o)) return 1;
    if (o.showHelp) { PrintUsage(); return 0; }
    g_jsonMode = o.json;

    if (!o.json) {
        printf("NextPerf simvk —— Vulkan 模拟游戏进程\n");
        printf("目的：给 NextPerf 将来的 Vulkan 隐式层（VK_LAYER_*）提供一个真实靶子。\n\n");
    }

    // 1ms 定时器精度：锁帧要靠它，否则 Sleep(1) 实际会睡 15.6ms
    ::timeBeginPeriod(1);
    ::SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
    atexit(RestoreDisplayMode);   // 改过显示模式就必须还原，任何路径都不能漏

    VkApi api;
    if (!api.LoadDll()) {
        Info("[致命] 加载 vulkan-1.dll 失败（GetLastError=%lu）。", ::GetLastError());
        Info("       本机没有 Vulkan 运行时，无法运行本测试。");
        if (o.json) JsonRaw("{\"type\":\"error\",\"stage\":\"load_vulkan_1_dll\",\"message\":\"vulkan-1.dll 未找到或无法加载\"}");
        ::timeEndPeriod(1);
        return 3;
    }

    // --list-layers 是纯枚举，不创建实例。
    // 刻意不创建：创建实例会把所有隐式层的 DLL 真的载进来（比如 Steam 覆盖层
    // 会弹窗、GamePP 会挂上），一个「我只是想看看装了什么」的命令不该有副作用。
    if (o.listLayers) {
        int rc = DoListLayers(api);
        api.Unload();
        ::timeEndPeriod(1);
        return rc;
    }

    // 启动时报告层情况 —— 这一段的全部意义就是回答「注入成功了吗」
    {
        LayerRegistryStats stats{};
        std::vector<std::string> notes;
        std::vector<LayerInfo> layers = CollectLayers(api, &stats, &notes);

        int total = (int)layers.size(), nImp = 0, nVkLayer = 0;
        for (auto& li : layers) { if (li.implicit) ++nImp; if (li.name.rfind("VK_LAYER_", 0) == 0) ++nVkLayer; }

        Info("[层] vkEnumerateInstanceLayerProperties 报告 %d 个层，其中 VK_LAYER_* 前缀 %d 个，隐式 %d 个：",
             total, nVkLayer, nImp);
        for (auto& li : layers) {
            Info("[层]   %s %-32s %s", li.implicit ? "[隐式]" : "[显式]",
                 li.name.c_str(), li.description.c_str());
            if (!li.libraryPath.empty() && !li.libraryExists)
                Info("[层]          ⚠ 层 DLL 缺失: %s", li.libraryPath.c_str());
        }
        char envBuf[1024];
        DWORD en = ::GetEnvironmentVariableA("VK_INSTANCE_LAYERS", envBuf, sizeof(envBuf));
        if (en > 0 && en < sizeof(envBuf)) {
            Info("[层] VK_INSTANCE_LAYERS = %s", envBuf);
            // 实测（loader 1.4.341）：VK_INSTANCE_LAYERS 里写一个**不存在**的层名，
            // vkCreateInstance **不会**失败 —— 新版 loader 只是忽略它并继续。
            // 所以「实例建成功」不能用来证明 VK_INSTANCE_LAYERS 生效了。
            // 想确认生效，用 VK_LOADER_DEBUG=layer 看 loader 日志，
            // 或看下面 [分派归属] 那段（层拦截了函数就会显示层 DLL 的模块名）。
            Info("[层]   → loader 会把这些层追加进启用列表。注意：实测本机 loader 对"
                 "「不存在的层名」是忽略而非报错，所以实例建成功≠该层已生效。");
        } else {
            Info("[层] VK_INSTANCE_LAYERS 未设置（隐式层仍会由 loader 自动启用）");
        }

        if (!o.json) {
            for (auto& li : layers) if (li.implicit)
                printf("    隐式层: %s —— %s\n", li.name.c_str(), li.description.c_str());
            printf("\n");
        }

        // 运行
        int rc = RunSim(o, layers);
        api.Unload();
        ::timeEndPeriod(1);
        return rc;
    }
}
