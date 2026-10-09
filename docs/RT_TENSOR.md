# RT Core / Tensor Core 负载测量 —— 完整技术说明

这是 NextPerf 最有价值、也最容易被"糊弄"的两个指标，所以单独写一篇，把能做什么、
不能做什么、数字是怎么来的全部讲清楚。

---

## 1. 坏消息：硬件计数器在民用卡上不存在

先确保我们不在自欺欺人。逐个排查过：

| 接口 | 有没有 RT / Tensor 占用率 |
| --- | --- |
| **NVAPI** `NvAPI_GPU_GetDynamicPstatesInfoEx` | ❌。官方只定义了 4 个域：图形引擎(GPU)、显存控制器(FB)、视频引擎(VID)、PCIe 总线(BUS)。`NVAPI_MAX_GPU_UTILIZATIONS` 是 8，剩下 4 个域在驱动里通常 `bIsPresent = 0` |
| **NVML** | ❌。GeForce 上只暴露 gpu / memory / encoder / decoder 四类占用率 |
| **DCGM** `DCGM_FI_PROF_TENSOR_ACTIVE` | ⚠️ 只面向数据中心卡（Tesla / Quadro），且 profiling 模式会和其它工具抢计数器 |
| **AMD AGS / ADL** | ❌ 完全没有 |
| **Intel** | ❌ 民用驱动不暴露 |
| **Windows ETW / DxgKrnl** | ❌ 只到"GPU Engine"粒度（3D / Copy / VideoDecode / VideoEncode …） |

结论：**任何在 GeForce 上声称"直接读硬件 RT 占用率"的工具，要么在做推断，要么在编。**

---

## 2. NextPerf 的做法

分三层，优先级从高到低，并且**在面板上标注这个数是实测还是估算**。

### 第一层：引擎 pass 级实测（默认路径）

光追在 D3D12 上走 DXR，对应的入口就两个：

```cpp
ID3D12GraphicsCommandList4::DispatchRays(...)                       // 发射光线
ID3D12GraphicsCommandList4::BuildRaytracingAccelerationStructure(...) // 构建 BVH
```

钩子劫持这两个函数，在调用前后各插一条 **D3D12 TIMESTAMP 查询**：

```cpp
void NpDispatchRays(ID3D12GraphicsCommandList4* cl, const D3D12_DISPATCH_RAYS_DESC* d) {
    uint32_t slot = TS_RT_BASE + gRtDispatches * 2;
    cl->EndQuery(gHeap, D3D12_QUERY_TYPE_TIMESTAMP, slot);      // RT pass 开始
    gOrigDR(cl, d);                                              // 原始调用
    cl->EndQuery(gHeap, D3D12_QUERY_TYPE_TIMESTAMP, slot + 1);  // RT pass 结束
    ++gRtDispatches;
}
```

因为命令列表此时正处于 recording 状态，往里插 `EndQuery` 是完全合法的。
一帧最多记录 8 组（32 个时间戳槽位的前 2 个给帧首帧尾、18/19 给 AI、其余给 RT）。

同一帧里还会在 `ExecuteCommandLists` 前和 `Present` 前各插一条时间戳，
得到本帧 GPU 总耗时。于是：

```
RT Core 负载 % = Σ(每组 RT pass 的 GPU 耗时) / 本帧 GPU 总耗时 × 100
```

**分子分母都是硬件时间戳实测**，精度取决于 GPU 时间戳分辨率（通常 ~10ns 量级）。

### 第二层：AI pass 实测 + 模块识别

超分 / 帧生成的共同特征是：**发生在所有几何绘制之后**。钩子利用这一点：

```cpp
void NpDispatch(ID3D12GraphicsCommandList* cl, UINT x, UINT y, UINT z) {
    if (gDraws > 0 && !gAiSpanOpen) {                    // 已经画过几何体了
        cl->EndQuery(gHeap, TIMESTAMP, TS_AI_START);     // AI 阶段起点
        gAiSpanOpen = true;
    }
    gOrigDispatch(cl, x, y, z);
    if (gAiSpanOpen)
        cl->EndQuery(gHeap, TIMESTAMP, TS_AI_END);       // 终点随每次 dispatch 后移
}
```

同时扫进程模块，把识别到的 AI 技术列在面板上：

