// check_lle.c - proc_slot statistics (RTL simulation) against the C golden model
//   gcc -I sw/common -o check_lle sim/check_lle.c sw/common/lle_global.c
//   ./check_lle lle_vectors.txt
// Vector lines (from sim/tb_proc_slot_lle.v):
//   P <frame> <pixel, AXI4-Stream order {R, B, G}>
//   S <frame> <count> <sum R> <sum G> <sum B> <hist 0..15>   (as read from the RTL)
// Also checks properties of the table builder: r = 0 gives the identity, every LiteIE
// table is monotonic and brightens (t[x] >= x).
#include <stdio.h>
#include <string.h>
#include "lle_global.h"

#define MAXF 8
#define MAXP 4096
static uint32_t img[MAXF][MAXP];
static int npx[MAXF];

static uint32_t axis_to_mem(uint32_t t) { return ((t >> 16) & 0xFF) << 16 | (t & 0xFF) << 8 | ((t >> 8) & 0xFF); }

int main(int argc, char **argv)
{
    FILE *f = fopen(argc > 1 ? argv[1] : "lle_vectors.txt", "r");
    if (!f) { perror("open"); return 2; }
    char tag[4];
    int bad = 0, sets = 0;
    while (fscanf(f, "%3s", tag) == 1) {
        if (tag[0] == 'P') {
            unsigned fr, p;
            if (fscanf(f, "%u %x", &fr, &p) != 2 || fr >= MAXF || npx[fr] >= MAXP) return 2;
            img[fr][npx[fr]++] = axis_to_mem(p);
        } else if (tag[0] == 'S') {
            unsigned fr, v[20];
            if (fscanf(f, "%u", &fr) != 1 || fr >= MAXF) return 2;
            for (int k = 0; k < 20; k++) if (fscanf(f, "%u", &v[k]) != 1) return 2;
            lle_stats_t s;
            lle_stats(img[fr], npx[fr], 1, npx[fr], &s);
            int ok = s.count == v[0] && s.sum[0] == v[1] && s.sum[1] == v[2] && s.sum[2] == v[3];
            for (int k = 0; k < 16; k++) ok &= s.hist[k] == v[4 + k];
            if (!ok) { bad++; printf("frame %u: RTL statistics differ from the C model\n", fr); }
            sets++;
        }
    }
    fclose(f);

    uint8_t t[3][256];
    const double zero[3] = { 0, 0, 0 };
    lle_build_table(t, zero, 8);
    for (int c = 0; c < 3; c++) for (int x = 0; x < 256; x++) if (t[c][x] != x) { bad++; c = 3; break; }
    for (int n = 1; n <= 15; n++) {
        lle_build_table(t, lle_liteie_r, n);
        for (int c = 0; c < 3; c++)
            for (int x = 1; x < 256; x++)
                if (t[c][x] < t[c][x - 1] || t[c][x] < x) { bad++; printf("table n=%d c=%d not monotonic/brightening at %d\n", n, c, x); c = 3; break; }
    }
    printf("%d statistics sets checked, table properties checked: %d mismatches\n", sets, bad);
    return bad != 0;
}
