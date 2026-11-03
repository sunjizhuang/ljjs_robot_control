#include "agora_service.h"
#include "yuyv_convert.h"
#include "../config/yaml_config.h"
#include "../util/log.h"

#include <sstream>
#include <cstring>
#include <chrono>
#include <algorithm>

#define TAG "agora"

/* 单例指针: SDK C 回调无用户参数, 以此路由回实例 */
AgoraService* AgoraService::s_inst_ = nullptr;

/* ══════════════════ 事件处理表 ══════════════════ */

agora_rtc_event_handler_t& AgoraService::sEventHandler()
{
    static agora_rtc_event_handler_t h = {
        &AgoraService::cb_error,                 /* on_error */
        &AgoraService::cb_join_success,          /* on_join_channel_success */
        &AgoraService::cb_reconnecting,          /* on_reconnecting */
        &AgoraService::cb_connection_lost,       /* on_connection_lost */
        &AgoraService::cb_rejoin_success,        /* on_rejoin_channel_success */
        &AgoraService::cb_license_failure,       /* on_license_validation_failure */
        &AgoraService::cb_token_expire,          /* on_token_privilege_will_expire */
        &AgoraService::cb_user_joined,           /* on_user_joined */
        &AgoraService::cb_user_offline,          /* on_user_offline */
        &AgoraService::cb_user_joined_ua,        /* on_user_joined_with_user_account */
        &AgoraService::cb_user_offline_ua,       /* on_user_offline_with_user_account */
        &AgoraService::cb_user_info_updated,     /* on_user_info_updated */
        &AgoraService::cb_user_mute_audio,       /* on_user_mute_audio */
        &AgoraService::cb_user_mute_video,       /* on_user_mute_video */
        &AgoraService::cb_audio_data,            /* on_audio_data */
        &AgoraService::cb_mixed_audio_data,      /* on_mixed_audio_data */
        &AgoraService::cb_video_data,            /* on_video_data */
        &AgoraService::cb_target_bitrate,        /* on_target_bitrate_changed */
        &AgoraService::cb_key_frame_req,         /* on_key_frame_gen_req */
        &AgoraService::cb_rtc_stats,             /* on_rtc_stats */
        &AgoraService::cb_media_ctrl_msg,        /* on_media_ctrl_msg */
        &AgoraService::cb_rdt_state,             /* on_rdt_state */
        &AgoraService::cb_rdt_msg,               /* on_rdt_msg */
        &AgoraService::cb_stream_message,        /* on_stream_message */
        NULL,                                    /* on_packet_input_hook  (@technical preview, 注册会破坏媒体上行: 服务端不登记轨道/无带宽反馈, 实测 2026-10-30) */
        NULL,                                    /* on_packet_output_hook (@technical preview, 同上) */
    };
    return h;
}

agora_rtm_handler_t& AgoraService::sRtmHandler()
{
    static agora_rtm_handler_t h = {
        &AgoraService::cb_rtm_event,             /* on_rtm_event */
        &AgoraService::cb_rtm_data,              /* on_rtm_data */
        &AgoraService::cb_rtm_send_result,       /* on_rtm_send_data_result */
        &AgoraService::cb_rtm_subscribe_result,  /* on_rtm_subscribe_result */
        &AgoraService::cb_rtm_subscribe_data,    /* on_rtm_subscribe_data */
        &AgoraService::cb_rtm_publish_result,    /* on_rtm_publish_result */
    };
    return h;
}

/* ══════════════════ 工具 ══════════════════ */

namespace {

uint32_t areaCodeOf(const std::string& s)
{
    if (s == "CN")  return AREA_CODE_CN;
    if (s == "NA")  return AREA_CODE_NA;
    if (s == "EU")  return AREA_CODE_EU;
    if (s == "AS")  return AREA_CODE_AS;
    if (s == "JP")  return AREA_CODE_JP;
    return AREA_CODE_GLOB;
}

rtc_log_level_e logLevelOf(int level)
{
    if (level < 0) level = 0;
    if (level > 8) level = 8;
    return (rtc_log_level_e)level;
}

} /* anonymous namespace */

/* ══════════════════ 生命周期 ══════════════════ */

