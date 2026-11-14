// zb_platform.c - shared bring-up for the zed_base platform. See zb_platform.h.

#include <string.h>
#include "xparameters.h"
#include "xil_printf.h"
#include "sleep.h"
#include "xil_io.h"
#include "xgpio.h"
#include "xgpiops.h"
#include "xiicps.h"
#include "xuartps_hw.h"
#include "xvtc.h"
#include "xv_frmbufrd_l2.h"
#include "xv_frmbufwr_l2.h"
#include "zb_platform.h"

#define EMIO_RST_RD   54
#define EMIO_RST_WR   55
#define EMIO_RST_RD2  56
#define FB_DONE_MASK  0x1       // ap_done bit in the HLS IER/ISR registers

#if defined(STDIN_BASEADDRESS)
#define ZB_UART_BASE STDIN_BASEADDRESS
#else
#define ZB_UART_BASE XPAR_XUARTPS_0_BASEADDR
#endif

static XGpio gpio_ledsw, gpio_btns;
static XGpioPs gpio_ps;
static XIicPs iic;
static XVtc vtc;
static int vtc_ready;
static XV_FrmbufRd_l2 fbr;
static XV_FrmbufWr_l2 fbw;
static int fbr_ready, fbw_ready;
int zb_diag;                            // print restart diagnostics
#ifdef XPAR_VFB_RD2_BASEADDR
static XV_FrmbufRd_l2 fbr2;
static int fbr2_ready;
#endif

// ---- board I/O ------------------------------------------------------------------
int zb_gpio_init(void)
{
    if (XGpio_Initialize(&gpio_ledsw, XPAR_AXI_GPIO_LEDSW_BASEADDR) != XST_SUCCESS) return XST_FAILURE;
    if (XGpio_Initialize(&gpio_btns,  XPAR_AXI_GPIO_BTNS_BASEADDR)  != XST_SUCCESS) return XST_FAILURE;
    XGpio_SetDataDirection(&gpio_ledsw, 1, 0x00);
    XGpio_SetDataDirection(&gpio_ledsw, 2, 0xFF);
    XGpio_SetDataDirection(&gpio_btns,  1, 0x1F);
    return XST_SUCCESS;
}
u32  zb_switches(void) { return XGpio_DiscreteRead(&gpio_ledsw, 2); }
u32  zb_buttons(void)  { return XGpio_DiscreteRead(&gpio_btns, 1); }
void zb_leds(u32 v)    { XGpio_DiscreteWrite(&gpio_ledsw, 1, v); }

int zb_uart_getc(void)
{
    if (!XUartPs_IsReceiveData(ZB_UART_BASE)) return -1;
    return (int)(XUartPs_ReadReg(ZB_UART_BASE, XUARTPS_FIFO_OFFSET) & 0xFF);
}

void zb_dma_reset_release(void)
{
    XGpioPs_Config *c = XGpioPs_LookupConfig(XPAR_XGPIOPS_0_BASEADDR);
    XGpioPs_CfgInitialize(&gpio_ps, c, c->BaseAddr);
    const int pins[3] = { EMIO_RST_RD, EMIO_RST_WR, EMIO_RST_RD2 };
    for (int i = 0; i < 3; i++) {
        XGpioPs_SetDirectionPin(&gpio_ps, pins[i], 1);
        XGpioPs_SetOutputEnablePin(&gpio_ps, pins[i], 1);
        XGpioPs_WritePin(&gpio_ps, pins[i], 0);
    }
    usleep(1000);
    for (int i = 0; i < 3; i++) XGpioPs_WritePin(&gpio_ps, pins[i], 1);
}

// Pulse one DMA's reset, as the Linux xilinx_frmbuf driver does after stopping the IP:
// a stopped frame buffer core is restarted from reset, not just re-armed.
static void dma_reset_pulse(int pin)
{
    XGpioPs_WritePin(&gpio_ps, pin, 0);
    usleep(10);
    XGpioPs_WritePin(&gpio_ps, pin, 1);
    usleep(10);
}

// Graceful stop of a frame buffer core: clear auto-restart and let the frame in progress
// finish, so every AXI burst it has started completes, then wait a little longer for the
// last write responses (the Linux driver waits 50 ms before resetting a stopped core).
// The Xilinx l2 Stop() instead FLUSHES (aborts) the frame; resetting the core right after
// an abort can leave a write address without its data in the PS HP port, which then
// shifts every later frame by one burst (32 px) until the whole system is reset.
// Falls back to the flush only if the core does not go idle (e.g. no input stream).
#define HLS_CTRL_IDLE    0x04
#define HLS_CTRL_AUTORST 0x80
static int hls_wait_idle(UINTPTR base, u32 timeout_us)
{
    for (u32 t = 0; t < timeout_us; t += 100) {
        if (Xil_In32(base) & HLS_CTRL_IDLE) return 1;
        usleep(100);
    }
    return 0;
}
static int hls_graceful_stop(UINTPTR base, const char *name)
{
    Xil_Out32(base, Xil_In32(base) & ~HLS_CTRL_AUTORST & 0xFF & ~0x01u);   // auto-restart off, no new start
    if (hls_wait_idle(base, 250000)) { usleep(5000); return 1; }           // frame done + margin
    if (zb_diag) xil_printf("  [%s] did not go idle within 250 ms\r\n", name);
    return 0;
}

