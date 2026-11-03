#ifndef YAML_CONFIG_H
#define YAML_CONFIG_H

#include <string>
#include <vector>

/*
 * 应用配置 —— YAML 配置文件加载
 *
 * 支持的 YAML 语法（极简子集，够本项目用）:
 *   - 注释:        以 # 开头（支持行尾注释）
 *   - 顶层键值:    key: value
 *   - 嵌套区块:    key: 换行 + 缩进子键
 *   - 字符串:      带引号或不带引号
 *   - 整数/浮点:   十进制
 *   - 布尔:        true/false/yes/no
 *
 * 不支持: 列表、锚点、多文档、流式风格 (JSON 式 {})
 */

/* ═══════════════════════════════════════════════════════════
 * Agora (RTSA) 音视频 + 信令集成配置
 *
 * 命名约定: 业务层统一使用 RTSA 称呼音视频通道；
 *           SDK C API 名 (agora_rtc_*) 保持官方原名不变。
 * ═══════════════════════════════════════════════════════════ */
struct AgoraConfig {
    /* ── 全局总开关 ── */
    bool        enabled    = false;      /* 总开关: false 时整个 Agora 模块不初始化 */
    std::string app_id     = "";         /* 声网 App ID (必填) */
    std::string license    = "";         /* 商用 License 值 (对齐 hello_rtsa --license, 可空) */
    std::string area_code  = "CN";       /* CN / GLOB */
    int         log_level  = 2;          /* SDK 日志级别: -1 关闭, 0-8 对应 rtc_log_level_e */

    /* ── RTM 信令 (常驻) ── */
    struct Rtm {
        bool        enabled     = true;
        std::string uid         = "robot-1";     /* 机器人 RTM uid */
        std::string token       = "";            /* RTM Token (与 RTSA/RTC Token 不可混用; App ID 鉴权留空) */
        std::string mode        = "both";        /* peer / channel / both */
        std::string channel     = "robot-cmd";   /* channel 模式指令+遥测频道 */
        std::string operator_uid = "operator-1"; /* peer 模式默认对端 */
        bool        ack_enabled = true;          /* av_* 指令回执 */
    } rtm;

    /* ── RTSA 音视频通道 ── */
    struct Rtsa {
        std::string channel       = "robot-av"; /* 默认音视频频道 */
        int         uid           = 1001;       /* 机器人 RTSA uid (0=SDK 自动分配) */
        std::string token         = "";         /* RTSA Token (官方称 RTC Token; App ID 鉴权留空) */
        bool        token_renew   = true;       /* 过期自动 renew */
        bool        rtcm_signaling = true;      /* 入频后启用 RTCM 信令路径 (低延迟) */
        bool        auto_join     = false;      /* 启动即入频推流 */
        bool        subscribe     = false;      /* 是否订阅远端媒体 (纯推流 false) */
    } rtsa;

    /* ── 视频 (V4L2 + MPP) ── */
    struct Video {
        bool        enabled      = true;
        std::string device       = "/dev/video11"; /* UVC 设备节点 */
        std::string mode         = "h264";        /* h264 (YUV→MPP硬编H264, 默认) / yuv (YUV420直传SDK) */
        int         width        = 1280;
        int         height       = 720;
        int         fps          = 25;
        int         bitrate_kbps = 1800;           /* 建议区间 1500-2000 */
        int         gop          = 75;             /* 关键帧间隔: 3s @25fps */
    } video;

    /* ── 音频 (ALSA) ── */
    struct Audio {
        bool        enabled     = true;
        std::string device_in  = "default";  /* 采集设备 */
        std::string device_out = "default";  /* 下行播放设备 */
        int         sample_rate = 16000;
        int         channels    = 1;
        int         frame_ms    = 20;         /* 每帧时长: 20ms */
        int         play_sample_rate = 0;     /* 下行播放采样率; 0=跟随 sample_rate。
                                                 若播放音调异常(SDK 解码输出采样率不同), 手动指定 */
        bool        listen      = false;      /* 启动即开下行对讲播放 (false=用 av_listen 指令控制) */
    } audio;

    /* ── VAD (预留, 本期无效) ── */
    bool vad_enabled = false;

