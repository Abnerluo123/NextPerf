// NextPerf tests/simvk —— 手写的 Vulkan 最小声明 + 动态加载封装
//
// ============================================================================
// 为什么不用官方的 <vulkan/vulkan.h>？
// ============================================================================
// 本机没有 Vulkan SDK，也没有 MSVC / Windows SDK。官方的 vulkan.h 是一个
// 8 万行的巨型头文件，而且它自己还要 #include <vulkan/vk_platform.h> 等一堆
// 兄弟头，版本一多就编不过去。既然我们只用到 Vulkan 的极小一个子集
// （instance / device / swapchain / render pass / 一个三角形 / 时间戳查询），
// 那就自己把这些声明写出来 —— 和本项目 src/sensors/np_vendor.h 里手写
// NVML / NVAPI / ADL 声明是同一个思路：零外部依赖，开箱即编。
//
// 关键事实（决定了为什么手写是可行的）：
//   * Vulkan 的 ABI 是纯 C 的，没有 C++ 的 name mangling / 虚表 / 异常；
//   * 所有 handle 无非两种：可分发句柄（内部是结构体指针）和不可分发句柄
//     （64 位下也是指针）。两者在 x86_64 上都是 8 字节，按值传递没有任何区别；
//   * VkResult / VkBool32 / 各类 Flags 都是 int32/uint32；
//   * 结构体永远是 { sType, pNext, ... } 开头，字段顺序和类型在 vk.xml 里
//     是固定不动的，只要照抄就能得到和官方头文件**完全相同的内存布局**。
//
// 为了让「照抄得对不对」这件事可验证，文件末尾有一组 static_assert：
// 官方头文件在 LP64/LLP64 下的 sizeof 是众所周知的确定值（比如
// sizeof(VkPhysicalDeviceLimits) == 504、sizeof(VkSwapchainCreateInfoKHR) == 104），
// 对不上就编译不过。这比「跑起来看着好像没崩」要可靠得多。
//
// ============================================================================
// 为什么全部走 LoadLibrary + GetProcAddress？
// ============================================================================
// 1) 链接期根本找不到 vulkan-1.lib —— 没有 SDK，就没有 import library。
//    直接 GetProcAddress 拿函数指针，绕开链接器。
// 2) 更重要的是：本项目要测的正是「Vulkan 隐式层能不能挂上」。
//    分派必须走 loader 的**正规路径**：
//        vkGetInstanceProcAddr(NULL, "vkCreateInstance")   ← 全局级
//        vkGetInstanceProcAddr(instance, "vkCreateDevice") ← instance 级
//        vkGetDeviceProcAddr(device, "vkQueuePresentKHR")  ← device 级
//    尤其最后一条：只有走 vkGetDeviceProcAddr 拿到的 vkQueuePresentKHR
//    才是「层链最外层的那个函数」。如果我们自己去 DLL 里 GetProcAddress
//    "vkQueuePresentKHR"，拿到的是 loader 的跳板，仍然会过层，但绕过了
//    vkGetDeviceProcAddr 这一层的协商语义 —— 那就不是在测真实游戏的行为了。
//    真实游戏全部走 vkGetDeviceProcAddr，所以我们也必须走。

#pragma once

#include <windows.h>
#include <cstdint>
#include <cstddef>

// ============================================================ 基础类型
// VkResult 的取值：0 = VK_SUCCESS，负数 = 各类错误，正数 = 非致命状态。
typedef int32_t  VkResult;
typedef uint32_t VkBool32;
typedef uint32_t VkFlags;
typedef uint64_t VkDeviceSize;
typedef uint32_t VkSampleMask;

// 各类 *Flags 在 ABI 上都是 uint32_t，官方头文件为每种标志起一个独立名字
// 只是为了类型安全和可读性。这里把用得到的都补上 —— 布局完全等价。
typedef VkFlags VkPipelineStageFlags;
typedef VkFlags VkAccessFlags;
typedef VkFlags VkImageUsageFlags;
typedef VkFlags VkBufferUsageFlags;
typedef VkFlags VkMemoryPropertyFlags;
typedef VkFlags VkMemoryHeapFlags;
typedef VkFlags VkQueueFlags;
typedef VkFlags VkSampleCountFlags;
typedef VkFlags VkColorComponentFlags;
typedef VkFlags VkImageAspectFlags;
typedef VkFlags VkQueryResultFlags;
typedef VkFlags VkFenceCreateFlags;
typedef VkFlags VkCommandPoolCreateFlags;
typedef VkFlags VkCommandBufferUsageFlags;
typedef VkFlags VkDependencyFlags;
typedef VkFlags VkSurfaceTransformFlagsKHR;
typedef VkFlags VkCompositeAlphaFlagsKHR;
typedef VkFlags VkSwapchainCreateFlagsKHR;
typedef VkFlags VkPipelineCreateFlags;
typedef VkFlags VkPipelineShaderStageCreateFlags;
typedef VkFlags VkPipelineVertexInputStateCreateFlags;
typedef VkFlags VkPipelineInputAssemblyStateCreateFlags;
typedef VkFlags VkPipelineViewportStateCreateFlags;
typedef VkFlags VkPipelineRasterizationStateCreateFlags;
typedef VkFlags VkPipelineMultisampleStateCreateFlags;
typedef VkFlags VkPipelineColorBlendStateCreateFlags;
typedef VkFlags VkPipelineDynamicStateCreateFlags;
typedef VkFlags VkPipelineLayoutCreateFlags;
typedef VkFlags VkRenderPassCreateFlags;
typedef VkFlags VkSubpassDescriptionFlags;
typedef VkFlags VkAttachmentDescriptionFlags;
typedef VkFlags VkFramebufferCreateFlags;
typedef VkFlags VkImageViewCreateFlags;
typedef VkFlags VkShaderModuleCreateFlags;
typedef VkFlags VkBufferCreateFlags;
typedef VkFlags VkQueryPoolCreateFlags;
typedef VkFlags VkSemaphoreCreateFlags;
typedef VkFlags VkInstanceCreateFlags;
typedef VkFlags VkDeviceCreateFlags;
typedef VkFlags VkDeviceQueueCreateFlags;

#define VK_TRUE  1u
#define VK_FALSE 0u

#define VK_SUCCESS                       0
#define VK_NOT_READY                     1
#define VK_TIMEOUT                       2
#define VK_INCOMPLETE                    5
#define VK_ERROR_OUT_OF_HOST_MEMORY     (-1)
#define VK_ERROR_OUT_OF_DEVICE_MEMORY   (-2)
#define VK_ERROR_INITIALIZATION_FAILED  (-3)
#define VK_ERROR_DEVICE_LOST            (-4)
#define VK_ERROR_MEMORY_MAP_FAILED      (-5)
#define VK_ERROR_LAYER_NOT_PRESENT      (-6)
#define VK_ERROR_EXTENSION_NOT_PRESENT  (-7)
#define VK_ERROR_FEATURE_NOT_PRESENT    (-8)
#define VK_ERROR_INCOMPATIBLE_DRIVER    (-9)
#define VK_ERROR_TOO_MANY_OBJECTS      (-10)
#define VK_ERROR_FORMAT_NOT_SUPPORTED  (-11)
#define VK_ERROR_SURFACE_LOST_KHR      (-1000000000)
#define VK_SUBOPTIMAL_KHR                1000001003
#define VK_ERROR_OUT_OF_DATE_KHR       (-1000001004)

// ============================================================ 句柄
// 可分发句柄（dispatchable）：内部是 pointer to dispatch table。
// 不可分发句柄（non-dispatchable）：64 位下由 VK_USE_64_BIT_PTR_DEFINES 决定，
//                                   Windows x64 上就是指针。
// 两种在我们的用法里都只是「按值传的 8 字节」，所以统一声明成不完整类型指针。
#define VK_DEFINE_HANDLE(object) typedef struct object##_T* object;
VK_DEFINE_HANDLE(VkInstance)
VK_DEFINE_HANDLE(VkPhysicalDevice)
VK_DEFINE_HANDLE(VkDevice)
VK_DEFINE_HANDLE(VkQueue)
VK_DEFINE_HANDLE(VkCommandBuffer)
VK_DEFINE_HANDLE(VkSemaphore)
VK_DEFINE_HANDLE(VkFence)
VK_DEFINE_HANDLE(VkSwapchainKHR)
VK_DEFINE_HANDLE(VkSurfaceKHR)
VK_DEFINE_HANDLE(VkImage)
VK_DEFINE_HANDLE(VkImageView)
VK_DEFINE_HANDLE(VkRenderPass)
VK_DEFINE_HANDLE(VkFramebuffer)
VK_DEFINE_HANDLE(VkShaderModule)
VK_DEFINE_HANDLE(VkPipelineLayout)
VK_DEFINE_HANDLE(VkPipeline)
VK_DEFINE_HANDLE(VkCommandPool)
VK_DEFINE_HANDLE(VkQueryPool)
VK_DEFINE_HANDLE(VkBuffer)
VK_DEFINE_HANDLE(VkDeviceMemory)
#undef VK_DEFINE_HANDLE