// ---- SCCB --------------------------------------------------------------------
static u8 cam_addr;             // 7-bit address of the detected sensor

static int sccb_write(u8 reg, u8 val)
{
    u8 buf[2] = { reg, val };
    int s = XIicPs_MasterSendPolled(&iic, buf, 2, cam_addr);
    while (XIicPs_BusIsBusy(&iic));
    return s;
}

static int sccb_read(u8 reg, u8 *val)
{
    // SCCB: register address write and data read are separate transactions (stop in between)
    int s = XIicPs_MasterSendPolled(&iic, &reg, 1, cam_addr);
    while (XIicPs_BusIsBusy(&iic));
    if (s != XST_SUCCESS) return s;
    s = XIicPs_MasterRecvPolled(&iic, val, 1, cam_addr);
    while (XIicPs_BusIsBusy(&iic));
    return s;
}

static int sccb_probe(u8 addr)
{
    u8 reg = 0x0A;
    int s = XIicPs_MasterSendPolled(&iic, &reg, 1, addr);
    while (XIicPs_BusIsBusy(&iic));
    return s == XST_SUCCESS;
}

static int write_table(const u8 (*t)[2], unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        if (sccb_write(t[i][0], t[i][1]) != XST_SUCCESS) {
            xil_printf("SCCB write [%u] reg 0x%02x failed\r\n", i, t[i][0]);
            return XST_FAILURE;
        }
        usleep(1000);
    }
    return XST_SUCCESS;
}

// OV7670 (0x21): VGA, RGB565 full range. Window values as in the Linux ov7670 driver
// (hstart 158, hstop 14, vstart 10, vstop 490). Colour matrix for RGB565.
static const u8 ov7670_vga_rgb565[][2] = {
    {0x12, 0x04},   // COM7: VGA, RGB
    {0x11, 0x00},   // CLKRC: internal clock = XCLK
    {0x0C, 0x00},   // COM3: no scaling
    {0x3E, 0x00},   // COM14: normal PCLK, no scaling
    {0x8C, 0x00},   // RGB444 off
    {0x40, 0xD0},   // COM15: RGB565, output range 00..FF
    {0x3A, 0x04},   // TSLB
    {0x15, 0x00},   // COM10: VSYNC active high, PCLK free-running
    {0x17, 0x13}, {0x18, 0x01}, {0x32, 0xB6},   // HSTART, HSTOP, HREF
    {0x19, 0x02}, {0x1A, 0x7A}, {0x03, 0x0A},   // VSTART, VSTOP, VREF
    {0x4F, 0xB3}, {0x50, 0xB3}, {0x51, 0x00},   // colour matrix
    {0x52, 0x3D}, {0x53, 0xA7}, {0x54, 0xE4}, {0x58, 0x9E},
    {0x3D, 0xC0},   // COM13: gamma + UV saturation auto adjust
    {0x13, 0xE7},   // COM8: fast AGC/AEC, AGC, AWB, AEC on
};

// OV2640 (0x30): table generated from the Linux driver, see ov2640_regs.h
#include "ov2640_regs.h"

static int cam_is_ov2640;
static int cam_size_idx = 3;          // VGA in ov2640_sizes[]

int zb_camera_is_ov2640(void)          { return cam_is_ov2640; }
int zb_camera_num_sizes(void)          { return cam_is_ov2640 ? (int)OV2640_NUM_SIZES : 1; }
int zb_camera_size(void)               { return cam_size_idx; }
int zb_camera_w(void)                  { return ov2640_sizes[cam_size_idx].w; }
int zb_camera_h(void)                  { return ov2640_sizes[cam_size_idx].h; }
const char *zb_camera_size_name(int i) { return (i >= 0 && i < (int)OV2640_NUM_SIZES) ? ov2640_sizes[i].name : "?"; }

static int ov2640_read_id(void)
{
    u8 pid = 0, ver = 0;
    sccb_write(0xFF, 0x01);                       // sensor register bank
    if (sccb_read(0x0A, &pid) != XST_SUCCESS || sccb_read(0x0B, &ver) != XST_SUCCESS) {
        // Writes are ACKed (device found at 0x30), so continue without the ID check.
        xil_printf("OV2640 ID read failed; continuing (writes are ACKed at 0x30)\r\n");
        return XST_SUCCESS;
    }
    xil_printf("Camera ID: PID=0x%02x VER=0x%02x (OV2640: 0x26 0x42)\r\n", pid, ver);
    if (pid != 0x26) xil_printf("Unexpected PID; continuing with the OV2640 tables\r\n");
    return XST_SUCCESS;
}

