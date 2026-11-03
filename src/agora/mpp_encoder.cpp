#include "mpp_encoder.h"
#include "../util/log.h"

#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_meta.h>
#include <rockchip/rk_venc_cfg.h>   /* MppEncCfg + mpp_enc_cfg_* API (此版本无独立 mpp_enc_cfg.h) */
#include <rockchip/rk_venc_cmd.h>   /* MPP_ENC_SET_CFG / SET_IDR_FRAME / HeaderMode */
#include <rockchip/rk_venc_rc.h>    /* MppEncRcMode: MPP_ENC_RC_MODE_CBR */
#include <cstring>
#include <cerrno>

#define TAG "mpp"

bool MppEncoder::open(int width, int height, int fps, int bitrate_kbps, int gop,
                      bool nv12, std::string& err)
{
    if (opened_) {
        err = "编码器已打开";
        return false;
    }
    width_  = width;
    height_ = height;
    fps_    = fps;
    bitrate_kbps_ = bitrate_kbps;
    gop_    = gop;
    nv12_   = nv12;
    /* NV12 = w*h*1.5, YU12 = w*h*1.5 (布局不同, 大小相同) */
    frame_size_ = (size_t)width * height * 3 / 2;

    MppCtx   ctx = nullptr;
    MppApi*  mpi = nullptr;
    MppEncCfg cfg = nullptr;
    int ret;

    /* 1. 创建编码器上下文 */
    ret = mpp_create(&ctx, &mpi);
    if (ret != MPP_OK) {
        err = "mpp_create 失败: " + std::to_string(ret);
        return false;
    }

    ret = mpp_init(ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC);
    if (ret != MPP_OK) {
        err = "mpp_init(ENC/AVC) 失败: " + std::to_string(ret);
        mpp_destroy(ctx);
        return false;
    }

    /* 2. 编码参数配置 */
    ret = mpp_enc_cfg_init(&cfg);
    if (ret != MPP_OK) {
        err = "mpp_enc_cfg_init 失败: " + std::to_string(ret);
        mpp_destroy(ctx);
        return false;
    }

    /* prep: 输入格式与步长 */
    mpp_enc_cfg_set_s32(cfg, "prep:width",      width_);
    mpp_enc_cfg_set_s32(cfg, "prep:height",     height_);
    mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", width_);   /* 连续存储, stride=width */
    mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", height_);
    mpp_enc_cfg_set_s32(cfg, "prep:format", nv12_ ? MPP_FMT_YUV420SP : MPP_FMT_YUV420P);

    /* rc: CBR 码控 */
    mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_flex",   0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num",    fps_);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denorm", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_flex",  0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num",   fps_);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denorm", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:gop", gop_);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_target", bitrate_kbps_ * 1000);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_max",    bitrate_kbps_ * 1000 * 4 / 3);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_min",    bitrate_kbps_ * 1000 * 2 / 3);

    /* h264: Baseline profile (66) / level 4.0 —— Web 端 WebRTC 协商的通常是
     * Constrained Baseline (42e01f), Chrome 软解 (OpenH264) 仅支持 Baseline。
     * High profile (100) 在无硬解的浏览器上会收得到包但解不出画面。
     * Baseline 禁用 CABAC, 只能 CAVLC。 */
    mpp_enc_cfg_set_s32(cfg, "h264:profile", 66);
    mpp_enc_cfg_set_s32(cfg, "h264:level",   40);
    mpp_enc_cfg_set_s32(cfg, "h264:cabac_en", 0);
    mpp_enc_cfg_set_s32(cfg, "h264:cabac_idc", 0);
    mpp_enc_cfg_set_s32(cfg, "h264:trans8x8", 0);

    ret = mpi->control(ctx, MPP_ENC_SET_CFG, cfg);
    if (ret != MPP_OK) {
        err = "MPP_ENC_SET_CFG 失败: " + std::to_string(ret);
        mpp_enc_cfg_deinit(cfg);
        mpp_destroy(ctx);
        return false;
    }

    /* 3. 每个关键帧携带 SPS/PPS (新观众即入即看) */
    MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
    ret = mpi->control(ctx, MPP_ENC_SET_HEADER_MODE, &header_mode);
    if (ret != MPP_OK) {
        LOG_WARN("[%s] MPP_ENC_SET_HEADER_MODE 失败(%d), 关键帧可能不带头", TAG, ret);
    }

    /* 4. 缓冲组 (内部导入模式, encode 时写入输入帧)
     * MPP 1.5.x: mpp_buffer_group_get(group, type, mode, tag, caller),
     * mpp_buffer_group_get_internal 是 (group, type) 两参宏且依赖 MODULE_TAG,
     * 为不依赖 MODULE_TAG 直接调用完整函数。 */
    MppBufferGroup group = nullptr;
    ret = mpp_buffer_group_get(&group, MPP_BUFFER_TYPE_DRM,
                               MPP_BUFFER_INTERNAL, TAG, __FUNCTION__);
    if (ret != MPP_OK) {
        /* 部分 RK3588 内核无 DRM CMA, 回退 DMA_HEAP */
        ret = mpp_buffer_group_get(&group, MPP_BUFFER_TYPE_DMA_HEAP,
                                   MPP_BUFFER_INTERNAL, TAG, __FUNCTION__);
    }
    if (ret != MPP_OK) {
        err = "mpp_buffer_group_get 失败: " + std::to_string(ret);
        mpp_enc_cfg_deinit(cfg);
        mpp_destroy(ctx);
        return false;
    }

    ctx_       = ctx;
    mpi_       = mpi;
    enc_cfg_   = cfg;
    buf_group_ = group;
    opened_    = true;
    force_idr_ = false;
    pending_bitrate_kbps_.store(-1);

    LOG_INFO("[%s] 打开: %dx%d@%dfps %s %dkbps gop=%d (frame_size=%zu)",
             TAG, width_, height_, fps_, nv12_ ? "NV12" : "YU12",
             bitrate_kbps_, gop_, frame_size_);
    return true;
}

