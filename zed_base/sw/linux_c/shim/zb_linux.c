/* zb_linux.c - Linux userspace back end for the Xilinx standalone BSP shim.
 *
 * Lets the bare-metal zed_base code (zb_platform.c, jig.c) and the unmodified Xilinx
 * drivers (vtc, v_frmbuf_rd/wr, video_common) run as a Linux process:
 *   Xil_In32/Out32      physical address -> UIO mapping of the PL IP (found in /sys/class/uio)
 *   frame buffer addr   pointer into the u-dma-buf mapping -> physical address
 *   Xil_DCache*Range    u-dma-buf sync_for_device / sync_for_cpu (buffer mapped cached)
 *   A9 global timer     emulated from CLOCK_MONOTONIC (zb_time)
 *   XGpio / XGpioPs     libgpiod v1 (kernel gpio-xilinx and zynq_gpio drivers)
 *   XIicPs              /dev/i2c-N
 *   UART input          stdin in raw mode
 * Needs: the UIO nodes of system-user.dtsi, u-dma-buf (/dev/udmabuf0), libgpiod-dev.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <gpiod.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "xil_types.h"
#include "xstatus.h"
#include "xil_io.h"
#include "xil_cache.h"
#include "xgpio.h"
#include "xgpiops.h"
#include "xiicps.h"
#include "xuartps_hw.h"
#include "xparameters.h"
#include "zb_linux.h"

static void die(const char *fmt, const char *a)
{
    fprintf(stderr, "zb_linux: ");
    fprintf(stderr, fmt, a);
    fprintf(stderr, "\n");
    exit(1);
}

/* ---- UIO register windows -------------------------------------------------------- */
#define MAX_REG 16
static struct { u32 phys, size; volatile u32 *virt; char name[32]; } reg[MAX_REG];
static int nreg;

static u32 rd_hex(const char *path)
{
    FILE *f = fopen(path, "r");
    unsigned long v = 0;
    if (!f || fscanf(f, "%lx", &v) != 1) die("cannot read %s", path);
    fclose(f);
    return (u32)v;
}

static void map_uio(void)
{
    DIR *d = opendir("/sys/class/uio");
    struct dirent *e;
    char p[256], dev[64];
    if (!d) die("%s", "no /sys/class/uio (UIO nodes missing in the device tree?)");
    while ((e = readdir(d)) && nreg < MAX_REG) {
        if (strncmp(e->d_name, "uio", 3)) continue;
        snprintf(p, sizeof p, "/sys/class/uio/%s/maps/map0/addr", e->d_name);
        u32 phys = rd_hex(p);
        snprintf(p, sizeof p, "/sys/class/uio/%s/maps/map0/size", e->d_name);
        u32 size = rd_hex(p);
        snprintf(dev, sizeof dev, "/dev/%s", e->d_name);
        int fd = open(dev, O_RDWR | O_SYNC);
        if (fd < 0) die("cannot open %s (permissions: group fpga?)", dev);
        void *v = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (v == MAP_FAILED) die("mmap %s failed", dev);
        reg[nreg].phys = phys; reg[nreg].size = size; reg[nreg].virt = v;
        snprintf(reg[nreg].name, sizeof reg[nreg].name, "%s", e->d_name);
        nreg++;
    }
    closedir(d);
}

/* ---- u-dma-buf --------------------------------------------------------------------- */
#define UDMA "/sys/class/u-dma-buf/udmabuf0"
static u8 *dma_virt;
static u32 dma_phys, dma_size;

static void sysfs_write(const char *attr, unsigned long v)
{
    char p[128];
    snprintf(p, sizeof p, UDMA "/%s", attr);
    FILE *f = fopen(p, "w");
    if (!f) die("cannot write %s", p);
    fprintf(f, "%lu", v);
    fclose(f);
}

