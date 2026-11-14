"""hw.py - Linux userspace port of zb_platform.c (zed_base, ZedBoard).

Runs on the PetaLinux 2024.2 kernel + Ubuntu 22.04 root filesystem.
Hardware access without kernel drivers for the video IP:
  UIO        PL IP registers (vfb_rd, vfb_rd2, vfb_wr, vtc, clk_wiz_pix, proc_slot, accel)
  u-dma-buf  physically contiguous frame buffers (/dev/udmabuf0)
  libgpiod   frame buffer resets on EMIO GPIO 54/55/56 (zynq_gpio)
  i2c-dev    OV2640 SCCB on /dev/i2c-0 (PS I2C0), ADV7511 on /dev/i2c-1 (PS I2C1)

Register sequences follow zb_platform.c (bare metal, verified on the board), including the
graceful-stop rule for the frame buffer cores (see Get_Started.md, camera seam bug).
All pynq-like functionality is kept in this one file so the rest of the code stays portable.
"""
import glob
import mmap
import os
import re
import time

import numpy as np

# ---------------------------------------------------------------------------------------
# Low-level access
# ---------------------------------------------------------------------------------------
class Regs:
    """32-bit register window mapped from a UIO device."""

    def __init__(self, dev, size, name=""):
        fd = os.open(dev, os.O_RDWR | os.O_SYNC)
        self.mm = mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=0)
        os.close(fd)
        self.w = np.frombuffer(self.mm, dtype=np.uint32)
        self.dev, self.name = dev, name

    def rd(self, off):
        return int(self.w[off >> 2])

    def wr(self, off, val):
        self.w[off >> 2] = val & 0xFFFFFFFF


def uio_find(name, addr):
    """Open the UIO device with this device-tree name and base address.
    uioN numbers change with the design, and two IPs share the name v_frmbuf_rd."""
    for d in sorted(glob.glob("/sys/class/uio/uio*")):
        n = open(d + "/name").read().strip()
        a = int(open(d + "/maps/map0/addr").read(), 16)
        if n == name and a == addr:
            size = int(open(d + "/maps/map0/size").read(), 16)
            return Regs("/dev/" + os.path.basename(d), size, name)
    raise RuntimeError(f"UIO device {name}@{addr:#x} not found (check system-user.dtsi)")


