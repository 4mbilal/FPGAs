// jig.c - zed_base test application: one program, everything from the UART menu.
//
// Source files for the Vitis app (all flat in src/): everything in sw/common/
// (zb_platform.c/.h, ov2640_regs.h, sw_kernels.c/.h). Replaces vga_test.c, cam_test.c
// and proc_test.c. Works without a camera (test patterns and memory test only).
//
// Sources shown on the display (VGA and HDMI show the same image):
//   camera   camera -> proc_slot -> vfb_wr -> rotating DRAM buffers -> vfb_rd -> VGA / HDMI
//   pattern  colour bars / checkerboard / grey ramp (display test, no camera needed)
// Memory test: DRAM test image -> vfb_rd2 -> proc_slot -> vfb_wr -> DRAM result,
//              compared bit-exact against the C golden model (sw_kernels.c)
//
// UART menu (115200 8N1, single key presses; 'h' prints it with the current state):
//   t        toggle source: camera / test pattern      p   next test pattern
//   d / D    next / previous display mode (640x480 .. 1920x1080)
//   c / C    next / previous camera size (OV2640: QCIF .. UXGA 1600x1200)
//   0 1 2 3  kernel: none, grayscale, invert, Sobel (Sobel runs on the CPU only)
//   4        kernel: low-light enhancement, global curve (LiteIE rule; proc_slot mode 3 with
//            a per-frame table built from the slot's frame statistics, or on the CPU;
//            needs proc_slot v5, ID 0x534C0005, shell v7)
//   x        run the kernel in HW (proc_slot) or SW (CPU)
//   n        LLE iteration count: auto (LiteIE rule) or forced 2 / 4 / 8 / 12
//   m        memory test, generated image          l   memory test, image loaded at 0x10000000 (Linux: $ZB_IMAGE)
//   r        next memory-test size                 s   status line now
//   v        debug: DMA / VTC / pixel clock state  h   menu
//   g        gap test: locates a memory-test pixel shift (read side, write side or stream)
//   H        HDMI status (ADV7511)                  k   next ADV7511 input clock delay
//   a        HDMI: automatic re-apply of a lost ADV7511 setup on/off
// Board switches do the same as 0-3 / x: SW0 = SW path, SW2:1 = kernel. Whichever was
// changed last (switch or key) is in effect.
//
// Buffers: NBUF "canvas" buffers of max(camera, display) size. The camera writes its
// frame centred in the canvas; the display scans a centred display-size region of it.
// That covers camera smaller than the display (window with a border) and larger (crop),
// with no copying. Buffers rotate on polled frame-done flags; the previous display
// buffer stays protected until the display has certainly moved on (sim/rotation_model.py).
//
// Memory test images live at fixed DRAM addresses so they can be loaded and read back
// over JTAG with xsct, e.g. (XRGB8888, little endian, i.e. 0x00RRGGBB per pixel):
//   xsct% dow -data image_1920x1080.bin 0x10000000
//   xsct% mrd -bin -file result.bin 0x11000000 [expr 1920*1080]
// tools/rawimg.py converts PNG <-> raw.
//
// Batch mode (datasets, no key presses): a mailbox in DRAM at 0x13000000 (see MBOX_*),
// driven from xsct by tools/batch.tcl. Each job: packed XRGB8888 image at 0x10000000,
// hardware result written packed to 0x11000000, C golden model result to 0x12000000.

#include <string.h>
#include "xparameters.h"
#include "xil_printf.h"
#include "xil_cache.h"
#include "xil_io.h"
#include "sleep.h"
#include "zb_platform.h"
#include "sw_kernels.h"
#include "lle_global.h"
// One source for both targets. The Linux build (sw/linux_c/Makefile) defines ZB_LINUX:
// buffers live in the u-dma-buf region, the CPU edge kernel is OpenCV Canny, and the
// DRAM mailbox batch mode is left out. (__linux__ cannot be used: the Linux build compiles
// the bare-metal code paths of the Xilinx drivers with -U__linux__.)
#ifdef ZB_LINUX
#include "zb_linux.h"
#include "zb_cv.h"
#define EDGE_NAME "Canny"
#define K_EDGE(p, w, h, s) k_canny(p, w, h, s)
#else
#define EDGE_NAME "Sobel"
#define K_EDGE(p, w, h, s) k_sobel(p, w, h, s, sobel_tmp)
#endif

// ---- proc_slot ---------------------------------------------------------------------
#ifdef XPAR_PROC_SLOT_BASEADDR
#define SLOT_BASE XPAR_PROC_SLOT_BASEADDR
#else
#define SLOT_BASE 0x43D00000
#endif
#define SLOT_CTRL          0x00
#define SLOT_ID            0x04
#define SLOT_FRAMES        0x08
#define SLOT_CYCLES_LAST   0x0C
#define SLOT_PIXELS_LAST   0x10
#define SLOT_LINES_LAST    0x14
#define SLOT_STALLS_LAST   0x18
#define SLOT_ACTIVE_CYCLES 0x1C
#define SLOT_PIXELS_RUN    0x20
#define SLOT_LINES_RUN     0x24
#define SLOT_CAPS          0x28
#define SLOT_LUT_BANK      0x2C
#define SLOT_STAT_SEQ      0x30
#define SLOT_STAT_CTRL     0x34
#define SLOT_IRQ_EN        0x38
#define SLOT_IRQ_STATUS    0x3C
#define SLOT_LUT0          0x400      /* R at 0x400, G at 0x800, B at 0xC00, one entry per word */
#define SLOT_STAT_COUNT    0x1000
#define SLOT_SUM_R         0x1004
#define SLOT_HIST0         0x1040
#define SLOT_ID_EXPECTED   0x534C0005u
#define SLOT_MODE_LUT      3
#define SLOT_SRC_MEM       0x10
#define SLOT_CLK_HZ        150000000u
static u32  slot_rd(u32 off)        { return Xil_In32(SLOT_BASE + off); }
static void slot_wr(u32 off, u32 v) { Xil_Out32(SLOT_BASE + off, v); }

// ---- curve table and frame statistics (proc_slot v4) ----------------------------------
static void lut_write(const u8 t[3][256])                 // into the inactive bank
{
    for (int c = 0; c < 3; c++) for (int x = 0; x < 256; x++) slot_wr(SLOT_LUT0 + 0x400 * c + 4 * x, t[c][x]);
}
static int  lut_swap_pending(void) { return slot_rd(SLOT_LUT_BANK) & 1; }
static void lut_swap(void)         { slot_wr(SLOT_LUT_BANK, 1); }   // at the next start of frame
static void slot_stats(lle_stats_t *s)                    // latched set
{
    s->count = slot_rd(SLOT_STAT_COUNT);
    for (int c = 0; c < 3; c++) s->sum[c] = slot_rd(SLOT_SUM_R + 4 * c);
    for (int k = 0; k < 16; k++) s->hist[k] = slot_rd(SLOT_HIST0 + 4 * k);
}
static int stats_equal(const lle_stats_t *a, const lle_stats_t *b)
{
    if (a->count != b->count) return 0;
    for (int c = 0; c < 3; c++) if (a->sum[c] != b->sum[c]) return 0;
    for (int k = 0; k < 16; k++) if (a->hist[k] != b->hist[k]) return 0;
    return 1;
}
static u32 mean_x1000(const lle_stats_t *s) { return (u32)(lle_mean(s) * 1000.0 + 0.5); }