static void map_udmabuf(void)
{
    dma_phys = rd_hex(UDMA "/phys_addr");
    FILE *f = fopen(UDMA "/size", "r");
    unsigned long s = 0;
    if (!f || fscanf(f, "%lu", &s) != 1) die("%s", "cannot read u-dma-buf size (module loaded?)");
    fclose(f);
    dma_size = (u32)s;
    int fd = open("/dev/udmabuf0", O_RDWR);          /* no O_SYNC: cached, synced explicitly */
    if (fd < 0) die("%s", "cannot open /dev/udmabuf0 (permissions: group fpga?)");
    void *v = mmap(NULL, dma_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (v == MAP_FAILED) die("%s", "mmap /dev/udmabuf0 failed");
    dma_virt = v;
}

UINTPTR zb_dma_base(void) { return (UINTPTR)dma_virt; }
u32     zb_dma_size(void) { return dma_size; }
u32     zb_dma_phys(void) { return dma_phys; }

static int in_dma(UINTPTR a) { return a >= (UINTPTR)dma_virt && a < (UINTPTR)dma_virt + dma_size; }

static void dma_sync(UINTPTR a, u32 len, int to_device)
{
    if (!len) return;
    if (!in_dma(a)) return;                          /* ordinary memory: nothing to do */
    u32 off = (u32)(a - (UINTPTR)dma_virt);
    if (off + len > dma_size) len = dma_size - off;
    sysfs_write("sync_offset", off);
    sysfs_write("sync_size", len);
    sysfs_write("sync_direction", to_device ? 1 : 2);   /* DMA_TO_DEVICE / DMA_FROM_DEVICE */
    sysfs_write(to_device ? "sync_for_device" : "sync_for_cpu", 1);
}
void Xil_DCacheFlushRange(UINTPTR a, u32 len)      { dma_sync(a, len, 1); }
void Xil_DCacheInvalidateRange(UINTPTR a, u32 len) { dma_sync(a, len, 0); }

/* ---- register access ----------------------------------------------------------------- */
#define GT_BASE 0xF8F00200U
static u64 gt_snap;

static u64 gt_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    /* A9 global timer: CPU clock / 2, prescaler 0 */
    return ((u64)t.tv_sec * 1000000000ULL + (u64)t.tv_nsec) * (XPAR_CPU_CORE_CLOCK_FREQ_HZ / 2) / 1000000000ULL;
}

static volatile u32 *lookup(UINTPTR addr, int *idx)
{
    for (int i = 0; i < nreg; i++)
        if (addr >= reg[i].phys && addr < (UINTPTR)reg[i].phys + reg[i].size) {
            if (idx) *idx = i;
            return reg[i].virt + ((addr - reg[i].phys) >> 2);
        }
    fprintf(stderr, "zb_linux: access to unmapped address 0x%08lx (no UIO node)\n", (unsigned long)addr);
    exit(1);
}

u32 Xil_In32(UINTPTR addr)
{
    if (addr >= GT_BASE && addr < GT_BASE + 0x20) {   /* emulated global timer */
        switch (addr - GT_BASE) {
        case 0x00: return (u32)gt_snap;
        case 0x04: gt_snap = gt_now(); return (u32)(gt_snap >> 32);
        case 0x08: return 1;                          /* enabled, prescaler 0 */
        default:   return 0;
        }
    }
    return *lookup(addr, NULL);
}

static int is_frmbuf(UINTPTR base)
{
    return base == XPAR_VFB_RD_BASEADDR || base == XPAR_VFB_WR_BASEADDR || base == XPAR_VFB_RD2_BASEADDR;
}

void Xil_Out32(UINTPTR addr, u32 val)
{
    if (addr >= GT_BASE && addr < GT_BASE + 0x20) return;
    /* Frame buffer DMA address: the code passes CPU pointers into the u-dma-buf mapping
     * (identity-mapped in bare metal); the IP needs the physical address. */
    if (is_frmbuf(addr & ~0xFFFFU) && in_dma((UINTPTR)val))
        val = dma_phys + (u32)((UINTPTR)val - (UINTPTR)dma_virt);
    *lookup(addr, NULL) = val;
}

/* ---- terminal (UART menu) -------------------------------------------------------------- */
static struct termios tio_saved;
static int tio_ok;
static void tio_restore(void) { if (tio_ok) tcsetattr(0, TCSANOW, &tio_saved); }
static void on_signal(int s) { (void)s; tio_restore(); _exit(0); }

int zb_stdin_ready(void)
{
    struct pollfd p = { .fd = 0, .events = POLLIN };
    return poll(&p, 1, 0) > 0;
}
int zb_stdin_getc(void)
{
    unsigned char c;
    return read(0, &c, 1) == 1 ? c : -1;
}

