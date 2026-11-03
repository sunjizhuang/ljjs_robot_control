/* ════════════════════════════════════════════════════════════════
 * agora_ab.c — RTSA A/B 对照推流工具 (自包含, 不依赖官方 example 依赖)
 *
 * 用途: 板端以官方 hello_rtsa 同款调用方式推 H264 文件 + PCM 音频,
 *       用于区分 "我们集成侧的问题" 与 "SDK/环境/账号侧的问题":
 *         - 本工具 + Web(h264) 能出画面  → 问题在 lljs_ctrl 集成侧
 *         - 本工具 + Web 也无画面        → 问题在 SDK/环境/账号侧
 *
 * 素材: SDK 仓库 example/test_data/send_video.h264 (x264, 320x240@15fps)
 *              example/test_data/send_audio_16k_1ch.pcm (16k/1ch/16bit)
 *
 * 编译 (Linux 交叉环境):
 *   cd test/agora_ab && mkdir -p build && cd build && cmake .. && make
 *
 * 板端运行 (先停掉 lljs_ctrl, 避免同频道互相干扰):
 *   cd /opt/lljs/ab && LD_LIBRARY_PATH=. ./agora_ab \
 *       -i <app_id> -c testChannel -u 1002 \
 *       -v send_video.h264 -a send_audio_16k_1ch.pcm -f 15
 * ════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <time.h>

#include "agora_rtc_api.h"

/* ───────────── 全局状态 ───────────── */
static bool g_joined = false;
static volatile bool g_stop = false;
static connection_id_t g_conn = 0;
static uint32_t g_fps = 15;

static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

static void sig_handler(int sig) { (void)sig; g_stop = true; }

/* ───────────── SDK 回调 (按头文件顺序全量注册) ───────────── */
static void cb_error(connection_id_t c, int code, const char *msg)
{ (void)c; printf("[cb] error: code=%d msg=%s\n", code, msg); }

static void cb_join_success(connection_id_t c, uint32_t uid, int elapsed_ms)
{ (void)c; g_joined = true; printf("[cb] 入频成功: uid=%u elapsed=%dms\n", uid, elapsed_ms); }

static void cb_reconnecting(connection_id_t c)
{ (void)c; g_joined = false; printf("[cb] 重连中...\n"); }

static void cb_connection_lost(connection_id_t c)
{ (void)c; g_joined = false; printf("[cb] 连接丢失(>10s)\n"); }

static void cb_rejoin_success(connection_id_t c, uint32_t uid, int elapsed_ms)
{ (void)c; g_joined = true; printf("[cb] 重入频成功: uid=%u elapsed=%dms\n", uid, elapsed_ms); }

static void cb_license_failure(connection_id_t c, int err)
{ (void)c; printf("[cb] ★ License 校验失败: err=%d (%s)\n", err, agora_rtc_err_2_str(err)); }

static void cb_token_expire(connection_id_t c, const char *token)
{ (void)c; (void)token; printf("[cb] token 即将过期\n"); }

/* ───────────── RTM 回调 (-r 时使用, 对齐 lljs_ctrl 行为) ───────────── */
static volatile bool g_rtm_logined = false;

static void cb_rtm_event(const char *rtm_uid, rtm_event_type_e type, rtm_err_code_e err)
{
    (void)rtm_uid;
    printf("[rtm] event type=%d err=%d%s\n", (int)type, (int)err,
           (type == RTM_EVENT_TYPE_LOGIN) ? " (110=ERR_INVALID_TOKEN: 项目已开 token 鉴权, 需要 -T 传入 RTM token)" : "");
    if (type == RTM_EVENT_TYPE_LOGIN && err == 0) g_rtm_logined = true;
}
static void cb_rtm_data(const char *rtm_uid, const void *msg, size_t msg_len,
                        rtm_message_type_e msg_type, const char *custom_type)
{ (void)rtm_uid; (void)msg; (void)msg_len; (void)msg_type; (void)custom_type; }
static void cb_rtm_send_result(const char *rtm_uid, uint32_t msg_id, rtm_msg_state_e state)
{ (void)rtm_uid; (void)msg_id; (void)state; }
static void cb_rtm_subscribe_result(const char *channel_name, rtm_err_code_e err_code)
{ (void)channel_name; (void)err_code; }
static void cb_rtm_subscribe_data(const char *channel_name, const char *rtm_uid,
                                  const void *msg, size_t msg_len, rtm_message_type_e msg_type,
                                  const char *custom_type)
{ (void)channel_name; (void)rtm_uid; (void)msg; (void)msg_len; (void)msg_type; (void)custom_type; }
static void cb_rtm_publish_result(const char *channel_name, uint32_t msg_id, rtm_msg_state_e state)
{ (void)channel_name; (void)msg_id; (void)state; }