// live HW path: table for the iteration count of the latest latched statistics
static u32 lle_seq_seen;
static int lle_n_active = -1, lle_n_last = -1;            // table in (or queued for) use; latest count
static u32 lle_mean_last;
// 'n': iteration count forced for demonstrations (0 = auto, the LiteIE rule)
static const int lle_force_tab[5] = { 0, 2, 4, 8, 12 };
static int lle_force;
static int lle_pick(const lle_stats_t *s) { return lle_force_tab[lle_force] ? lle_force_tab[lle_force] : lle_liteie_iters(s); }
// Returns 0 if an earlier table is still queued after 200 ms (no frame is arriving, e.g. no
// camera): writing now would overwrite the queued table, so nothing is loaded.
static int lle_load(int n)
{
    u8 t[3][256];
    u64 t0 = zb_time();
    while (lut_swap_pending())                            // the queued table must be in use first
        if (zb_time() - t0 > zb_time_hz() / 5) return 0;
    lle_build_table(t, lle_liteie_r, n);
    lut_write(t);
    lut_swap();
    lle_n_active = n;
    return 1;
}

// ---- memory test buffers ---------------------------------------------------------------
#ifdef ZB_LINUX
// in the u-dma-buf region, after the NBUF live buffers (6 x 1920x1200x4 = 55.3 MB);
// needs a u-dma-buf of at least 86 MB
#define TEST_IN   (zb_dma_base() + 0x03800000u)
#define TEST_OUT  (zb_dma_base() + 0x04200000u)
#define TEST_REF  (zb_dma_base() + 0x04C00000u)
#define TEST_END  0x05600000u
#define TEST_OK() (zb_dma_size() >= TEST_END)
#else
// fixed DRAM addresses, outside the program image
#define TEST_IN   0x10000000u
#define TEST_OUT  0x11000000u
#define TEST_REF  0x12000000u
#define TEST_OK() 1
#endif
#define TEST_MAX_W 1920
#define TEST_MAX_H 1200
static const struct { u16 w, h; } test_sizes[] = {
    { 640, 480 }, { 1280, 720 }, { 1600, 1200 }, { 1920, 1080 }, { 1920, 1200 } };
#define NUM_TEST_SIZES ((int)(sizeof(test_sizes) / sizeof(test_sizes[0])))
static int test_size = 3;
static u32 live_hw_mode;           // proc_slot mode for the live HW path (restored after a memory test)

// ---- user state ------------------------------------------------------------------------
static int cam_ok;                 // camera detected and configured
static int src_pattern;            // 1: show a test pattern instead of the camera
static int pattern;                // 0 bars, 1 checkerboard, 2 grey ramp
static int path_sw;                // 0: kernel in proc_slot, 1: kernel on the CPU
static int kern;                   // 0 none, 1 gray, 2 invert, 3 Sobel (SW only), 4 LLE global curve
static const char *kern_name[5] = { "none", "grayscale", "invert", EDGE_NAME, "LLE global" };
static u32 hw_mode_of(int k) { return k == 1 || k == 2 ? (u32)k : k == 4 ? SLOT_MODE_LUT : 0; }
static const char *pat_name[3]  = { "colour bars", "checkerboard", "grey ramp" };

// ---- live buffers ------------------------------------------------------------------
#define NBUF       6
#define CANVAS_MAX (1920 * 1200)
#ifdef ZB_LINUX
static u32 (*buf)[CANVAS_MAX];     // start of the u-dma-buf region (set in main)
#else
static u32 buf[NBUF][CANVAS_MAX] __attribute__((aligned(4096)));
#endif
static u8  sobel_tmp[TEST_MAX_W * TEST_MAX_H];

static int cam_w, cam_p, disp_reg, disp_scan, disp_prev = -1, raw_ready = -1, cpu_hold = -1;

// layout of the current configuration
static int canvas_w, canvas_h, disp_w, disp_h, cw, ch;
static u32 cam_off, disp_off;      // byte offsets inside a canvas buffer

static UINTPTR cam_addr(int i)  { return (UINTPTR)buf[i] + cam_off; }
static UINTPTR disp_addr(int i) { return (UINTPTR)buf[i] + disp_off; }
static u32    *cam_ptr(int i)   { return (u32 *)cam_addr(i); }
static u32     cam_span(void)   { return (u32)(ch - 1) * canvas_w * 4 + cw * 4; }

static int in_use(int i)
{
    return i == cam_w || i == cam_p || i == disp_reg || i == disp_scan || i == disp_prev ||
           i == raw_ready || i == cpu_hold;
}
static int pick_free(void)
{
    for (int i = 0; i < NBUF; i++) if (!in_use(i)) return i;
    return -1;   // cannot happen with NBUF = 6 (at most 5 roles occupied when picking)
}
static u32 disp_frames;             // display frame-done events (statistics)
static int disp_prev_age;

// A display frame-done event: the buffer in the address register was latched for scan-out.
// The previous buffer stays protected for two events, which also covers a latch that
// lands between polling the flag and writing a new address in display().
static void disp_event(void)
{
    disp_scan = disp_reg;
    if (disp_prev >= 0 && ++disp_prev_age >= 2) disp_prev = -1;
    disp_frames++;
}

static void display(int i)
{
    if (zb_fbr_done()) disp_event();   // attribute a pending event to the OLD address first
    disp_prev = disp_reg;              // may already be latched by the display DMA
    disp_prev_age = 0;
    disp_reg  = i;
    zb_fbr_set_addr(disp_addr(i));
}

// ---- configuration -----------------------------------------------------------------
static int max_i(int a, int b) { return a > b ? a : b; }

static void draw_canvas(int i)
{
    int x0 = (int)((cam_off / 4) % canvas_w), y0 = (int)((cam_off / 4) / canvas_w);
    u32 *b = buf[i];
    for (int y = 0; y < canvas_h; y++)
        for (int x = 0; x < canvas_w; x++) {
            int in_cam = (x >= x0 && x < x0 + cw && y >= y0 && y < y0 + ch);
            b[y * canvas_w + x] = in_cam ? 0 : (((x >> 5) ^ (y >> 5)) & 1 ? 0x303030 : 0x181818);
        }
    Xil_DCacheFlushRange((UINTPTR)b, (u32)canvas_w * canvas_h * 4);
}

// Full-screen test pattern with a red 1-pixel frame (checks that the edges are visible)
static void draw_pattern(u32 *b, int w, int h, int pat)
{
    static const u32 bars[8] = {
        0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00, 0xFF00FF, 0xFF0000, 0x0000FF, 0x000000 };
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            u32 p;
            switch (pat) {
            case 0:  p = bars[x * 8 / w]; break;
            case 1:  p = ((x >> 5) ^ (y >> 5)) & 1 ? 0xFFFFFF : 0x000000; break;
            default: { u32 g = (u32)x * 255 / (w - 1); p = (g << 16) | (g << 8) | g; }
            }
            if (x == 0 || y == 0 || x == w - 1 || y == h - 1) p = 0xFF0000;
            b[y * w + x] = p;
        }
    Xil_DCacheFlushRange((UINTPTR)b, (u32)w * h * 4);
}