class DmaBuf:
    """u-dma-buf region.
    cached=False (default): opened with O_SYNC, uncached; no cache maintenance needed, slow CPU access.
    cached=True: normal cached mapping (fast CPU access, as bare metal); call flush() after the CPU
    writes and invalidate() before the CPU reads DMA data (= Xil_DCacheFlushRange / InvalidateRange)."""

    def __init__(self, name="udmabuf0", cached=False):
        self.sysd = f"/sys/class/u-dma-buf/{name}"
        self.phys = int(open(self.sysd + "/phys_addr").read(), 16)
        self.size = int(open(self.sysd + "/size").read())
        self.cached = cached
        fd = os.open("/dev/" + name, os.O_RDWR if cached else (os.O_RDWR | os.O_SYNC))
        self.mm = mmap.mmap(fd, self.size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
        os.close(fd)
        self.base = np.frombuffer(self.mm, dtype=np.uint8)
        self.addr = self.base.ctypes.data           # CPU address of the mapping (for ctypes)

    def _sync(self, off, n, to_device):
        if not self.cached or n <= 0:
            return
        n = min(n, self.size - off)
        for attr, v in (("sync_offset", off), ("sync_size", n), ("sync_direction", 1 if to_device else 2),
                        ("sync_for_device" if to_device else "sync_for_cpu", 1)):
            with open(f"{self.sysd}/{attr}", "w") as f:
                f.write(str(v))

    def flush(self, off, n):
        self._sync(off, n, True)

    def invalidate(self, off, n):
        self._sync(off, n, False)

    def u32(self, offset, w, h, stride):
        """XRGB8888 view (h x w, uint32 0x00RRGGBB) of a frame at byte offset."""
        a = np.frombuffer(self.mm, dtype=np.uint32, count=h * stride // 4, offset=offset)
        return a.reshape(h, stride // 4)[:, :w]

    def bgrx(self, offset, w, h, stride):
        """Same frame as uint8 (h x w x 4), byte order B,G,R,X: [..., :3] is OpenCV BGR."""
        a = np.frombuffer(self.mm, dtype=np.uint8, count=h * stride, offset=offset)
        return a.reshape(h, stride)[:, : w * 4].reshape(h, w, 4)


class EmioResets:
    """Frame buffer reset lines on zynq_gpio (EMIO 54 = vfb_rd, 55 = vfb_wr, 56 = vfb_rd2).
    libgpiod v1 API (Ubuntu 22.04: python3-libgpiod 1.6). Lines stay held while this object lives."""

    PINS = {"rd": 54, "wr": 55, "rd2": 56}

    def __init__(self, label="zynq_gpio"):
        import gpiod
        chip = None
        for c in gpiod.ChipIter():
            if c.label() == label:
                chip = c
                break
        if chip is None:
            raise RuntimeError(f"GPIO chip '{label}' not found")
        self.chip = chip
        self.lines = {}
        for k, pin in self.PINS.items():
            ln = chip.get_line(pin)
            ln.request(consumer="zb_" + k, type=gpiod.LINE_REQ_DIR_OUT, default_val=0)
            self.lines[k] = ln

    def release_all(self):
        """zb_dma_reset_release(): hold all in reset 1 ms, then release."""
        for ln in self.lines.values():
            ln.set_value(0)
        time.sleep(0.001)
        for ln in self.lines.values():
            ln.set_value(1)

    def pulse(self, k):
        """dma_reset_pulse(): restart one stopped core from reset, as the Linux frmbuf driver does."""
        self.lines[k].set_value(0)
        time.sleep(10e-6)
        self.lines[k].set_value(1)
        time.sleep(10e-6)


class BoardIO:
    """LEDs (AXI GPIO 0x41200000 ch1 = lines 0-7), switches (ch2 = lines 8-15),
    buttons (0x41210000, 5 lines). As zb_gpio_init / zb_leds / zb_switches / zb_buttons."""

    def __init__(self):
        import gpiod
        c1 = gpiod.Chip("41200000.gpio", gpiod.Chip.OPEN_BY_LABEL)
        c2 = gpiod.Chip("41210000.gpio", gpiod.Chip.OPEN_BY_LABEL)
        self.leds = c1.get_lines(list(range(0, 8)))
        self.leds.request(consumer="zb_leds", type=gpiod.LINE_REQ_DIR_OUT, default_vals=[0] * 8)
        self.sw = c1.get_lines(list(range(8, 16)))
        self.sw.request(consumer="zb_sw", type=gpiod.LINE_REQ_DIR_IN)
        self.btn = c2.get_lines(list(range(c2.num_lines())))
        self.btn.request(consumer="zb_btn", type=gpiod.LINE_REQ_DIR_IN)
        self._chips = (c1, c2)

    @staticmethod
    def _pack(vals):
        return sum((v & 1) << i for i, v in enumerate(vals))

    def switches(self):
        return self._pack(self.sw.get_values())

    def buttons(self):
        return self._pack(self.btn.get_values())

    def set_leds(self, v):
        self.leds.set_values([(v >> i) & 1 for i in range(8)])


# ---------------------------------------------------------------------------------------
# Frame buffer read/write cores (Xilinx v_frmbuf_rd / v_frmbuf_wr, HLS control interface)
# ---------------------------------------------------------------------------------------
CTRL, GIE, IER, ISR = 0x00, 0x04, 0x08, 0x0C
WIDTH, HEIGHT, STRIDE, VFMT, ADDR, ADDR_HI = 0x10, 0x18, 0x20, 0x28, 0x30, 0x34
AP_START, AP_IDLE, AUTO_RESTART = 0x01, 0x04, 0x80
# Video format register code written by XVFrmbufRd_SetMemFormat(XVIDC_CSF_MEM_BGRX8)
# (verified against the 2024.2 driver: 0x1b; memory bytes B,G,R,X = 0x00RRGGBB)
FMT_BGRX8 = 27


class FrameBuf:
    def __init__(self, regs, resets, pin, name):
        self.r, self.resets, self.pin, self.name = regs, resets, pin, name
        self.started = False

    def wait_idle(self, timeout_s):
        t0 = time.monotonic()
        while time.monotonic() - t0 < timeout_s:
            if self.r.rd(CTRL) & AP_IDLE:
                return True
            time.sleep(100e-6)
        return False

    def graceful_stop(self):
        """Auto-restart off, no new start, let the current frame finish (no flush/abort)."""
        self.r.wr(CTRL, self.r.rd(CTRL) & 0xFF & ~AUTO_RESTART & ~AP_START)
        if self.wait_idle(0.25):
            time.sleep(0.005)
            return True
        print(f"  [{self.name}] did not go idle within 250 ms; resetting after 50 ms")
        time.sleep(0.05)
        return False

    def stop(self):
        if self.started:
            self.graceful_stop()

    def configure(self, addr, w, h, stride, continuous=True, graceful=True):
        """zb_fbr_config / zb_fbw_config (continuous) or *_single (one frame).
        graceful=False: reset pulse only (zb_fbr2_single: the core is idle after each test)."""
        if self.started:
            if graceful:
                self.graceful_stop()
            self.resets.pulse(self.pin)
        r = self.r
        r.wr(WIDTH, w)
        r.wr(HEIGHT, h)
        r.wr(STRIDE, stride)
        r.wr(VFMT, FMT_BGRX8)
        r.wr(ADDR, addr)
        r.wr(ADDR_HI, 0)
        r.wr(IER, 0x1)                      # latch ap_done in ISR for polling
        if r.rd(ISR) & 1:
            r.wr(ISR, 1)                    # toggle-on-write: clear only if set
        r.wr(CTRL, (AUTO_RESTART | AP_START) if continuous else AP_START)
        self.started = True

    def set_addr(self, addr):
        """Takes effect from a following frame."""
        self.r.wr(ADDR, addr)

    def done(self):
        """True once per completed frame."""
        if self.r.rd(ISR) & 1:
            self.r.wr(ISR, 1)
            return True
        return False

    def dump(self):
        c = self.r.rd(CTRL)
        return (f"{self.name:8s} ctrl {c & 0xFF:#04x} [start {c & 1} done {(c >> 1) & 1} idle {(c >> 2) & 1} "
                f"autorestart {(c >> 7) & 1}] {self.r.rd(WIDTH)}x{self.r.rd(HEIGHT)} stride {self.r.rd(STRIDE)} "
                f"fmt {self.r.rd(VFMT)} addr {self.r.rd(ADDR):#010x}")


# ---------------------------------------------------------------------------------------
# Display modes: pixel clock (clk_wiz dynamic reconfiguration) + VTC
# ---------------------------------------------------------------------------------------
# name, w, h, kHz, D, M, Mfrac, O, Ofrac, HFP, HSW, HBP, Hpol, VFP, VSW, VBP, Vpol  (as zb_modes[])
MODES = [
    ("640x480@60",   640,  480,  25175,  1, 9,    0, 35, 750,  16,  96,  48, 0, 10, 2, 33, 0),
    ("800x600@60",   800,  600,  40000,  1, 10,   0, 25,   0,  40, 128,  88, 1,  1, 4, 23, 1),
    ("1024x768@60",  1024, 768,  65000,  2, 13,   0, 10,   0,  24, 136, 160, 0,  3, 6, 29, 0),
    ("1280x720@60",  1280, 720,  74250,  5, 37, 125, 10,   0, 110,  40, 220, 1,  5, 5, 20, 1),
    ("1280x1024@60", 1280, 1024, 108000, 5, 54,   0, 10,   0,  48, 112, 248, 1,  1, 3, 38, 1),
    ("1920x1080@60", 1920, 1080, 148500, 5, 37, 125,  5,   0,  88,  44, 148, 1,  4, 5, 36, 1),
]


def mode_index(w, h):
    for i, m in enumerate(MODES):
        if m[1] == w and m[2] == h:
            return i
    return -1


WZ_STATUS, WZ_CFG0, WZ_CFG2, WZ_CFG3, WZ_LOAD = 0x04, 0x200, 0x208, 0x20C, 0x25C


def clkwiz_set(r, m):
    def locked():
        t0 = time.monotonic()
        while time.monotonic() - t0 < 1.0:
            if r.rd(WZ_STATUS) & 1:
                return True
            time.sleep(10e-6)
        return False

    _, _, _, _, d, m_int, m_frac, o_int, o_frac = m[:9]
    if not locked():
        return False
    r.wr(WZ_CFG2, (o_int & 0xFF) | ((o_frac & 0x3FF) << 8))
    r.wr(WZ_CFG0, (d & 0xFF) | ((m_int & 0xFF) << 8) | ((m_frac & 0x3FF) << 16))
    r.wr(WZ_CFG3, 0)
    r.wr(WZ_LOAD, 0x3)
    return locked()


VTC_CTL, VTC_GASIZE, VTC_GHSIZE, VTC_GVSIZE = 0x000, 0x060, 0x070, 0x074
CTL_GEN_ENABLE = 0x4

# Exact VTC register writes of XVtc_SetGeneratorTiming + RegUpdateEnable + EnableGenerator
# (embeddedsw xilinx_v2024.2), recorded by running the driver against a register model.
VTC_SEQ = {
    "640x480": [(0x06c, 0x00000070), (0x070, 0x00000320), (0x074, 0x020d020d), (0x060, 0x01e00280), (0x094, 0x01e00000), (0x078, 0x02f00290), (0x080, 0x01eb01e9), (0x08c, 0x01eb01e9), (0x068, 0x00000000), (0x07c, 0x02800280), (0x084, 0x02900290), (0x088, 0x02800280), (0x090, 0x02900290), (0x07c, 0x02800280), (0x084, 0x02900290), (0x088, 0x02800280), (0x090, 0x02900290), (0x000, 0x00000002), (0x000, 0x00000006)],
    "800x600": [(0x06c, 0x0000007f), (0x070, 0x00000420), (0x074, 0x02740274), (0x060, 0x02580320), (0x094, 0x02580000), (0x078, 0x03c80348), (0x080, 0x025c0258), (0x08c, 0x025c0258), (0x068, 0x00000000), (0x07c, 0x03200320), (0x084, 0x03480348), (0x088, 0x03200320), (0x090, 0x03480348), (0x07c, 0x03200320), (0x084, 0x03480348), (0x088, 0x03200320), (0x090, 0x03480348), (0x000, 0x00000002), (0x000, 0x00000006)],
    "1024x768": [(0x06c, 0x00000070), (0x070, 0x00000540), (0x074, 0x03260326), (0x060, 0x03000400), (0x094, 0x03000000), (0x078, 0x04a00418), (0x080, 0x03080302), (0x08c, 0x03080302), (0x068, 0x00000000), (0x07c, 0x04000400), (0x084, 0x04180418), (0x088, 0x04000400), (0x090, 0x04180418), (0x07c, 0x04000400), (0x084, 0x04180418), (0x088, 0x04000400), (0x090, 0x04180418), (0x000, 0x00000002), (0x000, 0x00000006)],
    "1280x720": [(0x06c, 0x0000007f), (0x070, 0x00000672), (0x074, 0x02ee02ee), (0x060, 0x02d00500), (0x094, 0x02d00000), (0x078, 0x0596056e), (0x080, 0x02d902d4), (0x08c, 0x02d902d4), (0x068, 0x00000000), (0x07c, 0x05000500), (0x084, 0x056e056e), (0x088, 0x05000500), (0x090, 0x056e056e), (0x07c, 0x05000500), (0x084, 0x056e056e), (0x088, 0x05000500), (0x090, 0x056e056e), (0x000, 0x00000002), (0x000, 0x00000006)],
    "1280x1024": [(0x06c, 0x0000007f), (0x070, 0x00000698), (0x074, 0x042a042a), (0x060, 0x04000500), (0x094, 0x04000000), (0x078, 0x05a00530), (0x080, 0x04030400), (0x08c, 0x04030400), (0x068, 0x00000000), (0x07c, 0x05000500), (0x084, 0x05300530), (0x088, 0x05000500), (0x090, 0x05300530), (0x07c, 0x05000500), (0x084, 0x05300530), (0x088, 0x05000500), (0x090, 0x05300530), (0x000, 0x00000002), (0x000, 0x00000006)],
    "1920x1080": [(0x06c, 0x0000007f), (0x070, 0x00000898), (0x074, 0x04650465), (0x060, 0x04380780), (0x094, 0x04380000), (0x078, 0x080407d8), (0x080, 0x0440043b), (0x08c, 0x0440043b), (0x068, 0x00000000), (0x07c, 0x07800780), (0x084, 0x07d807d8), (0x088, 0x07800780), (0x090, 0x07d807d8), (0x07c, 0x07800780), (0x084, 0x07d807d8), (0x088, 0x07800780), (0x090, 0x07d807d8), (0x000, 0x00000002), (0x000, 0x00000006)],
}


def vtc_set(r, m):
    """Program the VTC generator for mode m exactly as the Xilinx driver (and jig) does."""
    key = f"{m[1]}x{m[2]}"
    r.wr(VTC_CTL, 0)                        # XVtc_CfgInitialize + DisableGenerator
    for off, val in VTC_SEQ[key]:
        r.wr(off, val)


# ---------------------------------------------------------------------------------------
# I2C: OV2640 (SCCB) and ADV7511
# ---------------------------------------------------------------------------------------
def load_ov2640_table(path):
    """Flattened VGA table (older ov2640_regs.h): list of (reg, val)."""
    return parse_ov2640_header(path)["ov2640_vga_rgb565"]


def parse_ov2640_header(path):
    """All `static const unsigned char NAME[][2] = {...};` tables of ov2640_regs.h -> {NAME: [(reg, val)]}."""
    txt = re.sub(r"//[^\n]*", "", open(path).read())
    tables = {}
    for name, body in re.findall(r"static\s+const\s+unsigned\s+char\s+(\w+)\s*\[\s*\]\s*\[\s*2\s*\]\s*=\s*\{(.*?)\};",
                                 txt, re.S):
        tables[name] = [(int(a, 16), int(b, 16)) for a, b in
                        re.findall(r"\{\s*0x([0-9A-Fa-f]{1,2})\s*,\s*0x([0-9A-Fa-f]{1,2})\s*\}", body)]
    sizes = [(int(w), int(h), t, n) for w, h, t, n in
             re.findall(r"OV2640_SZ\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\w+)\s*,\s*\"([^\"]+)\"\s*\)", txt)]
    tables["__sizes__"] = sizes                  # [(w, h, table name, label)]
    return tables


class Camera:
    """OV2640 on /dev/i2c-0 (SCCB). Sequences as zb_camera_set_size() in zb_platform.c."""
    ADDR = 0x30

    def __init__(self, bus=0, header=None):
        if header is None:                     # sw/common/ov2640_regs.h of the repository
            header = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "common", "ov2640_regs.h")
        from smbus2 import SMBus
        self.bus = SMBus(bus)
        self.header = header
        self.t = None
        self.size_idx = 3

    def wr(self, reg, val):
        self.bus.write_byte_data(self.ADDR, reg, val)

    def rd(self, reg):
        # SCCB: register write and data read are separate transactions (stop in between)
        from smbus2 import i2c_msg
        self.bus.i2c_rdwr(i2c_msg.write(self.ADDR, [reg]))
        m = i2c_msg.read(self.ADDR, 1)
        self.bus.i2c_rdwr(m)
        return list(m)[0]

    def probe(self):
        try:
            self.bus.i2c_rdwr(__import__("smbus2").i2c_msg.write(self.ADDR, [0x0A]))
            return True
        except OSError:
            return False

    def write_table(self, table):
        for reg, val in table:
            self.wr(reg, val)
            time.sleep(0.001)

    @property
    def sizes(self):
        return self.t["__sizes__"] if self.t else []

    def init(self, size_idx=3):
        """zb_camera_init(): probe, ID, configure (default VGA). Returns False without a camera."""
        if not self.probe():
            print("No camera on SCCB (0x30 OV2640)")
            return False
        self.t = parse_ov2640_header(self.header)
        if not self.sizes:
            raise RuntimeError(f"{self.header}: no OV2640_SZ size table (needs the current zed_base header)")
        self.wr(0xFF, 0x01)
        try:
            print(f"Camera ID: PID={self.rd(0x0A):#04x} VER={self.rd(0x0B):#04x} (OV2640: 0x26 0x42)")
        except OSError:
            print("OV2640 ID read failed; continuing (writes are ACKed at 0x30)")
        return self.set_size(size_idx)

    def set_size(self, idx):
        if not 0 <= idx < len(self.sizes):
            return False
        w, h, tname, label = self.sizes[idx]
        self.wr(0xFF, 0x01)
        self.wr(0x12, 0x80)                       # COM7 soft reset
        time.sleep(0.01)
        for name in ("ov2640_init", "ov2640_size_preamble", tname, "ov2640_fmt_preamble", "ov2640_fmt_rgb565_be"):
            self.write_table(self.t[name])
        self.size_idx = idx
        return True

    def w(self):
        return self.sizes[self.size_idx][0]

    def h(self):
        return self.sizes[self.size_idx][1]

    def size_name(self, idx=None):
        return self.sizes[self.size_idx if idx is None else idx][3]

    def init_vga(self, table_path):
        """Older single-table path (cam_loop.py)."""
        self.wr(0xFF, 0x01)
        self.wr(0x12, 0x80)
        time.sleep(0.01)
        table = load_ov2640_table(table_path)
        self.write_table(table)
        print(f"OV2640: {len(table)} registers written (640x480 RGB565)")


ADV_CSC = [0x0734, 0x04ad, 0x0000, 0x1c1b, 0x1ddc, 0x04ad, 0x1f24, 0x0135, 0x0000, 0x04ad, 0x087c, 0x1b77]


class Hdmi:
    """ADV7511, register settings as adv_configure() in zb_platform.c."""
    ADDR = 0x39

    def __init__(self, bus=1, clock_delay=3, aspect_169=False):
        from smbus2 import SMBus
        self.bus = SMBus(bus)
        self.delay, self.aspect_169 = clock_delay, aspect_169

    def wr(self, reg, val):
        self.bus.write_byte_data(self.ADDR, reg, val & 0xFF)

    def rd(self, reg):
        return self.bus.read_byte_data(self.ADDR, reg)     # repeated start, as the Linux driver

    def upd(self, reg, mask, val):
        self.wr(reg, (self.rd(reg) & ~mask) | (val & mask))

    def init(self, quiet=False):
        rev = self.rd(0x00)
        if not quiet:
            print(f"ADV7511 found: revision {rev:#04x}, chip ID {self.rd(0xF5):02x}{self.rd(0xF6):02x}")
        self.upd(0xD6, 0xC0, 0xC0)          # HPD source: none (keeps setup without a monitor)
        self.upd(0x41, 0x40, 0x00)          # power up
        time.sleep(0.001)
        for reg, val in ((0x98, 0x03), (0x9A, 0xE0), (0x9C, 0x30), (0x9D, 0x61),
                         (0xA2, 0xA4), (0xA3, 0xA4), (0xE0, 0xD0), (0xF9, 0x00)):
            self.wr(reg, val)
        self.upd(0x15, 0x0F, 0x01)          # input ID 1: YCbCr 4:2:2, separate syncs
        self.wr(0x16, 0x38)                 # out RGB 4:4:4, 8 bit, style 1
        self.wr(0x48, 0x08)                 # right justified
        self.wr(0xD0, 0x0C)
        self.wr(0xBA, self.delay << 5)      # input clock delay
        self.upd(0x17, 0x62, 0x02 if self.aspect_169 else 0x00)
        self.wr(0x55, 0x00)
        self.upd(0x1A, 0x20, 0x20)          # CSC hold while loading
        for i, c in enumerate(ADV_CSC):
            self.upd(0x18 + 2 * i, 0x1F, c >> 8)
            self.wr(0x19 + 2 * i, c & 0xFF)
        self.upd(0x18, 0xE0, 0x80 | (2 << 5))   # CSC on, scaling x4
        self.upd(0x1A, 0x20, 0x00)          # apply
        self.upd(0xAF, 0x02, 0x00)          # DVI mode

    # state as zb_platform.c
    hpd, reapply, i2c_err, hpd_changes, auto = -1, 0, 0, 0, False

    def set_aspect(self, w, h):
        self.aspect_169 = (w * 9 == h * 16)
        self.upd(0x17, 0x02, 0x02 if self.aspect_169 else 0x00)

    def set_clock_delay(self, code):
        self.delay = code & 7
        self.wr(0xBA, self.delay << 5)

    def connected(self):
        return self.hpd == 1

    def poll(self):
        """zb_hdmi_poll(): True if the monitor state changed."""
        try:
            r41, r15, r16, r18, r42 = (self.rd(x) for x in (0x41, 0x15, 0x16, 0x18, 0x42))
        except OSError:
            self.i2c_err += 1
            return False
        if r41 == r15 == r16 == r18 == r42:
            self.i2c_err += 1
            return False
        if (r41 & 0x40) or (r15 & 0x0F) != 0x01 or r16 != 0x38 or not (r18 & 0x80):
            self.reapply += 1
            if self.reapply <= 10 or self.reapply % 50 == 0:
                print(f"[hdmi] setup lost #{self.reapply}: 41={r41:02x} 15={r15:02x} 16={r16:02x} "
                      f"18={r18:02x} 42={r42:02x} -> {'re-applied' if self.auto else 'left (auto off)'}")
            if self.auto:
                self.init(quiet=True)
        hpd = (r42 >> 6) & 1
        if hpd == self.hpd:
            return False
        if self.hpd >= 0:
            self.hpd_changes += 1
        self.hpd = hpd
        return True

    def status(self):
        g = self.rd
        st, pw, vic, pll = g(0x42), g(0x41), g(0x3E), g(0x9E)
        return (f"hdmi     monitor {'connected' if st & 0x40 else 'not connected'} (HPD {(st >> 6) & 1}, "
                f"sense {(st >> 5) & 1}), {'POWERED DOWN' if pw & 0x40 else 'powered'}, TMDS PLL "
                f"{'locked' if pll & 0x10 else 'NOT locked'}, input VIC {vic >> 2}, clock delay "
                f"{self.delay * 400 - 1200} ps, regs 15={g(0x15):02x} 16={g(0x16):02x} 17={g(0x17):02x} "
                f"48={g(0x48):02x} AF={g(0xAF):02x} BA={g(0xBA):02x} D6={g(0xD6):02x}\n"
                f"           CSC 18={g(0x18):02x} 1A={g(0x1A):02x}, setup re-applied {self.reapply} times "
                f"(auto {'on' if self.auto else 'off'}), HPD changes {self.hpd_changes}, I2C read errors {self.i2c_err}")


# ---------------------------------------------------------------------------------------
# Board
# ---------------------------------------------------------------------------------------
DISP_W, DISP_H = 1024, 768
DISP_STRIDE = DISP_W * 4
FRAME_BYTES = DISP_STRIDE * DISP_H          # 3 MB
CAM_W, CAM_H = 640, 480
CAM_X0, CAM_Y0 = (DISP_W - CAM_W) // 2, (DISP_H - CAM_H) // 2
CAM_WIN_OFF = (CAM_Y0 * DISP_W + CAM_X0) * 4


class Zed:
    def __init__(self, hdmi=True, cached=False):
        self.buf = DmaBuf(cached=cached)
        self.resets = EmioResets()
        self.vtc = uio_find("v_tc", 0x43C10000)
        self.clk = uio_find("clk_wiz", 0x43C30000)
        self.proc = uio_find("proc_slot", 0x43D00000)
        self.fbr = FrameBuf(uio_find("v_frmbuf_rd", 0x43C00000), self.resets, "rd", "vfb_rd")
        self.fbr2 = FrameBuf(uio_find("v_frmbuf_rd", 0x43C40000), self.resets, "rd2", "vfb_rd2")
        self.fbw = FrameBuf(uio_find("v_frmbuf_wr", 0x43C20000), self.resets, "wr", "vfb_wr")
        self.resets.release_all()
        self.hdmi = None
        if hdmi:
            try:
                self.hdmi = Hdmi()
                self.hdmi.init()
            except Exception as e:             # no ADV7511 / no monitor / I2C error: VGA only
                print(f"HDMI disabled ({type(e).__name__}: {e}); continuing with VGA")
                self.hdmi = None

    # frame buffer n (0..) inside the u-dma-buf region
    def frame_off(self, n):
        off = n * FRAME_BYTES
        if off + FRAME_BYTES > self.buf.size:
            raise ValueError("u-dma-buf too small for this frame index")
        return off

    def frame_phys(self, n):
        return self.buf.phys + self.frame_off(n)

    def frame(self, n, w=DISP_W, h=DISP_H, stride=DISP_STRIDE):
        return self.buf.u32(self.frame_off(n), w, h, stride)

    def set_mode(self, w=1024, h=768):
        m = MODES[mode_index(w, h)]
        self.vtc.wr(VTC_CTL, 0)
        if not clkwiz_set(self.clk, m):
            raise RuntimeError(f"pixel clock did not lock for {m[0]}")
        time.sleep(0.005)                    # rst_pix releases the VTC after lock
        vtc_set(self.vtc, m)
        if self.hdmi:
            try:
                self.hdmi.upd(0x17, 0x02, 0x02 if w * 9 == h * 16 else 0x00)
            except OSError as e:
                print(f"HDMI aspect update failed ({e}); ignored")

    def dump(self):
        ga = self.vtc.rd(VTC_GASIZE)
        print("Debug:")
        for f in (self.fbr, self.fbw, self.fbr2):
            print("  " + f.dump())
        print(f"  vtc      ctl {self.vtc.rd(VTC_CTL):#010x} status {self.vtc.rd(0x04):#010x} "
              f"active {ga & 0x1FFF}x{(ga >> 16) & 0x1FFF} htotal {self.vtc.rd(VTC_GHSIZE)} "
              f"vtotal {self.vtc.rd(VTC_GVSIZE) & 0x1FFF}")
        print(f"  clk_wiz  locked {self.clk.rd(WZ_STATUS) & 1} cfg0 {self.clk.rd(WZ_CFG0):#010x} "
              f"cfg2 {self.clk.rd(WZ_CFG2):#010x}")
        print(f"  udmabuf  phys {self.buf.phys:#010x} size {self.buf.size >> 20} MB")
        if self.hdmi:
            try:
                print("  " + self.hdmi.status())
            except OSError as e:
                print(f"  hdmi status read failed ({e})")