static void cb_user_joined(connection_id_t c, uint32_t uid, int elapsed_ms)
{ (void)c; printf("[cb] 远端用户上线: %u (elapsed=%dms)\n", uid, elapsed_ms); }

static void cb_user_offline(connection_id_t c, uint32_t uid, int reason)
{ (void)c; printf("[cb] 远端用户离线: %u reason=%d\n", uid, reason); }

static void cb_user_joined_ua(connection_id_t c, const user_info_t *u, int e)
{ (void)c; (void)u; (void)e; }

static void cb_user_offline_ua(connection_id_t c, const user_info_t *u, int r)
{ (void)c; (void)u; (void)r; }

static void cb_user_info_updated(connection_id_t c, const user_info_t *u)
{ (void)c; (void)u; }

static void cb_user_mute_audio(connection_id_t c, uint32_t uid, bool muted)
{ (void)c; printf("[cb] 用户 %u %s 音频\n", uid, muted ? "关闭" : "开启"); }

static void cb_user_mute_video(connection_id_t c, uint32_t uid, bool muted)
{ (void)c; printf("[cb] 用户 %u %s 视频\n", uid, muted ? "关闭" : "开启"); }

/* 下行接收统计 (对照实验: Web 推流时板端能否收到) */
static volatile uint32_t g_rx_audio = 0, g_rx_video = 0;

static void cb_audio_data(connection_id_t c, uint32_t uid, uint16_t ts,
                          const void *d, size_t l, const audio_frame_info_t *i)
{ (void)c; (void)uid; (void)ts; (void)d; (void)i; g_rx_audio++; }

static void cb_mixed_audio_data(connection_id_t c, const void *d, size_t l,
                                const audio_frame_info_t *i)
{ (void)c; (void)d; (void)l; (void)i; }

static void cb_video_data(connection_id_t c, uint32_t uid, uint16_t ts,
                          const void *d, size_t l, const video_frame_info_t *i)
{ (void)c; (void)uid; (void)ts; (void)d; (void)l; (void)i; g_rx_video++; }

static void cb_target_bitrate(connection_id_t c, uint32_t target_bps)
{ (void)c; printf("[cb] ★ 目标码率变化: %u bps\n", target_bps); }

static void cb_key_frame_req(connection_id_t c, uint32_t uid, video_stream_type_e st)
{ (void)c; printf("[cb] 远端 %u 请求关键帧 (stream=%d)\n", uid, (int)st); }

/* SDK 实际出包统计 (按类型: 1=audio 2=video 3=stream 4=rdt) */
static volatile uint32_t g_out_cnt[5] = { 0 };

static void cb_rtc_stats(connection_id_t c, rtc_stats_t stats)
{
    (void)c;
    static uint32_t cnt = 0;
    if (++cnt % 5 != 0) return;   /* SDK 2s 一次, 打印 10s 节流 */
    printf("[stats] tx音频=%ukbps tx视频=%ukbps (输入=%ukbps) rx音频=%uk rx视频=%uk "
           "lastmile=%ums 用户=%u | 出包: audio=%u video=%u | 收到: audio帧=%u video帧=%u\n",
           (unsigned)stats.tx_audio_kbitrate, (unsigned)stats.tx_video_kbitrate,
           (unsigned)stats.tx_video_input_kbitrate,
           (unsigned)stats.rx_audio_kbitrate, (unsigned)stats.rx_video_kbitrate,
           (unsigned)stats.lastmile_delay, (unsigned)stats.user_count,
           g_out_cnt[RTC_PACKET_TYPE_AUDIO], g_out_cnt[RTC_PACKET_TYPE_VIDEO],
           g_rx_audio, g_rx_video);
}

