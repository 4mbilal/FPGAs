// sw_kernels.h - software reference (golden model) kernels, plain C, no Xilinx dependencies.
// Pixels are XRGB8888 as stored in DRAM by the frame buffer DMAs: 0x00RRGGBB.
// The grayscale and invert kernels are bit-exact with rtl/proc_slot.v modes 1 and 2.
// Also builds on a PC (gcc) for offline verification against RTL simulation dumps.

#ifndef SW_KERNELS_H
#define SW_KERNELS_H

#include <stdint.h>

// Y = (77 R + 150 G + 29 B + 128) >> 8, replicated to R, G, B
static inline uint32_t k_gray_px(uint32_t p)
{
    uint32_t r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
    uint32_t y = (77 * r + 150 * g + 29 * b + 128) >> 8;
    return (y << 16) | (y << 8) | y;
}

static inline uint32_t k_invert_px(uint32_t p) { return (p ^ 0x00FFFFFF) & 0x00FFFFFF; }

// In-place kernels on a w x h image inside a buffer with stride_px pixels per line
void k_gray(uint32_t *img, int w, int h, int stride_px);
void k_invert(uint32_t *img, int w, int h, int stride_px);
// Sobel magnitude |Gx| + |Gy| on the grayscale image, clamped to 255; border = 0.
// tmp: caller-provided w*h bytes.
void k_sobel(uint32_t *img, int w, int h, int stride_px, uint8_t *tmp);

#endif
