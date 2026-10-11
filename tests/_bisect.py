# -*- coding: utf-8 -*-
"""可复用的二分实验脚本（放在工作区内，Temp 目录被沙箱禁止写入）。

用法:  python tests\\_bisect.py <实验名>

已确立的结论（别重复）：
  A  去掉整个 Record 调用                -> 不再崩 ✓ 问题在 D3D12 录制里
  B  去掉 L571-595（状态设置 + draw）    -> 不再崩 ✓ 锁定【状态污染】
  C  只去掉 SetDescriptorHeaps           -> 仍然崩 ✗ 不是它
  D  只去掉 RSSetViewports + ScissorRects
  E  只去掉 OMSetRenderTargets
  F  只去掉 SetPipelineState + RootSignature + RootDescriptorTable + IASetPrimitiveTopology

⚠ 陷阱记录：上一版脚本把原代码文本写进注释里，导致"是否还在"的断言误报
   （注释里含有该文本 -> 误判为没删掉）。现在断言改成逐行检查是否仍以模式开头。
"""
import io
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DRAW = os.path.join(ROOT, r"src\hook\np_draw.cpp")
PY = sys.executable
LOG = os.path.join(os.environ.get("TEMP", "."), "NextPerfHook.log")

EXPS = {
    "D": ["list->RSSetViewports(1, &vp);", "list->RSSetScissorRects(1, &scissor);"],
    "G": ["list->DrawInstanced(3, 1, 0, 0);"],
    "H": ["list->SetDescriptorHeaps(1, heaps);", "list->RSSetViewports(1, &vp);",
          "list->RSSetScissorRects(1, &scissor);", "list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);",
          "list->SetPipelineState(pso_);", "list->SetGraphicsRootSignature(root_);",
          "list->SetGraphicsRootDescriptorTable(0, srvHeap_->GetGPUDescriptorHandleForHeapStart());",
          "list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);"],
    "E": ["list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);"],
    "F": ["list->SetPipelineState(pso_);", "list->SetGraphicsRootSignature(root_);",
          "list->SetGraphicsRootDescriptorTable(0, srvHeap_->GetGPUDescriptorHandleForHeapStart());",
          "list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);"],
}

name = sys.argv[1] if len(sys.argv) > 1 else "D"
pats = EXPS.get(name)
if not pats:
    print("  未知实验: %s（可选 %s）" % (name, "/".join(sorted(EXPS))))
    sys.exit(1)

src = io.open(DRAW, encoding="utf-8").read()
lines = src.split("\n")
removed = 0
for i, l in enumerate(lines):
    if any(l.strip().startswith(p) for p in pats):
        lines[i] = "    // [bisect-%s removed]" % name
        removed += 1
if removed == 0:
    print("  ★ 实验无效 ✗ 一行都没删（模式没匹配上）")
    sys.exit(1)
new = "\n".join(lines)
for i, l in enumerate(new.split("\n")):        # 逐行断言（不再被注释干扰）
    for p in pats:
        if l.strip().startswith(p):
            print("  ★ 实验无效 ✗ 第 %d 行仍是原代码" % (i + 1))
            sys.exit(1)
io.open(DRAW, "w", encoding="utf-8", newline="").write(new)
print("  实验 %s: 已去掉 %d 行并断言生效 ✓" % (name, removed))

try:
    r = subprocess.run(["cmd", "/c", "build.bat"], cwd=ROOT, capture_output=True,
                       text=True, encoding="utf-8", errors="replace")
    print("  BUILD=%d" % r.returncode)
    if r.returncode != 0:
        print((r.stdout or "")[-700:])
        raise SystemExit(1)
    if os.path.exists(LOG):
        os.remove(LOG)
    r2 = subprocess.run([PY, os.path.join(ROOT, "tests", "sim_stress.py"), "--api=dx12"],
                        cwd=ROOT, capture_output=True, text=True,
                        encoding="utf-8", errors="replace", timeout=900)
    out = (r2.stdout or "") + (r2.stderr or "")
    print("  探针退出码=%d 输出=%d 字符" % (r2.returncode, len(out)))

    valid, why, logtxt = True, [], ""
    if len(out.strip()) < 200:
        valid = False; why.append("探针输出过短")
    if not os.path.exists(LOG):
        valid = False; why.append("钩子日志不存在")
    else:
        logtxt = io.open(LOG, encoding="gbk", errors="replace").read()
        if "attach" not in logtxt:
            valid = False; why.append("日志无 attach")
    if "交换链" not in out and "generation" not in out:
        valid = False; why.append("无交换链记录")

    print("  ====================================")
    if not valid:
        print("  ★ 实验无效 ✗: " + "; ".join(why))
    else:
        crashed = "SEH" in logtxt
        print("  实验 %s 有效 ✓" % name)
        print("  结论: %s" % ("仍然崩溃 ✗ -> 不是这一组" if crashed
                              else "不再崩溃 ✓✓ -> 元凶就是: " + ", ".join(x[:45] for x in pats)))
    print("  ====================================")
finally:
    io.open(DRAW, "w", encoding="utf-8", newline="").write(src)
    print("  已还原 np_draw.cpp")