// Full OV2640 sequence as the Linux driver's ov2640_set_params():
// reset -> init -> size preamble -> size -> format preamble -> RGB565 BE
int zb_camera_set_size(int idx)
{
    if (!cam_is_ov2640) {
        // OV7670 table is VGA only
        return (idx >= 0 && ov2640_sizes[idx].w == 640 && ov2640_sizes[idx].h == 480) ? XST_SUCCESS : XST_FAILURE;
    }
    if (idx < 0 || idx >= (int)OV2640_NUM_SIZES) return XST_FAILURE;
    sccb_write(0xFF, 0x01);
    sccb_write(0x12, 0x80);                       // COM7: soft reset
    usleep(10000);
    if (write_table(ov2640_init, sizeof(ov2640_init) / 2) != XST_SUCCESS) return XST_FAILURE;
    if (write_table(ov2640_size_preamble, sizeof(ov2640_size_preamble) / 2) != XST_SUCCESS) return XST_FAILURE;
    if (write_table(ov2640_sizes[idx].regs, ov2640_sizes[idx].n) != XST_SUCCESS) return XST_FAILURE;
    if (write_table(ov2640_fmt_preamble, sizeof(ov2640_fmt_preamble) / 2) != XST_SUCCESS) return XST_FAILURE;
    if (write_table(ov2640_fmt_rgb565_be, sizeof(ov2640_fmt_rgb565_be) / 2) != XST_SUCCESS) return XST_FAILURE;
    cam_size_idx = idx;
    return XST_SUCCESS;
}

static int init_ov7670(void)
{
    u8 pid = 0, ver = 0;
    if (sccb_read(0x0A, &pid) != XST_SUCCESS || sccb_read(0x0B, &ver) != XST_SUCCESS) {
        xil_printf("OV7670 ID read failed\r\n");
        return XST_FAILURE;
    }
    xil_printf("Camera ID: PID=0x%02x VER=0x%02x (OV7670: 0x76 0x73)\r\n", pid, ver);
    if (pid != 0x76) return XST_FAILURE;

    sccb_write(0x12, 0x80);                       // COM7: soft reset
    usleep(100000);
    return write_table(ov7670_vga_rgb565, sizeof(ov7670_vga_rgb565) / 2);
}

int zb_camera_init(void)
{
    XIicPs_Config *cfg = XIicPs_LookupConfig(XPAR_XIICPS_0_BASEADDR);
    if (!cfg || XIicPs_CfgInitialize(&iic, cfg, cfg->BaseAddress) != XST_SUCCESS) return XST_FAILURE;
    XIicPs_SetSClk(&iic, 100000);

    if (sccb_probe(0x30)) {
        cam_addr = 0x30; cam_is_ov2640 = 1;
        ov2640_read_id();
        return zb_camera_set_size(3);             // VGA
    }
    if (sccb_probe(0x21)) {
        cam_addr = 0x21; cam_is_ov2640 = 0; cam_size_idx = 3;
        return init_ov7670();
    }
    xil_printf("No camera on SCCB (0x30 OV2640 / 0x21 OV7670). Check SDA/SCL, pull-ups, XCLK\r\n");
    return XST_FAILURE;
}

// ---- display modes --------------------------------------------------------------------
// MMCM from the 100 MHz FCLK0 input; VCO kept in 600..1200 MHz. Timing: VESA / CEA-861.
const zb_mode_t zb_modes[] = {
    // name           w     h     kHz     D  M   Mfrac O   Ofrac  HFP  HSW  HBP Hp  VFP VSW VBP Vp
    { "640x480@60",   640,  480,  25175,  1, 9,    0, 35, 750,    16,  96,  48, 0,  10,  2, 33, 0 },
    { "800x600@60",   800,  600,  40000,  1, 10,   0, 25,   0,    40, 128,  88, 1,   1,  4, 23, 1 },
    { "1024x768@60",  1024, 768,  65000,  2, 13,   0, 10,   0,    24, 136, 160, 0,   3,  6, 29, 0 },
    { "1280x720@60",  1280, 720,  74250,  5, 37, 125, 10,   0,   110,  40, 220, 1,   5,  5, 20, 1 },
    { "1280x1024@60", 1280, 1024, 108000, 5, 54,   0, 10,   0,    48, 112, 248, 1,   1,  3, 38, 1 },
    { "1920x1080@60", 1920, 1080, 148500, 5, 37, 125,  5,   0,    88,  44, 148, 1,   4,  5, 36, 1 },
};
#define NUM_MODES ((int)(sizeof(zb_modes) / sizeof(zb_modes[0])))
static int disp_mode = -1;

int zb_num_modes(void)  { return NUM_MODES; }
int zb_display_mode(void) { return disp_mode; }
int zb_mode_index(int w, int h)
{
    for (int i = 0; i < NUM_MODES; i++) if (zb_modes[i].w == w && zb_modes[i].h == h) return i;
    return -1;
}