// Stop both live DMAs, apply camera size / display mode, redraw, restart rotation
// (camera source) or show a static pattern (pattern source).
static int apply_config(int mode_idx, int cam_idx, int reprogram_display, int reprogram_camera)
{
    zb_fbw_stop();
    zb_fbr_stop();
    if (reprogram_display) {
        if (zb_display_set_mode(mode_idx) != XST_SUCCESS) return XST_FAILURE;
        zb_hdmi_set_mode(mode_idx);
    }
    if (reprogram_camera  && zb_camera_set_size(cam_idx)   != XST_SUCCESS) return XST_FAILURE;

    disp_w = zb_modes[zb_display_mode()].w; disp_h = zb_modes[zb_display_mode()].h;

    if (src_pattern || !cam_ok) {
        // static pattern in buffer 0, camera DMA left stopped
        canvas_w = disp_w; canvas_h = disp_h; cam_off = disp_off = 0;
        draw_pattern(buf[0], disp_w, disp_h, pattern);
        disp_reg = disp_scan = 0; disp_prev = -1; disp_prev_age = 0; raw_ready = -1; cpu_hold = -1;
        cam_w = cam_p = -1;
        if (zb_fbr_config((UINTPTR)buf[0], disp_w, disp_h, disp_w * 4) != XST_SUCCESS) return XST_FAILURE;
        xil_printf("Display %s, test pattern: %s\r\n", zb_modes[zb_display_mode()].name, pat_name[pattern]);
        return XST_SUCCESS;
    }

    cw = zb_camera_w(); ch = zb_camera_h();
    canvas_w = max_i(cw, disp_w); canvas_h = max_i(ch, disp_h);
    // x offsets rounded down to 8 pixels (32 bytes): cache-line and DMA aligned
    cam_off  = (u32)(((canvas_h - ch) / 2) * canvas_w + (((canvas_w - cw) / 2) & ~7)) * 4;
    disp_off = (u32)(((canvas_h - disp_h) / 2) * canvas_w + (((canvas_w - disp_w) / 2) & ~7)) * 4;

    for (int i = 0; i < NBUF; i++) draw_canvas(i);
    if (reprogram_camera) usleep(200000);          // let the sensor settle after reprogramming

    disp_reg = disp_scan = 0; disp_prev = -1; disp_prev_age = 0; raw_ready = -1; cpu_hold = -1;
    cam_w = 1; cam_p = 2;
    if (zb_fbr_config(disp_addr(disp_reg), disp_w, disp_h, canvas_w * 4) != XST_SUCCESS) return XST_FAILURE;
    if (zb_fbw_config(cam_addr(cam_w), cw, ch, canvas_w * 4) != XST_SUCCESS) return XST_FAILURE;
    zb_fbw_set_addr(cam_addr(cam_p));
    xil_printf("Display %s, camera %s %dx%d (canvas %dx%d)\r\n", zb_modes[zb_display_mode()].name,
               zb_camera_size_name(zb_camera_size()), cw, ch, canvas_w, canvas_h);
    return XST_SUCCESS;
}

// ---- memory-to-memory test -----------------------------------------------------------
static u32 test_pixel(int x, int y)
{
    // gradients plus a hash: every byte value occurs, neighbouring pixels differ
    u32 h = (u32)x * 0x9E3779B1u ^ (u32)y * 0x85EBCA77u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
    u32 r = ((x * 255) / 1919 + (h & 0x1F)) & 0xFF;
    u32 g = ((y * 255) / 1199 + ((h >> 8) & 0x3F)) & 0xFF;
    u32 b = (h >> 16) & 0xFF;
    if (((x >> 6) ^ (y >> 6)) & 1) b = 255 - b;       // blocks for the Sobel kernel
    return (r << 16) | (g << 8) | b;
}

// ---- low-light enhancement on still images (memory test 'l'/'m' with kernel 4, batch) ----
// Two passes through proc_slot, as a still image has no previous frame:
//   1. pass-through, statistics latched with STAT_CTRL and read back
//   2. curve-table mode with the table built from those statistics
// The C golden model (sw/common/lle_global.c) computes its own statistics and table on the CPU;
// statistics, iteration count and every pixel must agree.
typedef struct { int ok; u32 act, px, lines; u64 t0, t1; } pass_t;

static int dma_pass(u32 ctrl, UINTPTR in, UINTPTR out, int w, int h, u32 stride, pass_t *r)
{
    const u64 hz = zb_time_hz();
    memset(r, 0, sizeof(*r));
    zb_fbw_stop();
    slot_wr(SLOT_CTRL, ctrl);
    r->t0 = zb_time();
    if (zb_fbw_single(out, w, h, stride) != XST_SUCCESS || zb_fbr2_single(in, w, h, stride) != XST_SUCCESS)
        return 0;
    int wr = 0, rd = 0, slot = 0;
    while (zb_time() - r->t0 < hz) {
        if (!wr) wr = zb_fbw_done();
        if (!rd) rd = zb_fbr2_done();
        r->px = slot_rd(SLOT_PIXELS_RUN); r->lines = slot_rd(SLOT_LINES_RUN);
        if (!slot) slot = (r->px == (u32)w * h && r->lines == (u32)h);
        if (wr && rd && slot) break;
    }
    r->t1 = zb_time();
    r->act = slot_rd(SLOT_ACTIVE_CYCLES);
    r->ok = slot && wr;
    return r->ok;
}

typedef struct {
    int ok, n_hw, n_sw, stats_ok;
    u32 mismatches, act, mean_x1000;
    u64 sw_ticks;
    lle_stats_t hw, sw;
} lle_job_t;

// in/out: images with stride (bytes) for the DMAs; ref: packed w x h for the C model
static void lle_job(int w, int h, UINTPTR in, UINTPTR out, u32 stride, u32 *ref, lle_job_t *j)
{
    const u32 bytes = (u32)h * stride;
    pass_t p1, p2;
    memset(j, 0, sizeof(*j));
    Xil_DCacheFlushRange(in, bytes);
    memset((void *)out, 0xA5, bytes);
    Xil_DCacheFlushRange(out, bytes);

    // pass 1: statistics
    if (!dma_pass(SLOT_SRC_MEM, in, out, w, h, stride, &p1)) { slot_wr(SLOT_CTRL, live_hw_mode); return; }
    slot_wr(SLOT_STAT_CTRL, 1);
    slot_stats(&j->hw);
    j->n_hw = lle_liteie_iters(&j->hw);
    lle_load(j->n_hw);
    // pass 2: curve table
    memset((void *)out, 0xA5, bytes);
    Xil_DCacheFlushRange(out, bytes);
    int ok = dma_pass(SLOT_SRC_MEM | SLOT_MODE_LUT, in, out, w, h, stride, &p2);
    slot_wr(SLOT_CTRL, live_hw_mode);
    lle_n_active = -1;                                    // live path rebuilds its table
    if (!ok) return;
    j->act = p2.act;

    // C golden model on the same input
    const u32 *src = (const u32 *)in;
    for (int y = 0; y < h; y++) memcpy(ref + (long)y * w, src + (long)y * (stride / 4), (u32)w * 4);
    u64 s0 = zb_time();
    u8 tab[3][256];
    lle_stats(ref, w, h, w, &j->sw);
    j->n_sw = lle_liteie_iters(&j->sw);
    lle_build_table(tab, lle_liteie_r, j->n_sw);
    lle_apply(ref, w, h, w, tab);
    j->sw_ticks = zb_time() - s0;
    j->stats_ok = stats_equal(&j->hw, &j->sw);
    j->mean_x1000 = mean_x1000(&j->sw);

    Xil_DCacheInvalidateRange(out, bytes);
    const u32 *o = (const u32 *)out;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if ((o[(long)y * (stride / 4) + x] & 0xFFFFFF) != (ref[(long)y * w + x] & 0xFFFFFF)) j->mismatches++;
    j->ok = 1;
}

