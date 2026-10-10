# -*- coding: utf-8 -*-
"""验证帧内合并的算法（与 C++ 逐行同规则，按**绝对时刻**建模）。

C++ 里的关键点：
    fm = now - gLastPresentQpc          # 与**上一次记账**的呈现之间的间隔
    if 合并: 不记账，**也不推进 gLastPresentQpc**
    else:    记账，并推进 gLastPresentQpc
所以被合并的那一次，会让下一次的间隔自然覆盖整个帧 —— 这正是合并能work的原因。
（第一版模拟脚本没按绝对时刻建模，所以错误地把 8.4 和 24.9 分别记了下来。）
"""
import random
import statistics


def simulate(present_times, name, expect_frame_ms):
    """与 C++ 逐行同规则。

    C++ 里有两个不同的时间戳，**这是整套方案能成立的关键**：
        gPrevPresentQpc  = 上一次**呈现**的时刻（无论是否合并都推进）-> 算真正的原始间隔
        gLastPresentQpc = 上一次**记账**的时刻（被合并时不推进）    -> 算记账用的间隔
    判定用前者、记账用后者。若把后者喂给原始序列，合并就会污染基准 -> 正反馈。
    """
    raw = []
    merged = []
    prev_present = None      # 上一次呈现（总是推进）
    last_recorded = None     # 上一次记账（合并时不推进）
    absorbed = 0
    for t in present_times:
        if prev_present is None:
            prev_present = last_recorded = t
            continue
        fm_raw = t - prev_present          # 真正的原始间隔
        prev_present = t
        raw.append(fm_raw)

        # 与 C++ 一致：**均值**（中位数在 50/50 双峰上会振荡）+ 阈值夹紧
        m = statistics.fmean(raw[-240:]) if len(raw) >= 8 else 0.0
        thresh = 0.0
        if m > 1.0:
            thresh = min(30.0, max(2.0, m * 0.6))

        if thresh > 0 and fm_raw < thresh:
            absorbed += 1
            continue                        # 不记账、**不推进** last_recorded
        fm = t - last_recorded
        merged.append(fm)
        last_recorded = t

    avg = sum(merged) / len(merged) if merged else 0
    p99 = statistics.quantiles(merged, n=100)[98] if len(merged) >= 100 else max(merged)
    low1 = 1000.0 / p99 if p99 > 0 else 0
    ok = abs(avg - expect_frame_ms) < 3.0
    print('  %-20s 原始均值=%6.2f  合并后均值=%6.2f  P99=%6.2f  1%%Low=%5.1f  吸收=%d  %s'
          % (name, sum(raw) / len(raw), avg, p99, low1, absorbed, '✅' if ok else '❌'))
    print('     合并后前 8 个: %s' % ' '.join('%.1f' % x for x in merged[:8]))


def times_from_pattern(pairs, repeat):
    """pairs = [(间隔, 次数), ...] 循环 repeat 遍"""
    t = 0.0
    out = [0.0]
    for _ in range(repeat):
        for gap, cnt in pairs:
            for _ in range(cnt):
                t += gap
                out.append(t)
    return out


print('=== A：一帧多次 Present（8.4 后接 24.9，真实帧周期 33.3ms）===')
simulate(times_from_pattern([(8.4, 1), (24.9, 1)], 200), 'A 8.4+24.9', 33.3)

print()
print('=== B：RE8 那样的干净 60fps（16.66ms 一帧一次）===')
simulate([i * 16.66 for i in range(400)], 'B 干净 16.66', 16.66)

print()
print('=== C：60fps 轻微抖动（14~19ms，绝不能被误合并）===')
random.seed(7)
t = 0.0
ts = [0.0]
for _ in range(400):
    t += 16.6 + random.uniform(-2.2, 2.2)
    ts.append(t)
simulate(ts, 'C 抖动 14~19', 16.6)

print()
print('=== D：真卡顿（偶发掉到 33ms，应该保留、不该被合并）===')
random.seed(3)
t = 0.0
ts = [0.0]
for i in range(400):
    t += 33.3 if (i % 40 == 0) else 16.66
    ts.append(t)
simulate(ts, 'D 偶发掉帧', 17.1)
