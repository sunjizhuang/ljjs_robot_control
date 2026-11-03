#include "v4l2_capture.h"
#include "../util/log.h"

#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>

#include <linux/videodev2.h>

#define TAG "v4l2"

/* ── ioctl 包装 ── */
static int xioctl(int fd, unsigned long req, void* arg)
{
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r < 0 && errno == EINTR);
    return r;
}

static const char* fmtName(V4l2Capture::PixFormat f)
{
    switch (f) {
    case V4l2Capture::FMT_YUYV:  return "YUYV(422)";
    case V4l2Capture::FMT_YU12:  return "YU12(I420)";
    case V4l2Capture::FMT_NV12:  return "NV12";
    case V4l2Capture::FMT_MJPEG: return "MJPEG";
    default:                     return "UNKNOWN";
    }
}

bool V4l2Capture::open(const std::string& dev, int width, int height, int fps, std::string& err)
{
    if (isOpen()) {
        err = "设备已打开";
        return false;
    }

    fd_ = ::open(dev.c_str(), O_RDWR | O_NONBLOCK, 0);
    if (fd_ < 0) {
        err = "无法打开 " + dev + ": " + strerror(errno);
        return false;
    }

    /* 设备能力检查 */
    v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (xioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) {
        err = "VIDIOC_QUERYCAP 失败: " + dev;
        close();
        return false;
    }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
        !(cap.capabilities & V4L2_CAP_STREAMING)) {
        err = "设备不支持视频采集或 streaming: " + dev;
        close();
        return false;
    }

    /* 格式探测 (多轮):
     *   R1: 请求分辨率下 YU12 → NV12 (免转换直送编码器)
     *   R2: YU12/NV12 在请求分辨率不可用 → 枚举设备真实支持尺寸自动降级
     *   R3: 仍无 YUV420 裸格式 (很多 UVC 摄像头只有 YUYV/MJPEG) → YUYV
     *       (采集层直接输出 YUYV, 上层用 yuyvToNv12 软件转换后编码)
     *   R4: 仅 MJPEG → 明确报错 (不支持解码) */
    width_  = width;
    height_ = height;
    fps_    = fps;

    struct YuvFmt { uint32_t v4l; PixFormat id; const char* name; };
    const YuvFmt yuv_fmts[] = {
        { V4L2_PIX_FMT_YUV420, FMT_YU12, "YU12" },
        { V4L2_PIX_FMT_NV12,   FMT_NV12, "NV12" },
    };

    /* 打分选最优分辨率: ≤请求面积中最大者优先, 宽高比一致 (±10%) 加分 */
    auto pickBest = [&](uint32_t pix_fmt, int& bw, int& bh) -> bool {
        const long   want_px = (long)width * height;
        const double want_ar = (double)width / height;
        double best_score = -1.0;
        bool   ok = false;
        std::vector<std::pair<int,int>> sizes;
        if (!enumFrameSizes(pix_fmt, sizes)) return false;
        for (const std::pair<int,int>& s : sizes) {
            const long px = (long)s.first * s.second;
            if (px <= 0) continue;
            const double ar = (double)s.first / s.second;
            const bool ar_ok = (ar > want_ar * 0.9 && ar < want_ar * 1.1);
            double score;
            if (px <= want_px)
                score = 1e9 + (double)px + (ar_ok ? 5e8 : 0.0);
            else
                score = 5e8 + (double)(want_px * 2 - px) + (ar_ok ? 5e8 : 0.0);
            if (score > best_score) {
                best_score = score; bw = s.first; bh = s.second; ok = true;
            }
        }
        return ok;
    };

    bool matched = false;
    for (const YuvFmt& f : yuv_fmts) {
        if (tryFormat(f.v4l)) { fmt_ = f.id; matched = true; break; }
    }

    if (!matched) {
        int bw = 0, bh = 0;
        for (const YuvFmt& f : yuv_fmts) {
            if (pickBest(f.v4l, bw, bh)) {
                width_  = bw;
                height_ = bh;
                if (tryFormat(f.v4l)) {
                    fmt_ = f.id;
                    matched = true;
                    LOG_WARN("[%s] 请求的 %dx%d 不支持 %s, 自动降级为 %s %dx%d",
                             TAG, width, height, f.name, f.name, bw, bh);
                    break;
                }
            }
        }
    }

    if (!matched) {
        /* R3: YUYV (4:2:2) — 先请求分辨率, 再自动降级 */
        const uint32_t yuyv_v4l = V4L2_PIX_FMT_YUYV;
        if (tryFormat(yuyv_v4l)) {
            fmt_ = FMT_YUYV;
            matched = true;
            LOG_WARN("[%s] 设备无 YUV420 裸格式, 采用 YUYV %dx%d (上层软件转 NV12)",
                     TAG, width_, height_);
        } else if (pickBest(yuyv_v4l, width_, height_) && tryFormat(yuyv_v4l)) {
            fmt_ = FMT_YUYV;
            matched = true;
            LOG_WARN("[%s] 设备无 YUV420 裸格式, 采用 YUYV %dx%d (降级分辨率, "
                     "上层软件转 NV12)", TAG, width_, height_);
        }
    }

    if (!matched) {
        /* R4: 仅 MJPEG → 给出明确错误 */
        if (tryFormat(V4L2_PIX_FMT_MJPEG)) {
            fmt_ = FMT_MJPEG;
            err = dev + " 在任何分辨率下都不支持 YU12/NV12/YUYV 裸输出 (仅 MJPEG)。"
                      "本期不支持 MJPEG 自动解码, 请更换摄像头, 或等待后续 "
                      "MJPEG 硬解支持 (MPP_VIDEO_CodingMJPEG)";
        } else {
            err = dev + " 不支持 YUYV/YU12/NV12/MJPEG 任何一种输出格式";
        }
        close();
        return false;
    }

    /* 实际分辨率/帧率由驱动回填 */
    v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd_, VIDIOC_G_FMT, &fmt) < 0) {
        err = "VIDIOC_G_FMT 失败";
        close();
        return false;
    }
    width_  = fmt.fmt.pix.width;
    height_ = fmt.fmt.pix.height;
    frame_size_ = fmt.fmt.pix.sizeimage;

    v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator   = 1;
    parm.parm.capture.timeperframe.denominator = fps_;
    if (xioctl(fd_, VIDIOC_S_PARM, &parm) == 0) {
        fps_ = parm.parm.capture.timeperframe.denominator;
    }

    if (!setupBuffers()) {
        err = "mmap 缓冲区建立失败";
        close();
        return false;
    }

    LOG_INFO("[%s] 已打开 %s: %s %dx%d@%dfps (frame_size=%zu)",
             TAG, dev.c_str(), fmtName(fmt_), width_, height_, fps_, frame_size_);
    return true;
}

