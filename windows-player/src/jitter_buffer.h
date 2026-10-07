/*
 * 单源抖动缓冲（支持可变带宽档位）
 *
 * 职责：
 *   1. 【预热】先攒够目标水位再开始播，否则 WiFi 抖动会直接变成断音。
 *      源迟迟不来时等满 800ms 强制开始，避免永远静音。
 *   2. 按序号放进环形槽位，按 nextSeq 顺序取帧
 *   3. 水位超限时纠偏：过高丢帧、过低重复上一帧 —— 对抗时钟漂移
 *
 * 【可变帧长】音频帧的样本数由带宽档位决定（48k 立体声 / 48k 单声道 /
 * 24k / 16k / 8k 单声道），因此槽位大小在构造时确定，不能再用编译期常量。
 */
#pragma once

#include "common.h"

#include <cstdint>
#include <cstring>
#include <algorithm>
#include <vector>

namespace ahub {

/** 序号差值（处理 uint16 回绕） */
inline int seqDiff(uint16_t a, uint16_t b) {
    return (int)(int16_t)(uint16_t)(a - b);
}

class JitterBuffer {
public:
    static constexpr int CAP = 64;                                        // 320ms
    static constexpr int TARGET_FRAMES = JITTER_TARGET_MS / FRAME_MS;     // 8 帧 = 40ms
    static constexpr int HIGH_FRAMES   = JITTER_HIGH_MS   / FRAME_MS;     // 16 帧
    static constexpr int LOW_FRAMES    = JITTER_LOW_MS    / FRAME_MS;     // 4 帧
    static constexpr int WARMUP_MAX_TICKS = 160;                          // 800ms

    explicit JitterBuffer(int samplesInterleaved)
        : samples_(samplesInterleaved > 0 ? samplesInterleaved : 2),
          slots_((size_t)CAP * (size_t)samples_, 0),
          lastFrame_((size_t)samples_, 0),
          present_(CAP, false) {}

    void setTargetMs(int ms) {
        if (ms < 10) ms = 10;
        if (ms > 200) ms = 200;
        targetMs_ = ms;
        warmedUp_ = false;
        warmWait_ = 0;
    }

    /** 每帧的交错样本数 */
    int frameSamples() const { return samples_; }

    void reset() {
        std::fill(present_.begin(), present_.end(), false);
        started_ = warmedUp_ = haveHighest_ = haveLast_ = false;
        nextSeq_ = highestSeq_ = 0;
        warmWait_ = 0;
        recvFrames = lostFrames = dupFrames = droppedFrames = lateFrames = underruns = 0;
    }

    bool started()  const { return started_; }
    bool warmedUp() const { return warmedUp_; }

    /** 推入一帧；pcm 至少要有 samples_ 个样本 */
    void push(uint16_t seq, const int16_t* pcm) {
        recvFrames++;

        if (!started_) { started_ = true; nextSeq_ = seq; }
        if (!haveHighest_) { highestSeq_ = seq; haveHighest_ = true; }
        else if (seqDiff(seq, highestSeq_) > 0) highestSeq_ = seq;

        if (seqDiff(seq, nextSeq_) < 0)    { lateFrames++;    return; }
        if (seqDiff(seq, nextSeq_) >= CAP) { droppedFrames++; return; }

        int idx = seq % CAP;
        if (present_[idx]) dupFrames++;
        present_[idx] = true;
        memcpy(&slots_[(size_t)idx * samples_], pcm, (size_t)samples_ * sizeof(int16_t));
    }

    int pending() const {
        if (!haveHighest_) return 0;
        int d = seqDiff(highestSeq_, nextSeq_) + 1;
        return d > 0 ? d : 0;
    }

    /** 取下一帧写入 out（samples_ 个样本）。返回 true 表示真实数据 */
    bool pop(int16_t* out) {
        if (!started_) {
            memset(out, 0, (size_t)samples_ * sizeof(int16_t));
            return false;
        }

        if (!warmedUp_) {
            int target = std::max(1, targetMs_ / FRAME_MS);
            if (pending() >= target) {
                warmedUp_ = true;
            } else if (++warmWait_ > WARMUP_MAX_TICKS) {
                warmedUp_ = true;
            } else {
                memset(out, 0, (size_t)samples_ * sizeof(int16_t));
                return false;
            }
        }

        int buffered = pending();
        if (buffered <= 0) {
            underruns++;
            memset(out, 0, (size_t)samples_ * sizeof(int16_t));
            return false;
        }

        int high = std::max(2, targetMs_ / FRAME_MS + 2);
        if (buffered > high) {
            present_[nextSeq_ % CAP] = false;
            nextSeq_++;
            droppedFrames++;
            buffered--;
        }

        int idx = nextSeq_ % CAP;
        bool ok = present_[idx];
        if (ok) {
            memcpy(out, &slots_[(size_t)idx * samples_], (size_t)samples_ * sizeof(int16_t));
            memcpy(lastFrame_.data(), &slots_[(size_t)idx * samples_],
                   (size_t)samples_ * sizeof(int16_t));
            haveLast_ = true;
            present_[idx] = false;
        } else {
            lostFrames++;
            if (buffered < LOW_FRAMES && haveLast_) {
                memcpy(out, lastFrame_.data(), (size_t)samples_ * sizeof(int16_t));
                dupFrames++;
            } else {
                memset(out, 0, (size_t)samples_ * sizeof(int16_t));
            }
        }
        nextSeq_++;
        return ok;
    }

    uint64_t recvFrames    = 0;
    uint64_t lostFrames    = 0;
    uint64_t dupFrames     = 0;
    uint64_t droppedFrames = 0;
    uint64_t lateFrames    = 0;
    uint64_t underruns     = 0;

private:
    int samples_;
    std::vector<int16_t> slots_;
    std::vector<int16_t> lastFrame_;
    std::vector<bool>    present_;

    bool     started_     = false;
    bool     warmedUp_    = false;
    bool     haveHighest_ = false;
    bool     haveLast_    = false;
    uint16_t nextSeq_     = 0;
    uint16_t highestSeq_  = 0;
    int      warmWait_    = 0;
    int      targetMs_    = JITTER_TARGET_MS;
};

} // namespace ahub
