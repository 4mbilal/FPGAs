// zb_platform.h - shared bring-up for the zed_base platform (bare metal, Vitis 2024.2 SDT)
// Board GPIO, EMIO DMA resets, camera (OV2640/OV7670 over SCCB), display modes
// (VTC + pixel clock), frame buffer DMAs, memory-test DMA, UART input, time base.
// Copy common/*.c and common/*.h flat into every test app's src folder.

#ifndef ZB_PLATFORM_H
#define ZB_PLATFORM_H

#include "xil_types.h"

// Defaults (v2-v4 layout: 1024x768 display, 640x480 camera window)
#define DISP_W       1024
#define DISP_H       768
#define DISP_STRIDE  (DISP_W * 4)          // bytes per display line (XRGB8888)
#define CAM_W        640
#define CAM_H        480
#define CAM_X0       ((DISP_W - CAM_W) / 2)
#define CAM_Y0       ((DISP_H - CAM_H) / 2)
#define CAM_WIN_OFF  ((CAM_Y0 * DISP_W + CAM_X0) * 4)   // byte offset of the camera window

// ---- board I/O ------------------------------------------------------------------
// LEDs on AXI GPIO ch1, switches ch2, buttons on a second AXI GPIO
int  zb_gpio_init(void);
u32  zb_switches(void);
u32  zb_buttons(void);
void zb_leds(u32 v);

// Non-blocking UART read: returns the character, or -1 if none
int  zb_uart_getc(void);

// Release the reset of all frame buffer DMAs (EMIO GPIO 54 = display read,
// 55 = camera/test write, 56 = memory-test read; 56 is unused on v4 hardware)
void zb_dma_reset_release(void);

// ---- camera -----------------------------------------------------------------------
// Detect (OV2640 at 0x30, OV7670 at 0x21) and configure 640x480 RGB565
int  zb_camera_init(void);
int  zb_camera_is_ov2640(void);
// OV2640 output sizes (index into the table in ov2640_regs.h); OV7670: VGA only.
int         zb_camera_num_sizes(void);
int         zb_camera_set_size(int idx);
int         zb_camera_size(void);            // current index
int         zb_camera_w(void);
int         zb_camera_h(void);
const char *zb_camera_size_name(int idx);

// ---- display modes ----------------------------------------------------------------
typedef struct {
    const char *name;
    u16 w, h;
    u32 pix_khz;
    u8  d; u8 m_int; u16 m_frac; u8 o_int; u16 o_frac;   // MMCM, fractions in 1/1000
    u16 hfp, hsw, hbp; u8 hpol;                            // pol: 1 = active high
    u16 vfp, vsw, vbp; u8 vpol;
} zb_mode_t;
extern const zb_mode_t zb_modes[];
int  zb_num_modes(void);
int  zb_mode_index(int w, int h);              // -1 if not in the table
// Programs the pixel clock (v5 hardware: clk_wiz dynamic reconfiguration) and the VTC.
// On v2-v4 hardware the pixel clock is fixed at 65 MHz: only 1024x768 works there.
int  zb_display_set_mode(int idx);
int  zb_display_mode(void);
// v2-v4 compatible: 1024x768@60
int  zb_vtc_init(void);

// ---- HDMI output (ADV7511 on PS I2C1, v6 hardware) ----------------------------------
// The FPGA sends 16-bit YCbCr 4:2:2 (hdmi_pack.v); the ADV7511 converts to RGB 4:4:4 and
// drives the monitor in DVI mode (works with HDMI and DVI monitors, full-range RGB).
// The ADV7511 is set to ignore hot-plug for power control, so it stays configured while
// no monitor is connected; zb_hdmi_poll() re-applies the setup if the chip lost it.
int  zb_hdmi_init(void);                 // XST_SUCCESS if the ADV7511 answers (v6 hardware)
int  zb_hdmi_present(void);
int  zb_hdmi_poll(void);                 // call every ~0.5 s: 1 if the monitor state changed
int  zb_hdmi_connected(void);            // hot-plug detect from the monitor
void zb_hdmi_set_mode(int mode_idx);     // after a display mode change (aspect ratio)
int  zb_hdmi_clock_delay(void);          // ADV7511 input clock delay code 0..7 (3 = 0 ps)
void zb_hdmi_set_clock_delay(int code);  // 400 ps steps: -1200 .. +1600 ps
void zb_hdmi_status(void);               // print chip state and event counters
int  zb_hdmi_auto(void);                 // 1: zb_hdmi_poll re-applies a lost setup (default off)
void zb_hdmi_set_auto(int on);

// ---- frame buffer DMAs (XRGB8888, Xilinx BGRX8) -----------------------------------
// Display read DMA (vfb_rd): continuous (auto-restart).
int  zb_fbr_init(UINTPTR addr);                                   // DISP_W x DISP_H, DISP_STRIDE
int  zb_fbr_config(UINTPTR addr, int w, int h, int stride_bytes); // stop, reformat, restart
void zb_fbr_stop(void);
void zb_fbr_set_addr(UINTPTR addr);                               // takes effect next frame
int  zb_fbr_done(void);                                           // 1 once per completed frame

// Camera/test write DMA (vfb_wr)
int  zb_fbw_init(UINTPTR addr);                                   // CAM_W x CAM_H, DISP_STRIDE
int  zb_fbw_config(UINTPTR addr, int w, int h, int stride_bytes); // continuous
int  zb_fbw_single(UINTPTR addr, int w, int h, int stride_bytes); // one frame, no auto-restart
void zb_fbw_stop(void);
void zb_fbw_set_addr(UINTPTR addr);
int  zb_fbw_done(void);

// Memory-test read DMA (vfb_rd2, v5 hardware): one frame from DRAM into proc_slot
int  zb_fbr2_available(void);
int  zb_fbr2_single(UINTPTR addr, int w, int h, int stride_bytes);
int  zb_fbr2_done(void);

// ---- debug -------------------------------------------------------------------------------
// Prints the state of the frame buffer DMAs, the VTC and the pixel clock
void zb_debug_dump(void);
extern int zb_diag;                     // 1: print DMA restart diagnostics

// ---- time base ----------------------------------------------------------------------
// Cortex-A9 global timer (always present on Zynq-7000, independent of the BSP's timer
// library). Ticks at CPU clock / 2 / (prescaler + 1).
u64  zb_time(void);
u32  zb_time_hz(void);

#endif
