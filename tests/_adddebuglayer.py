# -*- coding: utf-8 -*-
"""两处小改动，让 D3D12 验证层把真话吐到我们能读的地方。

## 设计（比上一版简单，且不需要动模拟器的头文件）
1. **模拟器**：创建设备前 `D3D12GetDebugInterface` + `EnableDebugLayer`（3 行）。
   拿不到就打印说明（系统没装"图形工具"），不影响正常跑。
2. **钩子**：在 D3D12 的 Present 路径里，从 `gDev12` 拿 `ID3D12InfoQueue`，
   把**新出现的**验证消息写进钩子日志（带条数上限，避免刷屏）。
   这样导出的日志里直接就有"哪一次调用错在哪"。

## 为什么要这么做
这份代码里 D3D12 的用法如果有错，验证层是唯一能直接说清楚的工具；
我之前一直靠排除法，慢且容易得出假结论。
"""
import io
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIM = os.path.join(ROOT, r"tests\sim\sim_dx12.cpp")
HOOK = os.path.join(ROOT, r"src\hook\np_hook.cpp")

# ---------------- 1) 模拟器：开 Debug Layer ----------------
sim = io.open(SIM, encoding="utf-8").read()
if "EnableDebugLayer" in sim:
    print("  模拟器: 已有 Debug Layer")
else:
    A = "        if (SUCCEEDED(D3D12CreateDevice(chosen.Get(), D3D_FEATURE_LEVEL_11_0,"
    if A not in sim:
        print("  !! 模拟器锚点没找到")
        sys.exit(1)
    B = """        // ★ 诊断：开 D3D12 Debug Layer（必须在创建设备**之前**）。
        //   钩子侧会从设备上取 InfoQueue，把验证消息写进钩子日志 ——
        //   这样"哪一次 D3D12 调用用错了"就能直接读出来，不必靠排除法猜。
        {
            ID3D12Debug* dbg = nullptr;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg))) && dbg) {
                dbg->EnableDebugLayer();
                dbg->Release();
                SimLog("D3D12 Debug Layer 已启用");
            } else {
                SimLog("D3D12 Debug Layer 不可用（未安装图形工具），继续");
            }
        }
        if (SUCCEEDED(D3D12CreateDevice(chosen.Get(), D3D_FEATURE_LEVEL_11_0,"""
    sim = sim.replace(A, B, 1)
    io.open(SIM, "w", encoding="utf-8", newline="").write(sim)
    print("  模拟器: 已加 Debug Layer")

# ---------------- 2) 钩子：转储 InfoQueue ----------------
hook = io.open(HOOK, encoding="utf-8").read()
if "InfoQueue" in hook:
    print("  钩子: 已有 InfoQueue 转储")
else:
    A2 = "                    ID3D12GraphicsCommandList* l = stateOk ? BeginList() : nullptr;"
    if A2 not in hook:
        print("  !! 钩子锚点没找到")
        sys.exit(1)
    B2 = """                    // ★ 诊断：把 D3D12 验证层新产生的消息写进钩子日志。
                    //   调用有没有用错，验证层说得最准 —— 不用再靠排除法猜。
                    {
                        static ID3D12InfoQueue* sIq = nullptr;
                        static UINT64 sSeen = 0;
                        static int sDumped = 0;
                        if (!sIq) {
                            if (FAILED(gDev12->QueryInterface(IID_PPV_ARGS(&sIq)))) sIq = nullptr;
                        }
                        if (sIq && sDumped < 40) {
                            UINT64 n = sIq->GetNumStoredMessages();
                            if (n > sSeen) {
                                UINT64 from = sSeen;
                                sSeen = n;
                                for (UINT64 i = from; i < n && sDumped < 40; ++i, ++sDumped) {
                                    SIZE_T len = 0;
                                    if (FAILED(sIq->GetMessage(i, nullptr, &len)) || !len) continue;
                                    char* buf = (char*)malloc(len);
                                    if (!buf) break;
                                    if (SUCCEEDED(sIq->GetMessage(i, (D3D12_MESSAGE*)buf, &len))) {
                                        D3D12_MESSAGE* m = (D3D12_MESSAGE*)buf;
                                        Log("D3D12 validation[%u] %s: %s",
                                            (unsigned)m->Severity, m->pID
                                                ? m->pID : "?", m->pDescription ? m->pDescription : "");
                                    }
                                    free(buf);
                                }
                            }
                        }
                    }
                    ID3D12GraphicsCommandList* l = stateOk ? BeginList() : nullptr;"""
    hook = hook.replace(A2, B2, 1)
    io.open(HOOK, "w", encoding="utf-8", newline="").write(hook)
    print("  钩子: 已加 InfoQueue 转储")