__attribute__((constructor)) static void zb_linux_init(void)
{
    if (isatty(0) && tcgetattr(0, &tio_saved) == 0) {
        struct termios t = tio_saved;
        t.c_lflag &= ~(ICANON | ECHO);               /* single key presses, Ctrl-C still works */
        t.c_cc[VMIN] = 1; t.c_cc[VTIME] = 0;
        tcsetattr(0, TCSANOW, &t);
        tio_ok = 1;
        atexit(tio_restore);
        signal(SIGINT, on_signal);
        signal(SIGTERM, on_signal);
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    map_uio();
    map_udmabuf();
    fprintf(stderr, "zb_linux: %d UIO regions, u-dma-buf %u MB at phys 0x%08x\n", nreg, dma_size >> 20, dma_phys);
}

/* ---- GPIO (libgpiod v1) ---------------------------------------------------------------- */
static struct gpiod_chip *chip_by_label(const char *label)
{
    struct gpiod_chip *c = gpiod_chip_open_by_label(label);
    if (!c) die("GPIO chip '%s' not found", label);
    return c;
}

static void line_dir(struct gpiod_line *l, int out, const char *who)
{
    if (gpiod_line_is_requested(l)) gpiod_line_release(l);
    int r = out ? gpiod_line_request_output(l, who, 0) : gpiod_line_request_input(l, who);
    if (r < 0) fprintf(stderr, "zb_linux: GPIO request failed: %s\n", strerror(errno));
}

int XGpio_Initialize(XGpio *g, UINTPTR base)
{
    char label[32];
    memset(g, 0, sizeof *g);
    g->BaseAddress = base;
    snprintf(label, sizeof label, "%08lx.gpio", (unsigned long)base);
    struct gpiod_chip *c = chip_by_label(label);
    g->chip = c;
    g->nlines = (int)gpiod_chip_num_lines(c);
    g->w1 = (g->nlines == 16) ? 8 : g->nlines;       /* zed_base: LEDs 8 + switches 8 */
    for (int i = 0; i < g->nlines && i < 32; i++) {
        g->lines[i] = gpiod_chip_get_line(c, (unsigned)i);
        line_dir(g->lines[i], 0, "zb_axi_gpio");
    }
    return XST_SUCCESS;
}
static void ch_range(XGpio *g, unsigned ch, int *lo, int *n)
{
    *lo = (ch == 2) ? g->w1 : 0;
    *n  = (ch == 2) ? g->nlines - g->w1 : g->w1;
}
void XGpio_SetDataDirection(XGpio *g, unsigned ch, u32 in_mask)
{
    int lo, n;
    ch_range(g, ch, &lo, &n);
    for (int b = 0; b < n; b++) line_dir(g->lines[lo + b], !((in_mask >> b) & 1), "zb_axi_gpio");
}
u32 XGpio_DiscreteRead(XGpio *g, unsigned ch)
{
    int lo, n;
    u32 v = 0;
    ch_range(g, ch, &lo, &n);
    for (int b = 0; b < n; b++) if (gpiod_line_get_value(g->lines[lo + b]) > 0) v |= 1U << b;
    return v;
}
void XGpio_DiscreteWrite(XGpio *g, unsigned ch, u32 v)
{
    int lo, n;
    ch_range(g, ch, &lo, &n);
    for (int b = 0; b < n; b++)
        if (gpiod_line_direction(g->lines[lo + b]) == GPIOD_LINE_DIRECTION_OUTPUT)
            gpiod_line_set_value(g->lines[lo + b], (v >> b) & 1);
}

static XGpioPs_Config gpiops_cfg = { XPAR_XGPIOPS_0_BASEADDR };
XGpioPs_Config *XGpioPs_LookupConfig(UINTPTR base) { (void)base; return &gpiops_cfg; }
int XGpioPs_CfgInitialize(XGpioPs *g, XGpioPs_Config *c, UINTPTR base)
{
    memset(g, 0, sizeof *g);
    g->Config = *c; (void)base;
    g->chip = chip_by_label("zynq_gpio");
    return XST_SUCCESS;
}
static struct gpiod_line *ps_line(XGpioPs *g, u32 pin)
{
    if (pin >= 118) die("%s", "PS GPIO pin out of range");
    if (!g->lines[pin]) g->lines[pin] = gpiod_chip_get_line(g->chip, pin);
    return g->lines[pin];
}
void XGpioPs_SetDirectionPin(XGpioPs *g, u32 pin, u32 out)
{
    struct gpiod_line *l = ps_line(g, pin);
    if (g->dir[pin] == (out ? 2 : 1)) return;
    line_dir(l, out, "zb_emio");
    g->dir[pin] = out ? 2 : 1;
}
void XGpioPs_SetOutputEnablePin(XGpioPs *g, u32 pin, u32 en) { (void)g; (void)pin; (void)en; }
void XGpioPs_WritePin(XGpioPs *g, u32 pin, u32 v) { gpiod_line_set_value(ps_line(g, pin), v ? 1 : 0); }
u32  XGpioPs_ReadPin(XGpioPs *g, u32 pin)         { return gpiod_line_get_value(ps_line(g, pin)) > 0; }

/* ---- I2C (i2c-dev) --------------------------------------------------------------------- */
static XIicPs_Config iic_cfg[2] = { { XPAR_XIICPS_0_BASEADDR }, { XPAR_XIICPS_1_BASEADDR } };
XIicPs_Config *XIicPs_LookupConfig(UINTPTR base)
{
    for (int i = 0; i < 2; i++) if (iic_cfg[i].BaseAddress == base) return &iic_cfg[i];
    return NULL;
}
static int i2c_bus_for(UINTPTR base)
{
    char p[128], t[256], key[16];
    snprintf(key, sizeof key, "%08lx.i2c", (unsigned long)base);
    for (int n = 0; n < 16; n++) {
        snprintf(p, sizeof p, "/sys/bus/i2c/devices/i2c-%d", n);
        ssize_t k = readlink(p, t, sizeof t - 1);
        if (k <= 0) continue;
        t[k] = 0;
        if (strstr(t, key)) return n;
    }
    return -1;
}
int XIicPs_CfgInitialize(XIicPs *i, XIicPs_Config *c, UINTPTR base)
{
    char dev[32];
    memset(i, 0, sizeof *i);
    i->Config = *c; (void)base;
    int n = i2c_bus_for(c->BaseAddress);
    if (n < 0) return XST_DEVICE_NOT_FOUND;
    snprintf(dev, sizeof dev, "/dev/i2c-%d", n);
    i->fd = open(dev, O_RDWR);
    return i->fd < 0 ? XST_DEVICE_NOT_FOUND : XST_SUCCESS;
}
int XIicPs_SetSClk(XIicPs *i, u32 hz) { (void)i; (void)hz; return XST_SUCCESS; }   /* set by the kernel (DT) */
int XIicPs_SetOptions(XIicPs *i, u32 o)   { i->opts |= o;  return XST_SUCCESS; }
int XIicPs_ClearOptions(XIicPs *i, u32 o) { i->opts &= ~o; return XST_SUCCESS; }
u32 XIicPs_GetOptions(XIicPs *i)          { return i->opts; }

static int i2c_xfer(XIicPs *i, struct i2c_msg *m, int n)
{
    struct i2c_rdwr_ioctl_data d = { .msgs = m, .nmsgs = (u32)n };
    return ioctl(i->fd, I2C_RDWR, &d) == n ? XST_SUCCESS : XST_FAILURE;
}
int XIicPs_MasterSendPolled(XIicPs *i, u8 *buf, s32 n, u16 addr)
{
    if ((i->opts & XIICPS_REP_START_OPTION) && n <= (s32)sizeof i->hbuf) {
        memcpy(i->hbuf, buf, (size_t)n);                 /* issued with the next receive */
        i->hlen = n; i->haddr = addr; i->held = 1;
        return XST_SUCCESS;
    }
    struct i2c_msg m = { .addr = addr, .flags = 0, .len = (u16)n, .buf = buf };
    return i2c_xfer(i, &m, 1);
}
int XIicPs_MasterRecvPolled(XIicPs *i, u8 *buf, s32 n, u16 addr)
{
    if (i->held) {
        i->held = 0;
        struct i2c_msg m[2] = { { .addr = i->haddr, .flags = 0, .len = (u16)i->hlen, .buf = i->hbuf },
                                { .addr = addr, .flags = I2C_M_RD, .len = (u16)n, .buf = buf } };
        return i2c_xfer(i, m, 2);
    }
    struct i2c_msg m = { .addr = addr, .flags = I2C_M_RD, .len = (u16)n, .buf = buf };
    return i2c_xfer(i, &m, 1);
}