#define VK_NULL_HANDLE nullptr

// ============================================================ 常量
#define VK_MAX_PHYSICAL_DEVICE_NAME_SIZE 256u
#define VK_UUID_SIZE                      16u
#define VK_MAX_EXTENSION_NAME_SIZE       256u
#define VK_MAX_DESCRIPTION_SIZE          256u
#define VK_WHOLE_SIZE            (~0ull)
#define VK_QUEUE_FAMILY_IGNORED  (~0u)
#define VK_SUBPASS_EXTERNAL      (~0u)
#define VK_REMAINING_MIP_LEVELS  (~0u)
#define VK_REMAINING_ARRAY_LAYERS (~0u)

// 版本号打包：major(10bit) | minor(10bit) | patch(12bit)，整体左移 12 位
#define VK_MAKE_API_VERSION(variant, major, minor, patch) \
    ((((uint32_t)(variant)) << 29) | (((uint32_t)(major)) << 22) | \
     (((uint32_t)(minor)) << 12) | ((uint32_t)(patch)))
#define VK_API_VERSION_1_0 VK_MAKE_API_VERSION(0, 1, 0, 0)
#define VK_API_VERSION_1_1 VK_MAKE_API_VERSION(0, 1, 1, 0)
#define VK_API_VERSION_1_3 VK_MAKE_API_VERSION(0, 1, 3, 0)

// ============================================================ VkStructureType
// 这些数字是 Vulkan 规范里写死的 ABI 一部分，抄错就必崩。
// 1000xxxxxx 段是扩展（用扩展的 "扩展号" 排序编码），KHR swapchain 是 1000001000。
typedef enum VkStructureType {
    VK_STRUCTURE_TYPE_APPLICATION_INFO                          = 0,
    VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO                      = 1,
    VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO                  = 2,
    VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO                        = 3,
    VK_STRUCTURE_TYPE_SUBMIT_INFO                               = 4,
    VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO                      = 5,
    VK_STRUCTURE_TYPE_FENCE_CREATE_INFO                         = 8,
    VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO                     = 9,
    VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO                    = 11,
    VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO                        = 12,
    VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO                    = 15,
    VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO                 = 16,
    VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO         = 18,
    VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO   = 19,
    VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO = 20,
    VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO       = 22,
    VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO  = 23,
    VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO    = 24,
    VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO    = 26,
    VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO        = 27,
    VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO             = 28,
    VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO               = 30,
    VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO                   = 37,
    VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO                   = 38,
    VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO                  = 39,
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO              = 40,
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO                 = 42,
    VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO                    = 43,
    VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO               = 47,
    VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO                 = 48,
    VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR                 = 1000001000,
    VK_STRUCTURE_TYPE_PRESENT_INFO_KHR                          = 1000001001,
    VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR             = 1000009000,
} VkStructureType;

// ============================================================ 枚举 / 位标志
typedef enum VkPhysicalDeviceType {
    VK_PHYSICAL_DEVICE_TYPE_OTHER          = 0,
    VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU = 1,
    VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU   = 2,
    VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU    = 3,
    VK_PHYSICAL_DEVICE_TYPE_CPU            = 4,
} VkPhysicalDeviceType;

typedef enum VkQueueFlagBits {
    VK_QUEUE_GRAPHICS_BIT = 0x00000001,
    VK_QUEUE_COMPUTE_BIT  = 0x00000002,
    VK_QUEUE_TRANSFER_BIT = 0x00000004,
    VK_QUEUE_SPARSE_BINDING_BIT = 0x00000008,
} VkQueueFlagBits;

typedef enum VkFormat {
    VK_FORMAT_UNDEFINED            = 0,
    VK_FORMAT_R8G8B8A8_UNORM       = 37,
    VK_FORMAT_R8G8B8A8_SRGB        = 43,
    VK_FORMAT_B8G8R8A8_UNORM       = 44,
    VK_FORMAT_B8G8R8A8_SRGB        = 50,
    VK_FORMAT_R32_SFLOAT           = 100,
    VK_FORMAT_R32G32_SFLOAT        = 103,
    VK_FORMAT_R32G32B32_SFLOAT     = 106,
    VK_FORMAT_R32G32B32A32_SFLOAT  = 109,
} VkFormat;

typedef enum VkColorSpaceKHR { VK_COLOR_SPACE_SRGB_NONLINEAR_KHR = 0 } VkColorSpaceKHR;

typedef enum VkPresentModeKHR {
    VK_PRESENT_MODE_IMMEDIATE_KHR    = 0,
    VK_PRESENT_MODE_MAILBOX_KHR      = 1,
    VK_PRESENT_MODE_FIFO_KHR         = 2,
    VK_PRESENT_MODE_FIFO_RELAXED_KHR = 3,
} VkPresentModeKHR;

typedef enum VkSharingMode {
    VK_SHARING_MODE_EXCLUSIVE  = 0,
    VK_SHARING_MODE_CONCURRENT = 1,
} VkSharingMode;

typedef enum VkImageLayout {
    VK_IMAGE_LAYOUT_UNDEFINED                = 0,
    VK_IMAGE_LAYOUT_GENERAL                  = 1,
    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL = 2,
    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL     = 7,
    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR          = 1000001002,
} VkImageLayout;

typedef enum VkImageType { VK_IMAGE_TYPE_2D = 1 } VkImageType;
typedef enum VkImageViewType { VK_IMAGE_VIEW_TYPE_2D = 1 } VkImageViewType;
typedef enum VkImageTiling { VK_IMAGE_TILING_OPTIMAL = 0, VK_IMAGE_TILING_LINEAR = 1 } VkImageTiling;

typedef enum VkImageUsageFlagBits {
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT    = 0x00000001,
    VK_IMAGE_USAGE_TRANSFER_DST_BIT    = 0x00000002,
    VK_IMAGE_USAGE_SAMPLED_BIT         = 0x00000004,
    VK_IMAGE_USAGE_STORAGE_BIT         = 0x00000008,
    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT = 0x00000010,
} VkImageUsageFlagBits;

typedef enum VkImageAspectFlagBits { VK_IMAGE_ASPECT_COLOR_BIT = 0x00000001 } VkImageAspectFlagBits;

typedef enum VkBufferUsageFlagBits {
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT = 0x00000001,
    VK_BUFFER_USAGE_TRANSFER_DST_BIT = 0x00000002,
    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT = 0x00000010,
    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT  = 0x00000080,
} VkBufferUsageFlagBits;

typedef enum VkMemoryPropertyFlagBits {
    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT = 0x00000001,
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT = 0x00000002,
    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT = 0x00000004,
    VK_MEMORY_PROPERTY_HOST_CACHED_BIT   = 0x00000008,
} VkMemoryPropertyFlagBits;

typedef enum VkComponentSwizzle { VK_COMPONENT_SWIZZLE_IDENTITY = 0 } VkComponentSwizzle;

typedef enum VkSampleCountFlagBits { VK_SAMPLE_COUNT_1_BIT = 0x00000001 } VkSampleCountFlagBits;

typedef enum VkAttachmentLoadOp  { VK_ATTACHMENT_LOAD_OP_LOAD = 0, VK_ATTACHMENT_LOAD_OP_CLEAR = 1, VK_ATTACHMENT_LOAD_OP_DONT_CARE = 2 } VkAttachmentLoadOp;
typedef enum VkAttachmentStoreOp { VK_ATTACHMENT_STORE_OP_STORE = 0, VK_ATTACHMENT_STORE_OP_DONT_CARE = 1 } VkAttachmentStoreOp;

typedef enum VkPipelineBindPoint { VK_PIPELINE_BIND_POINT_GRAPHICS = 0 } VkPipelineBindPoint;

typedef enum VkPrimitiveTopology { VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST = 3 } VkPrimitiveTopology;
typedef enum VkPolygonMode { VK_POLYGON_MODE_FILL = 0 } VkPolygonMode;
typedef enum VkCullModeFlagBits { VK_CULL_MODE_NONE = 0, VK_CULL_MODE_BACK_BIT = 0x00000002 } VkCullModeFlagBits;
typedef enum VkFrontFace { VK_FRONT_FACE_COUNTER_CLOCKWISE = 1 } VkFrontFace;
typedef enum VkLogicOp { VK_LOGIC_OP_COPY = 3 } VkLogicOp;
typedef enum VkBlendFactor {
    VK_BLEND_FACTOR_ZERO = 0, VK_BLEND_FACTOR_ONE = 1,
    VK_BLEND_FACTOR_SRC_ALPHA = 6, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA = 7,
} VkBlendFactor;
typedef enum VkBlendOp { VK_BLEND_OP_ADD = 0 } VkBlendOp;
typedef enum VkColorComponentFlagBits {
    VK_COLOR_COMPONENT_R_BIT = 0x1, VK_COLOR_COMPONENT_G_BIT = 0x2,
    VK_COLOR_COMPONENT_B_BIT = 0x4, VK_COLOR_COMPONENT_A_BIT = 0x8,
} VkColorComponentFlagBits;

