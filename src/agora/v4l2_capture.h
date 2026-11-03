#ifndef AGORA_V4L2_CAPTURE_H
#define AGORA_V4L2_CAPTURE_H

#include <string>
#include <vector>
#include <utility>
#include <functional>
#include <atomic>
#include <thread>
#include <cstdint>
#include <cstddef>

/*
 * V4L2 UVC 摄像头采集（RK3588, USB 摄像头）
 *
 * 能力:
 *   - 打开设备, 按优先级探测像素格式: YU12(I420) → NV12 → MJPEG
 *   - 请求 分辨率/帧率, 失败时由驱动协商并记录实际值
 *   - mmap 4 缓冲, poll + DQBUF 循环线程, 每帧回调一次
 *
 * 输出格式说明:
 *   - YU12/NV12: 可直接送 MPP 编码 (H.264) 或 YUV420 直传 RTSA
 *   - MJPEG:     不支持解码; 若设备仅提供 MJPEG, open() 会明确报错。
 *                YU12/NV12 请求分辨率不可用时自动枚举设备支持尺寸降级。
 *                TODO(mjpeg): 后续接入 MPP JPEG 硬解 (MPP_VIDEO_CodingMJPEG)。
 */
class V4l2Capture {
public:
    enum PixFormat {
        FMT_UNKNOWN = 0,
        FMT_YUYV,   /* V4L2_PIX_FMT_YUYV, 4:2:2 打包 (需 yuyvToNv12 转换后编码) */
        FMT_YU12,   /* V4L2_PIX_FMT_YUV420, 三平面 I420 */
        FMT_NV12,   /* V4L2_PIX_FMT_NV12, 双平面 */
        FMT_MJPEG,  /* V4L2_PIX_FMT_MJPEG, 仅探测, 不支持解码 */
    };

    /* 帧: data/len 在回调返回前有效 (回调内必须拷贝或完成编码) */
    typedef std::function<void(const uint8_t* data, size_t len)> FrameCallback;

    V4l2Capture() = default;
    ~V4l2Capture() { close(); }

    V4l2Capture(const V4l2Capture&) = delete;
    V4l2Capture& operator=(const V4l2Capture&) = delete;

    /* 打开并协商格式。失败返回 false 并填充 err。 */
    bool open(const std::string& dev, int width, int height, int fps, std::string& err);

    void close();
    bool isOpen() const { return fd_ >= 0; }

    /* 实际协商结果 */
    PixFormat format()   const { return fmt_; }
    int actualWidth()    const { return width_; }
    int actualHeight()   const { return height_; }
    int actualFps()      const { return fps_; }
    /* 每帧理论字节数 (YU12/NV12); MJPEG 不定长返回 0 */
    size_t frameSize()   const { return frame_size_; }

    void setFrameCallback(FrameCallback cb) { frame_cb_ = std::move(cb); }

    void start();   /* 启动采集线程 */
    void stop();    /* 停止线程并 STREAMOFF */

private:
    void captureLoop();
    bool tryFormat(uint32_t pix_fmt);
    bool setupBuffers();
    /* 枚举设备对某像素格式支持的所有分辨率 (discrete; stepwise 取常用档) */
    bool enumFrameSizes(uint32_t pix_fmt, std::vector<std::pair<int,int>>& sizes);

    int            fd_      = -1;
    PixFormat      fmt_     = FMT_UNKNOWN;
    int            width_   = 0;
    int            height_  = 0;
    int            fps_     = 0;
    size_t         frame_size_ = 0;

    /* mmap 缓冲 */
    struct Buffer {
        void*  start = nullptr;
        size_t length = 0;
    };
    std::vector<Buffer> buffers_;

    FrameCallback     frame_cb_;
    std::thread       thread_;
    std::atomic<bool> running_{false};
};

#endif /* AGORA_V4L2_CAPTURE_H */
