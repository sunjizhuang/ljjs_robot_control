#ifndef AGORA_YUYV_CONVERT_H
#define AGORA_YUYV_CONVERT_H

#include <cstdint>
#include <cstddef>

/*
 * YUYV(4:2:2 packed) → NV12(4:2:0 semi-planar) 无损打包重排
 *
 * UVC 摄像头普遍只提供 YUYV/MJPEG 而无 YUV420 裸格式; YUYV 转 NV12
 * 仅是像素重排 (无色域/采样插值损失外的计算), 纯软件实现即可满足
 * 640x480@15fps ~ 800x600@15fps 的低分辨率转换, RK3588 上单帧 <2ms。
 *
 * src: YUYV, 每像素 2 字节 (Y0 U Y1 V), 行 stride = w*2
 * dst: NV12, 总长 w*h*3/2 (前 w*h 为 Y 平面, 后 w*h/2 为 UV 交错平面)
 */
void yuyvToNv12(const uint8_t* src, uint8_t* dst, int w, int h);

#endif /* AGORA_YUYV_CONVERT_H */