#ifdef XPAR_CLK_WIZ_PIX_BASEADDR
// Clocking Wizard dynamic reconfiguration (same register sequence as the Linux
// clk-xlnx-clock-wizard driver for 7-series MMCMs)
#define WZ_STATUS   0x04
#define WZ_CFG0     0x200      // [7:0] DIVCLK, [15:8] CLKFBOUT_MULT, [25:16] MULT frac (1/1000)
#define WZ_CFG2     0x208      // [7:0] CLKOUT0_DIVIDE, [17:8] frac (1/1000)
#define WZ_CFG3     0x20C      // CLKOUT0 phase
#define WZ_LOAD     0x25C      // write 0x3: load + use the CFG registers
static int clkwiz_wait_locked(void)
{
    for (int i = 0; i < 100000; i++) {
        if (Xil_In32(XPAR_CLK_WIZ_PIX_BASEADDR + WZ_STATUS) & 1) return XST_SUCCESS;
        usleep(10);
    }
    return XST_FAILURE;
}
static int clkwiz_set(const zb_mode_t *m)
{
    const UINTPTR b = XPAR_CLK_WIZ_PIX_BASEADDR;
    if (clkwiz_wait_locked() != XST_SUCCESS) return XST_FAILURE;
    Xil_Out32(b + WZ_CFG2, (m->o_int & 0xFF) | ((u32)(m->o_frac & 0x3FF) << 8));
    Xil_Out32(b + WZ_CFG0, (m->d & 0xFF) | ((u32)(m->m_int & 0xFF) << 8) | ((u32)(m->m_frac & 0x3FF) << 16));
    Xil_Out32(b + WZ_CFG3, 0);
    Xil_Out32(b + WZ_LOAD, 0x3);
    return clkwiz_wait_locked();
}
#endif

int zb_display_set_mode(int idx)
{
    if (idx < 0 || idx >= NUM_MODES) return XST_FAILURE;
    const zb_mode_t *m = &zb_modes[idx];

    if (!vtc_ready) {
        XVtc_Config *cfg = XVtc_LookupConfig(XPAR_VTC_BASEADDR);
        if (!cfg || XVtc_CfgInitialize(&vtc, cfg, cfg->BaseAddress) != XST_SUCCESS) return XST_FAILURE;
        vtc_ready = 1;
    }
    XVtc_DisableGenerator(&vtc);

#ifdef XPAR_CLK_WIZ_PIX_BASEADDR
    if (clkwiz_set(m) != XST_SUCCESS) { xil_printf("Pixel clock did not lock for %s\r\n", m->name); return XST_FAILURE; }
    // rst_pix (proc_sys_reset on the pixel clock) holds the VTC and the video output in
    // reset for a while after lock; program the VTC only after it has been released.
    usleep(5000);
    if (XVtc_CfgInitialize(&vtc, XVtc_LookupConfig(XPAR_VTC_BASEADDR), XPAR_VTC_BASEADDR) != XST_SUCCESS) return XST_FAILURE;
#else
    if (m->pix_khz != 65000) {
        xil_printf("This hardware has a fixed 65 MHz pixel clock: only 1024x768@60 is possible\r\n");
        return XST_FAILURE;
    }
#endif

    XVtc_Timing t;
    memset(&t, 0, sizeof(t));
    t.HActiveVideo = m->w; t.HFrontPorch = m->hfp; t.HSyncWidth = m->hsw; t.HBackPorch = m->hbp;
    t.HSyncPolarity = m->hpol;
    t.VActiveVideo = m->h; t.V0FrontPorch = m->vfp; t.V0SyncWidth = m->vsw; t.V0BackPorch = m->vbp;
    t.VSyncPolarity = m->vpol;
    t.Interlaced = 0;
    XVtc_SetGeneratorTiming(&vtc, &t);
    XVtc_RegUpdateEnable(&vtc);
    XVtc_EnableGenerator(&vtc);
    disp_mode = idx;
    return XST_SUCCESS;
}

int zb_vtc_init(void) { return zb_display_set_mode(zb_mode_index(1024, 768)); }

// ---- HDMI: ADV7511 ---------------------------------------------------------------------
// Register settings follow the Linux adv7511 driver (drivers/gpu/drm/bridge/adv7511) with
// the ZedBoard device tree of the Analog Devices reference design:
//   input-colorspace yuv422, input-clock 1x, input-style 1, input-depth 8,
//   input-justification right, clock-delay 0 ps, separate syncs
// -> input ID 1, 0x16 = 0x38, 0x48 = 0x08, 0xBA = 0x60; CSC YCbCr -> RGB (BT.709, the
// driver's matrix, scaling x4); DVI mode (no infoframes, full-range RGB).
#define ADV_ADDR        0x39            // 7-bit address (0x72 8-bit)
#define I2C1_BASE       0xE0005000u     // PS I2C1 (EMIO in v6 hardware)

static XIicPs iic_hdmi;
static int    hdmi_ok, hdmi_hpd = -1, hdmi_delay = 3, hdmi_aspect_169;
// Re-apply of a "lost" setup: off by default. On the board (2026-11-07) the check reported
// lost setups while the picture was perfect, and the re-apply itself caused the purple
// flicker and sync losses. Key 'a' turns it on for testing.
static int    hdmi_auto = 0;
static u32    hdmi_reapply, hdmi_i2c_err, hdmi_hpd_changes;

