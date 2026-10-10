// 帧时间环形缓冲 + 百分位统计
//
// 关于「Low 帧」的定义：业界主要有两种口径
//   1) 取最慢的 1% 帧，求这些帧的平均帧时间，再换算成 FPS
//   2) 取帧时间的 99 百分位，直接换算成 FPS
// CapFrameX / OCAT 采用口径 1，本程序默认也用口径 1（更符合「卡顿感知」），
// 同时把百分位帧时间（p99 / p99.9）一并给出，方便对照。

#pragma once

#include <cstdint>
#include <cstring>
#include <algorithm>
#include <cmath>

namespace np {

class FrameStats {
public:
    static constexpr uint32_t kCap = 4096;

    void reset() { count_ = 0; write_ = 0; }

    void push(float ms) {
        buf_[write_] = ms;
        write_ = (write_ + 1) % kCap;
        if (count_ < kCap) ++count_;
    }

    uint32_t count() const { return count_; }

    // 复制最近 n 帧到 out（按时间顺序，最旧在前）
    uint32_t recent(float* out, uint32_t n) const {
        uint32_t take = std::min(n, count_);
        uint32_t start = (write_ + kCap - take) % kCap;
        for (uint32_t i = 0; i < take; ++i) out[i] = buf_[(start + i) % kCap];
        return take;
    }

    float last() const {
        if (!count_) return 0.0f;
        return buf_[(write_ + kCap - 1) % kCap];
    }

    // 平均 FPS（用最近窗口内的平均帧时间换算）
    //
    // ⚠ 这里的临时缓冲必须是 **thread_local**，不能是 static。
    //   原来是 `static float tmp[kCap]` —— 那是**所有线程共享**的一块 16KB 缓冲。
    //   而 PresentCommon（本类的唯一调用方）在游戏多线程呈现时会被并发进入，
    //   两个线程同时往这块缓冲里复制并排序，Low 帧 / 百分位就会算出垃圾值，
    //   而且完全没有报错、看起来"只是数不对"（用户反复反馈过 Low 帧不准）。
    float avgFps(uint32_t window) const {
        uint32_t take = std::min(window, count_);
        if (!take) return 0.0f;
        thread_local float tmp[kCap];
        take = recent(tmp, take);
        double sum = 0;
        for (uint32_t i = 0; i < take; ++i) sum += tmp[i];
        double avgMs = sum / take;
        return avgMs > 0.0001 ? (float)(1000.0 / avgMs) : 0.0f;
    }

    // 1% Low / 0.1% Low（**口径 2：x% low integral**，业界现行做法）
    //
    // 为什么换口径：口径 1（最差 x% 取平均）在锁帧场景下**天然偏低**。
    // 实测 1200 帧、稳定 60fps 时，最差 1% 是 12 帧；只要其中约一半是掉垂直同步的
    // 33.3ms、另一半是 16.7ms，平均就是 25ms -> 40 FPS，而玩家实际感受是满帧流畅。
    // 这正是用户反馈「流畅 60 却显示 38~42」的来源 —— 口径本身的问题，不是算错。
    //
    // integral 口径（MSI Afterburner / CapFrameX >= 1.5.3 采用）：
    //   1) 帧时间**降序**排列（最慢的在前）
    //   2) 依次累加，直到累计时间 >= 窗口总时长的 pct%
    //   3) 取**刚越过这个边界的那一帧**，换算成 FPS
    // 它回答的是「你有 (100-pct)% 的**时间**在这个 FPS 之上」，比口径 1 更贴合体感，
    // 而且在样本较少时也不会被单帧异常值带跑。
    float lowIntegral(float pct, uint32_t window = kCap) const {
        uint32_t take = std::min<uint32_t>(count_, std::min<uint32_t>(window, kCap));
        if (take < 20) return 0.0f;
        thread_local float tmp[kCap];
        take = recent(tmp, take);
        double total = 0;
        for (uint32_t i = 0; i < take; ++i) total += tmp[i];
        if (total <= 0.0) return 0.0f;
        // 最慢的排前面
        std::sort(tmp, tmp + take, std::greater<float>());
        double target = total * pct / 100.0;
        double acc = 0;
        uint32_t i = 0;
        for (; i < take; ++i) {
            acc += tmp[i];
            if (acc >= target) break;
        }
        if (i >= take) i = take - 1;
        float ms = tmp[i];
        return ms > 0.0001f ? (float)(1000.0 / ms) : 0.0f;
    }

    // 1% Low / 0.1% Low（口径 1：最差 x% 取平均 —— 保留供对照）
    //
    // window 是参与统计的**最近帧数**。默认取整个环形缓冲（4096 帧 ≈ 60fps 下 68 秒），
    // 但那样一来「进游戏那几秒的着色器编译卡顿」会在统计里赖着不走一分钟，
    // 玩家在稳定 60fps 的场景里看到 1% Low = 10 FPS 就是被它拖的。
    // 实时叠加用 ~1200 帧（60fps 下 20 秒）更贴近「当前的流畅度」。
    float lowPct(float pct, uint32_t window = kCap) const {
        uint32_t take = std::min<uint32_t>(count_, std::min<uint32_t>(window, kCap));
        if (take < 20) return 0.0f;
        thread_local float tmp[kCap];
        take = recent(tmp, take);
        uint32_t k = (uint32_t)std::ceil(take * pct / 100.0);
        if (k < 1) k = 1;
        // tmp[0..take) 部分排序：最慢的 k 帧放到前面
        std::partial_sort(tmp, tmp + k, tmp + take, std::greater<float>());
        double sum = 0;
        for (uint32_t i = 0; i < k; ++i) sum += tmp[i];
        double avgMs = sum / k;
        return avgMs > 0.0001 ? (float)(1000.0 / avgMs) : 0.0f;
    }

