#ifndef AGORA_ALSA_PLAYER_H
#define AGORA_ALSA_PLAYER_H

#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <cstdint>
#include <cstddef>

/*
 * ALSA 下行播放（双向对讲远端声音）
 *
 * 数据流: RTSA on_audio_data 回调(SDK 内部线程, 已解码 PCM)
 *         → write() 写入 RingBuffer → 播放线程阻塞 writei
 *
 * 延迟控制:
 *   - RingBuffer 目标水位 200ms, 上限 1040ms;
 *   - 超上限时丢弃最旧数据(保实时, 宁可丢帧不累积延迟);
 *   - 长期静默(2s 无数据)自动 drain + prepare, 防止陈旧数据堆积。
 */
class AlsaPlayer {
public:
    AlsaPlayer() = default;
    ~AlsaPlayer() { close(); }

    AlsaPlayer(const AlsaPlayer&) = delete;
    AlsaPlayer& operator=(const AlsaPlayer&) = delete;

    bool open(const std::string& dev, int sample_rate, int channels, std::string& err);
    void close();
    bool isOpen() const { return handle_ != nullptr; }

    /* 从 SDK 回调线程调用: 推入 RingBuffer (非阻塞) */
    void write(const int16_t* samples, size_t count);

    void start();
    void stop();

private:
    void playerLoop();

    void*       handle_ = nullptr;  /* snd_pcm_t* */
    int         rate_ = 16000;
    int         ch_   = 1;

    /* RingBuffer (样本数, 交织) */
    std::vector<int16_t> ring_;
    size_t  ring_cap_     = 0;      /* 容量(样本) */
    size_t  ring_high_    = 0;      /* 上限(样本) */
    size_t  rd_ = 0, wr_ = 0, cnt_ = 0;
    std::mutex ring_mtx_;

    std::thread       thread_;
    std::atomic<bool> running_{false};
};

#endif /* AGORA_ALSA_PLAYER_H */