static int adv_wr(u8 reg, u8 val)
{
    u8 b[2] = { reg, val };
    int s = XIicPs_MasterSendPolled(&iic_hdmi, b, 2, ADV_ADDR);
    while (XIicPs_BusIsBusy(&iic_hdmi));
    return s;
}
// Register read as one I2C transaction: address write, repeated start, read (as the Linux
// driver). The earlier write-stop-read version occasionally returned the same wrong value
// for every register on the board (logged 41=15=16=18=42=0x14).
static int adv_rd(u8 reg, u8 *val)
{
    XIicPs_SetOptions(&iic_hdmi, XIICPS_REP_START_OPTION);      // no stop after the write
    int s = XIicPs_MasterSendPolled(&iic_hdmi, &reg, 1, ADV_ADDR);
    XIicPs_ClearOptions(&iic_hdmi, XIICPS_REP_START_OPTION);    // stop after the read
    if (s != XST_SUCCESS) {
        while (XIicPs_BusIsBusy(&iic_hdmi));
        return s;
    }
    s = XIicPs_MasterRecvPolled(&iic_hdmi, val, 1, ADV_ADDR);
    while (XIicPs_BusIsBusy(&iic_hdmi));
    return s;
}
static u8 adv_get(u8 reg) { u8 v = 0; adv_rd(reg, &v); return v; }
static void adv_upd(u8 reg, u8 mask, u8 val) { adv_wr(reg, (adv_get(reg) & ~mask) | (val & mask)); }

// YCbCr -> RGB, BT.709 limited range in, full range out (Linux adv7511_csc_ycbcr_to_rgb).
// Registers 0x18..0x2F: A1..A4, B1..B4, C1..C4, 13-bit two's complement, upper 5 bits first.
static const u16 adv_csc[12] = { 0x0734, 0x04ad, 0x0000, 0x1c1b,
                                 0x1ddc, 0x04ad, 0x1f24, 0x0135,
                                 0x0000, 0x04ad, 0x087c, 0x1b77 };

static void adv_configure(void)
{
    adv_upd(0xD6, 0xC0, 0xC0);                  // HPD source: none (always powered, keeps setup)
    adv_upd(0x41, 0x40, 0x00);                  // power up
    usleep(1000);
    static const u8 fixed[][2] = {              // required by the programming guide
        {0x98, 0x03}, {0x9A, 0xE0}, {0x9C, 0x30}, {0x9D, 0x61},
        {0xA2, 0xA4}, {0xA3, 0xA4}, {0xE0, 0xD0}, {0xF9, 0x00} };
    for (unsigned i = 0; i < sizeof(fixed) / 2; i++) adv_wr(fixed[i][0], fixed[i][1]);

    adv_upd(0x15, 0x0F, 0x01);                  // input ID 1: YCbCr 4:2:2, separate syncs
    adv_wr(0x16, 0x38);                         // out 4:4:4 RGB, 8 bit, style 1 (code 2), rising edge
    adv_wr(0x48, 0x08);                         // right justified
    adv_wr(0xD0, 0x0C);                         // sync pulse: none (as the Linux driver)
    adv_wr(0xBA, (u8)(hdmi_delay << 5));        // input clock delay
    adv_upd(0x17, 0x62, hdmi_aspect_169 ? 0x02 : 0x00);   // syncs passed through, aspect ratio
    adv_wr(0x55, 0x00);                         // AVI infoframe: RGB (only used in HDMI mode)

    adv_upd(0x1A, 0x20, 0x20);                  // CSC update mode: hold while loading
    for (int i = 0; i < 12; i++) {
        adv_upd((u8)(0x18 + 2 * i), 0x1F, (u8)(adv_csc[i] >> 8));
        adv_wr((u8)(0x19 + 2 * i), (u8)(adv_csc[i] & 0xFF));
    }
    adv_upd(0x18, 0xE0, 0x80 | (2 << 5));       // CSC on, scaling x4
    adv_upd(0x1A, 0x20, 0x00);                  // apply

    adv_upd(0xAF, 0x02, 0x00);                  // DVI mode
}

int zb_hdmi_init(void)
{
    XIicPs_Config *cfg = XIicPs_LookupConfig(I2C1_BASE);
    if (!cfg || XIicPs_CfgInitialize(&iic_hdmi, cfg, cfg->BaseAddress) != XST_SUCCESS) return XST_FAILURE;
    XIicPs_SetSClk(&iic_hdmi, 100000);
    u8 rev;
    if (adv_rd(0x00, &rev) != XST_SUCCESS) return XST_FAILURE;
    xil_printf("ADV7511 found: revision 0x%02x, chip ID 0x%02x%02x\r\n", rev, adv_get(0xF5), adv_get(0xF6));
    hdmi_ok = 1;
    adv_configure();
    zb_hdmi_poll();
    return XST_SUCCESS;
}

int zb_hdmi_present(void)   { return hdmi_ok; }
int zb_hdmi_connected(void) { return hdmi_ok && hdmi_hpd == 1; }

