# -*- coding: utf-8 -*-
"""量化「一次卡顿之后，Low 帧要多久才恢复」——按不同的回看长度。

用户的质疑：数据是 FIFO、只缓存固定数量的帧，那几秒前的低帧就该被遗忘，
为什么实测会「钉住」很久？

答案：FIFO 没问题（np_stats.h 的 recent() 只取**最新** N 帧），
     问题出在 N 取成了**整个缓冲区**（kCap=4096 帧 ≈ 60fps 下 68 秒）。
     也就是说"遗忘"的边界不是我以为的 1 秒，而是 68 秒。

这个脚本用与 C++ 同规则的逻辑，量出不同回看长度下的恢复时间。
"""
import statistics


def simulate(lookback_frames, stall_ms=300.0, fps=60.0, total_s=120.0):
    """60fps 稳定跑，第 10 秒插入一次 stall_ms 的卡顿，看 lowWindowed 何时恢复。"""
    frame_ms = 1000.0 / fps
    n = int(total_s * fps)
    stall_at = int(10 * fps)

    frames = []
    for i in range(n):
        frames.append(stall_ms if i == stall_at else frame_ms)

    # 逐帧推进，每 0.5 秒采样一次 lowWindowed(1%, 500ms, lookback)
    samples = []          # (t, value)
    avgs_all = []
    for i in range(60, n):
        t = i / fps
        if i % 30:        # 每 0.5 秒采一次
            continue
        take = min(i, lookback_frames)
        seg = frames[i - take:i]
        # 从最新往回切 500ms 窗口
        avgs = []
        acc = 0.0
        cnt = 0
        for v in reversed(seg):
            acc += v
            cnt += 1
            if acc >= 500.0:
                avgs.append(acc / cnt)
                acc = 0.0
                cnt = 0
        if len(avgs) < 4:
            continue
        avgs.sort(reverse=True)
        k = max(1, int(len(avgs) * 0.01 + 0.999))
        ms = sum(avgs[:k]) / k
        samples.append((t, 1000.0 / ms))

    # 卡顿前的稳定值
    baseline = statistics.median([v for t, v in samples if 5 <= t <= 9])
    after = [(t, v) for t, v in samples if t > 10]
    recovered_at = None
    for t, v in after:
        if v >= baseline * 0.99:
            recovered_at = t
            break
    return baseline, min(v for _, v in after), recovered_at


print('  60fps 稳定，第 10 秒插入一次 300ms 卡顿；统计 1%% Low（窗口平均口径）')
print('  %-18s %-12s %-12s %s' % ('回看长度', '卡顿前', '卡顿后最低', '恢复用时'))
print('  ' + '-' * 62)
for name, frames in (('68 秒（旧代码）', 4096),
                     ('30 秒', 1800),
                     ('12 秒（现在默认）', 720),
                     ('5 秒', 300)):
    base, low, rec = simulate(frames)
    rec_s = ('%.1f 秒' % (rec - 10)) if rec else '未恢复'
    print('  %-18s %-12.1f %-12.1f %s' % (name, base, low, rec_s))
print()
print('  结论：FIFO 本身没问题，恢复时间基本等于**回看长度** ——')
print('        旧代码用整个缓冲区(68 秒)当回看，所以一次卡顿真的会钉住近 70 秒。')