bool AgoraService::start(std::string& err)
{
    if (running_.load()) {
        err = "AgoraService 已启动";
        return false;
    }
    if (s_inst_ && s_inst_ != this) {
        err = "进程内已存在另一个 AgoraService 实例";
        return false;
    }

    const AgoraConfig& cfg = g_config.agora;

    /* 1. SDK 初始化 (每进程一次) */
    s_inst_ = this;

    rtc_service_option_t opt;
    memset(&opt, 0, sizeof(opt));
    opt.area_code = areaCodeOf(cfg.area_code);
    snprintf(opt.product_id, sizeof(opt.product_id), "%s", "lljs_ctrl");
    opt.log_cfg.log_disable = (cfg.log_level < 0);
    opt.log_cfg.log_level   = logLevelOf(cfg.log_level);
    opt.log_cfg.log_path    = "io.agora.rtc_sdk";
    opt.log_cfg.log_tag     = "lljs_ctrl";
    opt.log_cfg.log_disable_desensitize = false;
    if (!cfg.license.empty()) {
        snprintf(opt.license_value, sizeof(opt.license_value), "%s", cfg.license.c_str());
    }
    opt.use_string_uid = false;
    opt.domain_limit   = false;

    int ret = agora_rtc_init(cfg.app_id.c_str(), &sEventHandler(), &opt);
    if (ret != ERR_OKAY) {
        err = std::string("agora_rtc_init 失败: ") + agora_rtc_err_2_str(ret) +
              " (检查 App ID / License)";
        s_inst_ = nullptr;
        return false;
    }

    /* 2. 创建连接 */
    ret = agora_rtc_create_connection(&conn_id_);
    if (ret != ERR_OKAY) {
        err = std::string("agora_rtc_create_connection 失败: ") + agora_rtc_err_2_str(ret);
        agora_rtc_fini();
        s_inst_ = nullptr;
        return false;
    }

    /* 3. RTM 常驻登录 (信令入口: join 指令本身要经 RTM 到达) */
    if (cfg.rtm.enabled) {
        agora_rtm_handler_t rtm = sRtmHandler();
        ret = agora_rtm_login(cfg.rtm.uid.c_str(),
                              cfg.rtm.token.empty() ? NULL : cfg.rtm.token.c_str(),
                              &rtm);
        if (ret != ERR_OKAY) {
            err = std::string("agora_rtm_login 失败: ") + agora_rtc_err_2_str(ret);
            agora_rtc_destroy_connection(conn_id_);
            agora_rtc_fini();
            s_inst_ = nullptr;
            return false;
        }
        /* 等待 on_rtm_event(RTM_EVENT_TYPE_LOGIN), 最多 10s */
        std::unique_lock<std::mutex> lk(msg_mtx_);
        if (!rtm_cv_.wait_for(lk, std::chrono::seconds(10),
                              [this]() { return rtm_logined_.load(); })) {
            err = "RTM 登录超时 (10s): 检查网络与 rtm.uid/token";
            agora_rtc_destroy_connection(conn_id_);
            agora_rtc_fini();
            s_inst_ = nullptr;
            return false;
        }
        LOG_INFO("[%s] RTM 已登录: uid=%s mode=%s",
                 TAG, cfg.rtm.uid.c_str(), cfg.rtm.mode.c_str());

        /* channel/both 模式: 订阅信令频道 */
        if (cfg.rtm.mode == "channel" || cfg.rtm.mode == "both") {
            agora_rtm_subscribe(cfg.rtm.channel.c_str());
        }
    }

    /* 4. 启动消息分发线程 */
    running_ = true;
    /* 音视频开关初始值取自配置 (av_video/av_audio 指令可运行时切换) */
    video_enabled_.store(cfg.video.enabled);
    audio_enabled_.store(cfg.audio.enabled);
    dispatch_thread_ = std::thread(&AgoraService::dispatchLoop, this);
    state_.store(AGORA_READY);

    LOG_INFO("[%s] 已启动: app_id=%s conn_id=%u, SDK 版本 %s",
             TAG, cfg.app_id.c_str(), conn_id_, agora_rtc_get_version());

    LOG_INFO("[%s] RTSA: channel=%s uid=%u auto_join=%s video_mode=%s",
             TAG, cfg.rtsa.channel.c_str(), (unsigned)cfg.rtsa.uid,
             cfg.rtsa.auto_join ? "true" : "false", cfg.video.mode.c_str());

    /* 5. auto_join: 启动即入频 */
    if (cfg.rtsa.auto_join) {
        std::string jerr;
        if (!joinChannel(cfg.rtsa.channel, (uint32_t)cfg.rtsa.uid, "", jerr)) {
            LOG_ERR("[%s] auto_join 失败: %s", TAG, jerr.c_str());
        }
    } else {
        LOG_INFO("[%s] RTSA 未入频, 等待 av_join 指令", TAG);
    }
    return true;
}

void AgoraService::stop()
{
    if (!running_.exchange(false)) return;

    stopMediaPipeline();

    if (state_.load() == AGORA_IN_CHANNEL || state_.load() == AGORA_JOINING) {
        agora_rtc_leave_channel(conn_id_);
    }
    agora_rtc_destroy_connection(conn_id_);

    if (g_config.agora.rtm.enabled) {
        agora_rtm_logout();
    }
    agora_rtc_fini();

    /* 唤醒并回收分发线程 */
    {
        std::lock_guard<std::mutex> lk(msg_mtx_);
        msg_cv_.notify_all();
    }
    if (dispatch_thread_.joinable()) dispatch_thread_.join();

    state_.store(AGORA_IDLE);
    s_inst_ = nullptr;
    LOG_INFO("[%s] 已停止", TAG);
}

/* ══════════════════ 入频 / 退频 ══════════════════ */