static void lle_report(const lle_job_t *j, int w, int h)
{
    const u32 hz = zb_time_hz();
    u32 hw_us_x10 = j->act ? (u32)(((u64)j->act * 10000000ULL) / SLOT_CLK_HZ) : 0;
    u32 sw_us = (u32)((j->sw_ticks * 1000000ULL) / hz);
    xil_printf("  mean %u.%03u -> %d iterations (HW statistics) / %d (C model); statistics %s\r\n",
               j->mean_x1000 / 1000, j->mean_x1000 % 1000, j->n_hw, j->n_sw,
               j->stats_ok ? "identical" : "DIFFER");
    xil_printf("  HW curve pass: %u.%u us in the slot; C model (statistics + table + apply): %u us\r\n",
               hw_us_x10 / 10, hw_us_x10 % 10, sw_us);
    xil_printf("  %s: %u of %u pixels differ between HW and the C golden model\r\n",
               (j->ok && !j->mismatches && j->stats_ok) ? "PASS" : "FAIL", j->mismatches, (u32)w * h);
}


// 'l': the input image. Bare metal: loaded over JTAG at TEST_IN (xsct dow -data).
// Linux: read from the raw XRGB8888 file named by ZB_IMAGE (tools/rawimg.py to-raw),
// w x h of the current memory-test size.
#ifdef ZB_LINUX
#include <stdio.h>
#include <stdlib.h>
#define LOADED_DESC "from $ZB_IMAGE"
static int load_input(int w, int h)
{
    const char *f = getenv("ZB_IMAGE");
    FILE *fp = f ? fopen(f, "rb") : NULL;
    if (!fp) { xil_printf("  set ZB_IMAGE=<raw XRGB8888 file of %dx%d> for 'l'\r\n", w, h); return 0; }
    size_t n = fread((void *)TEST_IN, 4, (size_t)w * h, fp);
    fclose(fp);
    if (n != (size_t)w * h) { xil_printf("  %s: %u pixels, expected %d\r\n", f, (u32)n, w * h); return 0; }
    return 1;
}
#else
#define LOADED_DESC "loaded at 0x10000000"
static int load_input(int w, int h) { (void)w; (void)h; return 1; }
#endif

static void lle_mem_test(int use_loaded)
{
    if (!zb_fbr2_available()) { xil_printf("Memory test needs v5 hardware (vfb_rd2)\r\n"); return; }
    if (!TEST_OK()) { xil_printf("Memory test needs a u-dma-buf of >= 86 MB\r\n"); return; }
    const int w = test_sizes[test_size].w, h = test_sizes[test_size].h;
    u32 *in = (u32 *)TEST_IN;
    xil_printf("\r\nMemory test %dx%d, low-light enhancement (global curve), input %s\r\n", w, h,
               use_loaded ? LOADED_DESC : "generated (darkened)");
    if (use_loaded && !load_input(w, h)) return;
    if (!use_loaded)
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) in[y * w + x] = (test_pixel(x, y) >> 2) & 0x3F3F3F;
    lle_job_t j;
    zb_diag = 0;
    lle_job(w, h, TEST_IN, TEST_OUT, (u32)w * 4, (u32 *)TEST_REF, &j);
    if (!j.ok) xil_printf("  FAIL: a test frame did not pass through proc_slot completely\r\n");
    lle_report(&j, w, h);
    xil_printf("  Input 0x%08x, HW result 0x%08x, C model 0x%08x\r\n", TEST_IN, TEST_OUT, TEST_REF);
    if (j.ok && w >= disp_w && h >= disp_h) {
        u32 off = (u32)(((h - disp_h) / 2) * w + (((w - disp_w) / 2) & ~7)) * 4;
        zb_fbr_config(TEST_OUT + off, disp_w, disp_h, w * 4);
        sleep(3);
    }
    apply_config(zb_display_mode(), zb_camera_size(), 0, 0);
}

#if !defined(ZB_LINUX)    /* bare metal only: under Linux, datasets go through sw/python/lle_batch.py */
// ---- batch mode: mailbox in DRAM, driven by xsct (tools/batch.tcl) -----------------
// Host: load the packed image at TEST_IN, write w, h, kernel, then cmd = 1 (last).
// Jig:  cmd = 0, status = 1 (busy); runs the job; fills the results; status = 2 (done) or
//       3 (error). Results: HW image packed at TEST_OUT, C model image packed at TEST_REF.
#define MBOX            0x13000000u
#define MBOX_CMD        0       /* word offsets */
#define MBOX_W          1
#define MBOX_H          2
#define MBOX_KERNEL     3       /* 4 = low-light enhancement, global curve */
#define MBOX_STATUS     4
#define MBOX_SEQ        5       /* incremented per finished job */
#define MBOX_ITERS      6       /* iteration count from the HW statistics */
#define MBOX_MEAN       7       /* frame mean x 1e6 */
#define MBOX_MISMATCH   8       /* pixels differing between HW and the C model */
#define MBOX_STATS_OK   9       /* 1: HW statistics identical to the C model's */
#define MBOX_CYCLES     10      /* slot active cycles of the curve pass */
#define MBOX_WORDS      16
#define PAD_IN          0x14000000u
#define PAD_OUT         0x15000000u

static void mbox_poll(void)
{
    volatile u32 *m = (volatile u32 *)MBOX;
    Xil_DCacheInvalidateRange(MBOX, MBOX_WORDS * 4);
    if (m[MBOX_CMD] != 1) return;
    m[MBOX_CMD] = 0; m[MBOX_STATUS] = 1;
    Xil_DCacheFlushRange(MBOX, MBOX_WORDS * 4);
    int w = (int)m[MBOX_W], h = (int)m[MBOX_H], k = (int)m[MBOX_KERNEL];
    int err = !zb_fbr2_available() || k != 4 || w < 8 || h < 8 || w > TEST_MAX_W || h > TEST_MAX_H;
    lle_job_t j;
    memset(&j, 0, sizeof(j));
    if (!err) {
        // the DMAs get a 256-byte aligned stride; the host image is packed
        u32 stride = ((u32)w * 4 + 255) & ~255u;
        Xil_DCacheInvalidateRange(TEST_IN, (u32)w * h * 4);
        for (int y = 0; y < h; y++)
            memcpy((u32 *)PAD_IN + (long)y * (stride / 4), (u32 *)TEST_IN + (long)y * w, (u32)w * 4);
        lle_job(w, h, PAD_IN, PAD_OUT, stride, (u32 *)TEST_REF, &j);
        for (int y = 0; y < h; y++)
            memcpy((u32 *)TEST_OUT + (long)y * w, (u32 *)PAD_OUT + (long)y * (stride / 4), (u32)w * 4);
        Xil_DCacheFlushRange(TEST_OUT, (u32)w * h * 4);
        Xil_DCacheFlushRange(TEST_REF, (u32)w * h * 4);
        err = !j.ok;
        apply_config(zb_display_mode(), zb_camera_size(), 0, 0);
    }
    m[MBOX_ITERS] = (u32)j.n_hw;
    m[MBOX_MEAN] = (u32)(lle_mean(&j.hw) * 1e6 + 0.5);
    m[MBOX_MISMATCH] = j.mismatches;
    m[MBOX_STATS_OK] = (u32)j.stats_ok;
    m[MBOX_CYCLES] = j.act;
    m[MBOX_SEQ] = m[MBOX_SEQ] + 1;
    m[MBOX_STATUS] = err ? 3 : 2;
    Xil_DCacheFlushRange(MBOX, MBOX_WORDS * 4);
    xil_printf("batch %u: %dx%d n=%d mismatches %u stats %s -> %s\r\n", m[MBOX_SEQ], w, h, j.n_hw,
               j.mismatches, j.stats_ok ? "ok" : "differ", err ? "ERROR" : "done");
}
#else
static void mbox_poll(void) { }
#endif