typedef enum VkDynamicState { VK_DYNAMIC_STATE_VIEWPORT = 0, VK_DYNAMIC_STATE_SCISSOR = 1 } VkDynamicState;

typedef enum VkShaderStageFlagBits {
    VK_SHADER_STAGE_VERTEX_BIT   = 0x00000001,
    VK_SHADER_STAGE_FRAGMENT_BIT = 0x00000010,
} VkShaderStageFlagBits;

typedef enum VkVertexInputRate { VK_VERTEX_INPUT_RATE_VERTEX = 0 } VkVertexInputRate;

typedef enum VkPipelineStageFlagBits {
    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT      = 0x00000001,
    VK_PIPELINE_STAGE_VERTEX_SHADER_BIT    = 0x00000008,
    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT = 0x00000400,
    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT   = 0x00002000,
    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT     = 0x00010000,
} VkPipelineStageFlagBits;

typedef enum VkAccessFlagBits {
    VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT        = 0x00000020,
    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT       = 0x00000100,
    VK_ACCESS_MEMORY_READ_BIT                  = 0x00008000,
} VkAccessFlagBits;

typedef enum VkDependencyFlagBits { VK_DEPENDENCY_BY_REGION_BIT = 0x1 } VkDependencyFlagBits;

typedef enum VkCommandBufferLevel { VK_COMMAND_BUFFER_LEVEL_PRIMARY = 0 } VkCommandBufferLevel;
typedef enum VkCommandBufferUsageFlagBits {
    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT = 0x0001,
    VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT = 0x0002,
} VkCommandBufferUsageFlagBits;

typedef enum VkCommandPoolCreateFlagBits {
    VK_COMMAND_POOL_CREATE_TRANSIENT_BIT = 0x0001,
    VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT = 0x0002,
} VkCommandPoolCreateFlagBits;

typedef enum VkSubpassContents { VK_SUBPASS_CONTENTS_INLINE = 0 } VkSubpassContents;

typedef enum VkFenceCreateFlagBits { VK_FENCE_CREATE_SIGNALED_BIT = 0x00000001 } VkFenceCreateFlagBits;
typedef enum VkQueryType { VK_QUERY_TYPE_OCCLUSION = 0, VK_QUERY_TYPE_TIMESTAMP = 2 } VkQueryType;
typedef enum VkQueryResultFlagBits {
    VK_QUERY_RESULT_64_BIT               = 0x00000002,
    VK_QUERY_RESULT_WAIT_BIT             = 0x00000001,
    VK_QUERY_RESULT_WITH_AVAILABILITY_BIT = 0x00000004,
} VkQueryResultFlagBits;

typedef enum VkSurfaceTransformFlagBitsKHR {
    VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR = 0x00000001,
} VkSurfaceTransformFlagBitsKHR;

typedef enum VkCompositeAlphaFlagBitsKHR {
    VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR          = 0x00000001,
    VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR  = 0x00000002,
    VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR = 0x00000004,
    VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR         = 0x00000008,
} VkCompositeAlphaFlagBitsKHR;

typedef enum VkResult_ { VK_RESULT_MAX_ENUM = 0x7FFFFFFF } VkResult_;

// ============================================================ 基础结构体
typedef struct VkExtent2D { uint32_t width; uint32_t height; } VkExtent2D;
typedef struct VkExtent3D { uint32_t width; uint32_t height; uint32_t depth; } VkExtent3D;
typedef struct VkOffset2D { int32_t x; int32_t y; } VkOffset2D;
typedef struct VkRect2D   { VkOffset2D offset; VkExtent2D extent; } VkRect2D;
typedef struct VkComponentMapping {
    VkComponentSwizzle r, g, b, a;
} VkComponentMapping;
typedef struct VkImageSubresourceRange {
    VkFlags  aspectMask;
    uint32_t baseMipLevel;
    uint32_t levelCount;
    uint32_t baseArrayLayer;
    uint32_t layerCount;
} VkImageSubresourceRange;

// VkClearValue 官方定义是 union { VkClearColorValue; VkClearDepthStencilValue; }。
// 两个成员都是 16 字节，所以这里直接用 16 字节的匿名联合体表达，语义等价。
typedef union VkClearValue {
    float    float32[4];
    int32_t  int32[4];
    uint32_t uint32[4];
} VkClearValue;

typedef struct VkApplicationInfo {
    VkStructureType sType;
    const void*     pNext;
    const char*     pApplicationName;
    uint32_t        applicationVersion;
    const char*     pEngineName;
    uint32_t        engineVersion;
    uint32_t        apiVersion;
} VkApplicationInfo;

typedef struct VkInstanceCreateInfo {
    VkStructureType             sType;
    const void*                 pNext;
    VkFlags                     flags;
    const VkApplicationInfo*    pApplicationInfo;
    uint32_t                    enabledLayerCount;
    const char* const*          ppEnabledLayerNames;
    uint32_t                    enabledExtensionCount;
    const char* const*          ppEnabledExtensionNames;
} VkInstanceCreateInfo;

typedef struct VkLayerProperties {
    char     layerName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    uint32_t specVersion;
    uint32_t implementationVersion;
    char     description[VK_MAX_DESCRIPTION_SIZE];
} VkLayerProperties;

typedef struct VkExtensionProperties {
    char     extensionName[VK_MAX_EXTENSION_NAME_SIZE];
    uint32_t specVersion;
} VkExtensionProperties;

typedef struct VkMemoryType { VkFlags propertyFlags; uint32_t heapIndex; } VkMemoryType;
typedef struct VkMemoryHeap { VkDeviceSize size; VkFlags flags; } VkMemoryHeap;

typedef struct VkPhysicalDeviceMemoryProperties {
    uint32_t     memoryTypeCount;
    VkMemoryType memoryTypes[32];
    uint32_t     memoryHeapCount;
    VkMemoryHeap memoryHeaps[16];
} VkPhysicalDeviceMemoryProperties;

// ---- VkPhysicalDeviceLimits ----------------------------------------------
// 这个结构体有 100 多个字段，我们真正要的只有 timestampPeriod 和
// timestampComputeAndGraphics 两个。但**必须把前面所有字段原样抄全**，
// 否则 timestampPeriod 的偏移就错了，读出来是垃圾。
// 好在字段类型很单调（基本都是 uint32/float，中间夹几个 8 字节的
// VkDeviceSize 和 size_t），按官方顺序排下来，编译器自动插入的 padding
// 和官方头文件完全一致。文件末尾的 static_assert 会验证这一点。
typedef struct VkPhysicalDeviceLimits {
    uint32_t    maxImageDimension1D;
    uint32_t    maxImageDimension2D;
    uint32_t    maxImageDimension3D;
    uint32_t    maxImageDimensionCube;
    uint32_t    maxImageArrayLayers;
    uint32_t    maxTexelBufferElements;
    uint32_t    maxUniformBufferRange;
    uint32_t    maxStorageBufferRange;
    uint32_t    maxPushConstantsSize;
    uint32_t    maxMemoryAllocationCount;
    uint32_t    maxSamplerAllocationCount;
    VkDeviceSize bufferImageGranularity;
    VkDeviceSize sparseAddressSpaceSize;
    uint32_t    maxBoundDescriptorSets;
    uint32_t    maxPerStageDescriptorSamplers;
    uint32_t    maxPerStageDescriptorUniformBuffers;
    uint32_t    maxPerStageDescriptorStorageBuffers;
    uint32_t    maxPerStageDescriptorSampledImages;
    uint32_t    maxPerStageDescriptorStorageImages;
    uint32_t    maxPerStageDescriptorInputAttachments;
    uint32_t    maxPerStageResources;
    uint32_t    maxDescriptorSetSamplers;
    uint32_t    maxDescriptorSetUniformBuffers;
    uint32_t    maxDescriptorSetUniformBuffersDynamic;
    uint32_t    maxDescriptorSetStorageBuffers;
    uint32_t    maxDescriptorSetStorageBuffersDynamic;
    uint32_t    maxDescriptorSetSampledImages;
    uint32_t    maxDescriptorSetStorageImages;
    uint32_t    maxDescriptorSetInputAttachments;
    uint32_t    maxVertexInputAttributes;
    uint32_t    maxVertexInputBindings;
    uint32_t    maxVertexInputAttributeOffset;
    uint32_t    maxVertexInputBindingStride;
    uint32_t    maxVertexOutputComponents;
    uint32_t    maxTessellationGenerationLevel;
    uint32_t    maxTessellationPatchSize;
    uint32_t    maxTessellationControlPerVertexInputComponents;
    uint32_t    maxTessellationControlPerVertexOutputComponents;
    uint32_t    maxTessellationControlPerPatchOutputComponents;
    uint32_t    maxTessellationControlTotalOutputComponents;
    uint32_t    maxTessellationEvaluationInputComponents;
    uint32_t    maxTessellationEvaluationOutputComponents;
    uint32_t    maxGeometryShaderInvocations;
    uint32_t    maxGeometryInputComponents;
    uint32_t    maxGeometryOutputComponents;
    uint32_t    maxGeometryOutputVertices;
    uint32_t    maxGeometryTotalOutputComponents;
    uint32_t    maxFragmentInputComponents;
    uint32_t    maxFragmentOutputAttachments;
    uint32_t    maxFragmentDualSrcAttachments;
    uint32_t    maxFragmentCombinedOutputResources;
    uint32_t    maxComputeSharedMemorySize;
    uint32_t    maxComputeWorkGroupCount[3];
    uint32_t    maxComputeWorkGroupInvocations;
    uint32_t    maxComputeWorkGroupSize[3];
    uint32_t    subPixelPrecisionBits;
    uint32_t    subTexelPrecisionBits;
    uint32_t    mipmapPrecisionBits;
    uint32_t    maxDrawIndexedIndexValue;
    uint32_t    maxDrawIndirectCount;
    float       maxSamplerLodBias;
    float       maxSamplerAnisotropy;
    uint32_t    maxViewports;
    uint32_t    maxViewportDimensions[2];
    float       viewportBoundsRange[2];
    uint32_t    viewportSubPixelBits;
    size_t      minMemoryMapAlignment;              // ← 8 字节（x64）
    VkDeviceSize minTexelBufferOffsetAlignment;
    VkDeviceSize minUniformBufferOffsetAlignment;
    VkDeviceSize minStorageBufferOffsetAlignment;
    int32_t     minTexelOffset;
    uint32_t    maxTexelOffset;
    int32_t     minTexelGatherOffset;
    uint32_t    maxTexelGatherOffset;
    float       minInterpolationOffset;
    float       maxInterpolationOffset;
    uint32_t    subPixelInterpolationOffsetBits;
    uint32_t    maxFramebufferWidth;
    uint32_t    maxFramebufferHeight;
    uint32_t    maxFramebufferLayers;
    VkFlags     framebufferColorSampleCounts;
    VkFlags     framebufferDepthSampleCounts;
    VkFlags     framebufferStencilSampleCounts;
    VkFlags     framebufferNoAttachmentsSampleCounts;
    uint32_t    maxColorAttachments;
    VkFlags     sampledImageColorSampleCounts;
    VkFlags     sampledImageIntegerSampleCounts;
    VkFlags     sampledImageDepthSampleCounts;
    VkFlags     sampledImageStencilSampleCounts;
    VkFlags     storageImageSampleCounts;
    uint32_t    maxSampleMaskWords;
    VkBool32    timestampComputeAndGraphics;
    float       timestampPeriod;                    // ← 我们要的：纳秒 / tick
    uint32_t    maxClipDistances;
    uint32_t    maxCullDistances;
    uint32_t    maxCombinedClipAndCullDistances;
    uint32_t    discreteQueuePriorities;
    float       pointSizeRange[2];
    float       lineWidthRange[2];
    float       pointSizeGranularity;
    float       lineWidthGranularity;
    VkBool32    strictLines;
    VkBool32    standardSampleLocations;
    VkDeviceSize optimalBufferCopyOffsetAlignment;
    VkDeviceSize optimalBufferCopyRowPitchAlignment;
    VkDeviceSize nonCoherentAtomSize;
} VkPhysicalDeviceLimits;

