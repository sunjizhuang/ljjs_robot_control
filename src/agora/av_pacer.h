#ifndef AGORA_AV_PACER_H
#define AGORA_AV_PACER_H

#include <cstdint>
#include <chrono>

/*
 * 发送节奏器（参考 agora_rtsa_sdk example/utility/pacer.c 思路）
 *
 * 音视频按固定节奏发送:
 *   - 视频: 1/fps 间隔（如 25fps → 40ms）
 *   - 音频: frame_ms 间隔（默认 20ms）
 *
 * 用法（单线程轮询）:
 *   AvPacer vp(40000);
 *   while (running) {
 *       if (vp.isTimeToSend()) { send_one_video_frame(); }
 *       else { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
 *   }
 */
class AvPacer {
public:
    explicit AvPacer(uint32_t interval_us)
        : interval_us_(interval_us),
          next_us_(nowUs() + (int64_t)interval_us) {}

    /* 是否到达下一个发送点；到达则推进调度时刻 */
    bool isTimeToSend()
    {
        int64_t now = nowUs();
        if (now >= next_us_) {
            /* 追帧: 最多补 2 个间隔，避免长时间阻塞后突发 */
            if (now - next_us_ > 2 * (int64_t)interval_us_) {
                next_us_ = now + (int64_t)interval_us_;
            } else {
                next_us_ += (int64_t)interval_us_;
            }
            return true;
        }
        return false;
    }

    /* 重置节奏（重新入频时调用） */
    void reset()
    {
        next_us_ = nowUs() + (int64_t)interval_us_;
    }

    void setInterval(uint32_t interval_us) { interval_us_ = interval_us; }

private:
    static int64_t nowUs()
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    uint32_t interval_us_;
    int64_t  next_us_;
};

#endif /* AGORA_AV_PACER_H */