static void run_mem_test(int kernel, int use_loaded)
{
    if (kernel == 4) { lle_mem_test(use_loaded); return; }
    if (!zb_fbr2_available()) { xil_printf("Memory test needs v5 hardware (vfb_rd2)\r\n"); return; }
    if (!TEST_OK()) { xil_printf("Memory test needs a u-dma-buf of >= 86 MB\r\n"); return; }
    const int w = test_sizes[test_size].w, h = test_sizes[test_size].h;
    const u32 bytes = (u32)w * h * 4, stride = (u32)w * 4;
    u32 *in = (u32 *)TEST_IN, *out = (u32 *)TEST_OUT, *ref = (u32 *)TEST_REF;
    const u32 hz = zb_time_hz();
    // kernels the slot implements; Sobel is SW-only, so the HW runs pass-through for it
    const u32 hw_mode = (kernel == 1 || kernel == 2) ? (u32)kernel : 0;
    const int ref_kernel = (int)hw_mode;
    static const char *kname[4] = { "none", "grayscale", "invert", EDGE_NAME "(SW only -> HW pass)" };   // kernel 4: lle_mem_test

    xil_printf("\r\nMemory test %dx%d, kernel %s, input %s\r\n", w, h, kname[kernel & 3],
               use_loaded ? LOADED_DESC : "generated");
    if (use_loaded && !load_input(w, h)) return;
    if (!use_loaded) {
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) in[y * w + x] = test_pixel(x, y);
    }
    Xil_DCacheFlushRange(TEST_IN, bytes);
    memset(out, 0xA5, bytes);                                  // so a missed write is visible
    Xil_DCacheFlushRange(TEST_OUT, bytes);

    // switch the slot to the memory source with the live camera write stopped
    zb_diag = 1;
    zb_fbw_stop();
    slot_wr(SLOT_CTRL, SLOT_SRC_MEM | hw_mode);
    u64 t0 = zb_time();
    if (zb_fbw_single(TEST_OUT, w, h, stride) != XST_SUCCESS ||
        zb_fbr2_single(TEST_IN, w, h, stride) != XST_SUCCESS) {
        xil_printf("DMA setup failed\r\n");
        slot_wr(SLOT_CTRL, live_hw_mode);
        apply_config(zb_display_mode(), zb_camera_size(), 0, 0);
        return;
    }
    // Wait for completion. The slot's own counters are the reference for "the whole frame
    // went through": PIXELS_RUN reaches w*h and LINES_RUN reaches h. The DMA done flags are
    // recorded separately, so an early (spurious) done or a stalled read DMA is visible.
    u64 t_wr = 0, t_rd = 0, t_slot = 0;
    u32 act = 0, px = 0, lines = 0;
    while (zb_time() - t0 < (u64)hz) {                         // up to 1 s
        u64 now = zb_time();
        if (!t_wr && zb_fbw_done())  t_wr = now;
        if (!t_rd && zb_fbr2_done()) t_rd = now;
        px = slot_rd(SLOT_PIXELS_RUN); lines = slot_rd(SLOT_LINES_RUN);
        if (!t_slot && px == (u32)w * h && lines == (u32)h) t_slot = now;
        if (t_wr && t_rd && t_slot) break;
    }
    u64 t1 = zb_time();
    act = slot_rd(SLOT_ACTIVE_CYCLES); px = slot_rd(SLOT_PIXELS_RUN); lines = slot_rd(SLOT_LINES_RUN);
#define US(t) ((t) ? (u32)((((t) - t0) * 1000000ULL) / hz) : 0)
    xil_printf("  done flags: write DMA %s %u us, read DMA %s %u us; slot saw %u of %u px, %u of %u lines%s\r\n",
               t_wr ? "at" : "NOT SEEN", US(t_wr), t_rd ? "at" : "NOT SEEN", US(t_rd),
               px, (u32)w * h, lines, (u32)h, t_slot ? "" : " (INCOMPLETE)");
    if (t_wr && (!t_slot || t_wr < t_slot))
        xil_printf("  NOTE: write DMA reported done before the whole frame had passed proc_slot\r\n");
    int ok = (t_slot != 0);
    zb_diag = 0;
    if (!ok) zb_debug_dump();                                  // DMA states before anything is restarted
    slot_wr(SLOT_CTRL, live_hw_mode);                          // back to the camera
    if (!ok) {
        xil_printf("  FAIL: the test frame did not pass through proc_slot completely\r\n");
        apply_config(zb_display_mode(), zb_camera_size(), 0, 0);
        return;
    }

    // software reference on the same input
    Xil_DCacheInvalidateRange(TEST_OUT, bytes);
    memcpy(ref, in, bytes);
    u64 s0 = zb_time();
    if (ref_kernel == 1) k_gray(ref, w, h, w);
    else if (ref_kernel == 2) k_invert(ref, w, h, w);
    u64 s1 = zb_time();
    u64 sobel_t = 0;
    if (kernel == 3) {                                          // SW-only Sobel timing on a copy
        u32 *tmp = (u32 *)TEST_REF;
        memcpy(tmp, in, bytes);
        u64 q0 = zb_time(); K_EDGE(tmp, w, h, w); sobel_t = zb_time() - q0;
        memcpy(ref, in, bytes);                                 // reference for pass-through
    }

    u32 bad = 0;
    for (u32 i = 0; i < (u32)w * h; i++) {
        if ((out[i] & 0xFFFFFF) != (ref[i] & 0xFFFFFF)) {
            if (bad < 5) xil_printf("  mismatch at (%d,%d): HW %06x SW %06x\r\n",
                                    (int)(i % w), (int)(i / w), out[i] & 0xFFFFFF, ref[i] & 0xFFFFFF);
            bad++;
        }
    }

    if (bad) {
        // Is the HW result the expected image shifted along the stream? Find k with
        // out[i] == ref[i + k] for the first 64 pixels (k > 0: the first k beats were lost).
        int found = 0;
        for (int k = -64; k <= 65536 && !found; k++) {
            int match = 1;
            for (int i = 0; i < 64 && match; i++) {
                long j = (long)i + k;
                if (j < 0 || j >= (long)w * h || (out[i] & 0xFFFFFF) != (ref[j] & 0xFFFFFF)) match = 0;
            }
            if (match) {
                xil_printf("  Result = expected image shifted by %d pixels along the stream (%d lines + %d px)\r\n",
                           k, k / w, k % w);
                found = 1;
            }
        }
        if (!found) xil_printf("  Result is not a shifted copy of the expected image (searched -64 .. 65536)\r\n");
    }
    u32 hw_us_x10  = act ? (u32)(((u64)act * 10000000ULL) / SLOT_CLK_HZ) : 0;     // slot busy time
    u32 e2e_us     = (u32)(((t1 - t0) * 1000000ULL) / hz);                          // incl. DMA start/finish
    u32 mpix_x10   = act ? (u32)(((u64)px * SLOT_CLK_HZ * 10ULL) / ((u64)act * 1000000ULL)) : 0;
    u32 sw_us      = (u32)(((s1 - s0) * 1000000ULL) / hz);
    xil_printf("  HW: %u px, %u lines, %u active cycles = %u.%u us in the slot (%u.%u Mpix/s), "
               "%u us end to end\r\n", px, lines, act, hw_us_x10 / 10, hw_us_x10 % 10,
               mpix_x10 / 10, mpix_x10 % 10, e2e_us);
    if (ref_kernel) xil_printf("  SW: %u us for the same kernel on the CPU (%u.%ux slower than the slot)\r\n",
                               sw_us, hw_us_x10 ? (sw_us * 10 / hw_us_x10) : 0,
                               hw_us_x10 ? ((sw_us * 100 / hw_us_x10) % 10) : 0);
    if (kernel == 3) xil_printf("  SW " EDGE_NAME ": %u us\r\n", (u32)((sobel_t * 1000000ULL) / hz));
    xil_printf("  %s: %u of %u pixels differ between HW and the C golden model\r\n",
               bad ? "FAIL" : "PASS", bad, (u32)w * h);
    xil_printf("  Input 0x%08x, HW result 0x%08x, SW reference 0x%08x (%u bytes each)\r\n",
               TEST_IN, TEST_OUT, TEST_REF, bytes);

    // show the HW result for 3 s if it covers the screen, then resume live video
    if (w >= disp_w && h >= disp_h) {
        u32 off = (u32)(((h - disp_h) / 2) * w + (((w - disp_w) / 2) & ~7)) * 4;
        zb_fbr_config(TEST_OUT + off, disp_w, disp_h, w * 4);
        sleep(3);
    }
    apply_config(zb_display_mode(), zb_camera_size(), 0, 0);
}