typedef struct VkPhysicalDeviceSparseProperties {
    VkBool32 residencyStandard2DBlockShape;
    VkBool32 residencyStandard2DMultisampleBlockShape;
    VkBool32 residencyStandard3DBlockShape;
    VkBool32 residencyAlignedMipSize;
    VkBool32 residencyNonResidentStrict;
} VkPhysicalDeviceSparseProperties;

typedef struct VkPhysicalDeviceProperties {
    uint32_t    apiVersion;
    uint32_t    driverVersion;
    uint32_t    vendorID;
    uint32_t    deviceID;
    VkPhysicalDeviceType deviceType;
    char        deviceName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    uint8_t     pipelineCacheUUID[VK_UUID_SIZE];
    VkPhysicalDeviceLimits limits;
    VkPhysicalDeviceSparseProperties sparseProperties;
} VkPhysicalDeviceProperties;

typedef struct VkQueueFamilyProperties {
    VkFlags     queueFlags;
    uint32_t    queueCount;
    uint32_t    timestampValidBits;   // 0 = 该队列族不支持时间戳
    VkExtent3D  minImageTransferGranularity;
} VkQueueFamilyProperties;

// ============================================================ device 创建
typedef struct VkDeviceQueueCreateInfo {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
    uint32_t        queueFamilyIndex;
    uint32_t        queueCount;
    const float*    pQueuePriorities;
} VkDeviceQueueCreateInfo;

typedef struct VkDeviceCreateInfo {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
    uint32_t        queueCreateInfoCount;
    const VkDeviceQueueCreateInfo* pQueueCreateInfos;
    uint32_t        enabledLayerCount;
    const char* const* ppEnabledLayerNames;
    uint32_t        enabledExtensionCount;
    const char* const* ppEnabledExtensionNames;
    const void*     pEnabledFeatures;   // 我们传 nullptr（不需要任何可选特性）
} VkDeviceCreateInfo;

// ============================================================ surface / swapchain
typedef struct VkWin32SurfaceCreateInfoKHR {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
    HINSTANCE       hinstance;
    HWND            hwnd;
} VkWin32SurfaceCreateInfoKHR;

typedef struct VkSurfaceCapabilitiesKHR {
    uint32_t     minImageCount;
    uint32_t     maxImageCount;
    VkExtent2D   currentExtent;
    VkExtent2D   minImageExtent;
    VkExtent2D   maxImageExtent;
    uint32_t     maxImageArrayLayers;
    VkFlags      supportedTransforms;
    VkFlags      currentTransform;
    VkFlags      supportedCompositeAlpha;
    VkFlags      supportedUsageFlags;
} VkSurfaceCapabilitiesKHR;

typedef struct VkSurfaceFormatKHR {
    VkFormat        format;
    VkColorSpaceKHR colorSpace;
} VkSurfaceFormatKHR;

typedef struct VkSwapchainCreateInfoKHR {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
    VkSurfaceKHR    surface;
    uint32_t        minImageCount;
    VkFormat        imageFormat;
    VkColorSpaceKHR imageColorSpace;
    VkExtent2D      imageExtent;
    uint32_t        imageArrayLayers;
    VkFlags         imageUsage;
    VkSharingMode   imageSharingMode;
    uint32_t        queueFamilyIndexCount;
    const uint32_t* pQueueFamilyIndices;
    VkFlags         preTransform;
    VkFlags         compositeAlpha;
    VkPresentModeKHR presentMode;
    VkBool32        clipped;
    VkSwapchainKHR  oldSwapchain;
} VkSwapchainCreateInfoKHR;

typedef struct VkPresentInfoKHR {
    VkStructureType sType;
    const void*     pNext;
    uint32_t        waitSemaphoreCount;
    const VkSemaphore* pWaitSemaphores;
    uint32_t        swapchainCount;
    const VkSwapchainKHR* pSwapchains;
    const uint32_t* pImageIndices;
    VkResult*       pResults;
} VkPresentInfoKHR;

// ============================================================ 命令缓冲
typedef struct VkCommandPoolCreateInfo {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
    uint32_t        queueFamilyIndex;
} VkCommandPoolCreateInfo;

typedef struct VkCommandBufferAllocateInfo {
    VkStructureType sType;
    const void*     pNext;
    VkCommandPool   commandPool;
    VkCommandBufferLevel level;
    uint32_t        commandBufferCount;
} VkCommandBufferAllocateInfo;

typedef struct VkCommandBufferBeginInfo {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
    const void*     pInheritanceInfo;
} VkCommandBufferBeginInfo;

// ============================================================ render pass
typedef struct VkAttachmentDescription {
    VkFlags         flags;
    VkFormat        format;
    VkFlags         samples;
    VkAttachmentLoadOp  loadOp;
    VkAttachmentStoreOp storeOp;
    VkAttachmentLoadOp  stencilLoadOp;
    VkAttachmentStoreOp stencilStoreOp;
    VkImageLayout   initialLayout;
    VkImageLayout   finalLayout;
} VkAttachmentDescription;

typedef struct VkAttachmentReference {
    uint32_t      attachment;
    VkImageLayout layout;
} VkAttachmentReference;

typedef struct VkSubpassDescription {
    VkFlags         flags;
    VkPipelineBindPoint pipelineBindPoint;
    uint32_t        inputAttachmentCount;
    const VkAttachmentReference* pInputAttachments;
    uint32_t        colorAttachmentCount;
    const VkAttachmentReference* pColorAttachments;
    const VkAttachmentReference* pResolveAttachments;
    const VkAttachmentReference* pDepthStencilAttachment;
    uint32_t        preserveAttachmentCount;
    const uint32_t* pPreserveAttachments;
} VkSubpassDescription;

typedef struct VkSubpassDependency {
    uint32_t    srcSubpass;
    uint32_t    dstSubpass;
    VkFlags     srcStageMask;
    VkFlags     dstStageMask;
    VkFlags     srcAccessMask;
    VkFlags     dstAccessMask;
    VkFlags     dependencyFlags;
} VkSubpassDependency;