| 模块 | 技术 |
| --- | --- |
| `nvngx_dlss.dll` / `nvngx_dlsssr.dll` | DLSS 超分（跑在 Tensor Core 上） |
| `nvngx_dlssd.dll` | DLSS 光线重建（Tensor Core） |
| `nvngx_dlssg.dll` | DLSS 帧生成（光流加速器 OFA） |
| `libxess.dll` / `igxess.dll` | Intel XeSS（XMX 或 DP4a） |
| `amd_fidelityfx_dx12.dll` / `_vk.dll` | AMD FSR |
| `DirectML.dll` | Windows 上的通用推理（很多 AI 画面增强走它） |
| `onnxruntime.dll` | ONNX Runtime |

**判定规则**：
* 量到了 AI 阶段的 GPU 时间 → 面板显示 `xx%（实测）`
* 只识别到模块、量不到时间（比如 D3D11 游戏、或者 AI pass 与普通 compute 混在一起）
  → 显示 `AI 已启用 · 估算中`，明确告诉你别全信

### 第三层：硬件计数器（一旦开放自动生效）

程序始终在做下面三件事，任何一件成功，面板上的来源就会从"实测/估算"变成"硬件"：

1. **NVAPI 未公开域 4..7**：
   `NV_GPU_DYNAMIC_PSTATES_INFO_EX.utilization[]` 一共 8 项，官方只用了前 4 项。
   NextPerf 每一轮都读后 4 项，只要 `bIsPresent = 1` 且数值在 0..100 之间就采信，
   并把"发现了扩展域"标在数据源文字里（方便将来判断这些域到底是什么）。
2. **HWiNFO / LibreHardwareMonitor** 里名字含 `Tensor` / `RT Core` 的读数。
3. **DCGM** profiling 字段（数据中心卡）。

---

## 3. 怎么用这两个数调画质

RT 负载和 Tensor 负载配合 GPU 帧延迟，可以很直接地判断瓶颈：

| 现象 | 判断 | 该动什么 |
| --- | --- | --- |
| GPU 帧延迟 ≈ CPU 帧延迟，GPU 占用 ~99%，**RT 负载 40%+** | 光追吃满了 | 降光追档位 / 降反射分辨率，收益最明显 |
| GPU 帧延迟 ≈ CPU 帧延迟，GPU 占用 ~99%，**RT 负载 <5%** | 瓶颈在传统光栅化 | 降阴影 / 体积光 / 后处理，光追可以继续开高 |
| **CPU 帧延迟 >> GPU 帧延迟**，GPU 占用 70% 上下 | CPU 侧瓶颈 | 调画质没用，去看 Draw 数和 CPU 占用 |
| **Tensor 负载很高** + 开了 DLSS/FSR | AI 管线在吃算力 | 关掉帧生成、或者降一档超分质量（Quality→Balanced） |
| 显示器刷新率没跑满，但 GPU 占用已经 99% | 要么 CPU 瓶颈，要么引擎锁帧 | 看 CPU 帧延迟 |

**关键点是"GPU 占用率 99%"本身没有信息量**——它只说明 GPU 在忙，不说明忙在什么单元上。
RT / Tensor 负载就是把这 99% 拆开看的工具。

---

## 4. 误差来源（诚实清单）

* **RT 时间戳把整段 DispatchRays 都算作 RT**：如果引擎在两次 DispatchRays 之间插了别的工作，
  这部分会被算进 RT 时间。一帧超过 8 次 RT dispatch 时只记前 8 组。
* **AI 区间是"第一次 draw 之后的 compute 到最后一次 compute"**，中间若夹了普通 compute 会被算进来。
* **D3D12 的帧时间戳量的是"GPU 队列上从帧首到帧尾的跨度"**，如果 GPU 中途空等过 CPU，
  等待时间也会被计进去（这一点和 PresentMon 的 GPUBusy / GPUWait 划分是同一类问题）。
* **D3D11 游戏拿不到 RT 实测**（DXR 只在 D3D12 上可用）。
* **Vulkan / OpenGL 游戏**目前完全没有引擎级数据。

这些都写在面板的数值标签里（"实测" / "估算" / "需注入" / "未检测到"），
**不会用一个看起来很确定、实际是猜的数字糊弄人**。