// ---- gap test: tells a read-side, write-side or stream offset apart ---------------------
// Same as the memory test (pass-through), but every line is followed by a 64-pixel gap
// (input gap = marker colour) and 1 KB before the output buffer is a guard area. With a
// k-pixel shift, the end of each output line then shows WHERE the offset arises:
//   gap marker          -> read DMA fetched k*4 bytes too far ahead
//   untouched (A5)      -> write DMA stored k*4 bytes too early (guard area overwritten)
//   start of next line  -> k pixels lost from the stream between the DMAs
#define GAP_PX      64
#define GAP_MARK    0x00FF00FFu        /* written to the input gaps (alpha byte set: never a test pixel) */
#define FILL        0xA5A5A5A5u
#define GUARD_BYTES 1024
static void run_gap_test(void)
{
    if (!zb_fbr2_available()) { xil_printf("Gap test needs v5 hardware (vfb_rd2)\r\n"); return; }
    if (!TEST_OK()) { xil_printf("Gap test needs a u-dma-buf of >= 86 MB\r\n"); return; }
    const int w = test_sizes[test_size].w, h = test_sizes[test_size].h, S = w + GAP_PX;
    u32 *in = (u32 *)TEST_IN, *out = (u32 *)TEST_OUT, *guard = (u32 *)(TEST_OUT - GUARD_BYTES);
    const u32 hz = zb_time_hz(), bytes = (u32)S * h * 4;

    xil_printf("\r\nGap test %dx%d, stride %d px (%d px gap after every line)\r\n", w, h, S, GAP_PX);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < S; x++) in[y * S + x] = (x < w) ? test_pixel(x, y) : GAP_MARK;
    for (u32 i = 0; i < bytes / 4; i++) out[i] = FILL;
    for (int i = 0; i < GUARD_BYTES / 4; i++) guard[i] = FILL;
    Xil_DCacheFlushRange(TEST_IN, bytes);
    Xil_DCacheFlushRange(TEST_OUT - GUARD_BYTES, bytes + GUARD_BYTES);

    zb_diag = 1;
    zb_fbw_stop();
    slot_wr(SLOT_CTRL, SLOT_SRC_MEM);                          // pass-through
    u64 t0 = zb_time();
    if (zb_fbw_single(TEST_OUT, w, h, S * 4) != XST_SUCCESS ||
        zb_fbr2_single(TEST_IN, w, h, S * 4) != XST_SUCCESS) {
        xil_printf("DMA setup failed\r\n");
        zb_diag = 0; slot_wr(SLOT_CTRL, live_hw_mode);
        apply_config(zb_display_mode(), zb_camera_size(), 0, 0);
        return;
    }
    int complete = 0;
    while (zb_time() - t0 < (u64)hz) {
        if (slot_rd(SLOT_PIXELS_RUN) == (u32)w * h && slot_rd(SLOT_LINES_RUN) == (u32)h) { complete = 1; break; }
    }
    usleep(2000);                                              // let the last writes land
    zb_diag = 0;
    slot_wr(SLOT_CTRL, live_hw_mode);
    Xil_DCacheInvalidateRange(TEST_OUT - GUARD_BYTES, bytes + GUARD_BYTES);
    if (!complete) { xil_printf("  Frame did not pass proc_slot completely\r\n"); zb_debug_dump(); }

    // compare the active pixels
    u32 bad = 0;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++)
        if ((out[y * S + x] & 0xFFFFFF) != (in[y * S + x] & 0xFFFFFF)) bad++;
    int guard_hit = 0;
    for (int i = 0; i < GUARD_BYTES / 4; i++) if (guard[i] != FILL) guard_hit++;
    if (!bad) {
        xil_printf("  PASS: all %u pixels correct, gaps untouched check below, guard %s\r\n",
                   (u32)w * h, guard_hit ? "OVERWRITTEN" : "untouched");
    } else {
        // shift k: out[x] == in[x + k] at the start of line 0
        int k = 0;
        for (int kk = 1; kk <= 256 && !k; kk++) {
            int m = 1;
            for (int x = 0; x < 16 && m; x++) if ((out[x] & 0xFFFFFF) != (in[x + kk] & 0xFFFFFF)) m = 0;
            if (m) k = kk;
        }
        xil_printf("  FAIL: %u of %u pixels differ; line 0 shifted by %d px; guard %s (%d words)\r\n",
                   bad, (u32)w * h, k, guard_hit ? "OVERWRITTEN" : "untouched", guard_hit);
        if (k) {
            // what fills the last k pixels of lines 0 and 1?
            for (int y = 0; y < 2 && y + 1 < h; y++) {
                int n_mark = 0, n_fill = 0, n_next = 0;
                for (int x = w - k; x < w; x++) {
                    u32 v = out[y * S + x];
                    if (v == GAP_MARK) n_mark++;
                    else if (v == FILL) n_fill++;
                    else if ((v & 0xFFFFFF) == (in[(y + 1) * S + (x - (w - k))] & 0xFFFFFF)) n_next++;
                }
                xil_printf("  line %d, last %d px: %d gap marker, %d untouched, %d = start of next line\r\n",
                           y, k, n_mark, n_fill, n_next);
            }
            xil_printf("  -> gap marker: READ side fetched %d bytes ahead; untouched + guard hit: WRITE side %d bytes early;\r\n"
                       "     next line: %d px LOST from the stream between the DMAs\r\n", k * 4, k * 4, k);
        }
    }
    // gaps in the output must never be written
    u32 gap_writes = 0;
    for (int y = 0; y < h; y++) for (int x = w; x < S; x++) if (out[y * S + x] != FILL) gap_writes++;
    xil_printf("  output gap words written: %u (expect 0)\r\n", gap_writes);
    apply_config(zb_display_mode(), zb_camera_size(), 0, 0);
}

