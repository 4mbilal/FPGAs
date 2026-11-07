// jig.c - zed_base test application: one program, everything from the UART menu.
//
// Source files for the Vitis app (all flat in src/): jig.c + everything in sw/common/
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
//   x        run the kernel in HW (proc_slot) or SW (CPU)
//   m        memory test, generated image          l   memory test, image loaded at 0x10000000
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
// sw/tools/rawimg.py converts PNG <-> raw.

#include <string.h>
#include "xparameters.h"
#include "xil_printf.h"
#include "xil_cache.h"
#include "xil_io.h"
#include "sleep.h"
#include "zb_platform.h"
#include "sw_kernels.h"

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
#define SLOT_SRC_MEM       0x10
#define SLOT_CLK_HZ        150000000u
static u32  slot_rd(u32 off)        { return Xil_In32(SLOT_BASE + off); }
static void slot_wr(u32 off, u32 v) { Xil_Out32(SLOT_BASE + off, v); }

// ---- memory test buffers (fixed addresses, outside the program image) ---------------
#define TEST_IN   0x10000000u
#define TEST_OUT  0x11000000u
#define TEST_REF  0x12000000u
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
static int kern;                   // 0 none, 1 gray, 2 invert, 3 Sobel (SW only)
static const char *kern_name[4] = { "none", "grayscale", "invert", "Sobel" };
static const char *pat_name[3]  = { "colour bars", "checkerboard", "grey ramp" };

// ---- live buffers ------------------------------------------------------------------
#define NBUF       6
#define CANVAS_MAX (1920 * 1200)
static u32 buf[NBUF][CANVAS_MAX] __attribute__((aligned(4096)));
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

static void run_mem_test(int kernel, int use_loaded)
{
    if (!zb_fbr2_available()) { xil_printf("Memory test needs v5 hardware (vfb_rd2)\r\n"); return; }
    const int w = test_sizes[test_size].w, h = test_sizes[test_size].h;
    const u32 bytes = (u32)w * h * 4, stride = (u32)w * 4;
    u32 *in = (u32 *)TEST_IN, *out = (u32 *)TEST_OUT, *ref = (u32 *)TEST_REF;
    const u32 hz = zb_time_hz();
    // kernels the slot implements; Sobel is SW-only, so the HW runs pass-through for it
    const u32 hw_mode = (kernel == 1 || kernel == 2) ? (u32)kernel : 0;
    const int ref_kernel = (int)hw_mode;
    static const char *kname[4] = { "none", "grayscale", "invert", "Sobel(SW only -> HW pass)" };

    xil_printf("\r\nMemory test %dx%d, kernel %s, input %s\r\n", w, h, kname[kernel & 3],
               use_loaded ? "loaded at 0x10000000" : "generated");
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
        u64 q0 = zb_time(); k_sobel(tmp, w, h, w, sobel_tmp); sobel_t = zb_time() - q0;
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
    if (kernel == 3) xil_printf("  SW Sobel: %u us\r\n", (u32)((sobel_t * 1000000ULL) / hz));
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

static void menu(void)
{
    xil_printf("\r\n---- zed_base jig ----------------------------------------------\r\n"
               " t  source: camera / test pattern      p  next test pattern\r\n"
               " d/D display mode                      c/C camera size\r\n"
               " 0-3 kernel: none, gray, invert, Sobel x  HW (proc_slot) / SW (CPU)\r\n"
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
    path_sw = new_path_sw; kern = new_kern & 3;
    if (!path_sw) raw_ready = -1;
    live_hw_mode = (!path_sw && kern <= 2) ? (u32)kern : 0;
    slot_wr(SLOT_CTRL, live_hw_mode);
    xil_printf("Kernel %s in %s%s\r\n", kern_name[kern], path_sw ? "SW (CPU)" : "HW (proc_slot)",
               (!path_sw && kern == 3) ? " -> Sobel is SW only, proc_slot passes through" : "");
}

int main(void)
{
    xil_printf("\r\nzed_base jig\r\n");
    if (zb_gpio_init() != XST_SUCCESS) { xil_printf("GPIO init failed\r\n"); return -1; }
    zb_dma_reset_release();
    u32 id = slot_rd(SLOT_ID);
    xil_printf("proc_slot ID = 0x%08x (expect 0x534c0002), memory test DMA %s\r\n",
               id, zb_fbr2_available() ? "present" : "absent");
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

        // SW path: process the newest camera frame, then display it
        if (path_sw && raw_ready >= 0) {
            cpu_hold = raw_ready; raw_ready = -1;
            u64 t0 = zb_time();
            if (kern) {
                Xil_DCacheInvalidateRange(cam_addr(cpu_hold), cam_span());
                u32 *p = cam_ptr(cpu_hold);
                if (kern == 1)      k_gray(p, cw, ch, canvas_w);
                else if (kern == 2) k_invert(p, cw, ch, canvas_w);
                else                k_sobel(p, cw, ch, canvas_w, sobel_tmp);
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
            case '0': case '1': case '2': case '3': set_proc(path_sw, c - '0'); reset_stats = 0; break;
            case 'x': set_proc(!path_sw, kern); reset_stats = 0; break;
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