    // 诊断用：窗口内超过阈值（毫秒）的帧数占比
    float overRatio(float ms, uint32_t window = kCap) const {
        uint32_t take = std::min<uint32_t>(count_, std::min<uint32_t>(window, kCap));
        if (!take) return 0.0f;
        thread_local float tmp[kCap];
        take = recent(tmp, take);
        uint32_t n = 0;
        for (uint32_t i = 0; i < take; ++i) if (tmp[i] > ms) ++n;
        return (float)n / (float)take;
    }

    // Low 帧（**口径 4：窗口平均**）—— 先把帧时间按 windowMs 分组求平均，
    // 再取最差 pct% 的那些「窗口平均」求平均并换算成 FPS。
    //
    // 与口径 3 的区别：
    //   口径 3 看的是**单帧**最差 -> 抓的是一次卡顿
    //   本口径看的是**一个时间窗的平均** -> 抓的是「持续半秒的低帧率」
    // 游戏内 overlay 显示的 FPS 本来就是窗口平均值，NVIDIA 驱动面板的 1% Low
    // 很可能也是在平滑后的序列上取的百分位 —— 提供本口径便于对照。
    float lowWindowed(float pct, float windowMs, uint32_t lookback = kCap) const {
        uint32_t take = std::min<uint32_t>(count_, std::min<uint32_t>(lookback, kCap));
        if (take < 60) return 0.0f;
        thread_local float tmp[kCap];
        take = recent(tmp, take);

        // 从**最新**往回切窗口：每凑够 windowMs 就得到一个「窗口平均帧时间」
        thread_local float avgs[256];
        int n = 0;
        double acc = 0;
        int cnt = 0;
        for (int i = (int)take - 1; i >= 0 && n < 256; --i) {
            acc += tmp[i];
            ++cnt;
            if (acc >= (double)windowMs) {
                avgs[n++] = (float)(acc / cnt);
                acc = 0;
                cnt = 0;
            }
        }
        if (n < 4) return 0.0f;   // 样本太少，百分位没有意义
        std::sort(avgs, avgs + n, std::greater<float>());
        uint32_t k = (uint32_t)std::ceil(n * pct / 100.0);
        if (k < 1) k = 1;
        double s = 0;
        for (uint32_t i = 0; i < k; ++i) s += avgs[i];
        double ms = s / k;
        return ms > 0.0001 ? (float)(1000.0 / ms) : 0.0f;
    }

    // Low 帧（**口径 3：百分位**，与 NVIDIA 驱动面板 / FrameView 一致）
    //
    // pct 传 99 表示 1% Low，99.9 表示 0.1% Low。
    // 注意百分位是**按帧数**算的，不是按时间 —— 这就是它和 integral 口径的分歧点：
    //   稳定 60fps、掉垂直同步的帧占 0.5% 时：
    //     按帧数(本函数): P99 落在正常帧上          -> 60 FPS  ← 与驱动面板一致
    //     按时间(integral): 6×33.3ms 正好用满 1% 预算 -> 30 FPS  ← 刀刃效应，极不稳定
    // 驱动面板显示的是前者。实测驱动 59 / 本程序 30，就是这个差异造成的。
    float lowPercentileFps(float pct, uint32_t window = kCap) const {
        float ms = percentileMs(pct, window);
        return ms > 0.0001f ? (float)(1000.0 / ms) : 0.0f;
    }

    // 帧时间百分位（ms），p 为 0..100，返回「有 p% 的帧快于该值」的边界
    float percentileMs(float p, uint32_t window = kCap) const {
        uint32_t take = std::min<uint32_t>(count_, std::min<uint32_t>(window, kCap));
        if (take < 20) return 0.0f;
        thread_local float tmp[kCap];
        take = recent(tmp, take);
        float idx = p / 100.0f * (take - 1);
        uint32_t lo = (uint32_t)std::floor(idx);
        uint32_t hi = std::min(lo + 1, take - 1);
        std::nth_element(tmp, tmp + lo, tmp + take);
        float a = tmp[lo];
        std::nth_element(tmp, tmp + hi, tmp + take);
        float b = tmp[hi];
        return a + (b - a) * (idx - lo);
    }

private:
    float    buf_[kCap]{};
    uint32_t count_ = 0;
    uint32_t write_ = 0;
};

// 指数移动平均，用于让读数不至于疯狂跳动
class Ema {
public:
    explicit Ema(float alpha = 0.25f) : a_(alpha) {}
    void reset(float v = 0.0f) { v_ = v; init_ = true; }
    float update(float x) {
        if (!init_) { v_ = x; init_ = true; }
        else v_ = v_ + a_ * (x - v_);
        return v_;
    }
    float value() const { return v_; }
private:
    float a_;
    float v_ = 0;
    bool init_ = false;
};

// 滑动窗口最大值，用于图表 Y 轴自适应
class RunningMax {
public:
    void push(float v) { if (v > cur_) { cur_ = v; age_ = 0; } else if (++age_ > 120) { cur_ *= 0.85f; age_ = 0; } }
    float value() const { return cur_; }
private:
    float cur_ = 0;
    int age_ = 0;
};

}  // namespace np