// ---- user interface ----------------------------------------------------------------------
static u32 ms_x100(u64 t)            { return (u32)((t * 100000ULL) / zb_time_hz()); }
static u32 rate_x100(u32 n, u64 dt)  { return (u32)(((u64)n * 100ULL * zb_time_hz()) / dt); }

static const char *hdmi_state(void)
{
    return !zb_hdmi_present() ? "absent" : zb_hdmi_connected() ? "monitor connected" : "no monitor";
}

#ifdef ZB_LINUX
#define MENU_BATCH ""
#else
#define MENU_BATCH "  batch mode: mailbox 0x13000000 (xsct)"
#endif
static void menu(void)
{
    xil_printf("\r\n---- zed_base jig ----------------------------------------------\r\n"
               " t  source: camera / test pattern      p  next test pattern\r\n"
               " d/D display mode                      c/C camera size\r\n"
               " 0-4 kernel: none, gray, invert, " EDGE_NAME ", LLE (global curve)\r\n"
               " x  kernel in HW (proc_slot) / SW (CPU)" MENU_BATCH "\r\n"
               " n  LLE iterations: auto (LiteIE rule) / 2 / 4 / 8 / 12 (forced, for demonstrations)\r\n"
               " m  memory test (generated image)      l  memory test (loaded image)\r\n"
               " r  memory-test size                   s  status   v  debug   h  menu\r\n"
               " g  gap test (where a memory-test shift arises: read, write or stream)\r\n"
               " H  HDMI status                        k  HDMI input clock delay (if sparkles)\r\n"
               " a  HDMI auto re-apply of lost setup on/off\r\n"
               " Switches: SW0 = SW path, SW2:1 = kernel (last change wins)\r\n");
    xil_printf(" Now: source %s | display %s, HDMI %s | camera %s | kernel %s in %s | memory test %dx%d\r\n",
               (src_pattern || !cam_ok) ? pat_name[pattern] : "camera",
               zb_modes[zb_display_mode()].name, hdmi_state(),
               cam_ok ? zb_camera_size_name(zb_camera_size()) : "not found",
               kern_name[kern], path_sw ? "SW" : "HW",
               test_sizes[test_size].w, test_sizes[test_size].h);
    xil_printf("----------------------------------------------------------------\r\n");
}

// Apply kernel / path: proc_slot runs it in the HW path; Sobel exists only in SW.
static void set_proc(int new_path_sw, int new_kern)
{
    path_sw = new_path_sw; kern = (new_kern >= 0 && new_kern <= 4) ? new_kern : 0;
    if (!path_sw) raw_ready = -1;
    live_hw_mode = path_sw ? 0 : hw_mode_of(kern);
    if (live_hw_mode == SLOT_MODE_LUT) {                    // a table must be in place first
        lle_seq_seen = slot_rd(SLOT_STAT_SEQ);
        lle_load(lle_force_tab[lle_force] ? lle_force_tab[lle_force] : lle_n_active > 0 ? lle_n_active : 8);
    }
    slot_wr(SLOT_CTRL, live_hw_mode);
    xil_printf("Kernel %s in %s%s\r\n", kern_name[kern], path_sw ? "SW (CPU)" : "HW (proc_slot)",
               (!path_sw && kern == 3) ? " -> " EDGE_NAME " is SW only, proc_slot passes through" : "");
}

