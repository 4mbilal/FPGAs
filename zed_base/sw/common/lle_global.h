// lle_global.h - global tone curve for low-light enhancement (golden model and CPU path).
// Plain C, no Xilinx dependencies; also builds on a PC (sim/check_lle.c, llie_fpga tools).
//
// The curve is the Zero-DCE / LiteIE quadratic, applied n times per channel:
//     x <- x + r_c (x^2 - x),   x in [0, 1]
// With r_c constant over the frame, the whole curve is a 256-entry table per channel,
// which proc_slot mode 3 applies in hardware (rtl/proc_slot.v).
//
// LiteIE (Expert Systems with Applications 2025, github.com/mubaisam/LiteIE): its released
// network outputs a near-constant curve value per channel (spread about 1e-4), and its
// iteration count comes from the frame mean v: 8 if v < 0.1, else a quartic in v rounded
// and clipped to 1..15. lle_liteie_table() reproduces that rule from the frame statistics.
//
// Statistics are exactly what proc_slot latches: 16-bin luma histogram (bin = Y >> 4 with
// Y = (77 R + 150 G + 29 B + 128) >> 8), channel sums and the pixel count.
// Pixels: XRGB8888, 0x00RRGGBB.

#ifndef LLE_GLOBAL_H
#define LLE_GLOBAL_H

#include <stdint.h>

typedef struct {
    uint32_t hist[16];
    uint32_t sum[3];        // R, G, B
    uint32_t count;
} lle_stats_t;

extern const double lle_liteie_r[3];       // per-channel curve value of the released LiteIE

void   lle_stats(const uint32_t *img, int w, int h, int stride_px, lle_stats_t *s);
double lle_mean(const lle_stats_t *s);                     // frame mean over R, G, B in [0, 1]
int    lle_liteie_iters(const lle_stats_t *s);             // LiteIE's iteration rule
// t[c][x] = floor(255 f(x / 255) + 0.5), f = n iterations with r[c]; c = 0 R, 1 G, 2 B
void   lle_build_table(uint8_t t[3][256], const double r[3], int n);
void   lle_liteie_table(uint8_t t[3][256], const lle_stats_t *s);   // r = lle_liteie_r
void   lle_apply(uint32_t *img, int w, int h, int stride_px, const uint8_t t[3][256]);

#endif
