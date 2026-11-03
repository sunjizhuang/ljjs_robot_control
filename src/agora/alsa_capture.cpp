#include "alsa_capture.h"
#include "../util/log.h"

#include <alsa/asoundlib.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

#define TAG "alsa-capture"

bool AlsaCapture::open(const std::string& dev, int sample_rate, int channels,
                       int frame_ms, std::string& err)
{
    if (handle_) {
        err = "采集设备已打开";
        return false;
    }

    snd_pcm_t* pcm = nullptr;
    int ret = snd_pcm_open(&pcm, dev.c_str(), SND_PCM_STREAM_CAPTURE, 0);
    if (ret < 0) {
        err = "snd_pcm_open(捕获 " + dev + ") 失败: " + snd_strerror(ret);
        return false;
    }

    snd_pcm_hw_params_t* hw = nullptr;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);

    ret = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (ret < 0) { err = "set_access 失败: " + std::string(snd_strerror(ret)); goto FAIL; }

    ret = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
    if (ret < 0) { err = "set_format(S16_LE) 失败: " + std::string(snd_strerror(ret)); goto FAIL; }

    {
        unsigned int rate = (unsigned int)sample_rate;
        ret = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, 0);
        if (ret < 0) { err = "set_rate 失败: " + std::string(snd_strerror(ret)); goto FAIL; }
        rate_ = (int)rate;
    }

    ret = snd_pcm_hw_params_set_channels(pcm, hw, (unsigned)channels);
    if (ret < 0) { err = "set_channels 失败: " + std::string(snd_strerror(ret)); goto FAIL; }
    ch_ = channels;

    /* 周期 = frame_ms 一帧, 缓冲 4 周期。
     * 注意: 所有 hw 参数必须在 snd_pcm_hw_params() apply 之前设置。 */
    frame_samples_ = (unsigned)(rate_ * frame_ms / 1000);
    {
        snd_pcm_uframes_t period = frame_samples_;
        snd_pcm_uframes_t buffer = period * 4;
        ret = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, 0);
        if (ret < 0) { err = "set_period_size 失败: " + std::string(snd_strerror(ret)); goto FAIL; }
        ret = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer);
        if (ret < 0) { err = "set_buffer_size 失败: " + std::string(snd_strerror(ret)); goto FAIL; }
        frame_samples_ = (unsigned)period;
    }
    bytes_per_frame_ = frame_samples_ * ch_ * 2;

    ret = snd_pcm_hw_params(pcm, hw);
    if (ret < 0) { err = "snd_pcm_hw_params 失败: " + std::string(snd_strerror(ret)); goto FAIL; }

    ret = snd_pcm_prepare(pcm);
    if (ret < 0) { err = "snd_pcm_prepare 失败: " + std::string(snd_strerror(ret)); goto FAIL; }

    handle_ = pcm;
    dev_ = dev;
    frame_ms_ = (int)((uint64_t)frame_samples_ * 1000 / rate_);
    LOG_INFO("[%s] 已打开 %s: %dHz/%dch, 每帧 %u 样本 (%dms, %uB)",
             TAG, dev.c_str(), rate_, ch_, frame_samples_, frame_ms_, bytes_per_frame_);
    return true;

FAIL:
    snd_pcm_close(pcm);
    return false;
}

void AlsaCapture::close()
{
    stop();
    if (handle_) {
        snd_pcm_close((snd_pcm_t*)handle_);
        handle_ = nullptr;
        LOG_INFO("[%s] 设备已关闭", TAG);
    }
}

void AlsaCapture::start()
{
    if (!handle_ || running_) return;
    running_ = true;
    thread_ = std::thread(&AlsaCapture::captureLoop, this);
}

void AlsaCapture::stop()
{
    if (!running_) return;
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void AlsaCapture::captureLoop()
{
    snd_pcm_t* pcm = (snd_pcm_t*)handle_;
    std::vector<int16_t> buf(frame_samples_ * ch_);

    /* 电平检测: 每 10s 打印一次峰值, 便于诊断"采到静音"类问题 */
    int16_t peak = 0;
    int64_t level_ts = 0;

    while (running_) {
        snd_pcm_sframes_t n = snd_pcm_readi(pcm, buf.data(), frame_samples_);
        if (n == -EPIPE) {
            /* overrun, 重新准备 */
            LOG_WARN("[%s] overrun, 重新准备", TAG);
            snd_pcm_prepare(pcm);
            continue;
        }
        if (n < 0) {
            LOG_ERR("[%s] readi 失败: %s", TAG, snd_strerror((int)n));
            break;
        }
        if (n > 0) {
            /* 累计峰值 */
            for (snd_pcm_uframes_t i = 0; i < (snd_pcm_uframes_t)n * ch_; i++) {
                int16_t v = buf[i];
                if (v < 0) v = (int16_t)(-v);
                if (v > peak) peak = v;
            }
            int64_t now_ms = (int64_t)(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
            if (now_ms - level_ts >= 10000) {
                LOG_INFO("[%s] 电平(1s峰值/10s): %d / 32767 %s", TAG, peak,
                         peak < 100 ? "  <-- 近似静音, 检查麦克风/输入源" : "");
                peak = 0;
                level_ts = now_ms;
            }
        }
        if (n > 0 && frame_cb_) {
            frame_cb_(buf.data(), (size_t)n * ch_);
        }
    }
    running_ = false;
}