bool AgoraService::joinChannel(const std::string& channel, uint32_t uid,
                               const std::string& token, std::string& err)
{
    const AgoraConfig& cfg = g_config.agora;
    const std::string& ch = channel.empty() ? cfg.rtsa.channel : channel;
    LOG_INFO("[%s] 正在入频: channel=%s uid=%u token=%s",
             TAG, ch.c_str(), uid, token.empty() ? "无" : "有");

    if (state_.load() == AGORA_JOINING) {
        err = "正在入频中, 请稍候";
        return false;
    }
    if (state_.load() == AGORA_IN_CHANNEL) {
        if (ch == channel_) {
            err = "已在频道 " + ch + " 中";
            return true;  /* 幂等 */
        }
        std::string lerr;
        leaveChannel(lerr);  /* 换频道: 先退再进 */
    }
    if (state_.load() != AGORA_READY) {
        err = "当前状态不允许入频";
        return false;
    }

    rtc_channel_options_t opt;
    memset(&opt, 0, sizeof(opt));
    /* 音频订阅: 音频功能开启即订阅 (双向对讲; 是否播放由 listen_enabled/av_listen 控制)。
     * 注意: auto_subscribe_audio=false 且中途 av_listen 无法补救 (mute_remote_audio 不重协商)。
     * 视频订阅: 仅 rtsa.subscribe=true (纯推流模式不下行视频) */
    const bool subscribe_audio = cfg.audio.enabled;
    opt.auto_subscribe_audio = subscribe_audio;
    opt.auto_subscribe_video = g_config.agora.rtsa.subscribe;
    opt.enable_audio_jitter_buffer = subscribe_audio;
    opt.enable_audio_decode = subscribe_audio;
    opt.audio_codec_opt.audio_codec_type  = AUDIO_CODEC_TYPE_OPUS;
    opt.audio_codec_opt.pcm_sample_rate   = cfg.audio.sample_rate;
    opt.audio_codec_opt.pcm_channel_num   = cfg.audio.channels;
    opt.audio_codec_opt.pcm_duration      = cfg.audio.frame_ms;
    opt.enable_rdt = false;

    channel_       = ch;
    uid_           = uid ? uid : (uint32_t)cfg.rtsa.uid;
    current_token_ = token.empty() ? cfg.rtsa.token : token;

    /* BWE 上限: SDK 默认约 1Mbps, 会把 target_bps 压在 1024kbps,
     * 低于视频编码码率 (1800kbps)。入频前显式抬高, 对齐官方 hello_rtsa 做法 */
    const uint32_t max_bps = (uint32_t)cfg.video.bitrate_kbps * 1000 + 400000; /* 视频 + 音频/重传余量 */
    int ret = agora_rtc_set_bwe_param(conn_id_, 200000, max_bps, max_bps / 2);
    if (ret != ERR_OKAY) {
        LOG_WARN("[%s] set_bwe_param 失败(%d), 沿用 SDK 默认值", TAG, ret);
    } else {
        LOG_INFO("[%s] BWE: min=200k max=%uk start=%uk", TAG, max_bps / 1000, max_bps / 2000);
    }

    ret = agora_rtc_join_channel(conn_id_, ch.c_str(), uid_,
                                     current_token_.empty() ? NULL : current_token_.c_str(),
                                     &opt);
    /* 关键诊断: 实际使用的鉴权值 (此前打印的是入参 token, 会掩盖 config.yaml 兜底值) */
    LOG_INFO("[%s] 实际入频鉴权: token=%s (来源: %s)", TAG,
             current_token_.empty() ? "无" : "有",
             token.empty() ? "config.yaml" : "join 指令");
    if (ret != ERR_OKAY) {
        err = std::string("agora_rtc_join_channel 失败: ") + agora_rtc_err_2_str(ret);
        return false;
    }
    state_.store(AGORA_JOINING);
    LOG_INFO("[%s] 入频请求已发出: channel=%s uid=%u", TAG, ch.c_str(), uid_);
    return true;
}

bool AgoraService::leaveChannel(std::string& err)
{
    (void)err;
    if (state_.load() != AGORA_IN_CHANNEL && state_.load() != AGORA_JOINING) {
        return true;  /* 幂等 */
    }
    stopMediaPipeline();
    agora_rtc_leave_channel(conn_id_);
    state_.store(AGORA_READY);
    channel_.clear();
    LOG_INFO("[%s] 已退频", TAG);
    return true;
}

/* ══════════════════ 音视频开关 ══════════════════ */

bool AgoraService::enableVideo(bool en, std::string& err)
{
    (void)err;
    video_enabled_.store(en);
    if (state_.load() == AGORA_IN_CHANNEL) {
        if (en && !v4l2_.isOpen()) {
            std::string perr;
            openVideoPipeline(perr);
            if (!perr.empty()) { LOG_ERR("[%s] 启动视频失败: %s", TAG, perr.c_str()); return false; }
        } else if (!en) {
            std::string perr;
            closeVideoPipeline();
            (void)perr;
        }
    }
    LOG_INFO("[%s] 视频 %s", TAG, en ? "开启" : "关闭");
    return true;
}

bool AgoraService::enableAudio(bool en, std::string& err)
{
    (void)err;
    audio_enabled_.store(en);
    if (state_.load() == AGORA_IN_CHANNEL) {
        if (en && !alsa_in_.isOpen()) {
            std::string perr;
            if (!alsa_in_.open(g_config.agora.audio.device_in,
                               g_config.agora.audio.sample_rate,
                               g_config.agora.audio.channels,
                               g_config.agora.audio.frame_ms, perr)) {
                LOG_ERR("[%s] 启动音频采集失败: %s", TAG, perr.c_str());
                return false;
            }
            alsa_in_.setFrameCallback(
                [this](const int16_t* s, size_t n) { onCapturedAudioFrame(s, n); });
            alsa_in_.start();
        } else if (!en) {
            alsa_in_.stop();
            alsa_in_.close();
        }
    }
    LOG_INFO("[%s] 音频上行 %s", TAG, en ? "开启" : "关闭");
    return true;
}