void MppEncoder::close()
{
    if (!opened_) return;
    opened_ = false;

    if (buf_group_) {
        mpp_buffer_group_put((MppBufferGroup)buf_group_);
        buf_group_ = nullptr;
    }
    if (enc_cfg_) {
        mpp_enc_cfg_deinit((MppEncCfg)enc_cfg_);
        enc_cfg_ = nullptr;
    }
    if (ctx_) {
        mpp_destroy((MppCtx)ctx_);
        ctx_ = nullptr;
        mpi_ = nullptr;
    }
    LOG_INFO("[%s] 编码器已关闭", TAG);
}

bool MppEncoder::applyBitrate(int kbps)
{
    if (kbps < 200)  kbps = 200;
    if (kbps > 4000) kbps = 4000;
    if (kbps == bitrate_kbps_) return true;

    MppEncCfg cfg = (MppEncCfg)enc_cfg_;
    mpp_enc_cfg_set_s32(cfg, "rc:bps_target", kbps * 1000);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_max",    kbps * 1000 * 4 / 3);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_min",    kbps * 1000 * 2 / 3);

    MppApi* mpi = (MppApi*)mpi_;
    int ret = mpi->control((MppCtx)ctx_, MPP_ENC_SET_CFG, cfg);
    if (ret != MPP_OK) {
        LOG_WARN("[%s] 动态调码率失败(%d): %dkbps", TAG, ret, kbps);
        return false;
    }
    LOG_INFO("[%s] 码率调整: %d → %d kbps", TAG, bitrate_kbps_, kbps);
    bitrate_kbps_ = kbps;
    return true;
}

bool MppEncoder::encode(const uint8_t* yuv, size_t len)
{
    if (!opened_) return false;
    if (len < frame_size_) {
        LOG_ERR("[%s] 输入帧过小: %zu < %zu", TAG, len, frame_size_);
        return false;
    }

    MppCtx  ctx = (MppCtx)ctx_;
    MppApi* mpi = (MppApi*)mpi_;

    /* lazy 处理: 码率调整 / 强制 IDR */
    int new_kbps = pending_bitrate_kbps_.exchange(-1);
    if (new_kbps > 0) applyBitrate(new_kbps);
    if (force_idr_.exchange(false)) {
        mpi->control(ctx, MPP_ENC_SET_IDR_FRAME, NULL);
    }

    /* 输入帧 → MPP buffer */
    MppBuffer buf = nullptr;
    int ret = mpp_buffer_get((MppBufferGroup)buf_group_, &buf, frame_size_);
    if (ret != MPP_OK) {
        LOG_ERR("[%s] mpp_buffer_get 失败: %d", TAG, ret);
        return false;
    }
    void* dst = mpp_buffer_get_ptr(buf);
    if (!dst) {
        mpp_buffer_put(buf);
        LOG_ERR("[%s] mpp_buffer_get_ptr 失败", TAG);
        return false;
    }
    memcpy(dst, yuv, frame_size_);

    MppFrame frame = nullptr;
    mpp_frame_init(&frame);
    mpp_frame_set_width(frame, width_);
    mpp_frame_set_height(frame, height_);
    mpp_frame_set_hor_stride(frame, width_);
    mpp_frame_set_ver_stride(frame, height_);
    mpp_frame_set_fmt(frame, nv12_ ? MPP_FMT_YUV420SP : MPP_FMT_YUV420P);
    mpp_frame_set_buffer(frame, buf);
    mpp_frame_set_eos(frame, 0);

    ret = mpi->encode_put_frame(ctx, frame);
    mpp_frame_deinit(&frame);
    mpp_buffer_put(buf);
    if (ret != MPP_OK) {
        LOG_ERR("[%s] encode_put_frame 失败: %d", TAG, ret);
        return false;
    }

    /* 取出所有已编码 packet */
    bool ok = true;
    for (;;) {
        MppPacket pkt = nullptr;
        ret = mpi->encode_get_packet(ctx, &pkt);
        if (ret != MPP_OK || !pkt) break;

        const void* pkt_data = mpp_packet_get_data(pkt);
        size_t      pkt_len  = mpp_packet_get_length(pkt);
        bool        is_eos   = mpp_packet_get_eos(pkt);

        if (pkt_data && pkt_len > 0 && packet_cb_) {
            /* KEY_OUTPUT_INTRA meta: 1=本 packet 为 intra 帧 (MPP 1.5.x 无 KEY_FRAME) */
            bool keyframe = false;
            MppMeta meta = mpp_packet_get_meta(pkt);
            if (meta) {
                int is_key = 0;
                mpp_meta_get_s32(meta, KEY_OUTPUT_INTRA, &is_key);
                keyframe = (is_key != 0);
            }
            packet_cb_((const uint8_t*)pkt_data, pkt_len, keyframe);
        }

        mpp_packet_deinit(&pkt);
        if (is_eos) break;
    }
    return ok;
}
