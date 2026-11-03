#ifndef AGORA_SERVICE_H
#define AGORA_SERVICE_H

#include <string>
#include <deque>
#include <vector>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cstdint>
#include <cstddef>

/* SDK 头文件自带 extern "C" 守卫, 直接包含 */
#include "agora_rtc_api.h"

#include "v4l2_capture.h"
#include "mpp_encoder.h"
#include "alsa_capture.h"
#include "alsa_player.h"
#include "av_pacer.h"
#include "av_source.h"

/*
 * AgoraService —— RTSA 音视频 + RTM/RTCM 信令核心封装
 *
 * 职责:
 *   - agora_rtc_init / create_connection / join_channel / leave / fini 生命周期
 *   - agora_rtm_login 常驻 (peer + channel 双模式)
 *   - 音视频采集编码发送线程组 (V4L2 + MPP / ALSA)
 *   - SDK 回调联动: 关键帧请求 / 码率调整 / Token 续期 / 带宽超限退避
 *   - RTM/RTCM 收到消息 → 无锁分发队列 → MessageSink (统一进入指令链)
 *
 * 状态机:
 *   IDLE ──start()──► READY(待指令) ──joinChannel()──► IN_CHANNEL ──leaveChannel()──► READY
 *   任意态 ──致命错误──► ERROR
 *
 * 线程清单 (全部由本类内部管理):
 *   rtm dispatch / video send / [v4l2 capture(采集线程内编码)] / alsa capture / alsa playback
 */
enum AgoraState {
    AGORA_IDLE = 0,
    AGORA_READY,        /* init + RTM login 完成, 待指令 */
    AGORA_JOINING,      /* join 请求已发出, 等待 on_join_channel_success */
    AGORA_IN_CHANNEL,   /* 推流中 */
    AGORA_ERROR,        /* 致命错误 (License/AppID 等), 需重启进程 */
};

struct AgoraMessage {
    std::string payload;   /* 消息原文 (JSON) */
    std::string source;    /* "rtm:peer:<uid>" / "rtm:ch:<channel>" / "rtcm:conn0" */
};

class AgoraService {
public:
    /* 收到远端信令 (RTM/RTCM) 时的分发回调 */
    typedef std::function<void(const AgoraMessage&)> MessageSink;

    AgoraService() = default;
    ~AgoraService() { stop(); }

    AgoraService(const AgoraService&) = delete;
    AgoraService& operator=(const AgoraService&) = delete;

    /* 初始化 SDK + RTM 常驻登录。读 g_config.agora。 */
    bool start(std::string& err);
    void stop();

    /* 入频 (av_join)。token 为空时使用配置值。 */
    bool joinChannel(const std::string& channel, uint32_t uid,
                     const std::string& token, std::string& err);
    /* 退频并停止采集编码线程组 (av_leave) */
    bool leaveChannel(std::string& err);

    bool enableVideo(bool en, std::string& err);   /* av_video */
    bool enableAudio(bool en, std::string& err);   /* av_audio */
    bool enableListen(bool en, std::string& err);  /* av_listen (下行播放) */

    /* ── 发送原语 ── */
    /* RTCM: 频道内控制消息, ≤1024B; remote_uid=0 广播 */
    bool sendMediaCtrl(const void* data, size_t len, uint32_t remote_uid = 0);
    bool sendMediaCtrlStr(const std::string& s, uint32_t remote_uid = 0)
    { return sendMediaCtrl(s.data(), s.size(), remote_uid); }

    bool rtmSendPeer(const std::string& peer_uid, const std::string& msg);
    bool rtmPublishChannel(const std::string& channel, const std::string& msg);

    /* ── 状态查询 ── */
    AgoraState  state()     const { return (AgoraState)state_.load(); }
    bool        inChannel() const { return state_.load() == AGORA_IN_CHANNEL; }
    bool        rtmLogined() const { return rtm_logined_.load(); }
    const std::string& channel() const { return channel_; }
    std::string statusJson() const;

    void setMessageSink(MessageSink sink) { sink_ = std::move(sink); }

private:
    /* ══ 事件处理表 ══ */
    static agora_rtc_event_handler_t& sEventHandler();
    static agora_rtm_handler_t&       sRtmHandler();

    /* ══ SDK C 回调 (静态 → 单例 s_inst_) ══ */
    static AgoraService* s_inst_;