bool AgoraService::enableListen(bool en, std::string& err)
{
    (void)err;
    listen_enabled_.store(en);
    if (state_.load() == AGORA_IN_CHANNEL) {
        if (en && !alsa_out_.isOpen()) {
            std::string perr;
            int play_rate = g_config.agora.audio.play_sample_rate > 0
                          ? g_config.agora.audio.play_sample_rate
                          : g_config.agora.audio.sample_rate;
            if (!alsa_out_.open(g_config.agora.audio.device_out,
                                play_rate, g_config.agora.audio.channels, perr)) {
                LOG_ERR("[%s] 启动下行播放失败: %s", TAG, perr.c_str());
                return false;
            }
            alsa_out_.start();
        } else if (!en) {
            alsa_out_.stop();
            alsa_out_.close();
        }
    }
    LOG_INFO("[%s] 下行对讲 %s", TAG, en ? "开启" : "关闭");
    return true;
}

/* ══════════════════ 发送原语 ══════════════════ */

bool AgoraService::sendMediaCtrl(const void* data, size_t len, uint32_t remote_uid)
{
    if (!inChannel()) return false;
    if (len > 1024) {
        LOG_WARN("[%s] RTCM 消息超限: %zu > 1024B, 放弃发送", TAG, len);
        return false;
    }
    int ret = agora_rtc_send_media_ctrl_msg(conn_id_, remote_uid, data, len);
    if (ret != ERR_OKAY) {
        LOG_WARN("[%s] RTCM 发送失败: %s", TAG, agora_rtc_err_2_str(ret));
        return false;
    }
    return true;
}

bool AgoraService::rtmSendPeer(const std::string& peer_uid, const std::string& msg)
{
    if (!rtm_logined_.load()) return false;
    int ret = agora_rtm_send_data(peer_uid.c_str(), msg.data(), msg.size(),
                                  rtm_msg_id_.fetch_add(1),
                                  RTM_MESSAGE_TYPE_STRING, NULL);
    if (ret != ERR_OKAY) {
        LOG_WARN("[%s] RTM peer 发送失败: %s", TAG, agora_rtc_err_2_str(ret));
        return false;
    }
    return true;
}

bool AgoraService::rtmPublishChannel(const std::string& channel, const std::string& msg)
{
    if (!rtm_logined_.load()) return false;
    int ret = agora_rtm_publish(channel.c_str(), msg.data(), msg.size(),
                                rtm_msg_id_.fetch_add(1),
                                RTM_MESSAGE_TYPE_STRING, NULL);
    if (ret != ERR_OKAY) {
        LOG_WARN("[%s] RTM publish 失败: %s", TAG, agora_rtc_err_2_str(ret));
        return false;
    }
    return true;
}

std::string AgoraService::statusJson() const
{
    static const char* state_names[] = { "IDLE", "READY", "JOINING", "IN_CHANNEL", "ERROR" };
    const AgoraConfig& cfg = g_config.agora;
    std::ostringstream os;
    os << "{\"cmd\":\"av_status\",\"state\":\"" << state_names[state_.load()]
       << "\",\"rtm_logined\":" << (rtm_logined_.load() ? "true" : "false")
       << ",\"rtm_mode\":\"" << cfg.rtm.mode << "\""
       << ",\"channel\":\"" << channel_ << "\""
       << ",\"uid\":" << uid_
       << ",\"video_enabled\":" << (video_enabled_.load() ? "true" : "false")
       << ",\"audio_enabled\":" << (audio_enabled_.load() ? "true" : "false")
       << ",\"listen_enabled\":" << (listen_enabled_.load() ? "true" : "false")
       << ",\"rtcm_signaling\":" << (cfg.rtsa.rtcm_signaling ? "true" : "false")
       << ",\"video_mode\":\"" << cfg.video.mode << "\""
       << ",\"target_bps\":" << target_bps_.load()
       << ",\"tx_video_frames\":" << tx_video_frames_.load()
       << ",\"tx_audio_frames\":" << tx_audio_frames_.load()
       << ",\"sdk_version\":\"" << agora_rtc_get_version() << "\""
       << "}";
    return os.str();
}

/* ══════════════════ 媒体管线 ══════════════════ */

void AgoraService::startMediaPipeline(std::string& err)
{
    const AgoraConfig& cfg = g_config.agora;

    if (cfg.video.enabled && video_enabled_.load()) {
        openVideoPipeline(err);
        if (!err.empty()) {
            LOG_ERR("[%s] 视频管线启动失败: %s (继续音频)", TAG, err.c_str());
            err.clear();
        }
    }
    if (cfg.audio.enabled && audio_enabled_.load()) {
        enableAudio(true, err);
    }
    /* 下行对讲: 配置 listen=true 时启动即开播放, 否则用 av_listen 指令动态开 */
    if (g_config.agora.audio.listen) {
        listen_enabled_.store(true);
    }
    if (listen_enabled_.load()) {
        enableListen(true, err);
    }
}

void AgoraService::stopMediaPipeline()
{
    closeVideoPipeline();
    if (alsa_in_.isOpen())  { alsa_in_.stop(); alsa_in_.close(); }
    if (alsa_out_.isOpen()) { alsa_out_.stop(); alsa_out_.close(); }
}

