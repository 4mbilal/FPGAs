// sw_kernels.c - see sw_kernels.h
#include "sw_kernels.h"

void k_gray(uint32_t *img, int w, int h, int stride_px)
{
    for (int y = 0; y < h; y++) {
        uint32_t *row = img + (long)y * stride_px;
        for (int x = 0; x < w; x++) row[x] = k_gray_px(row[x]);
    }
}

void k_invert(uint32_t *img, int w, int h, int stride_px)
{
    for (int y = 0; y < h; y++) {
        uint32_t *row = img + (long)y * stride_px;
        for (int x = 0; x < w; x++) row[x] = k_invert_px(row[x]);
    }
}

void k_sobel(uint32_t *img, int w, int h, int stride_px, uint8_t *tmp)
{
    for (int y = 0; y < h; y++) {
        const uint32_t *row = img + (long)y * stride_px;
        for (int x = 0; x < w; x++) tmp[y * w + x] = (uint8_t)(k_gray_px(row[x]) & 0xFF);
    }
    for (int y = 0; y < h; y++) {
        uint32_t *out = img + (long)y * stride_px;
        for (int x = 0; x < w; x++) {
            if (y == 0 || y == h - 1 || x == 0 || x == w - 1) { out[x] = 0; continue; }
            const uint8_t *p = tmp + (y - 1) * w + (x - 1);
            int gx = -p[0] + p[2] - 2 * p[w] + 2 * p[w + 2] - p[2 * w] + p[2 * w + 2];
            int gy = -p[0] - 2 * p[1] - p[2] + p[2 * w] + 2 * p[2 * w + 1] + p[2 * w + 2];
            int m = (gx < 0 ? -gx : gx) + (gy < 0 ? -gy : gy);
            uint32_t v = m > 255 ? 255 : (uint32_t)m;
            out[x] = (v << 16) | (v << 8) | v;
        }
    }
}
