#ifndef AGORA_ALSA_CAPTURE_H
#define AGORA_ALSA_CAPTURE_H

#include <string>
#include <functional>
#include <atomic>
#include <thread>
#include <cstdint>
#include <cstddef>

/*
 * ALSA 音频采集 (16bit S16_LE PCM)
 *
 * 典型配置: 16000Hz / 1ch / 20ms 帧长
 *   → 每帧 320 样本 = 640 字节, 正好对应 RTSA 的
 *     audio_codec_opt { OPUS, 16000, 1, 20 }, SDK 内置 Opus 编码上行。
 *
 * 采集线程阻塞 readi, 每帧回调一次; 回调内可直接发送
 * (agora_rtc_send_audio_data 非阻塞)。
 */
class AlsaCapture {
public:
    typedef std::function<void(const int16_t* samples, size_t count)> FrameCallback;

    AlsaCapture() = default;
    ~AlsaCapture() { close(); }

    AlsaCapture(const AlsaCapture&) = delete;
    AlsaCapture& operator=(const AlsaCapture&) = delete;

    bool open(const std::string& dev, int sample_rate, int channels, int frame_ms,
              std::string& err);
    void close();
    bool isOpen() const { return handle_ != nullptr; }

    /* 实际协商值 */
    int sampleRate() const { return rate_; }
    int channels()   const { return ch_; }
    int frameSamples() const { return frame_samples_; }

    void setFrameCallback(FrameCallback cb) { frame_cb_ = std::move(cb); }

    void start();
    void stop();

private:
    void captureLoop();

    void*        handle_ = nullptr;   /* snd_pcm_t* */
    std::string  dev_;
    int          rate_ = 16000;
    int          ch_   = 1;
    int          frame_ms_ = 20;
    unsigned int frame_samples_ = 320;  /* 每帧每声道样本数 */
    unsigned int bytes_per_frame_ = 640;

    FrameCallback  frame_cb_;
    std::thread    thread_;
    std::atomic<bool> running_{false};
};

#endif /* AGORA_ALSA_CAPTURE_H */
