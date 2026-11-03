#include "alsa_player.h"
#include "../util/log.h"

#include <alsa/asoundlib.h>
#include <chrono>
#include <cstring>

#define TAG "alsa-play"

bool AlsaPlayer::open(const std::string& dev, int sample_rate, int channels, std::string& err)
{
    if (handle_) {
        err = "播放设备已打开";
        return false;
    }

    snd_pcm_t* pcm = nullptr;
    int ret = snd_pcm_open(&pcm, dev.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (ret < 0) {
        err = "snd_pcm_open(播放 " + dev + ") 失败: " + snd_strerror(ret);
        return false;
    }

    snd_pcm_hw_params_t* hw = nullptr;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);

    ret = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (ret < 0) { err = "set_access 失败: " + std::string(snd_strerror(ret)); goto FAIL; }

    ret = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
    if (ret < 0) { err = "set_format 失败: " + std::string(snd_strerror(ret)); goto FAIL; }

    {
        unsigned int rate = (unsigned)sample_rate;
        ret = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, 0);
        if (ret < 0) { err = "set_rate 失败: " + std::string(snd_strerror(ret)); goto FAIL; }
        rate_ = (int)rate;
    }

    ret = snd_pcm_hw_params_set_channels(pcm, hw, (unsigned)channels);
    if (ret < 0) { err = "set_channels 失败: " + std::string(snd_strerror(ret)); goto FAIL; }
    ch_ = channels;

    ret = snd_pcm_hw_params(pcm, hw);
    if (ret < 0) { err = "snd_pcm_hw_params 失败: " + std::string(snd_strerror(ret)); goto FAIL; }

    ret = snd_pcm_prepare(pcm);
    if (ret < 0) { err = "snd_pcm_prepare 失败: " + std::string(snd_strerror(ret)); goto FAIL; }

    /* RingBuffer: 上限 1040ms, 目标水位 200ms */
    ring_cap_  = (size_t)rate_ * ch_ * 1040 / 1000;
    ring_high_ = (size_t)rate_ * ch_ * 1000;  /* >1000ms 即触发丢旧 */
    ring_.assign(ring_cap_ * 2, 0);
    rd_ = wr_ = cnt_ = 0;

    handle_ = pcm;
    LOG_INFO("[%s] 已打开 %s: %dHz/%dch (ring 上限 %zu ms)",
             TAG, dev.c_str(), rate_, ch_, (size_t)1040);
    return true;

FAIL:
    snd_pcm_close(pcm);
    return false;
}

void AlsaPlayer::close()
{
    stop();
    if (handle_) {
        snd_pcm_close((snd_pcm_t*)handle_);
        handle_ = nullptr;
        LOG_INFO("[%s] 设备已关闭", TAG);
    }
}

void AlsaPlayer::write(const int16_t* samples, size_t count)
{
    if (!handle_ || count == 0) return;

    std::lock_guard<std::mutex> lk(ring_mtx_);
    /* 超上限: 丢最旧，保证不累积延迟 */
    if (cnt_ + count > ring_high_) {
        size_t drop = cnt_ + count - ring_high_;
        if (drop > cnt_) drop = cnt_;
        rd_ = (rd_ + drop) % ring_cap_;
        cnt_ -= drop;
    }
    for (size_t i = 0; i < count; ++i) {
        ring_[wr_] = samples[i];
        wr_ = (wr_ + 1) % ring_cap_;
    }
    cnt_ += count;
}

void AlsaPlayer::start()
{
    if (!handle_ || running_) return;
    running_ = true;
    thread_ = std::thread(&AlsaPlayer::playerLoop, this);
}

void AlsaPlayer::stop()
{
    if (!running_) return;
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void AlsaPlayer::playerLoop()
{
    snd_pcm_t* pcm = (snd_pcm_t*)handle_;
    const unsigned chunk = (unsigned)(rate_ / 50) * ch_;  /* 20ms */
    std::vector<int16_t> buf(chunk);
    int idle_rounds = 0;

    while (running_) {
        size_t avail = 0;
        {
            std::lock_guard<std::mutex> lk(ring_mtx_);
            avail = cnt_;
        }
        if (avail < chunk) {
            /* 无足够数据: 累计 2s 静默后 drain, 防陈旧数据 */
            if (++idle_rounds > 100) {
                idle_rounds = 0;
                std::lock_guard<std::mutex> lk(ring_mtx_);
                if (cnt_ == 0) {
                    snd_pcm_drop(pcm);
                    snd_pcm_prepare(pcm);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        idle_rounds = 0;

        size_t got = 0;
        {
            std::lock_guard<std::mutex> lk(ring_mtx_);
            got = (cnt_ < chunk) ? cnt_ : chunk;
            for (size_t i = 0; i < got; ++i) {
                buf[i] = ring_[rd_];
                rd_ = (rd_ + 1) % ring_cap_;
            }
            cnt_ -= got;
        }

        const int16_t* p = buf.data();
        size_t remaining = got;
        while (remaining > 0 && running_) {
            snd_pcm_uframes_t frames = remaining / ch_;
            snd_pcm_sframes_t n = snd_pcm_writei(pcm, p, frames);
            if (n == -EPIPE) {
                LOG_WARN("[%s] underrun, 重新准备", TAG);
                snd_pcm_prepare(pcm);
                continue;
            }
            if (n < 0) {
                LOG_ERR("[%s] writei 失败: %s", TAG, snd_strerror((int)n));
                break;
            }
            p += (size_t)n * ch_;
            remaining -= (size_t)n * ch_;
        }
    }
    running_ = false;
}