typedef struct VkRenderPassCreateInfo {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
    uint32_t        attachmentCount;
    const VkAttachmentDescription* pAttachments;
    uint32_t        subpassCount;
    const VkSubpassDescription* pSubpasses;
    uint32_t        dependencyCount;
    const VkSubpassDependency* pDependencies;
} VkRenderPassCreateInfo;

typedef struct VkRenderPassBeginInfo {
    VkStructureType sType;
    const void*     pNext;
    VkRenderPass    renderPass;
    VkFramebuffer   framebuffer;
    VkRect2D        renderArea;
    uint32_t        clearValueCount;
    const VkClearValue* pClearValues;
} VkRenderPassBeginInfo;

typedef struct VkFramebufferCreateInfo {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
    VkRenderPass    renderPass;
    uint32_t        attachmentCount;
    const VkImageView* pAttachments;
    uint32_t        width;
    uint32_t        height;
    uint32_t        layers;
} VkFramebufferCreateInfo;

typedef struct VkImageViewCreateInfo {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
    VkImage         image;
    VkImageViewType viewType;
    VkFormat        format;
    VkComponentMapping components;
    VkImageSubresourceRange subresourceRange;
} VkImageViewCreateInfo;

// ============================================================ pipeline
typedef struct VkShaderModuleCreateInfo {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
    size_t          codeSize;   // 字节数，必须是 4 的倍数
    const uint32_t* pCode;
} VkShaderModuleCreateInfo;

typedef struct VkPipelineShaderStageCreateInfo {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
    VkFlags         stage;
    VkShaderModule  module;
    const char*     pName;      // 入口点名，SPIR-V 里 OpEntryPoint 的名字
    const void*     pSpecializationInfo;
} VkPipelineShaderStageCreateInfo;

typedef struct VkVertexInputBindingDescription {
    uint32_t        binding;
    uint32_t        stride;
    VkVertexInputRate inputRate;
} VkVertexInputBindingDescription;

typedef struct VkVertexInputAttributeDescription {
    uint32_t location;
    uint32_t binding;
    VkFormat format;
    uint32_t offset;
} VkVertexInputAttributeDescription;

typedef struct VkPipelineVertexInputStateCreateInfo {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
    uint32_t        vertexBindingDescriptionCount;
    const VkVertexInputBindingDescription* pVertexBindingDescriptions;
    uint32_t        vertexAttributeDescriptionCount;
    const VkVertexInputAttributeDescription* pVertexAttributeDescriptions;
} VkPipelineVertexInputStateCreateInfo;

typedef struct VkPipelineInputAssemblyStateCreateInfo {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
    VkPrimitiveTopology topology;
    VkBool32        primitiveRestartEnable;
} VkPipelineInputAssemblyStateCreateInfo;

typedef struct VkViewport {
    float x, y, width, height, minDepth, maxDepth;
} VkViewport;

typedef struct VkPipelineViewportStateCreateInfo {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
    uint32_t        viewportCount;
    const VkViewport* pViewports;
    uint32_t        scissorCount;
    const VkRect2D* pScissors;
} VkPipelineViewportStateCreateInfo;

typedef struct VkPipelineRasterizationStateCreateInfo {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
    VkBool32        depthClampEnable;
    VkBool32        rasterizerDiscardEnable;
    VkPolygonMode   polygonMode;
    VkFlags         cullMode;
    VkFrontFace     frontFace;
    VkBool32        depthBiasEnable;
    float           depthBiasConstantFactor;
    float           depthBiasClamp;
    float           depthBiasSlopeFactor;
    float           lineWidth;
} VkPipelineRasterizationStateCreateInfo;

typedef struct VkPipelineMultisampleStateCreateInfo {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
    VkFlags         rasterizationSamples;
    VkBool32        sampleShadingEnable;
    float           minSampleShading;
    const VkSampleMask* pSampleMask;
    VkBool32        alphaToCoverageEnable;
    VkBool32        alphaToOneEnable;
} VkPipelineMultisampleStateCreateInfo;

typedef struct VkPipelineColorBlendAttachmentState {
    VkBool32    blendEnable;
    VkBlendFactor srcColorBlendFactor;
    VkBlendFactor dstColorBlendFactor;
    VkBlendOp   colorBlendOp;
    VkBlendFactor srcAlphaBlendFactor;
    VkBlendFactor dstAlphaBlendFactor;
    VkBlendOp   alphaBlendOp;
    VkFlags     colorWriteMask;
} VkPipelineColorBlendAttachmentState;

typedef struct VkPipelineColorBlendStateCreateInfo {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
    VkBool32        logicOpEnable;
    VkLogicOp       logicOp;
    uint32_t        attachmentCount;
    const VkPipelineColorBlendAttachmentState* pAttachments;
    float           blendConstants[4];
} VkPipelineColorBlendStateCreateInfo;

typedef struct VkPipelineDynamicStateCreateInfo {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
    uint32_t        dynamicStateCount;
    const VkDynamicState* pDynamicStates;
} VkPipelineDynamicStateCreateInfo;

typedef struct VkPushConstantRange {
    VkFlags  stageFlags;
    uint32_t offset;
    uint32_t size;
} VkPushConstantRange;

typedef struct VkPipelineLayoutCreateInfo {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
    uint32_t        setLayoutCount;
    const void* const* pSetLayouts;
    uint32_t        pushConstantRangeCount;
    const VkPushConstantRange* pPushConstantRanges;
} VkPipelineLayoutCreateInfo;

typedef struct VkGraphicsPipelineCreateInfo {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
    uint32_t        stageCount;
    const VkPipelineShaderStageCreateInfo* pStages;
    const VkPipelineVertexInputStateCreateInfo* pVertexInputState;
    const VkPipelineInputAssemblyStateCreateInfo* pInputAssemblyState;
    const void*     pTessellationState;
    const VkPipelineViewportStateCreateInfo* pViewportState;
    const VkPipelineRasterizationStateCreateInfo* pRasterizationState;
    const VkPipelineMultisampleStateCreateInfo* pMultisampleState;
    const void*     pDepthStencilState;
    const VkPipelineColorBlendStateCreateInfo* pColorBlendState;
    const VkPipelineDynamicStateCreateInfo* pDynamicState;
    VkPipelineLayout layout;
    VkRenderPass    renderPass;
    uint32_t        subpass;
    VkPipeline      basePipelineHandle;
    int32_t         basePipelineIndex;
} VkGraphicsPipelineCreateInfo;

// ============================================================ 同步 / 提交 / 查询
typedef struct VkSemaphoreCreateInfo {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
} VkSemaphoreCreateInfo;

typedef struct VkFenceCreateInfo {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
} VkFenceCreateInfo;

typedef struct VkSubmitInfo {
    VkStructureType sType;
    const void*     pNext;
    uint32_t        waitSemaphoreCount;
    const VkSemaphore* pWaitSemaphores;
    const VkFlags*  pWaitDstStageMask;
    uint32_t        commandBufferCount;
    const VkCommandBuffer* pCommandBuffers;
    uint32_t        signalSemaphoreCount;
    const VkSemaphore* pSignalSemaphores;
} VkSubmitInfo;

typedef struct VkQueryPoolCreateInfo {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
    VkQueryType     queryType;
    uint32_t        queryCount;
    VkFlags         pipelineStatistics;
} VkQueryPoolCreateInfo;

// ============================================================ buffer / memory
typedef struct VkBufferCreateInfo {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
    VkDeviceSize    size;
    VkFlags         usage;
    VkSharingMode   sharingMode;
    uint32_t        queueFamilyIndexCount;
    const uint32_t* pQueueFamilyIndices;
} VkBufferCreateInfo;

typedef struct VkMemoryRequirements {
    VkDeviceSize size;
    VkDeviceSize alignment;
    uint32_t     memoryTypeBits;
} VkMemoryRequirements;

typedef struct VkMemoryAllocateInfo {
    VkStructureType sType;
    const void*     pNext;
    VkDeviceSize    allocationSize;
    uint32_t        memoryTypeIndex;
} VkMemoryAllocateInfo;