int zb_hdmi_poll(void)
{
    if (!hdmi_ok) return 0;
    u8 r41, r15, r16, r18, r42;
    if (adv_rd(0x41, &r41) != XST_SUCCESS || adv_rd(0x15, &r15) != XST_SUCCESS ||
        adv_rd(0x16, &r16) != XST_SUCCESS || adv_rd(0x18, &r18) != XST_SUCCESS ||
        adv_rd(0x42, &r42) != XST_SUCCESS) {
        hdmi_i2c_err++;                      // no decision on a failed read
        return 0;
    }
    if (r41 == r15 && r15 == r16 && r16 == r18 && r18 == r42) {
        hdmi_i2c_err++;                      // five different registers, one value: bad read
        if (hdmi_i2c_err <= 10 || hdmi_i2c_err % 50 == 0)
            xil_printf("[hdmi] suspect I2C read #%u: all registers read 0x%02x\r\n", hdmi_i2c_err, r41);
        return 0;
    }
    // setup lost: powered down, input ID / format changed, or colour conversion off
    // (0x18 bit 7: CSC enable; without it the monitor shows YCbCr as RGB = purple tint)
    int lost = (r41 & 0x40) || (r15 & 0x0F) != 0x01 || r16 != 0x38 || !(r18 & 0x80);
    if (lost) {
        hdmi_reapply++;
        if (hdmi_reapply <= 10 || hdmi_reapply % 50 == 0)
            xil_printf("[hdmi] setup lost #%u: 41=%02x 15=%02x 16=%02x 18=%02x 42=%02x -> %s\r\n",
                       hdmi_reapply, r41, r15, r16, r18, r42, hdmi_auto ? "re-applied" : "left (auto off)");
        if (hdmi_auto) adv_configure();
    }
    int hpd = (r42 >> 6) & 1;
    if (hpd == hdmi_hpd) return 0;
    if (hdmi_hpd >= 0) hdmi_hpd_changes++;
    hdmi_hpd = hpd;
    return 1;
}

int  zb_hdmi_auto(void)        { return hdmi_auto; }
void zb_hdmi_set_auto(int on)  { hdmi_auto = on; }

void zb_hdmi_set_mode(int idx)
{
    if (idx < 0 || idx >= NUM_MODES) return;
    // 16:9 for 1280x720 and 1920x1080 (infoframe content; DVI mode ignores it)
    hdmi_aspect_169 = (zb_modes[idx].w * 9 == zb_modes[idx].h * 16);
    if (hdmi_ok) adv_upd(0x17, 0x02, hdmi_aspect_169 ? 0x02 : 0x00);
}

int  zb_hdmi_clock_delay(void) { return hdmi_delay; }
void zb_hdmi_set_clock_delay(int code)
{
    hdmi_delay = code & 7;
    if (hdmi_ok) adv_wr(0xBA, (u8)(hdmi_delay << 5));
}

void zb_hdmi_status(void)
{
    if (!hdmi_ok) { xil_printf("  hdmi     ADV7511 not found (needs v6 hardware: I2C1 on EMIO)\r\n"); return; }
    u8 st = adv_get(0x42), pw = adv_get(0x41), vic = adv_get(0x3E), pll = adv_get(0x9E);
    xil_printf("  hdmi     monitor %s (HPD %u, sense %u), %s, TMDS PLL %s, input VIC %u, "
               "clock delay %d ps, regs 15=%02x 16=%02x 17=%02x 48=%02x AF=%02x BA=%02x D6=%02x\r\n",
               (st & 0x40) ? "connected" : "not connected", (st >> 6) & 1, (st >> 5) & 1,
               (pw & 0x40) ? "POWERED DOWN" : "powered", (pll & 0x10) ? "locked" : "NOT locked", vic >> 2,
               hdmi_delay * 400 - 1200, adv_get(0x15), adv_get(0x16), adv_get(0x17), adv_get(0x48),
               adv_get(0xAF), adv_get(0xBA), adv_get(0xD6));
    xil_printf("           CSC 18=%02x 1A=%02x, setup re-applied %u times (auto %s), HPD changes %u, "
               "I2C read errors %u\r\n", adv_get(0x18), adv_get(0x1A), hdmi_reapply, hdmi_auto ? "on" : "off",
               hdmi_hpd_changes, hdmi_i2c_err);
}

// ---- frame buffer DMAs ------------------------------------------------------------
static void stream_rgb(XVidC_VideoStream *s, int w, int h)
{
    memset(s, 0, sizeof(*s));
    s->ColorFormatId  = XVIDC_CSF_RGB;
    s->ColorDepth     = XVIDC_BPC_8;
    s->PixPerClk      = XVIDC_PPC_1;
    s->FrameRate      = XVIDC_FR_60HZ;
    s->VmId           = XVIDC_VM_CUSTOM;
    s->Timing.HActive = w;
    s->Timing.VActive = h;
}

// display read DMA
int zb_fbr_config(UINTPTR addr, int w, int h, int stride)
{
    XVidC_VideoStream s;
    if (fbr_ready) {
        if (!hls_graceful_stop(XPAR_VFB_RD_BASEADDR, "vfb_rd")) { XVFrmbufRd_Stop(&fbr); usleep(50000); }
        dma_reset_pulse(EMIO_RST_RD);
    }
    if (XVFrmbufRd_Initialize(&fbr, XPAR_VFB_RD_BASEADDR) != XST_SUCCESS) return XST_FAILURE;
    fbr_ready = 1;
    stream_rgb(&s, w, h);
    if (XVFrmbufRd_SetMemFormat(&fbr, stride, XVIDC_CSF_MEM_BGRX8, &s) != XST_SUCCESS) return XST_FAILURE;
    if (XVFrmbufRd_SetBufferAddr(&fbr, addr) != XST_SUCCESS) return XST_FAILURE;
    XVFrmbufRd_Start(&fbr);
    XV_frmbufrd_InterruptEnable(&fbr.FrmbufRd, FB_DONE_MASK);   // latch ap_done in ISR for polling
    XV_frmbufrd_InterruptClear(&fbr.FrmbufRd, FB_DONE_MASK);
    return XST_SUCCESS;
}
int  zb_fbr_init(UINTPTR addr)     { return zb_fbr_config(addr, DISP_W, DISP_H, DISP_STRIDE); }
void zb_fbr_stop(void)
{
    if (fbr_ready && !hls_graceful_stop(XPAR_VFB_RD_BASEADDR, "vfb_rd")) { XVFrmbufRd_Stop(&fbr); usleep(50000); }
}
void zb_fbr_set_addr(UINTPTR addr) { XVFrmbufRd_SetBufferAddr(&fbr, addr); }
int  zb_fbr_done(void)
{
    if (!(XV_frmbufrd_InterruptGetStatus(&fbr.FrmbufRd) & FB_DONE_MASK)) return 0;
    XV_frmbufrd_InterruptClear(&fbr.FrmbufRd, FB_DONE_MASK);
    return 1;
}