bool V4l2Capture::tryFormat(uint32_t pix_fmt)
{
    v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = width_;
    fmt.fmt.pix.height      = height_;
    fmt.fmt.pix.pixelformat = pix_fmt;
    fmt.fmt.pix.field       = V4L2_FIELD_NONE;

    /* 先 TRY 看驱动能否接受 */
    if (xioctl(fd_, VIDIOC_TRY_FMT, &fmt) < 0) return false;
    if (fmt.fmt.pix.pixelformat != pix_fmt)    return false;

    /* 再正式 S_FMT */
    if (xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0)   return false;
    return fmt.fmt.pix.pixelformat == pix_fmt;
}

bool V4l2Capture::enumFrameSizes(uint32_t pix_fmt, std::vector<std::pair<int,int>>& sizes)
{
    sizes.clear();
    v4l2_frmsizeenum fs;
    memset(&fs, 0, sizeof(fs));
    fs.pixel_format = pix_fmt;

    for (unsigned idx = 0; idx < 128; ++idx) {
        fs.index = idx;
        if (xioctl(fd_, VIDIOC_ENUM_FRAMESIZES, &fs) < 0) break;
        if (fs.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
            sizes.emplace_back((int)fs.discrete.width, (int)fs.discrete.height);
        } else {
            /* stepwise: 取边界 + 常见档位 (避免 step=1 时组合爆炸) */
            const v4l2_frmsize_stepwise& sw = fs.stepwise;
            const int common[][2] = {
                { 1920, 1080 }, { 1600, 1200 }, { 1280, 960 }, { 1280, 720 },
                { 800, 600 },   { 640, 480 },   { 640, 360 },  { 352, 288 },
                { 320, 240 },   { 176, 144 },
            };
            for (const auto& c : common) {
                if ((unsigned)c[0] >= sw.min_width  && (unsigned)c[0] <= sw.max_width &&
                    (unsigned)c[1] >= sw.min_height && (unsigned)c[1] <= sw.max_height) {
                    sizes.emplace_back(c[0], c[1]);
                }
            }
            sizes.emplace_back((int)sw.min_width, (int)sw.min_height);
            sizes.emplace_back((int)sw.max_width, (int)sw.max_height);
            break;
        }
    }
    return !sizes.empty();
}