// ============================================================ 函数指针类型
// 命名遵循官方 vk_platform.h 的 PFN_vkXxx 约定，方便和官方文档对照。
typedef void* (*PFN_vkGetInstanceProcAddr)(VkInstance, const char*);
typedef void* (*PFN_vkGetDeviceProcAddr)(VkDevice, const char*);
typedef VkResult (*PFN_vkCreateInstance)(const VkInstanceCreateInfo*, const void*, VkInstance*);
typedef VkResult (*PFN_vkEnumerateInstanceLayerProperties)(uint32_t*, VkLayerProperties*);
typedef VkResult (*PFN_vkEnumerateInstanceExtensionProperties)(const char*, uint32_t*, VkExtensionProperties*);
typedef VkResult (*PFN_vkEnumerateInstanceVersion)(uint32_t*);
typedef void     (*PFN_vkDestroyInstance)(VkInstance, const void*);
typedef VkResult (*PFN_vkEnumeratePhysicalDevices)(VkInstance, uint32_t*, VkPhysicalDevice*);
typedef void     (*PFN_vkGetPhysicalDeviceProperties)(VkPhysicalDevice, VkPhysicalDeviceProperties*);
typedef void     (*PFN_vkGetPhysicalDeviceQueueFamilyProperties)(VkPhysicalDevice, uint32_t*, VkQueueFamilyProperties*);
typedef void     (*PFN_vkGetPhysicalDeviceMemoryProperties)(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties*);
typedef VkResult (*PFN_vkCreateDevice)(VkPhysicalDevice, const VkDeviceCreateInfo*, const void*, VkDevice*);
typedef void     (*PFN_vkDestroyDevice)(VkDevice, const void*);
typedef void     (*PFN_vkGetDeviceQueue)(VkDevice, uint32_t, uint32_t, VkQueue*);
typedef VkResult (*PFN_vkDeviceWaitIdle)(VkDevice);
typedef VkResult (*PFN_vkQueueWaitIdle)(VkQueue);
typedef VkResult (*PFN_vkCreateWin32SurfaceKHR)(VkInstance, const VkWin32SurfaceCreateInfoKHR*, const void*, VkSurfaceKHR*);
typedef void     (*PFN_vkDestroySurfaceKHR)(VkInstance, VkSurfaceKHR, const void*);
typedef VkResult (*PFN_vkGetPhysicalDeviceSurfaceSupportKHR)(VkPhysicalDevice, uint32_t, VkSurfaceKHR, VkBool32*);
typedef VkResult (*PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR*);
typedef VkResult (*PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)(VkPhysicalDevice, VkSurfaceKHR, uint32_t*, VkSurfaceFormatKHR*);
typedef VkResult (*PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)(VkPhysicalDevice, VkSurfaceKHR, uint32_t*, VkPresentModeKHR*);
typedef VkResult (*PFN_vkCreateSwapchainKHR)(VkDevice, const VkSwapchainCreateInfoKHR*, const void*, VkSwapchainKHR*);
typedef void     (*PFN_vkDestroySwapchainKHR)(VkDevice, VkSwapchainKHR, const void*);
typedef VkResult (*PFN_vkGetSwapchainImagesKHR)(VkDevice, VkSwapchainKHR, uint32_t*, VkImage*);
typedef VkResult (*PFN_vkAcquireNextImageKHR)(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t*);
typedef VkResult (*PFN_vkQueuePresentKHR)(VkQueue, const VkPresentInfoKHR*);
typedef VkResult (*PFN_vkQueueSubmit)(VkQueue, uint32_t, const VkSubmitInfo*, VkFence);
typedef VkResult (*PFN_vkCreateCommandPool)(VkDevice, const VkCommandPoolCreateInfo*, const void*, VkCommandPool*);
typedef void     (*PFN_vkDestroyCommandPool)(VkDevice, VkCommandPool, const void*);
typedef VkResult (*PFN_vkAllocateCommandBuffers)(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer*);
typedef VkResult (*PFN_vkResetCommandBuffer)(VkCommandBuffer, VkFlags);
typedef VkResult (*PFN_vkBeginCommandBuffer)(VkCommandBuffer, const VkCommandBufferBeginInfo*);
typedef VkResult (*PFN_vkEndCommandBuffer)(VkCommandBuffer);
typedef void     (*PFN_vkCmdBeginRenderPass)(VkCommandBuffer, const VkRenderPassBeginInfo*, VkSubpassContents);
typedef void     (*PFN_vkCmdEndRenderPass)(VkCommandBuffer);
typedef void     (*PFN_vkCmdBindPipeline)(VkCommandBuffer, VkPipelineBindPoint, VkPipeline);
typedef void     (*PFN_vkCmdSetViewport)(VkCommandBuffer, uint32_t, uint32_t, const VkViewport*);
typedef void     (*PFN_vkCmdSetScissor)(VkCommandBuffer, uint32_t, uint32_t, const VkRect2D*);
typedef void     (*PFN_vkCmdDraw)(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t);
typedef void     (*PFN_vkCmdBindVertexBuffers)(VkCommandBuffer, uint32_t, uint32_t, const VkBuffer*, const VkDeviceSize*);
typedef void     (*PFN_vkCmdPushConstants)(VkCommandBuffer, VkPipelineLayout, VkFlags, uint32_t, uint32_t, const void*);
typedef void     (*PFN_vkCmdResetQueryPool)(VkCommandBuffer, VkQueryPool, uint32_t, uint32_t);
typedef void     (*PFN_vkCmdWriteTimestamp)(VkCommandBuffer, VkFlags, VkQueryPool, uint32_t);
typedef VkResult (*PFN_vkCreateQueryPool)(VkDevice, const VkQueryPoolCreateInfo*, const void*, VkQueryPool*);
typedef void     (*PFN_vkDestroyQueryPool)(VkDevice, VkQueryPool, const void*);
typedef VkResult (*PFN_vkGetQueryPoolResults)(VkDevice, VkQueryPool, uint32_t, uint32_t, size_t, void*, VkDeviceSize, VkFlags);
typedef VkResult (*PFN_vkCreateBuffer)(VkDevice, const VkBufferCreateInfo*, const void*, VkBuffer*);
typedef void     (*PFN_vkDestroyBuffer)(VkDevice, VkBuffer, const void*);
typedef void     (*PFN_vkGetBufferMemoryRequirements)(VkDevice, VkBuffer, VkMemoryRequirements*);
typedef VkResult (*PFN_vkAllocateMemory)(VkDevice, const VkMemoryAllocateInfo*, const void*, VkDeviceMemory*);
typedef void     (*PFN_vkFreeMemory)(VkDevice, VkDeviceMemory, const void*);
typedef VkResult (*PFN_vkBindBufferMemory)(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize);
typedef VkResult (*PFN_vkMapMemory)(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkFlags, void**);
typedef void     (*PFN_vkUnmapMemory)(VkDevice, VkDeviceMemory);
typedef VkResult (*PFN_vkCreateImageView)(VkDevice, const VkImageViewCreateInfo*, const void*, VkImageView*);
typedef void     (*PFN_vkDestroyImageView)(VkDevice, VkImageView, const void*);
typedef VkResult (*PFN_vkCreateRenderPass)(VkDevice, const VkRenderPassCreateInfo*, const void*, VkRenderPass*);
typedef void     (*PFN_vkDestroyRenderPass)(VkDevice, VkRenderPass, const void*);
typedef VkResult (*PFN_vkCreateFramebuffer)(VkDevice, const VkFramebufferCreateInfo*, const void*, VkFramebuffer*);
typedef void     (*PFN_vkDestroyFramebuffer)(VkDevice, VkFramebuffer, const void*);
typedef VkResult (*PFN_vkCreateShaderModule)(VkDevice, const VkShaderModuleCreateInfo*, const void*, VkShaderModule*);
typedef void     (*PFN_vkDestroyShaderModule)(VkDevice, VkShaderModule, const void*);
typedef VkResult (*PFN_vkCreatePipelineLayout)(VkDevice, const VkPipelineLayoutCreateInfo*, const void*, VkPipelineLayout*);
typedef void     (*PFN_vkDestroyPipelineLayout)(VkDevice, VkPipelineLayout, const void*);
typedef VkResult (*PFN_vkCreateGraphicsPipelines)(VkDevice, VkPipeline, uint32_t, const VkGraphicsPipelineCreateInfo*, const void*, VkPipeline*);
typedef void     (*PFN_vkDestroyPipeline)(VkDevice, VkPipeline, const void*);
typedef VkResult (*PFN_vkCreateSemaphore)(VkDevice, const VkSemaphoreCreateInfo*, const void*, VkSemaphore*);
typedef void     (*PFN_vkDestroySemaphore)(VkDevice, VkSemaphore, const void*);
typedef VkResult (*PFN_vkCreateFence)(VkDevice, const VkFenceCreateInfo*, const void*, VkFence*);
typedef void     (*PFN_vkDestroyFence)(VkDevice, VkFence, const void*);
typedef VkResult (*PFN_vkWaitForFences)(VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t);
typedef VkResult (*PFN_vkResetFences)(VkDevice, uint32_t, const VkFence*);

// ============================================================ 动态加载封装
// 三级加载，顺序不能乱：
//   1) LoadDll()       —— 从 vulkan-1.dll 拿 vkGetInstanceProcAddr + 全局函数
//   2) LoadInstance()  —— 从 vkGetInstanceProcAddr(instance, ...) 拿 instance 级
//   3) LoadDevice()    —— 从 vkGetDeviceProcAddr(device, ...) 拿 device 级
// 第 3 步是重点：真实游戏就是这么取 vkQueuePresentKHR 的，也只有这样拿到
// 的才是「层链最外层」的函数指针 —— 将来测 NextPerf 的 Vulkan 层时，
// 这一步拿到的到底是不是层的包装函数，正是我们要验证的东西。
struct VkApi {
    HMODULE dll = nullptr;

    // ---- 1) 全局级
    PFN_vkGetInstanceProcAddr                 vkGetInstanceProcAddr = nullptr;
    PFN_vkCreateInstance                      vkCreateInstance = nullptr;
    PFN_vkEnumerateInstanceLayerProperties    vkEnumerateInstanceLayerProperties = nullptr;
    PFN_vkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties = nullptr;
    PFN_vkEnumerateInstanceVersion            vkEnumerateInstanceVersion = nullptr; // 1.1+ 才有，可空

