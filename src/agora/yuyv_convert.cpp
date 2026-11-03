#include "yuyv_convert.h"

void yuyvToNv12(const uint8_t* src, uint8_t* dst, int w, int h)
{
    uint8_t* dstY  = dst;
    uint8_t* dstUV = dst + (size_t)w * h;

    /* Y 平面: YUYV 每像素 2 字节, Y 位于偶数偏移, 直接抽取 */
    for (int row = 0; row < h; ++row) {
        const uint8_t* s = src + (size_t)row * w * 2;
        uint8_t*       d = dstY + (size_t)row * w;
        for (int col = 0; col < w; ++col) {
            d[col] = s[col * 2];
        }
    }

    /* UV 平面: 4:2:0 下采样, 每 2x2 像素块取第一像素对的 U/V */
    for (int row = 0; row < h / 2; ++row) {
        const uint8_t* s0 = src + (size_t)(row * 2) * w * 2;  /* 偶数行 */
        uint8_t*       d  = dstUV + (size_t)row * w;
        for (int col = 0; col < w / 2; ++col) {
            d[col * 2]     = s0[col * 4 + 1];  /* U (像素对共享) */
            d[col * 2 + 1] = s0[col * 4 + 3];  /* V (像素对共享) */
        }
    }
}
