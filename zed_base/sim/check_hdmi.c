// check_hdmi.c - C golden model for rtl/hdmi_pack.v
//   gcc -O2 -o check_hdmi sim/check_hdmi.c && ./check_hdmi hdmi_vectors.txt
// 1. Reads the testbench vectors (per clock: de hs vs rgb | out_de out_hs out_vs out_d),
//    computes the expected 4:2:2 output for every input clock and checks the RTL output
//    bit-exact at the fixed latency.
// 2. Round trip of the colour conversion: RGB -> YCbCr (this model) -> RGB with the
//    BT.709 matrix that zb_platform.c loads into the ADV7511 CSC. Reports the maximum
//    error per channel over the RGB cube (without subsampling).
#include <stdio.h>
#include <stdlib.h>

#define LAT 6            /* rows between an input row and its output row in the vector file */

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int asr10(int v) { return v >= 0 ? v >> 10 : -((-v + 1023) >> 10); }   /* floor(v / 1024) */

static void rgb2ycc(int r, int g, int b, int *y, int *u, int *v)
{
    *y = clampi(16  + asr10( 187 * r + 629 * g +  63 * b + 512), 16, 235);
    *u = clampi(128 + asr10(-103 * r - 347 * g + 450 * b + 512), 16, 240);
    *v = clampi(128 + asr10( 450 * r - 409 * g -  41 * b + 512), 16, 240);
}

/* ADV7511 CSC as configured by zb_platform.c (Linux adv7511 coefficients, scaling x4):
   out = (A1*Cr + A2*Y + A3*Cb) / 1024 + A4 / 4, coefficients 13-bit two's complement */
static const int csc[12] = { 0x0734, 0x04ad, 0x0000, 0x1c1b,
                             0x1ddc, 0x04ad, 0x1f24, 0x0135,
                             0x0000, 0x04ad, 0x087c, 0x1b77 };
static int s13(int v) { return v & 0x1000 ? v - 0x2000 : v; }
static int adv_out(int row, int y, int cb, int cr)
{
    const int *a = &csc[row * 4];
    double o = (s13(a[0]) * cr + s13(a[1]) * y + s13(a[2]) * cb) / 1024.0 + s13(a[3]) / 4.0;
    return clampi((int)(o + 0.5 - (o < 0)), 0, 255);
}

#define MAXN 100000
static int ide[MAXN], ihs[MAXN], ivs[MAXN], irgb[MAXN], ode[MAXN], ohs[MAXN], ovs[MAXN], od[MAXN];

int main(int argc, char **argv)
{
    FILE *f = fopen(argc > 1 ? argv[1] : "hdmi_vectors.txt", "r");
    if (!f) { perror("vectors"); return 1; }
    int n = 0, idx;
    char a[8], b[8], c[8], d[8], e[8], g[8], rs[16], ds[16], line[256];
    while (n < MAXN && fgets(line, sizeof line, f)) {
        if (sscanf(line, "%d %7s %7s %7s %15s %7s %7s %7s %15s", &idx, a, b, c, rs, d, e, g, ds) != 9) continue;
        char *end;
        ide[n] = a[0] == '1'; ihs[n] = b[0] == '1'; ivs[n] = c[0] == '1';
        irgb[n] = (int)strtoul(rs, NULL, 16);
        ode[n] = d[0] == '1' ? 1 : d[0] == '0' ? 0 : -1;       /* -1: X/Z (pipeline not yet filled) */
        ohs[n] = e[0] == '1' ? 1 : e[0] == '0' ? 0 : -1;
        ovs[n] = g[0] == '1' ? 1 : g[0] == '0' ? 0 : -1;
        od[n]  = (int)strtoul(ds, &end, 16); if (*end) od[n] = -1;
        n++;
    }
    fclose(f);

    /* per input clock: Y/Cb/Cr, then filtered chroma and phase */
    static int Y[MAXN], U[MAXN], V[MAXN], exp_d[MAXN];
    for (int i = 0; i < n; i++) {
        int r = (irgb[i] >> 16) & 255, bb = (irgb[i] >> 8) & 255, gg = irgb[i] & 255;
        rgb2ycc(r, gg, bb, &Y[i], &U[i], &V[i]);
    }
    int ph = 0, cr_hold = 0, pixels = 0;
    for (int i = 0; i < n; i++) {
        if (!ide[i]) { exp_d[i] = 0x8010; ph = 0; continue; }
        int l = (i > 0 && ide[i - 1]) ? i - 1 : i, rr = (i + 1 < n && ide[i + 1]) ? i + 1 : i;
        int uf = (U[l] + 2 * U[i] + U[rr] + 2) >> 2, vf = (V[l] + 2 * V[i] + V[rr] + 2) >> 2;
        if (!ph) { exp_d[i] = (uf << 8) | Y[i]; cr_hold = vf; }
        else       exp_d[i] = (cr_hold << 8) | Y[i];
        ph ^= 1; pixels++;
    }

    int errs = 0;
    int checked = 0;
    for (int i = 0; i + LAT < n; i++) {
        int o = i + LAT;
        if (i < 2) continue;                      /* RTL pipeline has no reset: first rows are X */
        checked++;
        int ok = ode[o] == ide[i] && ohs[o] == ihs[i] && ovs[o] == ivs[i] && (!ide[i] || od[o] == exp_d[i]);
        if (!ide[i] && od[o] != 0x8010) ok = 0;
        if (!ok && errs++ < 10)
            printf("ERR clock %d: in de%d hs%d vs%d rgb %06x | got de%d hs%d vs%d d %04x | exp d %04x\n",
                   i, ide[i], ihs[i], ivs[i], irgb[i], ode[o], ohs[o], ovs[o], od[o], exp_d[i]);
    }
    printf("hdmi_pack: %d clocks checked, %d active pixels, latency %d, %d errors -> %s\n",
           checked, pixels, LAT, errs, (errs || !checked) ? "FAIL" : "PASS");

    /* colour round trip (4:4:4) */
    int maxe[3] = { 0, 0, 0 }; double sum[3] = { 0, 0, 0 }; long cnt = 0;
    for (int r = 0; r < 256; r += 3) for (int gg = 0; gg < 256; gg += 3) for (int bb = 0; bb < 256; bb += 3) {
        int y, u, v; rgb2ycc(r, gg, bb, &y, &u, &v);
        int o[3] = { adv_out(0, y, u, v), adv_out(1, y, u, v), adv_out(2, y, u, v) }, in[3] = { r, gg, bb };
        for (int k = 0; k < 3; k++) { int e2 = abs(o[k] - in[k]); if (e2 > maxe[k]) maxe[k] = e2; sum[k] += e2; }
        cnt++;
    }
    printf("RGB -> YCbCr (hdmi_pack) -> RGB (ADV7511 CSC): max error R %d G %d B %d, mean %.2f %.2f %.2f LSB\n",
           maxe[0], maxe[1], maxe[2], sum[0] / cnt, sum[1] / cnt, sum[2] / cnt);
    return errs ? 1 : 0;
}