int main(void)
{
#ifdef ZB_LINUX
    xil_printf("\r\nzed_base jig (Linux)\r\n");
    buf = (u32 (*)[CANVAS_MAX])zb_dma_base();
    if ((u32)NBUF * CANVAS_MAX * 4 > zb_dma_size()) {
        xil_printf("u-dma-buf too small: %u MB, live buffers need %u MB\r\n",
                   zb_dma_size() >> 20, ((u32)NBUF * CANVAS_MAX * 4) >> 20);
        return -1;
    }
    if (!TEST_OK()) xil_printf("u-dma-buf %u MB: live video only; memory/gap tests need >= 86 MB\r\n",
                               zb_dma_size() >> 20);
#else
    xil_printf("\r\nzed_base jig\r\n");
#endif
    if (zb_gpio_init() != XST_SUCCESS) { xil_printf("GPIO init failed\r\n"); return -1; }
    zb_dma_reset_release();
    u32 id = slot_rd(SLOT_ID);
    xil_printf("proc_slot ID = 0x%08x (expect 0x%08x), memory test DMA %s\r\n",
               id, SLOT_ID_EXPECTED, zb_fbr2_available() ? "present" : "absent");
    if (id != SLOT_ID_EXPECTED) xil_printf("WARNING: older proc_slot: kernel 4 (LLE) needs the v5 slot (shell v7)\r\n");
#if !defined(ZB_LINUX)
    memset((void *)MBOX, 0, MBOX_WORDS * 4);
    Xil_DCacheFlushRange(MBOX, MBOX_WORDS * 4);
#endif
    slot_wr(SLOT_CTRL, 0);

    // Start-up: 640x480 display and 640x480 camera (camera fills the screen)
    if (zb_display_set_mode(zb_mode_index(640, 480)) != XST_SUCCESS) { xil_printf("Display init failed\r\n"); return -1; }
    if (zb_hdmi_init() == XST_SUCCESS) {
        zb_hdmi_set_mode(zb_display_mode());
        xil_printf("HDMI: %s\r\n", hdmi_state());
    } else {
        xil_printf("HDMI: no ADV7511 on I2C1 (v5 hardware: VGA only)\r\n");
    }
    cam_ok = (zb_camera_init() == XST_SUCCESS);
    if (!cam_ok) xil_printf("No camera: test patterns and memory test only\r\n");
    src_pattern = !cam_ok;
    if (apply_config(zb_display_mode(), zb_camera_size(), 0, 0) != XST_SUCCESS) { xil_printf("Config failed\r\n"); return -1; }
    menu();

    u32 cam_frames = 0, disp_flips = 0, sw_frames = 0;
    u64 sw_time = 0, t_last = zb_time(), t_now, t_poll = t_last, t_hdmi = t_last;
    const u64 hz = zb_time_hz();
    u32 last_sw = 0xFFFFFFFFu, led_tog = 0;
    int status_now = 0;

    while (1) {
        // display frame complete: the programmed buffer is now being scanned
        if (zb_fbr_done()) disp_event();

        // camera frame complete
        if (cam_w >= 0 && zb_fbw_done()) {
            int finished = cam_w;
            cam_w = cam_p;                       // the DMA has moved on to the programmed buffer
            cam_frames++;
            led_tog ^= 0x80;
            if (!path_sw) { display(finished); disp_flips++; }
            else          raw_ready = finished;
            cam_p = pick_free();
            zb_fbw_set_addr(cam_addr(cam_p));
        }

        // HW LLE: new statistics latched -> table for their iteration count (queued for the next frame)
        if (!path_sw && kern == 4 && slot_rd(SLOT_STAT_SEQ) != lle_seq_seen && !lut_swap_pending()) {
            lle_stats_t s;
            lle_seq_seen = slot_rd(SLOT_STAT_SEQ);
            slot_stats(&s);
            lle_n_last = lle_pick(&s); lle_mean_last = mean_x1000(&s);
            if (lle_n_last != lle_n_active) lle_load(lle_n_last);
        }

        // SW path: process the newest camera frame, then display it
        if (path_sw && raw_ready >= 0) {
            cpu_hold = raw_ready; raw_ready = -1;
            u64 t0 = zb_time();
            if (kern) {
                Xil_DCacheInvalidateRange(cam_addr(cpu_hold), cam_span());
                u32 *p = cam_ptr(cpu_hold);
                if (kern == 1)      k_gray(p, cw, ch, canvas_w);
                else if (kern == 2) k_invert(p, cw, ch, canvas_w);
                else if (kern == 3) K_EDGE(p, cw, ch, canvas_w);
                else {                                       // LLE: statistics, table, apply
                    lle_stats_t s; u8 tab[3][256];
                    lle_stats(p, cw, ch, canvas_w, &s);
                    lle_n_last = lle_pick(&s); lle_mean_last = mean_x1000(&s);
                    lle_build_table(tab, lle_liteie_r, lle_n_last);
                    lle_apply(p, cw, ch, canvas_w, tab);
                }
                Xil_DCacheFlushRange(cam_addr(cpu_hold), cam_span());
            }
            sw_time += zb_time() - t0;
            sw_frames++;
            display(cpu_hold);
            disp_flips++;
            cpu_hold = -1;
        }

        t_now = zb_time();

        // switches every ~20 ms: act on changes only, so keys and switches can be mixed
        if (t_now - t_poll > hz / 50) {
            t_poll = t_now;
            u32 sw = zb_switches();
            zb_leds((sw & 0x7F) | led_tog);
            if ((sw & 7) != (last_sw & 7)) set_proc(sw & 1, (sw >> 1) & 3);
            last_sw = sw;
            mbox_poll();
        }

        // HDMI monitor plugged / unplugged (every ~0.5 s)
        if (t_now - t_hdmi > hz / 2) {
            t_hdmi = t_now;
            if (zb_hdmi_poll()) xil_printf("HDMI: %s\r\n", hdmi_state());
        }

        // UART menu
        int c = zb_uart_getc();
        if (c > 0) {
            int nm = zb_num_modes(), ns = zb_camera_num_sizes(), reset_stats = 1;
            switch (c) {
            case 't':
                if (!cam_ok) { xil_printf("No camera: test patterns only\r\n"); break; }
                src_pattern = !src_pattern;
                apply_config(zb_display_mode(), zb_camera_size(), 0, 0);
                break;
            case 'p':
                pattern = (pattern + 1) % 3;
                if (src_pattern || !cam_ok) apply_config(zb_display_mode(), zb_camera_size(), 0, 0);
                else xil_printf("Pattern set to %s (press t to show it)\r\n", pat_name[pattern]);
                break;
            case 'd': apply_config((zb_display_mode() + 1) % nm, zb_camera_size(), 1, 0); break;
            case 'D': apply_config((zb_display_mode() + nm - 1) % nm, zb_camera_size(), 1, 0); break;
            case 'c': case 'C':
                if (!cam_ok || !zb_camera_is_ov2640()) { xil_printf("Size change needs the OV2640\r\n"); break; }
                apply_config(zb_display_mode(), (zb_camera_size() + (c == 'c' ? 1 : ns - 1)) % ns, 0, 1);
                break;
            case '0': case '1': case '2': case '3': case '4': set_proc(path_sw, c - '0'); reset_stats = 0; break;
            case 'x': set_proc(!path_sw, kern); reset_stats = 0; break;
            case 'n':
                lle_force = (lle_force + 1) % 5;
                if (lle_force_tab[lle_force]) xil_printf("LLE iterations: %d (forced)\r\n", lle_force_tab[lle_force]);
                else                          xil_printf("LLE iterations: auto (LiteIE rule)\r\n");
                lle_seq_seen = ~0u;                          // rebuild on the next statistics
                reset_stats = 0; break;
            case 'r':
                test_size = (test_size + 1) % NUM_TEST_SIZES;
                xil_printf("Memory test size %dx%d\r\n", test_sizes[test_size].w, test_sizes[test_size].h);
                reset_stats = 0;
                break;
            case 'm': run_mem_test(kern, 0); break;
            case 'l': run_mem_test(kern, 1); break;
            case 'g': run_gap_test(); break;
            case 's': status_now = 1; reset_stats = 0; break;
            case 'v': zb_debug_dump(); reset_stats = 0; break;
            case 'H': xil_printf("HDMI:\r\n"); zb_hdmi_status(); reset_stats = 0; break;
            case 'a':
                zb_hdmi_set_auto(!zb_hdmi_auto());
                xil_printf("HDMI auto re-apply %s\r\n", zb_hdmi_auto() ? "on" : "off");
                reset_stats = 0;
                break;
            case 'k':
                if (!zb_hdmi_present()) { xil_printf("No ADV7511\r\n"); reset_stats = 0; break; }
                zb_hdmi_set_clock_delay((zb_hdmi_clock_delay() + 1) & 7);
                xil_printf("ADV7511 input clock delay %d ps\r\n", zb_hdmi_clock_delay() * 400 - 1200);
                reset_stats = 0;
                break;
            case 'h': case '?': menu(); reset_stats = 0; break;
            default: reset_stats = 0; break;
            }
            if (reset_stats) {
                cam_frames = disp_flips = disp_frames = sw_frames = 0; sw_time = 0; t_last = zb_time();
            }
        }

        // statistics every 2 s (camera source), or on 's'
        if ((t_now - t_last > 2 * hz && !(src_pattern || !cam_ok)) || (status_now && t_now > t_last)) {
            u64 dt = t_now - t_last;
            status_now = 0;
            u32 cam_r = rate_x100(cam_frames, dt), flip_r = rate_x100(disp_flips, dt);
            u32 disp_r = rate_x100(disp_frames, dt);
            u32 cyc = slot_rd(SLOT_CYCLES_LAST);
            u32 slot_r = cyc ? (u32)((SLOT_CLK_HZ * 100ULL) / cyc) : 0;
            xil_printf("[%s | %s | %s %s] cam %u.%02u fps | flips %u.%02u/s | display %u.%02u fps | "
                       "slot: %u.%02u fps, %u px, %u lines, %u stall cyc",
                       zb_modes[zb_display_mode()].name,
                       (src_pattern || !cam_ok) ? pat_name[pattern] : zb_camera_size_name(zb_camera_size()),
                       kern_name[kern], path_sw ? "SW" : "HW",
                       cam_r / 100, cam_r % 100, flip_r / 100, flip_r % 100, disp_r / 100, disp_r % 100,
                       slot_r / 100, slot_r % 100,
                       slot_rd(SLOT_PIXELS_LAST), slot_rd(SLOT_LINES_LAST), slot_rd(SLOT_STALLS_LAST));
            if (kern == 4 && lle_n_last > 0)
                xil_printf(" | LLE mean %u.%03u, %d iterations", lle_mean_last / 1000, lle_mean_last % 1000, lle_n_last);
            if (path_sw && sw_frames) {
                u32 avg = ms_x100(sw_time / sw_frames);
                xil_printf(" | SW kernel %u.%02u ms/frame", avg / 100, avg % 100);
            }
            xil_printf("\r\n");
            cam_frames = disp_flips = disp_frames = sw_frames = 0;
            sw_time = 0;
            t_last = t_now;
        }
    }
}