static void cb_media_ctrl_msg(connection_id_t c, uint32_t uid, const void *p, size_t l)
{ (void)c; (void)uid; (void)p; (void)l; }

static void cb_rdt_state(connection_id_t c, uint32_t uid, rdt_state_e s)
{ (void)c; (void)uid; (void)s; }

static void cb_rdt_msg(connection_id_t c, uint32_t uid, rdt_stream_type_e t,
                       const void *m, size_t l)
{ (void)c; (void)uid; (void)t; (void)m; (void)l; }

static void cb_stream_message(connection_id_t c, uint32_t uid, int sid,
                              const char *d, size_t l, uint64_t ts)
{ (void)c; (void)uid; (void)sid; (void)d; (void)l; (void)ts; }

static int cb_packet_output_hook(connection_id_t c, rtc_packet_type_e t,
                                 const void *src, uint16_t ss, void *dst, uint16_t ds)
{
    (void)c; (void)src; (void)dst; (void)ds;
    if (t < 1 || t > 4) return 0;
    uint32_t n = ++g_out_cnt[t];
    if (n <= 3 || (n % 1000) == 0)
        printf("[pkt] SDK 出包: type=%u size=%u 该类型累计=%u\n", (unsigned)t, (unsigned)ss, n);
    return 0;
}

static int cb_packet_input_hook(connection_id_t c, rtc_packet_type_e t,
                                const void *src, uint16_t ss, void *dst, uint16_t ds)
{
    (void)c; (void)t; (void)src; (void)ss; (void)dst; (void)ds;
    return 0;
}

/* ───────────── H264 Annex-B 分帧 (无依赖) ───────────── */
typedef struct {
    uint8_t *ptr;
    size_t   len;
    bool     is_key;
} au_t;

static uint8_t *g_vbuf  = NULL;
static size_t   g_vlen  = 0;
static au_t    *g_aus   = NULL;
static int      g_au_cnt = 0;
static int      g_au_cap = 0;

static size_t start_code_len(const uint8_t *p, size_t remain)
{
    if (remain >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) return 4;
    if (remain >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1) return 3;
    return 0;
}

static void push_au(size_t off, size_t len, bool key)
{
    if (g_au_cnt >= g_au_cap) {
        g_au_cap *= 2;
        g_aus = realloc(g_aus, sizeof(au_t) * (size_t)g_au_cap);
    }
    g_aus[g_au_cnt].ptr    = g_vbuf + off;
    g_aus[g_au_cnt].len    = len;
    g_aus[g_au_cnt].is_key = key;
    g_au_cnt++;
}

/* 按 "相邻 VCL NAL 属于不同帧" 的规则切访问单元 */
static int split_h264(void)
{
    size_t pos = 0, au_start = (size_t)-1;
    bool au_has_vcl = false, au_key = false;

    g_au_cap = 256;
    g_aus = malloc(sizeof(au_t) * (size_t)g_au_cap);
    g_au_cnt = 0;

    while (pos < g_vlen) {
        size_t sc = start_code_len(g_vbuf + pos, g_vlen - pos);
        if (sc == 0) { pos++; continue; }
        size_t nal = pos + sc;
        if (nal >= g_vlen) break;
        int t = g_vbuf[nal] & 0x1F;
        bool vcl = (t >= 1 && t <= 5);

        if (vcl && au_has_vcl) {          /* 新帧开始 */
            push_au(au_start, pos - au_start, au_key);
            au_has_vcl = false;
            au_key = false;
            au_start = pos;
        }
        if (au_start == (size_t)-1) au_start = pos;
        if (vcl) {
            au_has_vcl = true;
            if (t == 5) au_key = true;    /* IDR */
        }
        pos = nal + 1;
    }
    if (au_start != (size_t)-1 && au_has_vcl)
        push_au(au_start, g_vlen - au_start, au_key);

    return g_au_cnt > 0 ? 0 : -1;
}