void AgoraService::openVideoPipeline(std::string& err)
{
    const AgoraConfig& cfg = g_config.agora;

    /* 采集 */
    if (!v4l2_.open(cfg.video.device, cfg.video.width, cfg.video.height,
                    cfg.video.fps, err)) {
        return;
    }
    video_pacer_.setInterval((uint32_t)(1000000 / (v4l2_.actualFps() > 0 ? v4l2_.actualFps() : cfg.video.fps)));

    /* 编码 (mpp 模式) 或直传 (yuv 模式) */
    if (cfg.video.mode == "h264") {
        bool nv12;
        if (v4l2_.format() == V4l2Capture::FMT_NV12 ||
            v4l2_.format() == V4l2Capture::FMT_YUYV) {
            nv12 = true;
        } else {
            nv12 = false;   /* YU12 (I420) */
        }
        if (!encoder_.open(v4l2_.actualWidth(), v4l2_.actualHeight(), v4l2_.actualFps(),
                           cfg.video.bitrate_kbps, cfg.video.gop, nv12, err)) {
            v4l2_.close();
            return;
        }
        encoder_.setPacketCallback(
            [this](const uint8_t* d, size_t l, bool k) { onEncodedPacket(d, l, k); });

        /* YUYV: 采集回调里先软件转 NV12 再进编码链 */
        conv_w_ = conv_h_ = 0;
        conv_buf_.clear();
        if (v4l2_.format() == V4l2Capture::FMT_YUYV) {
            conv_w_ = v4l2_.actualWidth();
            conv_h_ = v4l2_.actualHeight();
            conv_buf_.resize((size_t)conv_w_ * conv_h_ * 3 / 2);
            LOG_INFO("[%s] 摄像头输出 YUYV, 启用软件转 NV12 (%dx%d)",
                     TAG, conv_w_, conv_h_);
        }
        v4l2_.setFrameCallback(
            [this](const uint8_t* d, size_t l) {
                if (conv_w_ > 0) {
                    yuyvToNv12(d, conv_buf_.data(), conv_w_, conv_h_);
                    onCapturedVideoFrame(conv_buf_.data(), conv_buf_.size());
                } else {
                    onCapturedVideoFrame(d, l);
                }
            });
    } else {
        /* yuv 直传: 格式必须是 YU12 (VIDEO_DATA_TYPE_YUV420) */
        if (v4l2_.format() != V4l2Capture::FMT_YU12) {
            err = "yuv 直传模式要求摄像头输出 YU12(I420), 实际: NV12";
            v4l2_.close();
            return;
        }
        v4l2_.setFrameCallback(
            [this](const uint8_t* d, size_t l) { onCapturedVideoFrame(d, l); });
    }

    video_mtx_.lock();
    video_queue_.clear();
    video_mtx_.unlock();

    v4l2_.start();
    if (!video_thread_.joinable()) {
        video_thread_ = std::thread(&AgoraService::videoSendLoop, this);
    }
    LOG_INFO("[%s] 视频管线已启动 (%s)", TAG, cfg.video.mode.c_str());
}

void AgoraService::closeVideoPipeline()
{
    v4l2_.stop();
    v4l2_.close();
    encoder_.close();

    if (video_thread_.joinable()) {
        /* sendLoop 以 running_ 为退出条件, 这里先置 running_=false 由 stop() 兜底;
         * 运行期仅退频时: 用临时标志让线程退出 */
        video_stop_.store(true);
        video_thread_.join();
        video_stop_.store(false);
    }
    std::lock_guard<std::mutex> lk(video_mtx_);
    video_queue_.clear();
}

void AgoraService::onCapturedVideoFrame(const uint8_t* data, size_t len)
{
    const AgoraConfig& cfg = g_config.agora;
    if (cfg.video.mode == "h264") {
        /* 在采集线程内同步编码; 编码回调把 H264 包压入发送队列 */
        if (!encoder_.encode(data, len)) {
            LOG_WARN("[%s] 编码一帧失败", TAG);
        }
    } else {
        /* yuv 直传: 拷贝入队 */
        std::lock_guard<std::mutex> lk(video_mtx_);
        if (video_queue_.size() >= 3) video_queue_.pop_front();
        VideoPacket pkt;
        pkt.data.assign(data, data + len);
        pkt.keyframe = true;  /* AUTO_DETECT */
        video_queue_.push_back(std::move(pkt));
    }
}

void AgoraService::onEncodedPacket(const uint8_t* data, size_t len, bool keyframe)
{
    /* 首包与关键帧日志: 确认 MPP 编码持续出流 (GOP 内间隔较大属正常) */
    static std::atomic<uint64_t> s_enc_pkts{0};
    const uint64_t n = s_enc_pkts.fetch_add(1);
    if (n == 0) {
        LOG_INFO("[%s] 编码器输出首包: %zu 字节, key=%d", TAG, len, (int)keyframe);
    } else if (keyframe) {
        LOG_INFO("[%s] 编码器输出关键帧 #%llu: %zu 字节", TAG,
                 (unsigned long long)(n + 1), len);
    }

    std::lock_guard<std::mutex> lk(video_mtx_);
    if (video_queue_.size() >= 3) video_queue_.pop_front();
    VideoPacket pkt;
    pkt.data.assign(data, data + len);
    pkt.keyframe = keyframe;
    video_queue_.push_back(std::move(pkt));
}