    static void cb_error(connection_id_t, int, const char*);
    static void cb_join_success(connection_id_t, uint32_t, int);
    static void cb_reconnecting(connection_id_t);
    static void cb_connection_lost(connection_id_t);
    static void cb_rejoin_success(connection_id_t, uint32_t, int);
    static void cb_license_failure(connection_id_t, int);
    static void cb_token_expire(connection_id_t, const char*);
    static void cb_user_joined(connection_id_t, uint32_t, int);
    static void cb_user_offline(connection_id_t, uint32_t, int);
    static void cb_user_joined_ua(connection_id_t, const user_info_t*, int);
    static void cb_user_offline_ua(connection_id_t, const user_info_t*, int);
    static void cb_user_info_updated(connection_id_t, const user_info_t*);
    static void cb_user_mute_audio(connection_id_t, uint32_t, bool);
    static void cb_user_mute_video(connection_id_t, uint32_t, bool);
    static void cb_audio_data(connection_id_t, uint32_t, uint16_t, const void*, size_t, const audio_frame_info_t*);
    static void cb_mixed_audio_data(connection_id_t, const void*, size_t, const audio_frame_info_t*);
    static void cb_video_data(connection_id_t, uint32_t, uint16_t, const void*, size_t, const video_frame_info_t*);
    static void cb_target_bitrate(connection_id_t, uint32_t);
    static void cb_key_frame_req(connection_id_t, uint32_t, video_stream_type_e);
    static void cb_rtc_stats(connection_id_t, rtc_stats_t);
    static void cb_media_ctrl_msg(connection_id_t, uint32_t, const void*, size_t);
    static void cb_rdt_state(connection_id_t, uint32_t, rdt_state_e);
    static void cb_rdt_msg(connection_id_t, uint32_t, rdt_stream_type_e, const void*, size_t);
    static void cb_stream_message(connection_id_t, uint32_t, int, const char*, size_t, uint64_t);
    static int  cb_packet_input_hook(connection_id_t, rtc_packet_type_e, const void*, uint16_t, void*, uint16_t);
    static int  cb_packet_output_hook(connection_id_t, rtc_packet_type_e, const void*, uint16_t, void*, uint16_t);

    /* RTM 回调 */
    static void cb_rtm_event(const char*, rtm_event_type_e, rtm_err_code_e);
    static void cb_rtm_data(const char*, const void*, size_t, rtm_message_type_e, const char*);
    static void cb_rtm_send_result(const char*, uint32_t, rtm_msg_state_e);
    static void cb_rtm_subscribe_result(const char*, rtm_err_code_e);
    static void cb_rtm_subscribe_data(const char*, const char*, const void*, size_t, rtm_message_type_e, const char*);
    static void cb_rtm_publish_result(const char*, uint32_t, rtm_msg_state_e);

    /* ══ 内部 ══ */
    void postMessage(const std::string& payload, const std::string& source);
    void dispatchLoop();
    void videoSendLoop();

    void startMediaPipeline(std::string& err);   /* 入频成功后启动 */
    void stopMediaPipeline();                    /* 退频/停止时关闭 */

    /* 视频管线单独开关 (av_video enable/disable, 入频后调用) */
    void openVideoPipeline(std::string& err);
    void closeVideoPipeline();

    void onEncodedPacket(const uint8_t* data, size_t len, bool keyframe);
    void onCapturedVideoFrame(const uint8_t* data, size_t len);
    void onCapturedAudioFrame(const int16_t* samples, size_t count);

    /* ── 状态 ── */
    std::atomic<int>   state_{AGORA_IDLE};
    std::atomic<bool>  rtm_logined_{false};
    std::atomic<bool>  running_{false};

    std::string        channel_;
    uint32_t           uid_ = 0;
    std::string        current_token_;
    connection_id_t    conn_id_ = 0;

    /* 音视频开关 (入频后生效位) */
    std::atomic<bool>  video_enabled_{false};
    std::atomic<bool>  audio_enabled_{false};
    std::atomic<bool>  listen_enabled_{false};

    /* 下行音频统计 (诊断对讲: SDK 收到/因未开播放丢弃) */
    std::atomic<uint64_t> rx_audio_frames_{0};
    std::atomic<uint64_t> rx_audio_drops_{0};

    /* ── 媒体管线 ── */
    V4l2Capture  v4l2_;
    MppEncoder   encoder_;
    AlsaCapture  alsa_in_;
    AlsaPlayer   alsa_out_;
    VadGate      vad_;

    /* 编码/采集输出 → 发送队列 (容量 3, 满丢最旧) */
    struct VideoPacket {
        std::vector<uint8_t> data;
        bool keyframe = false;
    };
    std::deque<VideoPacket> video_queue_;
    std::mutex              video_mtx_;

    /* YUYV 摄像头 → 软件转 NV12 的转换缓冲 (conv_w_==0 表示不需要转换) */
    int                 conv_w_ = 0;
    int                 conv_h_ = 0;
    std::vector<uint8_t> conv_buf_;

    std::thread video_thread_;
    std::atomic<bool> video_stop_{false};
    AvPacer     video_pacer_{40000};   /* 25fps, join 后按实际 fps 重设 */

    /* RTM/RTCM 消息分发队列 */
    std::deque<AgoraMessage> msg_queue_;
    std::mutex               msg_mtx_;
    std::condition_variable  msg_cv_;
    std::condition_variable  rtm_cv_;   /* RTM 登录完成等待 */
    std::thread              dispatch_thread_;

    /* RTM 发送序号 */
    std::atomic<uint32_t> rtm_msg_id_{1};

    /* 统计 */
    std::atomic<uint64_t> tx_video_frames_{0};
    std::atomic<uint64_t> tx_audio_frames_{0};
    std::atomic<uint32_t> target_bps_{0};

    MessageSink sink_;
};

#endif /* AGORA_SERVICE_H */