    // ---- 2) instance 级
    PFN_vkGetDeviceProcAddr                       vkGetDeviceProcAddr = nullptr;
    PFN_vkDestroyInstance                         vkDestroyInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices                vkEnumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceProperties             vkGetPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties  vkGetPhysicalDeviceQueueFamilyProperties = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties       vkGetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkCreateDevice                            vkCreateDevice = nullptr;
    PFN_vkCreateWin32SurfaceKHR                   vkCreateWin32SurfaceKHR = nullptr;
    PFN_vkDestroySurfaceKHR                       vkDestroySurfaceKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR      vkGetPhysicalDeviceSurfaceSupportKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR vkGetPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR      vkGetPhysicalDeviceSurfaceFormatsKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR vkGetPhysicalDeviceSurfacePresentModesKHR = nullptr;

    // ---- 3) device 级
    PFN_vkDestroyDevice            vkDestroyDevice = nullptr;
    PFN_vkGetDeviceQueue           vkGetDeviceQueue = nullptr;
    PFN_vkDeviceWaitIdle           vkDeviceWaitIdle = nullptr;
    PFN_vkQueueWaitIdle            vkQueueWaitIdle = nullptr;
    PFN_vkCreateSwapchainKHR       vkCreateSwapchainKHR = nullptr;
    PFN_vkDestroySwapchainKHR      vkDestroySwapchainKHR = nullptr;
    PFN_vkGetSwapchainImagesKHR    vkGetSwapchainImagesKHR = nullptr;
    PFN_vkAcquireNextImageKHR      vkAcquireNextImageKHR = nullptr;
    PFN_vkQueuePresentKHR          vkQueuePresentKHR = nullptr;
    PFN_vkQueueSubmit              vkQueueSubmit = nullptr;
    PFN_vkCreateCommandPool        vkCreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool       vkDestroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers   vkAllocateCommandBuffers = nullptr;
    PFN_vkResetCommandBuffer       vkResetCommandBuffer = nullptr;
    PFN_vkBeginCommandBuffer       vkBeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer         vkEndCommandBuffer = nullptr;
    PFN_vkCmdBeginRenderPass       vkCmdBeginRenderPass = nullptr;
    PFN_vkCmdEndRenderPass         vkCmdEndRenderPass = nullptr;
    PFN_vkCmdBindPipeline          vkCmdBindPipeline = nullptr;
    PFN_vkCmdSetViewport           vkCmdSetViewport = nullptr;
    PFN_vkCmdSetScissor            vkCmdSetScissor = nullptr;
    PFN_vkCmdDraw                  vkCmdDraw = nullptr;
    PFN_vkCmdBindVertexBuffers     vkCmdBindVertexBuffers = nullptr;
    PFN_vkCmdPushConstants         vkCmdPushConstants = nullptr;
    PFN_vkCmdResetQueryPool        vkCmdResetQueryPool = nullptr;
    PFN_vkCmdWriteTimestamp        vkCmdWriteTimestamp = nullptr;
    PFN_vkCreateQueryPool          vkCreateQueryPool = nullptr;
    PFN_vkDestroyQueryPool         vkDestroyQueryPool = nullptr;
    PFN_vkGetQueryPoolResults      vkGetQueryPoolResults = nullptr;
    PFN_vkCreateBuffer             vkCreateBuffer = nullptr;
    PFN_vkDestroyBuffer            vkDestroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements = nullptr;
    PFN_vkAllocateMemory           vkAllocateMemory = nullptr;
    PFN_vkFreeMemory               vkFreeMemory = nullptr;
    PFN_vkBindBufferMemory         vkBindBufferMemory = nullptr;
    PFN_vkMapMemory                vkMapMemory = nullptr;
    PFN_vkUnmapMemory              vkUnmapMemory = nullptr;
    PFN_vkCreateImageView          vkCreateImageView = nullptr;
    PFN_vkDestroyImageView         vkDestroyImageView = nullptr;
    PFN_vkCreateRenderPass         vkCreateRenderPass = nullptr;
    PFN_vkDestroyRenderPass        vkDestroyRenderPass = nullptr;
    PFN_vkCreateFramebuffer        vkCreateFramebuffer = nullptr;
    PFN_vkDestroyFramebuffer       vkDestroyFramebuffer = nullptr;
    PFN_vkCreateShaderModule       vkCreateShaderModule = nullptr;
    PFN_vkDestroyShaderModule      vkDestroyShaderModule = nullptr;
    PFN_vkCreatePipelineLayout     vkCreatePipelineLayout = nullptr;
    PFN_vkDestroyPipelineLayout    vkDestroyPipelineLayout = nullptr;
    PFN_vkCreateGraphicsPipelines  vkCreateGraphicsPipelines = nullptr;
    PFN_vkDestroyPipeline          vkDestroyPipeline = nullptr;
    PFN_vkCreateSemaphore          vkCreateSemaphore = nullptr;
    PFN_vkDestroySemaphore         vkDestroySemaphore = nullptr;
    PFN_vkCreateFence              vkCreateFence = nullptr;
    PFN_vkDestroyFence             vkDestroyFence = nullptr;
    PFN_vkWaitForFences            vkWaitForFences = nullptr;
    PFN_vkResetFences              vkResetFences = nullptr;

    // 记录加载过程中到底哪些函数没拿到，便于报错时定位
    const char* firstMiss = nullptr;

    // ---- 第 1 步
    bool LoadDll() {
        // 优先用系统目录里的 loader。不写全路径，让 Windows 的 DLL 搜索
        // 顺序决定 —— 这样如果用户把某个自定义 loader 放在 exe 旁边，
        // 也能被用上（调试层链时偶尔需要）。
        dll = ::LoadLibraryW(L"vulkan-1.dll");
        if (!dll) return false;

        vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)
            (void*)::GetProcAddress(dll, "vkGetInstanceProcAddr");
        if (!vkGetInstanceProcAddr) return false;

        // vkGetInstanceProcAddr(NULL, ...) 是规范定义的「取全局命令」的方式，
        // 比直接 GetProcAddress 更正确：某些 loader 会在这里做层协商。
        vkCreateInstance = (PFN_vkCreateInstance)
            vkGetInstanceProcAddr(nullptr, "vkCreateInstance");
        vkEnumerateInstanceLayerProperties = (PFN_vkEnumerateInstanceLayerProperties)
            vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceLayerProperties");
        vkEnumerateInstanceExtensionProperties = (PFN_vkEnumerateInstanceExtensionProperties)
            vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceExtensionProperties");
        // vkEnumerateInstanceVersion 是 1.1 才有的，1.0 loader 上取不到是正常的
        vkEnumerateInstanceVersion = (PFN_vkEnumerateInstanceVersion)
            vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion");