    bool has_rtsa_token() const { return !rtsa.token.empty(); }
    bool has_rtm_token()  const { return !rtm.token.empty(); }
};

struct AppConfig {
    /* ── 全局 ── */
    std::string device     = "both";   /* yunji / lite3 / both */
    std::string log_level  = "info";   /* debug / info / warn / error */
    std::string default_active = "";   /* 默认激活设备名，空 = 自动 */

    /* ── 控制端网络模式 ── */
    std::string network_mode = "client"; /* 兼容旧配置：server / client；新配置使用独立开关 */
    bool        network_server_enabled = false;
    bool        network_client_enabled = true;
    bool        network_mqtt_enabled = false;
    std::string client_server_ip = "127.0.0.1";
    int         client_server_port = 9000;
    int         client_reconnect_interval_ms = 5000;
    std::string client_edge_id = "edge-1";
    std::string client_robot_id = "robot-1";
    std::string client_device = ""; /* yunji / lite3；留空时由主程序按当前设备推断 */

    /* ── MQTT 控制端 ── */
    /* MQTT Broker/Server（由 lljs_ctrl 管理官方 mosquitto Broker 进程） */
    bool        mqtt_broker_enabled = false;
    std::string mqtt_broker_bind = "0.0.0.0";
    int         mqtt_broker_port = 1883;
    std::string mqtt_broker_executable = "";
    std::string mqtt_broker_config = "/tmp/lljs_ctrl_mosquitto.conf";
    bool        mqtt_broker_allow_anonymous = true;

    /* MQTT Client：可连接本地 lljs_ctrl Broker，也可连接其它 Broker */
    bool        mqtt_client_enabled = false;
    std::string mqtt_host = "127.0.0.1";
    int         mqtt_port = 1883;
    std::string mqtt_client_id = "lljs_ctrl";
    std::string mqtt_username = "";
    std::string mqtt_password = "";
    int         mqtt_reconnect_interval_ms = 5000;

    /* ── 云迹 SC668s ── */
    std::string yunji_ip   = "192.168.10.10";
    int         yunji_port = 31001;

    /* ── 云深处 Lite3 ── */
    std::string lite3_ip   = "192.168.2.1";
    int         lite3_port = 43893;
    int         lite3_receive_port = 43897;

    /* ── TCP Server（外部控制端接入） ── */
    std::string server_bind = "0.0.0.0";
    int         server_port = 9000;

    /* ── 遥控参数（云迹） ── */
    int    joy_hz       = 5;      /* joy_control 发送频率 (Hz)，需 >2 */
    double max_linear   = 0.5;    /* m/s，API 限定 [-0.5, 0.5] */
    double max_angular  = 1.0;    /* rad/s，API 限定 [-1.0, 1.0] */
    double linear_step  = 0.05;   /* 每次按键线速度变化量 */
    double angular_step = 0.1;    /* 每次按键角速度变化量 */

    /* ── Lite3 心跳 ── */
    int lite3_heartbeat_ms = 20;  /* 心跳间隔 (ms) */

    /* ── 状态轮询 ── */
    int status_poll_hz = 2;       /* 机器人状态查询频率 (Hz) */

    /* ── 日志 ── */
    int log_queue_size = 20;      /* 终端显示的最近日志条数 */

    /* ── Agora (RTSA) 音视频 + 信令 ── */
    AgoraConfig agora;

    /* ── 加载来源记录 ── */
    std::string loaded_from;      /* 非空表示从该文件加载 */

    /* ── 派生量 ── */
    int joySendIntervalMs() const { return (joy_hz > 0) ? (1000 / joy_hz) : 200; }

    /* ── 操作 ── */

    /* 从 YAML 文件加载（缺失项保留默认值）。
     * 成功返回 true；失败返回 false 并填充 err_msg（文件不可读/语法错误） */
    bool loadFromFile(const std::string& path, std::string& err_msg);

    /* 逻辑校验。合法返回 true；否则返回 false 并填充 err_msg */
    bool validate(std::string& err_msg) const;

    /* 输出最终生效的完整配置（含默认值填充后的结果） */
    std::string dump() const;
};

/* 全局配置实例（main 中加载一次，各模块直接读取） */
extern AppConfig g_config;

#endif /* YAML_CONFIG_H */