bool V4l2Capture::setupBuffers()
{
    v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count  = 4;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
        LOG_ERR("[%s] VIDIOC_REQBUFS 失败", TAG);
        return false;
    }

    buffers_.resize(req.count);
    for (unsigned i = 0; i < req.count; ++i) {
        v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (xioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) return false;

        buffers_[i].length = buf.length;
        buffers_[i].start = mmap(NULL, buf.length,
                                 PROT_READ | PROT_WRITE, MAP_SHARED,
                                 fd_, buf.m.offset);
        if (buffers_[i].start == MAP_FAILED) return false;

        if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) return false;
    }
    return true;
}

void V4l2Capture::start()
{
    if (!isOpen() || running_) return;

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
        LOG_ERR("[%s] VIDIOC_STREAMON 失败: %s", TAG, strerror(errno));
        return;
    }

    running_ = true;
    thread_ = std::thread(&V4l2Capture::captureLoop, this);
}

void V4l2Capture::stop()
{
    if (!running_) return;
    running_ = false;
    if (thread_.joinable()) thread_.join();

    if (fd_ >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(fd_, VIDIOC_STREAMOFF, &type);
    }
}

void V4l2Capture::captureLoop()
{
    struct pollfd pfd;
    pfd.fd = fd_;
    pfd.events = POLLIN;

    while (running_) {
        int ret = poll(&pfd, 1, 500);
        if (ret < 0) {
            if (errno == EINTR) continue;
            LOG_ERR("[%s] poll 失败: %s", TAG, strerror(errno));
            break;
        }
        if (ret == 0) continue; /* 超时, 继续等 */

        v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) continue;
            LOG_ERR("[%s] VIDIOC_DQBUF 失败: %s", TAG, strerror(errno));
            break;
        }

        if (buf.bytesused > 0 && frame_cb_) {
            frame_cb_((const uint8_t*)buffers_[buf.index].start, buf.bytesused);
        }

        if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
            LOG_ERR("[%s] VIDIOC_QBUF 失败: %s", TAG, strerror(errno));
            break;
        }
    }
    running_ = false;
}

void V4l2Capture::close()
{
    stop();

    for (auto& b : buffers_) {
        if (b.start && b.start != MAP_FAILED) munmap(b.start, b.length);
    }
    buffers_.clear();

    if (fd_ >= 0) {
        v4l2_requestbuffers req;
        memset(&req, 0, sizeof(req));
        req.count  = 0;
        req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        xioctl(fd_, VIDIOC_REQBUFS, &req); /* 释放缓冲 */

        ::close(fd_);
        fd_ = -1;
    }
    fmt_ = FMT_UNKNOWN;
    LOG_INFO("[%s] 设备已关闭", TAG);
}