        return vkCreateInstance && vkEnumerateInstanceLayerProperties;
    }

    // ---- 第 2 步
    bool LoadInstance(VkInstance instance) {
        #define NP_VK_GI(name)                                                        \
            name = (PFN_##name)vkGetInstanceProcAddr(instance, #name);                 \
            if (!name && !firstMiss) firstMiss = #name;
        NP_VK_GI(vkGetDeviceProcAddr)
        NP_VK_GI(vkDestroyInstance)
        NP_VK_GI(vkEnumeratePhysicalDevices)
        NP_VK_GI(vkGetPhysicalDeviceProperties)
        NP_VK_GI(vkGetPhysicalDeviceQueueFamilyProperties)
        NP_VK_GI(vkGetPhysicalDeviceMemoryProperties)
        NP_VK_GI(vkCreateDevice)
        NP_VK_GI(vkCreateWin32SurfaceKHR)
        NP_VK_GI(vkDestroySurfaceKHR)
        NP_VK_GI(vkGetPhysicalDeviceSurfaceSupportKHR)
        NP_VK_GI(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)
        NP_VK_GI(vkGetPhysicalDeviceSurfaceFormatsKHR)
        NP_VK_GI(vkGetPhysicalDeviceSurfacePresentModesKHR)
        #undef NP_VK_GI
        // 注意：vkGetDeviceProcAddr 和 vkCreateDevice 拿到之后才能进入第 3 步
        return vkGetDeviceProcAddr && vkCreateDevice && vkEnumeratePhysicalDevices &&
               vkCreateWin32SurfaceKHR && vkGetPhysicalDeviceSurfacePresentModesKHR;
    }

    // ---- 第 3 步
    bool LoadDevice(VkDevice device) {
        #define NP_VK_GD(name)                                                        \
            name = (PFN_##name)vkGetDeviceProcAddr(device, #name);                     \
            if (!name && !firstMiss) firstMiss = #name;
        NP_VK_GD(vkDestroyDevice)
        NP_VK_GD(vkGetDeviceQueue)
        NP_VK_GD(vkDeviceWaitIdle)
        NP_VK_GD(vkQueueWaitIdle)
        NP_VK_GD(vkCreateSwapchainKHR)
        NP_VK_GD(vkDestroySwapchainKHR)
        NP_VK_GD(vkGetSwapchainImagesKHR)
        NP_VK_GD(vkAcquireNextImageKHR)
        NP_VK_GD(vkQueuePresentKHR)
        NP_VK_GD(vkQueueSubmit)
        NP_VK_GD(vkCreateCommandPool)
        NP_VK_GD(vkDestroyCommandPool)
        NP_VK_GD(vkAllocateCommandBuffers)
        NP_VK_GD(vkResetCommandBuffer)
        NP_VK_GD(vkBeginCommandBuffer)
        NP_VK_GD(vkEndCommandBuffer)
        NP_VK_GD(vkCmdBeginRenderPass)
        NP_VK_GD(vkCmdEndRenderPass)
        NP_VK_GD(vkCmdBindPipeline)
        NP_VK_GD(vkCmdSetViewport)
        NP_VK_GD(vkCmdSetScissor)
        NP_VK_GD(vkCmdDraw)
        NP_VK_GD(vkCmdBindVertexBuffers)
        NP_VK_GD(vkCmdPushConstants)
        NP_VK_GD(vkCmdResetQueryPool)
        NP_VK_GD(vkCmdWriteTimestamp)
        NP_VK_GD(vkCreateQueryPool)
        NP_VK_GD(vkDestroyQueryPool)
        NP_VK_GD(vkGetQueryPoolResults)
        NP_VK_GD(vkCreateBuffer)
        NP_VK_GD(vkDestroyBuffer)
        NP_VK_GD(vkGetBufferMemoryRequirements)
        NP_VK_GD(vkAllocateMemory)
        NP_VK_GD(vkFreeMemory)
        NP_VK_GD(vkBindBufferMemory)
        NP_VK_GD(vkMapMemory)
        NP_VK_GD(vkUnmapMemory)
        NP_VK_GD(vkCreateImageView)
        NP_VK_GD(vkDestroyImageView)
        NP_VK_GD(vkCreateRenderPass)
        NP_VK_GD(vkDestroyRenderPass)
        NP_VK_GD(vkCreateFramebuffer)
        NP_VK_GD(vkDestroyFramebuffer)
        NP_VK_GD(vkCreateShaderModule)
        NP_VK_GD(vkDestroyShaderModule)
        NP_VK_GD(vkCreatePipelineLayout)
        NP_VK_GD(vkDestroyPipelineLayout)
        NP_VK_GD(vkCreateGraphicsPipelines)
        NP_VK_GD(vkDestroyPipeline)
        NP_VK_GD(vkCreateSemaphore)
        NP_VK_GD(vkDestroySemaphore)
        NP_VK_GD(vkCreateFence)
        NP_VK_GD(vkDestroyFence)
        NP_VK_GD(vkWaitForFences)
        NP_VK_GD(vkResetFences)
        #undef NP_VK_GD
        // 这几个是渲染循环的命脉，缺一个就没法跑
        return vkQueuePresentKHR && vkAcquireNextImageKHR && vkCreateSwapchainKHR &&
               vkQueueSubmit && vkCmdDraw && vkCreateGraphicsPipelines &&
               vkCmdWriteTimestamp && vkGetQueryPoolResults;
    }

    void Unload() {
        if (dll) { ::FreeLibrary(dll); dll = nullptr; }
    }
};

// ============================================================ 布局自检
// 这些 sizeof 是官方头文件在 LP64/LLP64（Windows x64）下的确定值。
// 只要我们对不上，说明手写声明抄错了 —— 编译期就拦住，别等到运行时
// 读出一个垃圾 timestampPeriod 才发现。
static_assert(sizeof(VkPhysicalDeviceLimits) == 504,
              "VkPhysicalDeviceLimits 布局与官方头文件不一致（应为 504 字节）");
static_assert(sizeof(VkPhysicalDeviceProperties) == 824,
              "VkPhysicalDeviceProperties 布局与官方头文件不一致（应为 824 字节）");
static_assert(sizeof(VkPhysicalDeviceMemoryProperties) == 520,
              "VkPhysicalDeviceMemoryProperties 布局与官方头文件不一致（应为 520 字节）");
static_assert(sizeof(VkQueueFamilyProperties) == 24, "VkQueueFamilyProperties 应为 24 字节");
static_assert(sizeof(VkApplicationInfo) == 48, "VkApplicationInfo 应为 48 字节");
static_assert(sizeof(VkInstanceCreateInfo) == 64, "VkInstanceCreateInfo 应为 64 字节");
static_assert(sizeof(VkDeviceCreateInfo) == 72, "VkDeviceCreateInfo 应为 72 字节");
static_assert(sizeof(VkDeviceQueueCreateInfo) == 40, "VkDeviceQueueCreateInfo 应为 40 字节");
static_assert(sizeof(VkSwapchainCreateInfoKHR) == 104, "VkSwapchainCreateInfoKHR 应为 104 字节");
static_assert(sizeof(VkPresentInfoKHR) == 64, "VkPresentInfoKHR 应为 64 字节");
static_assert(sizeof(VkSurfaceCapabilitiesKHR) == 52, "VkSurfaceCapabilitiesKHR 应为 52 字节");
static_assert(sizeof(VkAttachmentDescription) == 36, "VkAttachmentDescription 应为 36 字节");
static_assert(sizeof(VkSubpassDescription) == 72, "VkSubpassDescription 应为 72 字节");
static_assert(sizeof(VkSubpassDependency) == 28, "VkSubpassDependency 应为 28 字节");
static_assert(sizeof(VkRenderPassCreateInfo) == 64, "VkRenderPassCreateInfo 应为 64 字节");
static_assert(sizeof(VkRenderPassBeginInfo) == 64, "VkRenderPassBeginInfo 应为 64 字节");
static_assert(sizeof(VkFramebufferCreateInfo) == 64, "VkFramebufferCreateInfo 应为 64 字节");
static_assert(sizeof(VkImageViewCreateInfo) == 80, "VkImageViewCreateInfo 应为 80 字节");
static_assert(sizeof(VkShaderModuleCreateInfo) == 40, "VkShaderModuleCreateInfo 应为 40 字节");
static_assert(sizeof(VkPipelineShaderStageCreateInfo) == 48, "VkPipelineShaderStageCreateInfo 应为 48 字节");
static_assert(sizeof(VkPipelineVertexInputStateCreateInfo) == 48, "VkPipelineVertexInputStateCreateInfo 应为 48 字节");
static_assert(sizeof(VkPipelineInputAssemblyStateCreateInfo) == 32, "VkPipelineInputAssemblyStateCreateInfo 应为 32 字节");
static_assert(sizeof(VkPipelineViewportStateCreateInfo) == 48, "VkPipelineViewportStateCreateInfo 应为 48 字节");
static_assert(sizeof(VkPipelineRasterizationStateCreateInfo) == 64, "VkPipelineRasterizationStateCreateInfo 应为 64 字节");
static_assert(sizeof(VkPipelineMultisampleStateCreateInfo) == 48, "VkPipelineMultisampleStateCreateInfo 应为 48 字节");
static_assert(sizeof(VkPipelineColorBlendStateCreateInfo) == 56, "VkPipelineColorBlendStateCreateInfo 应为 56 字节");
static_assert(sizeof(VkPipelineDynamicStateCreateInfo) == 32, "VkPipelineDynamicStateCreateInfo 应为 32 字节");
static_assert(sizeof(VkPipelineLayoutCreateInfo) == 48, "VkPipelineLayoutCreateInfo 应为 48 字节");
static_assert(sizeof(VkGraphicsPipelineCreateInfo) == 144, "VkGraphicsPipelineCreateInfo 应为 144 字节");
static_assert(sizeof(VkSubmitInfo) == 72, "VkSubmitInfo 应为 72 字节");
static_assert(sizeof(VkQueryPoolCreateInfo) == 32, "VkQueryPoolCreateInfo 应为 32 字节");
static_assert(sizeof(VkBufferCreateInfo) == 56, "VkBufferCreateInfo 应为 56 字节");
static_assert(sizeof(VkMemoryRequirements) == 24, "VkMemoryRequirements 应为 24 字节");
static_assert(sizeof(VkMemoryAllocateInfo) == 32, "VkMemoryAllocateInfo 应为 32 字节");
static_assert(sizeof(VkCommandBufferAllocateInfo) == 32, "VkCommandBufferAllocateInfo 应为 32 字节");
static_assert(sizeof(VkCommandBufferBeginInfo) == 32, "VkCommandBufferBeginInfo 应为 32 字节");
static_assert(sizeof(VkCommandPoolCreateInfo) == 24, "VkCommandPoolCreateInfo 应为 24 字节");
static_assert(sizeof(VkWin32SurfaceCreateInfoKHR) == 40, "VkWin32SurfaceCreateInfoKHR 应为 40 字节");
static_assert(sizeof(VkLayerProperties) == 520, "VkLayerProperties 应为 520 字节");
static_assert(sizeof(VkClearValue) == 16, "VkClearValue 应为 16 字节");
