// check_vectors.c - compares proc_slot RTL simulation output with the C golden model
//   gcc -I sw/common -o check_vectors sim/check_vectors.c sw/common/sw_kernels.c
//   ./check_vectors slot_vectors.txt
// Vector lines: mode in out, pixels in AXI4-Stream order {R, B, G}.
#include <stdio.h>
#include "sw_kernels.h"

static uint32_t axis_to_mem(uint32_t t) { return ((t >> 16) & 0xFF) << 16 | (t & 0xFF) << 8 | ((t >> 8) & 0xFF); }

int main(int argc, char **argv)
{
    FILE *f = fopen(argc > 1 ? argv[1] : "slot_vectors.txt", "r");
    if (!f) { perror("open"); return 2; }
    unsigned mode, in, out, n = 0, bad = 0, per_mode[4] = {0};
    while (fscanf(f, "%u %x %x", &mode, &in, &out) == 3) {
        uint32_t m_in = axis_to_mem(in), m_hw = axis_to_mem(out), m_sw;
        m_sw = mode == 1 ? k_gray_px(m_in) : mode == 2 ? k_invert_px(m_in) : m_in;
        if ((m_hw & 0xFFFFFF) != (m_sw & 0xFFFFFF)) {
            if (bad++ < 5) printf("mismatch mode %u in %06x hw %06x sw %06x\n", mode, m_in, m_hw, m_sw);
        }
        per_mode[mode & 3]++; n++;
    }
    printf("%u vectors (mode0 %u, mode1 %u, mode2 %u): %u mismatches between RTL and C golden model\n",
           n, per_mode[0], per_mode[1], per_mode[2], bad);
    return bad != 0;
}
