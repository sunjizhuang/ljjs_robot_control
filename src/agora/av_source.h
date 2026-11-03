#ifndef AGORA_AV_SOURCE_H
#define AGORA_AV_SOURCE_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <atomic>

/*
 * 音视频源抽象接口 + VAD 预留
 *
 * 设计目的:
 *   1. 采集层（V4L2/ALSA）与编码/发送层解耦，后续可替换采集源
 *      （如 MIPI+RKISP、网络流）而不影响发送链路；
 *   2. 为 VAD（语音活动检测）预留插入点: 采集帧先经过 VadGate，
 *      再决定是否送入发送链路。本期 VAD 恒直通，无任何行为。
 */

/* ── 视频帧接收端（编码器或直传发送器实现） ── */
class IVideoFrameSink {
public:
    virtual ~IVideoFrameSink() {}
    /* data: YU12/NV12 裸帧或已编码裸流; len: 字节数 */
    virtual void onVideoFrame(const uint8_t* data, size_t len) = 0;
};

/* ── 音频帧接收端（发送器实现） ── */
class IAudioFrameSink {
public:
    virtual ~IAudioFrameSink() {}
    /* samples: 交织 PCM (S16_LE); count: 样本数(含所有声道) */
    virtual void onAudioFrame(const int16_t* samples, size_t count) = 0;
};

/* ══════════════════════════════════════════════════════════
 * VAD 门控（预留占位，本期恒直通）
 *
 * 后续实现方向:
 *   - open: 接入 WebRTC VAD 或能量阈值检测 (frame 10-30ms, 16k/mono)
 *   - enabled 时: 连续 N 帧无人声 → 通知上层停发/降码率; 检测到人声
 *     → 立即恢复发送并补发一个舒适噪声包头
 *   - 通过 agora.vad.enabled 配置开关，默认关闭
 * ══════════════════════════════════════════════════════════ */
class VadGate {
public:
    explicit VadGate(bool enabled = false) : enabled_(enabled) {}

    void setEnabled(bool en) { enabled_ = en; }
    bool enabled() const { return enabled_; }

    /* 返回 true = 该帧应继续送入发送链路。当前恒返回 true。 */
    bool process(const int16_t* samples, size_t count)
    {
        (void)samples;
        (void)count;
        /* TODO(VAD): 无人声时 return false 并向上层报告静默事件 */
        return true;
    }

private:
    std::atomic<bool> enabled_;
};

#endif /* AGORA_AV_SOURCE_H */