static int read_file(const char *path, uint8_t **out, size_t *out_len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { printf("无法打开文件: %s\n", path); return -1; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)sz);
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        fclose(fp); free(buf); return -1;
    }
    fclose(fp);
    *out = buf;
    *out_len = (size_t)sz;
    return 0;
}

/* ───────────── main ───────────── */
int main(int argc, char **argv)
{
    const char *app_id = NULL, *channel = NULL, *token = "", *license = "";
    const char *video_path = NULL, *audio_path = NULL;
    const char *product_id = "agora_ab";
    const char *rtm_uid = NULL;
    const char *rtm_token = "";
    uint32_t uid = 1002;
    uint32_t max_bps = 2000000;
    bool no_hooks = false;                     /* -H: 不注册 packet hooks (对齐官方 hello_rtsa) */
    uint32_t area = 0x1;                       /* CN, 与 lljs_ctrl 一致 */
    int log_level = 7;                         /* INFO */
    bool send_video = false, send_audio = false;

    int ch;
    while ((ch = getopt(argc, argv, "i:c:u:t:T:l:v:a:f:p:r:A:L:m:Hh")) != -1) {
        switch (ch) {
        case 'H': no_hooks = true; break;
        case 'i': app_id = optarg; break;
        case 'c': channel = optarg; break;
        case 'u': uid = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 't': token = optarg; break;
        case 'l': license = optarg; break;
        case 'v': video_path = optarg; break;
        case 'a': audio_path = optarg; break;
        case 'f': g_fps = (uint32_t)atoi(optarg); break;
        case 'p': product_id = optarg; break;
        case 'r': rtm_uid = optarg; break;
        case 'T': rtm_token = optarg; break;
        case 'A': area = (uint32_t)strtoul(optarg, NULL, 16); break;
        case 'L': log_level = atoi(optarg); break;
        case 'm': max_bps = (uint32_t)strtoul(optarg, NULL, 10); break;
        default:
            printf("用法: %s -i <app_id> -c <channel> [-u uid] [-t token] [-T rtm_token] [-l license]\n"
                   "         [-v video.h264] [-a audio.pcm] [-f fps] [-p product_id] [-r rtm_uid] [-A area_hex] [-L log_level] [-m max_bps]\n"
                   "         [-H 关闭 packet hooks (对齐官方 hello_rtsa)]\n",
                   argv[0]);
            return -1;
        }
    }
    if (!app_id || !channel) { printf("必须提供 -i app_id 和 -c channel\n"); return -1; }
    printf("[ab] 参数: app_id=\"%s\" channel=\"%s\" uid=%u product_id=\"%s\" rtm_uid=%s token=%s\n",
           app_id, channel, uid, product_id, rtm_uid ? rtm_uid : "无",
           token[0] ? "有" : "无");
    if (g_fps == 0) g_fps = 15;
    send_video = (video_path != NULL);
    send_audio = (audio_path != NULL);
    if (!send_video && !send_audio) { printf("至少提供 -v 或 -a 之一\n"); return -1; }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    /* 加载视频文件 */
    if (send_video) {
        if (read_file(video_path, &g_vbuf, &g_vlen) < 0) return -1;
        if (split_h264() < 0) { printf("H264 分帧失败\n"); return -1; }
        int keys = 0;
        for (int i = 0; i < g_au_cnt; i++) keys += g_aus[i].is_key ? 1 : 0;
        printf("[ab] 视频: %s → %d 帧 (其中关键帧 %d)\n", video_path, g_au_cnt, keys);
    }
    /* 加载音频文件 */
    uint8_t *g_abuf = NULL;
    size_t g_alen = 0;
    if (send_audio) {
        if (read_file(audio_path, &g_abuf, &g_alen) < 0) return -1;
        printf("[ab] 音频: %s → %zu 字节 (20ms/640B ≈ %zu 秒)\n",
               audio_path, g_alen, g_alen / 640 / 50);
    }

    /* 1. init */
    agora_rtc_event_handler_t h = { 0 };
    h.on_error                     = cb_error;
    h.on_join_channel_success      = cb_join_success;
    h.on_reconnecting              = cb_reconnecting;
    h.on_connection_lost           = cb_connection_lost;
    h.on_rejoin_channel_success    = cb_rejoin_success;
    h.on_license_validation_failure = cb_license_failure;
    h.on_token_privilege_will_expire = cb_token_expire;
    h.on_user_joined               = cb_user_joined;
    h.on_user_offline              = cb_user_offline;
    h.on_user_joined_with_user_account = cb_user_joined_ua;
    h.on_user_offline_with_user_account = cb_user_offline_ua;
    h.on_user_info_updated         = cb_user_info_updated;
    h.on_user_mute_audio           = cb_user_mute_audio;
    h.on_user_mute_video           = cb_user_mute_video;
    h.on_audio_data                = cb_audio_data;
    h.on_mixed_audio_data          = cb_mixed_audio_data;
    h.on_video_data                = cb_video_data;
    h.on_target_bitrate_changed    = cb_target_bitrate;
    h.on_key_frame_gen_req         = cb_key_frame_req;
    h.on_rtc_stats                 = cb_rtc_stats;
    h.on_media_ctrl_msg            = cb_media_ctrl_msg;
    h.on_rdt_state                 = cb_rdt_state;
    h.on_rdt_msg                   = cb_rdt_msg;
    h.on_stream_message            = cb_stream_message;
    if (!no_hooks) {
        h.on_packet_input_hook     = cb_packet_input_hook;
        h.on_packet_output_hook    = cb_packet_output_hook;
    }

    rtc_service_option_t sopt = { 0 };
    sopt.area_code = area;
    snprintf(sopt.product_id, sizeof(sopt.product_id), "%s", product_id);
    sopt.log_cfg.log_path = ".";
    sopt.log_cfg.log_disable = (log_level == -1);
    sopt.log_cfg.log_level = log_level;
    sopt.use_string_uid = false;
    snprintf(sopt.license_value, sizeof(sopt.license_value), "%s", license);

    int r = agora_rtc_init(app_id, &h, &sopt);
    if (r < 0) { printf("agora_rtc_init 失败: %s\n", agora_rtc_err_2_str(r)); return -1; }
    printf("[ab] SDK 初始化完成, 版本 %s, area=0x%x\n", agora_rtc_get_version(), area);

    /* 2. connection + bwe (与 hello_rtsa 一致) */
    r = agora_rtc_create_connection(&g_conn);
    if (r < 0) { printf("create_connection 失败: %s\n", agora_rtc_err_2_str(r)); return -1; }

    r = agora_rtc_set_bwe_param(g_conn, max_bps / 10, max_bps, max_bps / 2);
    printf("[ab] BWE: min=%uk max=%uk start=%uk (ret=%d)\n",
           max_bps / 10000, max_bps / 1000, max_bps / 2000, r);

    /* 2.5 RTM 登录 (对齐 lljs_ctrl: RTM 先于 RTC 入频) */
    if (rtm_uid) {
        agora_rtm_handler_t rh = {
            cb_rtm_event, cb_rtm_data, cb_rtm_send_result,
            cb_rtm_subscribe_result, cb_rtm_subscribe_data, cb_rtm_publish_result,
        };
        r = agora_rtm_login(rtm_uid, rtm_token[0] ? rtm_token : NULL, &rh);
        if (r < 0) { printf("rtm_login 失败: %s\n", agora_rtc_err_2_str(r)); return -1; }
        printf("[ab] RTM 登录请求已发出: rtm_uid=%s, 等待...\n", rtm_uid);
        for (int i = 0; i < 100 && !g_rtm_logined; i++) usleep(100000);
        if (!g_rtm_logined) { printf("RTM 登录超时 (10s)\n"); return -1; }
        printf("[ab] RTM 已登录\n");
    }

    /* 3. join */
    rtc_channel_options_t copt = { 0 };
    copt.auto_subscribe_audio = true;
    copt.auto_subscribe_video = true;
    if (send_audio) {
        copt.audio_codec_opt.pcm_duration   = 20;
        copt.audio_codec_opt.audio_codec_type = AUDIO_CODEC_TYPE_OPUS;
        copt.audio_codec_opt.pcm_sample_rate  = 16000;
        copt.audio_codec_opt.pcm_channel_num  = 1;
    }
    void *p_token = (strlen(token) == 0) ? NULL : (void *)token;
    r = agora_rtc_join_channel(g_conn, channel, uid, p_token, &copt);
    if (r < 0) { printf("join_channel 失败: %s\n", agora_rtc_err_2_str(r)); return -1; }
    printf("[ab] 入频请求已发出: channel=%s uid=%u, 等待 join success...\n", channel, uid);

    /* 4. 发送主循环 */
    int64_t next_a = 0, next_v = 0;
    size_t a_pos = 0;
    int v_idx = 0, v_sent = 0, a_sent = 0, last_v = 0, last_a = 0;
    int64_t stat_ts = now_us();

    while (!g_stop) {
        int64_t now = now_us();
        if (!g_joined) {
            usleep(100 * 1000);
            next_a = next_v = now_us();
            continue;
        }

        if (send_audio && now >= next_a) {
            size_t n = (g_alen - a_pos) >= 640 ? 640 : (g_alen - a_pos);
            if (n == 640) {
                audio_frame_info_t info = { 0 };
                info.data_type = AUDIO_DATA_TYPE_PCM;
                if (agora_rtc_send_audio_data(g_conn, g_abuf + a_pos, n, &info) < 0)
                    printf("[ab] send_audio_data 失败\n");
                else a_sent++;
            }
            a_pos += n;
            if (a_pos >= g_alen) a_pos = 0;      /* 循环播放 */
            next_a += 20000;
            if (next_a < now) next_a = now + 20000;
        }

        if (send_video && now >= next_v) {
            au_t *au = &g_aus[v_idx % g_au_cnt];
            video_frame_info_t info = { 0 };
            info.data_type  = VIDEO_DATA_TYPE_H264;
            info.stream_type = VIDEO_STREAM_HIGH;
            info.frame_type = au->is_key ? VIDEO_FRAME_KEY : VIDEO_FRAME_DELTA;
            info.frame_rate = (uint16_t)g_fps;
            if (agora_rtc_send_video_data(g_conn, au->ptr, au->len, &info) < 0)
                printf("[ab] send_video_data 失败\n");
            else {
                v_sent++;
                if (au->is_key)
                    printf("[ab] 视频帧 #%d (关键帧, %zu 字节) 已发出\n",
                           v_idx + 1, au->len);
            }
            v_idx++;
            next_v += (int64_t)(1000000 / g_fps);
            if (next_v < now) next_v = now + (int64_t)(1000000 / g_fps);
        }

        if (now - stat_ts >= 10000000) {         /* 10s 统计, 对齐 lljs_ctrl */
            printf("[ab] 统计(10s): 视频帧=%d (+%d) 音频帧=%d (+%d) | SDK出包 audio=%u video=%u\n",
                   v_sent, v_sent - last_v, a_sent, a_sent - last_a,
                   (unsigned)g_out_cnt[RTC_PACKET_TYPE_AUDIO],
                   (unsigned)g_out_cnt[RTC_PACKET_TYPE_VIDEO]);
            last_v = v_sent;
            last_a = a_sent;
            stat_ts = now;
        }

        usleep(2000);
    }

    printf("[ab] 退出中...\n");
    agora_rtc_leave_channel(g_conn);
    agora_rtc_destroy_connection(g_conn);
    agora_rtc_fini();
    printf("[ab] 已退出\n");
    return 0;
}
