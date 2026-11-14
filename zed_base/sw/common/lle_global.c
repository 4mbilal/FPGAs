// lle_global.c - see lle_global.h
#include "lle_global.h"

// Curve values of the released LiteIE weights (snapshots/epoth_199.pth), per channel R, G, B,
// averaged over the 15 LOL-v1 test images (input-independent to about 1e-4).
const double lle_liteie_r[3] = { -0.37086818, -0.36684415, -0.37049425 };

static const double liteie_coeffs[5] = { 10.6301, -49.3563, 193.9553, -507.2211, 582.7439 };

void lle_stats(const uint32_t *img, int w, int h, int stride_px, lle_stats_t *s)
{
    for (int k = 0; k < 16; k++) s->hist[k] = 0;
    s->sum[0] = s->sum[1] = s->sum[2] = 0;
    s->count = 0;
    for (int y = 0; y < h; y++) {
        const uint32_t *row = img + (long)y * stride_px;
        for (int x = 0; x < w; x++) {
            uint32_t p = row[x], r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
            uint32_t l = (77 * r + 150 * g + 29 * b + 128) >> 8;
            s->hist[l >> 4]++;
            s->sum[0] += r; s->sum[1] += g; s->sum[2] += b;
            s->count++;
        }
    }
}

double lle_mean(const lle_stats_t *s)
{
    if (!s->count) return 0.0;
    return ((double)s->sum[0] + (double)s->sum[1] + (double)s->sum[2]) / (3.0 * 255.0 * (double)s->count);
}

static double round_half_even(double v)
{
    double f = (double)(long long)v;                      // truncation toward zero
    if (v < 0 && f != v) f -= 1.0;                        // floor
    double d = v - f;
    if (d > 0.5) return f + 1.0;
    if (d < 0.5) return f;
    return ((long long)f % 2 == 0) ? f : f + 1.0;         // numpy.round: half to even
}

int lle_liteie_iters(const lle_stats_t *s)
{
    double v = lle_mean(s);
    if (v < 0.1) return 8;
    const double *c = liteie_coeffs;
    double p = c[0] + c[1] * v + c[2] * v * v + c[3] * v * v * v + c[4] * v * v * v * v;
    double n = round_half_even(p);
    if (n < 1) n = 1;
    if (n > 15) n = 15;
    return (int)n;
}

void lle_build_table(uint8_t t[3][256], const double r[3], int n)
{
    for (int c = 0; c < 3; c++)
        for (int x = 0; x < 256; x++) {
            double v = x / 255.0;
            for (int i = 0; i < n; i++) v = v + r[c] * (v * v - v);
            if (v < 0) v = 0;
            if (v > 1) v = 1;
            int q = (int)(v * 255.0 + 0.5);                // v >= 0: truncation = floor
            t[c][x] = (uint8_t)(q > 255 ? 255 : q);
        }
}

void lle_liteie_table(uint8_t t[3][256], const lle_stats_t *s)
{
    lle_build_table(t, lle_liteie_r, lle_liteie_iters(s));
}

void lle_apply(uint32_t *img, int w, int h, int stride_px, const uint8_t t[3][256])
{
    for (int y = 0; y < h; y++) {
        uint32_t *row = img + (long)y * stride_px;
        for (int x = 0; x < w; x++) {
            uint32_t p = row[x];
            row[x] = ((uint32_t)t[0][(p >> 16) & 0xFF] << 16) | ((uint32_t)t[1][(p >> 8) & 0xFF] << 8) |
                     (uint32_t)t[2][p & 0xFF];
        }
    }
}