// camera / test write DMA
static int fbw_setup(UINTPTR addr, int w, int h, int stride)
{
    XVidC_VideoStream s;
    if (fbw_ready) {
        u32 w_before = Xil_In32(XPAR_VFB_WR_BASEADDR + 0x10);   // HwReg_width
        if (!hls_graceful_stop(XPAR_VFB_WR_BASEADDR, "vfb_wr")) { XVFrmbufWr_Stop(&fbw); usleep(50000); }
        dma_reset_pulse(EMIO_RST_WR);
        // Diagnostic: HLS registers reset to 0, so a non-zero width here means the reset
        // pulse did not reach the core and its internal state survives the restart.
        u32 c_after = Xil_In32(XPAR_VFB_WR_BASEADDR + 0x00), w_after = Xil_In32(XPAR_VFB_WR_BASEADDR + 0x10);
        if (zb_diag) xil_printf("  [vfb_wr restart] width before %u, after reset pulse: width %u ctrl 0x%02x%s\r\n",
                                w_before, w_after, c_after & 0xFF, w_after ? "  <- NOT RESET" : "");
    }
    if (XVFrmbufWr_Initialize(&fbw, XPAR_VFB_WR_BASEADDR) != XST_SUCCESS) return XST_FAILURE;
    fbw_ready = 1;
    stream_rgb(&s, w, h);
    if (XVFrmbufWr_SetMemFormat(&fbw, stride, XVIDC_CSF_MEM_BGRX8, &s) != XST_SUCCESS) return XST_FAILURE;
    if (XVFrmbufWr_SetBufferAddr(&fbw, addr) != XST_SUCCESS) return XST_FAILURE;
    return XST_SUCCESS;
}
int zb_fbw_config(UINTPTR addr, int w, int h, int stride)
{
    if (fbw_setup(addr, w, h, stride) != XST_SUCCESS) return XST_FAILURE;
    XVFrmbufWr_Start(&fbw);
    XV_frmbufwr_InterruptEnable(&fbw.FrmbufWr, FB_DONE_MASK);
    XV_frmbufwr_InterruptClear(&fbw.FrmbufWr, FB_DONE_MASK);
    return XST_SUCCESS;
}
int zb_fbw_single(UINTPTR addr, int w, int h, int stride)
{
    if (fbw_setup(addr, w, h, stride) != XST_SUCCESS) return XST_FAILURE;
    XV_frmbufwr_InterruptEnable(&fbw.FrmbufWr, FB_DONE_MASK);
    XV_frmbufwr_InterruptClear(&fbw.FrmbufWr, FB_DONE_MASK);
    XV_frmbufwr_DisableAutoRestart(&fbw.FrmbufWr);
    XV_frmbufwr_Start(&fbw.FrmbufWr);
    return XST_SUCCESS;
}
int  zb_fbw_init(UINTPTR addr)     { return zb_fbw_config(addr, CAM_W, CAM_H, DISP_STRIDE); }
void zb_fbw_stop(void)
{
    if (fbw_ready && !hls_graceful_stop(XPAR_VFB_WR_BASEADDR, "vfb_wr")) { XVFrmbufWr_Stop(&fbw); usleep(50000); }
}
void zb_fbw_set_addr(UINTPTR addr) { XVFrmbufWr_SetBufferAddr(&fbw, addr); }
int  zb_fbw_done(void)
{
    if (!(XV_frmbufwr_InterruptGetStatus(&fbw.FrmbufWr) & FB_DONE_MASK)) return 0;
    XV_frmbufwr_InterruptClear(&fbw.FrmbufWr, FB_DONE_MASK);
    return 1;
}

