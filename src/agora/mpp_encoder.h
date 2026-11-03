#ifndef AGORA_MPP_ENCODER_H
#define AGORA_MPP_ENCODER_H

#include <string>
#include <functional>
#include <atomic>
#include <cstdint>
#include <cstddef>

/*
 * RK3588 Rockchip MPP 硬件 H.264 编码器
 *
 * 输入:  YU12(I420) 或 NV12 裸帧 (来自 V4L2 采集)
 * 输出:  H.264 Annex-B 裸流, 每次编码输出一个完整 access unit。
 *        采用 MPP_ENC_HEADER_MODE_EACH_IDR: 每个关键帧自动携带 SPS/PPS,
 *        新观众/丢包恢复后可直接解码, 无需单独发头。
 *
 * 联动 (与 RTSA 回调):
 *   - on_key_frame_gen_req      → requestKeyframe()  下一帧强制 IDR
 *   - on_target_bitrate_changed → setBitrateKbps()   动态调整码率
 *
 * 参数建议 (方案 §3.3):
 *   720p@25fps, 码率 1500-2000 Kbps, GOP 75 (3s), CBR, Main profile
 */
class MppEncoder {
public:
    /* packet: 编码输出; keyframe: 是否关键帧。回调返回前数据有效。 */
    typedef std::function<void(const uint8_t* data, size_t len, bool keyframe)> PacketCallback;

    MppEncoder() = default;
    ~MppEncoder() { close(); }

    MppEncoder(const MppEncoder&) = delete;
    MppEncoder& operator=(const MppEncoder&) = delete;

    /* nv12=true: 输入 NV12; false: 输入 YU12(I420) */
    bool open(int width, int height, int fps, int bitrate_kbps, int gop,
              bool nv12, std::string& err);
    void close();
    bool isOpen() const { return opened_; }

    /* 编码一帧（阻塞, 在采集线程调用）。输入帧大小必须等于 frameSize() */
    bool encode(const uint8_t* yuv, size_t len);

    /* RTSA 请求关键帧 → 下一帧强制 IDR */
    void requestKeyframe() { force_idr_ = true; }

    /* RTSA 带宽回调 → 动态调整码率 (lazy, 下次编码时生效) */
    void setBitrateKbps(int kbps) { pending_bitrate_kbps_.store(kbps); }

    size_t frameSize() const { return frame_size_; }

    void setPacketCallback(PacketCallback cb) { packet_cb_ = std::move(cb); }

private:
    bool applyBitrate(int kbps);

    int      width_   = 0;
    int      height_  = 0;
    int      fps_     = 0;
    int      bitrate_kbps_ = 0;
    int      gop_     = 0;
    bool     nv12_    = false;
    size_t   frame_size_   = 0;
    bool     opened_ = false;

    /* MPP 句柄用 void* 存放, 头文件不暴露 MPP 类型 (隔离 SDK 依赖) */
    void* ctx_        = nullptr;   /* MppCtx */
    void* mpi_        = nullptr;   /* MppApi* */
    void* enc_cfg_    = nullptr;   /* MppEncCfg */
    void* buf_group_  = nullptr;   /* MppBufferGroup */

    std::atomic<bool> force_idr_{false};
    std::atomic<int>  pending_bitrate_kbps_{-1};
    PacketCallback    packet_cb_;
};

#endif /* AGORA_MPP_ENCODER_H */