void AgoraService::videoSendLoop()
{
    const AgoraConfig& cfg = g_config.agora;
    video_pacer_.reset();

    while (running_.load() && !video_stop_.load()) {
        if (!inChannel()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        if (!video_pacer_.isTimeToSend()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        VideoPacket pkt;
        {
            std::lock_guard<std::mutex> lk(video_mtx_);
            if (video_queue_.empty()) continue;
            pkt = std::move(video_queue_.front());
            video_queue_.pop_front();
        }

        video_frame_info_t info;
        memset(&info, 0, sizeof(info));
        if (cfg.video.mode == "h264") {
            info.data_type   = VIDEO_DATA_TYPE_H264;
            info.frame_type  = pkt.keyframe ? VIDEO_FRAME_KEY : VIDEO_FRAME_DELTA;
        } else {
            info.data_type   = VIDEO_DATA_TYPE_YUV420;
            info.frame_type  = VIDEO_FRAME_AUTO_DETECT;
        }
        info.stream_type = VIDEO_STREAM_HIGH;
        info.frame_rate  = (video_frame_rate_e)(v4l2_.actualFps() > 0 ? v4l2_.actualFps() : cfg.video.fps);
        info.rotation    = VIDEO_ORIENTATION_0;

        int ret = agora_rtc_send_video_data(conn_id_, pkt.data.data(), pkt.data.size(), &info);
        if (ret == ERR_VIDEO_SEND_OVER_BANDWIDTH_LIMIT) {
            /* 带宽超限: 退避 200ms, SDK 侧会触发 on_target_bitrate_changed 调码率 */
            LOG_WARN("[%s] 视频发送超带宽, 退避 200ms", TAG);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        } else if (ret != ERR_OKAY) {
            LOG_WARN("[%s] 视频发送失败: %s", TAG, agora_rtc_err_2_str(ret));
        } else {
            if (tx_video_frames_.load() == 0) {
                LOG_INFO("[%s] 视频首包已发出: %zu 字节 (%s)",
                         TAG, pkt.data.size(),
                         pkt.keyframe ? "关键帧" : "delta帧");
            }
            tx_video_frames_.fetch_add(1);
        }
    }
}

void AgoraService::onCapturedAudioFrame(const int16_t* samples, size_t count)
{
    if (!inChannel() || !audio_enabled_.load()) return;
    /* VAD 门控 (本期恒直通) */
    if (!vad_.process(samples, count)) return;

    audio_frame_info_t info;
    memset(&info, 0, sizeof(info));
    info.data_type = AUDIO_DATA_TYPE_PCM;

    int ret = agora_rtc_send_audio_data(conn_id_, samples, count * sizeof(int16_t), &info);
    if (ret == ERR_OKAY) {
        tx_audio_frames_.fetch_add(1);
    } else {
        LOG_WARN("[%s] 音频发送失败: %s", TAG, agora_rtc_err_2_str(ret));
    }
}

/* ══════════════════ 消息分发 ══════════════════ */

void AgoraService::postMessage(const std::string& payload, const std::string& source)
{
    {
        std::lock_guard<std::mutex> lk(msg_mtx_);
        if (msg_queue_.size() >= 64) msg_queue_.pop_front();  /* 防堆积 */
        AgoraMessage m;
        m.payload = payload;
        m.source  = source;
        msg_queue_.push_back(std::move(m));
    }
    msg_cv_.notify_one();
}

void AgoraService::dispatchLoop()
{
    while (running_.load()) {
        AgoraMessage m;
        {
            std::unique_lock<std::mutex> lk(msg_mtx_);
            msg_cv_.wait(lk, [this]() {
                return !msg_queue_.empty() || !running_.load();
            });
            if (!running_.load()) break;
            m = std::move(msg_queue_.front());
            msg_queue_.pop_front();
        }
        if (sink_) sink_(m);   /* 串行进入指令链, 规避 SDK 回调线程并发 */
    }
}

/* ══════════════════ SDK C 回调实现 ══════════════════ */

void AgoraService::cb_error(connection_id_t conn, int code, const char* msg)
{
    (void)conn;
    LOG_ERR("[%s] on_error %d: %s", TAG, code, msg ? msg : "");
    if (code == ERR_INVALID_APP_ID || code == ERR_INVALID_TOKEN ||
        code == ERR_TOKEN_EXPIRED || code == ERR_LICENSE_VERIFICATION_FAILURE) {
        if (s_inst_) s_inst_->state_.store(AGORA_ERROR);
    }
}

void AgoraService::cb_join_success(connection_id_t conn, uint32_t uid, int elapsed)
{
    (void)conn;
    AgoraService* s = s_inst_;
    if (!s) return;
    LOG_INFO("[%s] 入频成功: uid=%u elapsed=%dms", TAG, uid, elapsed);
    if (s->uid_ == 0) s->uid_ = uid;  /* SDK 自动分配 */
    s->state_.store(AGORA_IN_CHANNEL);

    /* BWE: min=max/10, max=视频码率, start=max/2 (对齐 hello_rtsa) */
    const AgoraConfig& cfg = g_config.agora;
    uint32_t max_bps = (uint32_t)cfg.video.bitrate_kbps * 1000;
    if (max_bps > 0) {
        agora_rtc_set_bwe_param(s->conn_id_, max_bps / 10, max_bps, max_bps / 2);
    }

    std::string err;
    s->startMediaPipeline(err);
}

void AgoraService::cb_reconnecting(connection_id_t conn)
{
    (void)conn;
    LOG_WARN("[%s] 连接中断, 重连中...", TAG);
}

void AgoraService::cb_connection_lost(connection_id_t conn)
{
    (void)conn;
    LOG_WARN("[%s] 连接丢失 (>10s)", TAG);
}

void AgoraService::cb_rejoin_success(connection_id_t conn, uint32_t uid, int elapsed)
{
    (void)conn; (void)uid; (void)elapsed;
    LOG_INFO("[%s] 重新入频成功", TAG);
    if (s_inst_) s_inst_->state_.store(AGORA_IN_CHANNEL);
}

void AgoraService::cb_license_failure(connection_id_t conn, int error)
{
    (void)conn;
    LOG_ERR("[%s] License 校验失败: %d (1=无效 2=过期 3=超时长 4=非有效期 5=多设备 99=内部)", TAG, error);
    if (s_inst_) s_inst_->state_.store(AGORA_ERROR);
}

void AgoraService::cb_token_expire(connection_id_t conn, const char* token)
{
    AgoraService* s = s_inst_;
    if (!s) return;
    /* 部分版本回调会带刷新后的 token: 非空则采纳, 空则沿用配置值 */
    if (token && token[0]) s->current_token_ = token;
    LOG_WARN("[%s] Token 即将过期", TAG);
    if (g_config.agora.rtsa.token_renew && !s->current_token_.empty()) {
        int ret = agora_rtc_renew_token(conn, s->current_token_.c_str());
        LOG_INFO("[%s] Token 续期: %s", TAG,
                 ret == ERR_OKAY ? "成功" : agora_rtc_err_2_str(ret));
    }
}

void AgoraService::cb_user_joined(connection_id_t, uint32_t uid, int)
{
    LOG_INFO("[%s] 远端用户上线: %u", TAG, uid);
}
void AgoraService::cb_user_offline(connection_id_t, uint32_t uid, int reason)
{
    LOG_INFO("[%s] 远端用户离线: %u reason=%d", TAG, uid, reason);
}
void AgoraService::cb_user_joined_ua(connection_id_t, const user_info_t* u, int)
{
    if (u) LOG_INFO("[%s] 远端用户上线: uid=%u account=%s", TAG, u->uid, u->user_account);
}
void AgoraService::cb_user_offline_ua(connection_id_t, const user_info_t* u, int)
{
    if (u) LOG_INFO("[%s] 远端用户离线: uid=%u", TAG, u->uid);
}
void AgoraService::cb_user_info_updated(connection_id_t, const user_info_t* u)
{
    if (u) LOG_INFO("[%s] 用户信息更新: uid=%u", TAG, u->uid);
}
void AgoraService::cb_user_mute_audio(connection_id_t, uint32_t uid, bool muted)
{
    LOG_INFO("[%s] 用户 %u %s 音频", TAG, uid, muted ? "关闭" : "开启");
}
void AgoraService::cb_user_mute_video(connection_id_t, uint32_t uid, bool muted)
{
    LOG_INFO("[%s] 用户 %u %s 视频", TAG, uid, muted ? "关闭" : "开启");
}

void AgoraService::cb_audio_data(connection_id_t, uint32_t uid, uint16_t,
                                 const void* data, size_t len, const audio_frame_info_t* info)
{
    /* 下行对讲: 已解码 PCM → RingBuffer → 播放线程 */
    AgoraService* s = s_inst_;
    if (!s || !data || len == 0) return;
    s->rx_audio_frames_.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<bool> first_logged{false};
    bool expect = false;
    if (first_logged.compare_exchange_strong(expect, true)) {
        LOG_INFO("[%s] 收到下行音频首帧: uid=%u len=%zu type=%d", TAG,
                 uid, len, info ? (int)info->data_type : -1);
    }
    if (s->listen_enabled_.load() && s->alsa_out_.isOpen()) {
        s->alsa_out_.write((const int16_t*)data, len / 2);
    } else {
        /* 未开下行播放 (av_listen), 帧被丢弃 — 计数便于诊断"听不到"类问题 */
        s->rx_audio_drops_.fetch_add(1, std::memory_order_relaxed);
    }
}
void AgoraService::cb_mixed_audio_data(connection_id_t, const void*, size_t, const audio_frame_info_t*)
{
    /* 未启用 enable_audio_mixer, 不应触发 */
}
void AgoraService::cb_video_data(connection_id_t, uint32_t, uint16_t,
                                 const void*, size_t, const video_frame_info_t*)
{
    /* subscribe=false (纯推流), 不处理远端视频 */
}

void AgoraService::cb_target_bitrate(connection_id_t, uint32_t target_bps)
{
    AgoraService* s = s_inst_;
    if (!s) return;
    s->target_bps_.store(target_bps);
    if (g_config.agora.video.mode == "h264") {
        s->encoder_.setBitrateKbps((int)(target_bps / 1000));
        LOG_INFO("[%s] 带宽变化, 目标码率 %u bps", TAG, target_bps);
    }
}

void AgoraService::cb_key_frame_req(connection_id_t, uint32_t uid, video_stream_type_e st)
{
    (void)uid; (void)st;
    /* 远端丢包请求关键帧 → MPP 下一帧强制 IDR (防花屏) */
    AgoraService* s = s_inst_;
    if (s && g_config.agora.video.mode == "h264") {
        s->encoder_.requestKeyframe();
        LOG_INFO("[%s] 收到关键帧请求, 下一帧强制 IDR", TAG);
    }
}

void AgoraService::cb_rtc_stats(connection_id_t, rtc_stats_t stats)
{
    /* SDK 每 2s 回调一次; 每 5 次 (10s) 打印一条 INFO 便于诊断发送链路 */
    AgoraService* s = s_inst_;
    if (!s) return;
    static uint32_t s_cnt = 0;
    if (++s_cnt % 5 != 0) return;
    LOG_INFO("[%s] 统计(10s): tx视频=%llu帧(本地累计) tx视频码率=%ukbps "
             "tx音频码率=%ukbps 队列=%zu 延迟=%ums 用户=%u | 下行音频帧=%llu (丢弃=%llu 播放=%s)",
             TAG,
             (unsigned long long)s->tx_video_frames_.load(),
             (unsigned)stats.tx_video_kbitrate,
             (unsigned)stats.tx_audio_kbitrate,
             s->video_queue_.size(),
             (unsigned)stats.lastmile_delay,
             (unsigned)stats.user_count,
             (unsigned long long)s->rx_audio_frames_.load(),
             (unsigned long long)s->rx_audio_drops_.load(),
             s->listen_enabled_.load() ? "开" : "关");
}

void AgoraService::cb_media_ctrl_msg(connection_id_t, uint32_t uid, const void* payload, size_t len)
{
    /* RTCM 信令路径: 频道内低延迟控制消息 */
    AgoraService* s = s_inst_;
    if (s && payload && len > 0) {
        s->postMessage(std::string((const char*)payload, len),
                       "rtcm:" + std::to_string(uid));
    }
}
void AgoraService::cb_rdt_state(connection_id_t, uint32_t uid, rdt_state_e state)
{
    LOG_DBG("[%s] RDT 状态: uid=%u state=%d", TAG, uid, (int)state);
}
void AgoraService::cb_rdt_msg(connection_id_t, uint32_t, rdt_stream_type_e, const void*, size_t)
{
    /* 未启用 RDT 信令路径 */
}
void AgoraService::cb_stream_message(connection_id_t, uint32_t, int, const char*, size_t, uint64_t)
{
    /* 未启用数据流 */
}
int AgoraService::cb_packet_input_hook(connection_id_t, rtc_packet_type_e, const void*, uint16_t, void*, uint16_t)
{
    return 0;
}
int AgoraService::cb_packet_output_hook(connection_id_t, rtc_packet_type_e, const void*, uint16_t, void*, uint16_t)
{
    return 0;
}

/* ── RTM 回调 ── */

void AgoraService::cb_rtm_event(const char* rtm_uid, rtm_event_type_e type, rtm_err_code_e err)
{
    AgoraService* s = s_inst_;
    if (!s) return;
    switch (type) {
    case RTM_EVENT_TYPE_LOGIN:
        if ((int)err == (int)ERR_OKAY) {
            LOG_INFO("[%s] RTM 登录成功: %s", TAG, rtm_uid ? rtm_uid : "");
            s->rtm_logined_.store(true);
        } else {
            /* 110=ERR_INVALID_TOKEN: 项目开启 Token 鉴权后, RTM 登录需要 rtm.token (与 RTC Token 不通用) */
            LOG_ERR("[%s] RTM 登录失败: uid=%s err=%d (110=需要有效 rtm.token, 见 config.yaml rtm.token)",
                    TAG, rtm_uid ? rtm_uid : "", (int)err);
            s->rtm_logined_.store(false);
        }
        s->rtm_cv_.notify_all();
        break;
    case RTM_EVENT_TYPE_KICKOFF:
        LOG_WARN("[%s] RTM 被踢下线 (同 uid 异地登录?)", TAG);
        s->rtm_logined_.store(false);
        break;
    case RTM_EVENT_TYPE_EXIT:
        LOG_WARN("[%s] RTM 退出: err=%d", TAG, (int)err);
        s->rtm_logined_.store(false);
        break;
    }
}

void AgoraService::cb_rtm_data(const char* rtm_uid, const void* msg, size_t len,
                               rtm_message_type_e msg_type, const char*)
{
    /* RTM peer 信令路径 */
    AgoraService* s = s_inst_;
    if (s && msg && len > 0 && rtm_uid) {
        s->postMessage(std::string((const char*)msg, len),
                       std::string("rtm:peer:") + rtm_uid);
    }
    (void)msg_type;
}

void AgoraService::cb_rtm_send_result(const char*, uint32_t, rtm_msg_state_e) {}
void AgoraService::cb_rtm_subscribe_result(const char* channel, rtm_err_code_e err)
{
    LOG_INFO("[%s] RTM 订阅频道 %s: %s", TAG, channel ? channel : "",
             err == ERR_RTM_OK ? "成功" : "失败");
}
void AgoraService::cb_rtm_subscribe_data(const char* channel, const char* rtm_uid,
                                         const void* msg, size_t len,
                                         rtm_message_type_e, const char*)
{
    /* RTM channel 信令路径 */
    AgoraService* s = s_inst_;
    if (s && msg && len > 0) {
        s->postMessage(std::string((const char*)msg, len),
                       std::string("rtm:ch:") + (channel ? channel : "?") +
                       ":" + (rtm_uid ? rtm_uid : "?"));
    }
}
void AgoraService::cb_rtm_publish_result(const char*, uint32_t, rtm_msg_state_e) {}