// memory-test read DMA
#ifdef XPAR_VFB_RD2_BASEADDR
int zb_fbr2_available(void) { return 1; }
int zb_fbr2_single(UINTPTR addr, int w, int h, int stride)
{
    XVidC_VideoStream s;
    if (fbr2_ready) dma_reset_pulse(EMIO_RST_RD2);   // single-shot: idle after each test
    if (XVFrmbufRd_Initialize(&fbr2, XPAR_VFB_RD2_BASEADDR) != XST_SUCCESS) return XST_FAILURE;
    fbr2_ready = 1;
    stream_rgb(&s, w, h);
    if (XVFrmbufRd_SetMemFormat(&fbr2, stride, XVIDC_CSF_MEM_BGRX8, &s) != XST_SUCCESS) return XST_FAILURE;
    if (XVFrmbufRd_SetBufferAddr(&fbr2, addr) != XST_SUCCESS) return XST_FAILURE;
    XV_frmbufrd_InterruptEnable(&fbr2.FrmbufRd, FB_DONE_MASK);
    XV_frmbufrd_InterruptClear(&fbr2.FrmbufRd, FB_DONE_MASK);
    XV_frmbufrd_DisableAutoRestart(&fbr2.FrmbufRd);
    XV_frmbufrd_Start(&fbr2.FrmbufRd);
    return XST_SUCCESS;
}
int zb_fbr2_done(void)
{
    if (!(XV_frmbufrd_InterruptGetStatus(&fbr2.FrmbufRd) & FB_DONE_MASK)) return 0;
    XV_frmbufrd_InterruptClear(&fbr2.FrmbufRd, FB_DONE_MASK);
    return 1;
}
#else
int zb_fbr2_available(void) { return 0; }
int zb_fbr2_single(UINTPTR addr, int w, int h, int stride) { (void)addr; (void)w; (void)h; (void)stride; return XST_FAILURE; }
int zb_fbr2_done(void) { return 0; }
#endif

// ---- debug -------------------------------------------------------------------------------
// HLS control register (offset 0x00): bit0 ap_start, 1 ap_done, 2 ap_idle, 3 ap_ready,
// 7 auto_restart. VTC: 0x00 control, 0x04 status, 0x60 generator active size (V<<16 | H).
static void dump_hls(const char *name, UINTPTR b)
{
    u32 c = Xil_In32(b);
    xil_printf("  %-8s ctrl 0x%02x [start %u done %u idle %u ready %u autorestart %u] "
               "width %u height %u stride %u\r\n", name, c & 0xFF, c & 1, (c >> 1) & 1, (c >> 2) & 1,
               (c >> 3) & 1, (c >> 7) & 1, Xil_In32(b + 0x10), Xil_In32(b + 0x18), Xil_In32(b + 0x20));
}
void zb_debug_dump(void)
{
    xil_printf("Debug:\r\n");
    dump_hls("vfb_rd", XPAR_VFB_RD_BASEADDR);
    dump_hls("vfb_wr", XPAR_VFB_WR_BASEADDR);
#ifdef XPAR_VFB_RD2_BASEADDR
    dump_hls("vfb_rd2", XPAR_VFB_RD2_BASEADDR);
#endif
    u32 ga = Xil_In32(XPAR_VTC_BASEADDR + 0x60);
    xil_printf("  vtc      control 0x%08x status 0x%08x active %ux%u\r\n",
               Xil_In32(XPAR_VTC_BASEADDR + 0x00), Xil_In32(XPAR_VTC_BASEADDR + 0x04), ga & 0x1FFF, (ga >> 16) & 0x1FFF);
#ifdef XPAR_CLK_WIZ_PIX_BASEADDR
    xil_printf("  clk_wiz  locked %u cfg0 0x%08x cfg2 0x%08x\r\n", Xil_In32(XPAR_CLK_WIZ_PIX_BASEADDR + 0x04) & 1,
               Xil_In32(XPAR_CLK_WIZ_PIX_BASEADDR + 0x200), Xil_In32(XPAR_CLK_WIZ_PIX_BASEADDR + 0x208));
#else
    xil_printf("  clk_wiz  no dynamic reconfiguration in this hardware (fixed 65 MHz)\r\n");
#endif
    zb_hdmi_status();
}

// ---- time base: Cortex-A9 global timer ----------------------------------------------
#define GT_BASE     0xF8F00200u
#define GT_CNT_LO   (GT_BASE + 0x00)
#define GT_CNT_HI   (GT_BASE + 0x04)
#define GT_CTRL     (GT_BASE + 0x08)

#if defined(XPAR_CPU_CORE_CLOCK_FREQ_HZ)
#define ZB_CPU_HZ XPAR_CPU_CORE_CLOCK_FREQ_HZ
#elif defined(XPAR_PS7_CORTEXA9_0_CPU_CLK_FREQ_HZ)
#define ZB_CPU_HZ XPAR_PS7_CORTEXA9_0_CPU_CLK_FREQ_HZ
#else
#define ZB_CPU_HZ 666666687u            /* ZedBoard PS7 preset */
#endif

u64 zb_time(void)
{
    if (!(Xil_In32(GT_CTRL) & 1)) Xil_Out32(GT_CTRL, 1);    // enable if the BSP has not
    u32 hi, lo;
    do {                                                      // consistent 64-bit read
        hi = Xil_In32(GT_CNT_HI);
        lo = Xil_In32(GT_CNT_LO);
    } while (hi != Xil_In32(GT_CNT_HI));
    return ((u64)hi << 32) | lo;
}

u32 zb_time_hz(void)
{
    u32 prescale = (Xil_In32(GT_CTRL) >> 8) & 0xFF;
    return (u32)(ZB_CPU_HZ / 2u / (prescale + 1u));
}
