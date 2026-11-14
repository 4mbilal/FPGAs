#!/usr/bin/env python3
"""jig.py - Python port of jig.c (zed_base test application), same functionality and keys.

Sources shown on the display (VGA and HDMI show the same image):
  camera   camera -> proc_slot -> vfb_wr -> rotating DRAM buffers -> vfb_rd -> VGA / HDMI
  pattern  colour bars / checkerboard / grey ramp
Memory test: DRAM test image -> vfb_rd2 -> proc_slot -> vfb_wr -> DRAM result, compared
bit-exact against the C golden models (sw/common/sw_kernels.c and lle_global.c, through ctypes).

Keys (single key presses, 'h' prints the menu with the current state):
  t  source camera / test pattern      p  next test pattern
  d/D display mode                     c/C camera size
  0-4 kernel: none, gray, invert, Canny (OpenCV, CPU only), LLE (global curve)
  x  kernel in HW (proc_slot) or SW (CPU)
  n  LLE iteration count: auto (LiteIE rule from the frame statistics), 2, 4, 8, 12
  m  memory test (generated image)     l  memory test (image file, see --load)
  r  memory-test size  s status  v debug  g gap test
  H  HDMI status  k  HDMI input clock delay  a  HDMI auto re-apply on/off   q  quit
Board switches: SW0 = SW path, SW2:1 = kernel (whichever changed last wins).

Differences from jig.c (Linux only):
  - buffers live in /dev/udmabuf0 (cached, explicit sync as Xil_DCache*Range)
  - time base is time.perf_counter(); 'l' loads a raw XRGB8888 file given with --load
  - kernel 3 is OpenCV Canny (as the Linux C build), not the C Sobel
Run (from anywhere in the repository; sw/common and sw/linux_c/shim are found relative to this file):
  sudo python3 sw/python/jig.py [--load image.raw]
"""
import argparse
import ctypes
import os
import select
import subprocess
import sys
import termios
import time
import tty

import numpy as np

import hw

# ---- proc_slot ---------------------------------------------------------------------
SLOT_CTRL, SLOT_ID, SLOT_FRAMES = 0x00, 0x04, 0x08
SLOT_CYCLES_LAST, SLOT_PIXELS_LAST, SLOT_LINES_LAST, SLOT_STALLS_LAST = 0x0C, 0x10, 0x14, 0x18
SLOT_ACTIVE_CYCLES, SLOT_PIXELS_RUN, SLOT_LINES_RUN = 0x1C, 0x20, 0x24
SLOT_CAPS, SLOT_LUT_BANK, SLOT_STAT_SEQ, SLOT_STAT_CTRL = 0x28, 0x2C, 0x30, 0x34
SLOT_LUT0, SLOT_STAT_COUNT, SLOT_SUM_R, SLOT_HIST0 = 0x400, 0x1000, 0x1004, 0x1040
SLOT_ID_EXPECTED = 0x534C0005               # proc_slot v5 (shell v7)
SLOT_MODE_LUT = 3
SLOT_SRC_MEM = 0x10
SLOT_CLK_HZ = 150_000_000
LLE_FORCE = [0, 2, 4, 8, 12]                # 'n': 0 = auto (LiteIE rule)

# ---- buffers in the u-dma-buf region (same layout as the Linux C build) ------------
NBUF = 6
CANVAS_MAX = 1920 * 1200                  # pixels per canvas buffer
TEST_IN, TEST_OUT, TEST_REF, TEST_END = 0x03800000, 0x04200000, 0x04C00000, 0x05600000
TEST_SIZES = [(640, 480), (1280, 720), (1600, 1200), (1920, 1080), (1920, 1200)]

KERN_NAME = ["none", "grayscale", "invert", "Canny", "LLE global"]
PAT_NAME = ["colour bars", "checkerboard", "grey ramp"]


def now():
    return time.perf_counter()


# ---- golden models (C, through ctypes) -------------------------------------------------
class LleStats(ctypes.Structure):
    _fields_ = [("hist", ctypes.c_uint32 * 16), ("sum", ctypes.c_uint32 * 3), ("count", ctypes.c_uint32)]

    def key(self):
        return (tuple(self.hist), tuple(self.sum), self.count)


Table = (ctypes.c_uint8 * 256) * 3


class Models:
    """sw_kernels.c (k_gray, k_invert) and lle_global.c from sw/common, built once as one
    shared library, so SW results stay bit-exact with the bare-metal and C Linux versions."""

    def __init__(self, common, shim):
        srcs = [os.path.join(common, f) for f in ("sw_kernels.c", "lle_global.c")]
        lib = os.path.join(os.path.dirname(os.path.abspath(__file__)), "libzbmodels.so")
        if not os.path.exists(lib) or os.path.getmtime(lib) < max(os.path.getmtime(f) for f in srcs):
            cmd = ["gcc", "-O2", "-shared", "-fPIC", "-U__linux__", "-I" + shim, "-I" + common] + srcs + ["-o", lib]
            print("Building", lib)
            subprocess.check_call(cmd)
        L = self.lib = ctypes.CDLL(lib)
        img = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int]
        for f in ("k_gray", "k_invert"):
            getattr(L, f).argtypes, getattr(L, f).restype = img, None
        L.lle_stats.argtypes, L.lle_stats.restype = img + [ctypes.POINTER(LleStats)], None
        L.lle_mean.argtypes, L.lle_mean.restype = [ctypes.POINTER(LleStats)], ctypes.c_double
        L.lle_liteie_iters.argtypes, L.lle_liteie_iters.restype = [ctypes.POINTER(LleStats)], ctypes.c_int
        L.lle_build_table.argtypes = [ctypes.POINTER(Table), ctypes.POINTER(ctypes.c_double), ctypes.c_int]
        L.lle_build_table.restype = None
        L.lle_apply.argtypes, L.lle_apply.restype = img + [ctypes.POINTER(Table)], None
        self.r = (ctypes.c_double * 3).in_dll(L, "lle_liteie_r")
        lo, hi = float(os.environ.get("ZB_CANNY_LO", 50)), float(os.environ.get("ZB_CANNY_HI", 150))
        self.canny_lo, self.canny_hi = lo, hi
        self.canny_blur = os.environ.get("ZB_CANNY_BLUR", "1") != "0"

    def gray(self, addr, w, h, stride_px):
        self.lib.k_gray(addr, w, h, stride_px)

    def invert(self, addr, w, h, stride_px):
        self.lib.k_invert(addr, w, h, stride_px)

    # low-light enhancement, global curve (lle_global.h)
    def stats(self, addr, w, h, stride_px):
        st = LleStats()
        self.lib.lle_stats(addr, w, h, stride_px, ctypes.byref(st))
        return st

    def mean(self, st):
        return self.lib.lle_mean(ctypes.byref(st))

    def iters(self, st):
        return self.lib.lle_liteie_iters(ctypes.byref(st))

    def table(self, n):
        t = Table()
        self.lib.lle_build_table(ctypes.byref(t), self.r, n)
        return t

    def apply(self, addr, w, h, stride_px, t):
        self.lib.lle_apply(addr, w, h, stride_px, ctypes.byref(t))

    def canny(self, view):
        """In place on an (h, w, 4) B,G,R,X view: white edges on black (as zb_cv.cpp)."""
        import cv2
        gray = cv2.cvtColor(np.ascontiguousarray(view), cv2.COLOR_BGRA2GRAY)
        if self.canny_blur:
            gray = cv2.GaussianBlur(gray, (3, 3), 0)
        e = cv2.Canny(gray, self.canny_lo, self.canny_hi)
        view[..., 0] = e
        view[..., 1] = e
        view[..., 2] = e


# ---- terminal --------------------------------------------------------------------------
class Keys:
    def __init__(self):
        self.fd = sys.stdin.fileno()
        self.saved = termios.tcgetattr(self.fd) if os.isatty(self.fd) else None
        if self.saved:
            tty.setcbreak(self.fd)

    def get(self):
        if select.select([self.fd], [], [], 0)[0]:
            return os.read(self.fd, 1).decode(errors="ignore")
        return ""

    def restore(self):
        if self.saved:
            termios.tcsetattr(self.fd, termios.TCSADRAIN, self.saved)


# ---- the jig -----------------------------------------------------------------------------
class Jig:
    def __init__(self, args):
        self.z = hw.Zed(cached=True)
        self.b = self.z.buf
        if NBUF * CANVAS_MAX * 4 > self.b.size:
            sys.exit(f"u-dma-buf too small: {self.b.size >> 20} MB, live buffers need {NBUF * CANVAS_MAX * 4 >> 20} MB")
        self.test_ok = self.b.size >= TEST_END
        if not self.test_ok:
            print(f"u-dma-buf {self.b.size >> 20} MB: live video only; memory/gap tests need >= 86 MB")
        self.io = hw.BoardIO()
        self.k = Models(args.common, args.shim)
        self.load = args.load
        self.cam = hw.Camera(header=os.path.join(args.common, "ov2640_regs.h"))
        self.u32 = np.frombuffer(self.b.mm, dtype=np.uint32)          # whole region as pixels

        self.cam_ok = False
        self.src_pattern = 0
        self.pattern = 0
        self.path_sw = 0
        self.kern = 0
        self.test_size = 3
        self.live_hw_mode = 0
        # low-light enhancement (kernel 4)
        self.lle_seq_seen = 0
        self.lle_n_active = -1                     # table in (or queued for) use
        self.lle_n_last = -1                       # latest iteration count
        self.lle_mean_last = 0.0
        self.lle_force = 0                         # index into LLE_FORCE
        self.disp_mode = -1
        # rotation state (indices into the NBUF buffers, -1 = none)
        self.cam_w = self.cam_p = self.disp_reg = self.disp_scan = -1
        self.disp_prev = self.raw_ready = self.cpu_hold = -1
        self.disp_prev_age = 0
        self.disp_frames = 0
        self.canvas_w = self.canvas_h = self.disp_w = self.disp_h = self.cw = self.ch = 0
        self.cam_off = self.disp_off = 0

    # ---- addresses -------------------------------------------------------------------
    @staticmethod
    def buf_off(i):
        return i * CANVAS_MAX * 4

    def phys(self, off):
        return self.b.phys + off

    def cam_addr(self, i):                          # physical, for the DMA
        return self.phys(self.buf_off(i) + self.cam_off)

    def disp_addr(self, i):
        return self.phys(self.buf_off(i) + self.disp_off)

    def cam_span(self):
        return (self.ch - 1) * self.canvas_w * 4 + self.cw * 4

    def bgrx(self, off, w, h, stride_px):
        """(h, w, 4) uint8 view (no copy) at byte offset off."""
        return np.lib.stride_tricks.as_strided(self.b.base[off:], shape=(h, w, 4), strides=(stride_px * 4, 4, 1))

    def pix(self, off, w, h, stride_px):
        """(h, w) uint32 view (no copy)."""
        start = off // 4
        return np.lib.stride_tricks.as_strided(self.u32[start:], shape=(h, w), strides=(stride_px * 4, 4))

    # ---- buffer rotation (as jig.c) --------------------------------------------------
    def in_use(self, i):
        return i in (self.cam_w, self.cam_p, self.disp_reg, self.disp_scan, self.disp_prev,
                     self.raw_ready, self.cpu_hold)

    def pick_free(self):
        for i in range(NBUF):
            if not self.in_use(i):
                return i
        return -1

    def disp_event(self):
        self.disp_scan = self.disp_reg
        if self.disp_prev >= 0:
            self.disp_prev_age += 1
            if self.disp_prev_age >= 2:
                self.disp_prev = -1
        self.disp_frames += 1

    def display(self, i):
        if self.z.fbr.done():
            self.disp_event()
        self.disp_prev = self.disp_reg
        self.disp_prev_age = 0
        self.disp_reg = i
        self.z.fbr.set_addr(self.disp_addr(i))

    # ---- drawing -----------------------------------------------------------------------
    def draw_canvas(self, i):
        x0, y0 = (self.cam_off // 4) % self.canvas_w, (self.cam_off // 4) // self.canvas_w
        y, x = np.mgrid[0:self.canvas_h, 0:self.canvas_w]
        img = np.where(((x >> 5) ^ (y >> 5)) & 1, 0x303030, 0x181818).astype(np.uint32)
        img[y0:y0 + self.ch, x0:x0 + self.cw] = 0
        self.pix(self.buf_off(i), self.canvas_w, self.canvas_h, self.canvas_w)[:] = img
        self.b.flush(self.buf_off(i), self.canvas_w * self.canvas_h * 4)

    def draw_pattern(self, off, w, h, pat):
        y, x = np.mgrid[0:h, 0:w]
        if pat == 0:
            bars = np.array([0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00, 0xFF00FF, 0xFF0000, 0x0000FF, 0x000000],
                            dtype=np.uint32)
            img = bars[(x * 8) // w]
        elif pat == 1:
            img = np.where(((x >> 5) ^ (y >> 5)) & 1, 0xFFFFFF, 0x000000).astype(np.uint32)
        else:
            g = (x.astype(np.uint32) * 255 // (w - 1)).astype(np.uint32)
            img = (g << 16) | (g << 8) | g
        img = img.astype(np.uint32)
        img[0, :] = img[-1, :] = 0xFF0000
        img[:, 0] = img[:, -1] = 0xFF0000
        self.pix(off, w, h, w)[:] = img
        self.b.flush(off, w * h * 4)

    # ---- configuration ---------------------------------------------------------------
    def apply_config(self, mode_idx, cam_idx, reprogram_display, reprogram_camera):
        z = self.z
        z.fbw.stop()
        z.fbr.stop()
        if reprogram_display:
            m = hw.MODES[mode_idx]
            z.set_mode(m[1], m[2])
            self.disp_mode = mode_idx
        if reprogram_camera and not self.cam.set_size(cam_idx):
            return False
        m = hw.MODES[self.disp_mode]
        self.disp_w, self.disp_h = m[1], m[2]

        if self.src_pattern or not self.cam_ok:
            self.canvas_w, self.canvas_h, self.cam_off, self.disp_off = self.disp_w, self.disp_h, 0, 0
            self.draw_pattern(self.buf_off(0), self.disp_w, self.disp_h, self.pattern)
            self.disp_reg = self.disp_scan = 0
            self.disp_prev, self.disp_prev_age, self.raw_ready, self.cpu_hold = -1, 0, -1, -1
            self.cam_w = self.cam_p = -1
            z.fbr.configure(self.phys(self.buf_off(0)), self.disp_w, self.disp_h, self.disp_w * 4)
            print(f"Display {m[0]}, test pattern: {PAT_NAME[self.pattern]}")
            return True

        self.cw, self.ch = self.cam.w(), self.cam.h()
        self.canvas_w, self.canvas_h = max(self.cw, self.disp_w), max(self.ch, self.disp_h)
        self.cam_off = (((self.canvas_h - self.ch) // 2) * self.canvas_w + (((self.canvas_w - self.cw) // 2) & ~7)) * 4
        self.disp_off = (((self.canvas_h - self.disp_h) // 2) * self.canvas_w
                         + (((self.canvas_w - self.disp_w) // 2) & ~7)) * 4
        for i in range(NBUF):
            self.draw_canvas(i)
        if reprogram_camera:
            time.sleep(0.2)                          # let the sensor settle

        self.disp_reg = self.disp_scan = 0
        self.disp_prev, self.disp_prev_age, self.raw_ready, self.cpu_hold = -1, 0, -1, -1
        self.cam_w, self.cam_p = 1, 2
        z.fbr.configure(self.disp_addr(self.disp_reg), self.disp_w, self.disp_h, self.canvas_w * 4)
        z.fbw.configure(self.cam_addr(self.cam_w), self.cw, self.ch, self.canvas_w * 4)
        z.fbw.set_addr(self.cam_addr(self.cam_p))
        print(f"Display {m[0]}, camera {self.cam.size_name()} {self.cw}x{self.ch} "
              f"(canvas {self.canvas_w}x{self.canvas_h})")
        return True

    def reapply(self):
        return self.apply_config(self.disp_mode, self.cam.size_idx, False, False)

    # ---- low-light enhancement: curve table and frame statistics (proc_slot v5) --------
    def lut_write(self, t):                        # into the inactive bank
        wr = self.z.proc.wr
        for c in range(3):
            base = SLOT_LUT0 + 0x400 * c
            for x in range(256):
                wr(base + 4 * x, t[c][x])

    def lut_swap_pending(self):
        return self.z.proc.rd(SLOT_LUT_BANK) & 1

    def slot_stats(self):                          # latched set
        rd, st = self.z.proc.rd, LleStats()
        st.count = rd(SLOT_STAT_COUNT)
        for c in range(3):
            st.sum[c] = rd(SLOT_SUM_R + 4 * c)
        for k in range(16):
            st.hist[k] = rd(SLOT_HIST0 + 4 * k)
        return st

    def lle_n(self, st):
        """Iteration count for these statistics: the LiteIE rule, or the forced value ('n')."""
        f = LLE_FORCE[self.lle_force]
        return f if f else self.k.iters(st)

    def lle_load(self, n):
        """Table for n iterations into the inactive bank, swapped at the next start of frame.
        False if an earlier table is still queued after 200 ms (no frames arriving)."""
        t0 = now()
        while self.lut_swap_pending():
            if now() - t0 > 0.2:
                return False
        self.lut_write(self.k.table(n))
        self.z.proc.wr(SLOT_LUT_BANK, 1)
        self.lle_n_active = n
        return True

    @staticmethod
    def hw_mode_of(k):
        return k if k in (1, 2) else SLOT_MODE_LUT if k == 4 else 0

    def set_proc(self, path_sw, kern):
        self.path_sw, self.kern = path_sw, (kern if 0 <= kern <= 4 else 0)
        if not self.path_sw:
            self.raw_ready = -1
        self.live_hw_mode = 0 if self.path_sw else self.hw_mode_of(self.kern)
        if self.live_hw_mode == SLOT_MODE_LUT:      # a table must be in place first
            self.lle_seq_seen = self.z.proc.rd(SLOT_STAT_SEQ)
            f = LLE_FORCE[self.lle_force]
            self.lle_load(f or (self.lle_n_active if self.lle_n_active > 0 else 8))
        self.z.proc.wr(SLOT_CTRL, self.live_hw_mode)
        extra = " -> Canny is SW only, proc_slot passes through" if (not self.path_sw and self.kern == 3) else ""
        print(f"Kernel {KERN_NAME[self.kern]} in {'SW (CPU)' if self.path_sw else 'HW (proc_slot)'}{extra}")

    # ---- memory-to-memory test ---------------------------------------------------------
    @staticmethod
    def test_image(w, h):
        y, x = np.mgrid[0:h, 0:w].astype(np.uint32)
        with np.errstate(over="ignore"):
            hsh = (x * np.uint32(0x9E3779B1)) ^ (y * np.uint32(0x85EBCA77))
            hsh ^= hsh >> np.uint32(15)
            hsh *= np.uint32(0x2C1B3C6D)
            hsh ^= hsh >> np.uint32(12)
        r = ((x * 255) // 1919 + (hsh & 0x1F)) & 0xFF
        g = ((y * 255) // 1199 + ((hsh >> 8) & 0x3F)) & 0xFF
        b = (hsh >> 16) & 0xFF
        b = np.where(((x >> 6) ^ (y >> 6)) & 1, 255 - b, b)
        return ((r << 16) | (g << 8) | b).astype(np.uint32)

    def mem_dma_run(self, w, h, stride_px, hw_mode):
        """Start vfb_wr (single) and vfb_rd2 (single) through proc_slot from TEST_IN to TEST_OUT."""
        z = self.z
        z.fbw.stop()
        z.proc.wr(SLOT_CTRL, SLOT_SRC_MEM | hw_mode)
        t0 = now()
        z.fbw.configure(self.phys(TEST_OUT), w, h, stride_px * 4, continuous=False)
        z.fbr2.configure(self.phys(TEST_IN), w, h, stride_px * 4, continuous=False, graceful=False)
        return t0

    def load_input(self, w, h):
        n = w * h
        if not self.load:
            print("  no image file: start with --load file.raw (XRGB8888, little endian)")
            return False
        data = np.fromfile(self.load, dtype=np.uint32, count=n)
        if data.size < n:
            print(f"  {self.load}: {data.size} pixels, need {n}")
            return False
        self.pix(TEST_IN, w, h, w)[:] = data.reshape(h, w)
        return True

    def dma_pass(self, w, h, hw_mode):
        """One memory frame TEST_IN -> proc_slot -> TEST_OUT; returns (complete, active cycles)."""
        z = self.z
        t0 = self.mem_dma_run(w, h, w, hw_mode)
        wr = rd = slot = False
        while now() - t0 < 1.0:
            wr = wr or z.fbw.done()
            rd = rd or z.fbr2.done()
            slot = slot or (z.proc.rd(SLOT_PIXELS_RUN) == w * h and z.proc.rd(SLOT_LINES_RUN) == h)
            if wr and rd and slot:
                break
        return (slot and wr), z.proc.rd(SLOT_ACTIVE_CYCLES)

    def lle_mem_test(self, use_loaded):
        """Low-light enhancement on a still image, two passes through proc_slot (a still image
        has no previous frame): 1. pass-through, statistics latched and read back;
        2. curve-table mode with the table from those statistics. The C golden model
        (lle_global.c) computes its own statistics and table; all must agree."""
        if not self.test_ok:
            print("Memory test needs a u-dma-buf of >= 86 MB")
            return
        z = self.z
        w, h = TEST_SIZES[self.test_size]
        n, nbytes = w * h, w * h * 4
        print(f"\nMemory test {w}x{h}, low-light enhancement (global curve), input "
              f"{'loaded from ' + str(self.load) if use_loaded else 'generated (darkened)'}")
        inp, out, ref = self.pix(TEST_IN, w, h, w), self.pix(TEST_OUT, w, h, w), self.pix(TEST_REF, w, h, w)
        if use_loaded:
            if not self.load_input(w, h):
                return
        else:
            inp[:] = (self.test_image(w, h) >> 2) & 0x3F3F3F
        self.b.flush(TEST_IN, nbytes)
        out[:] = 0xA5A5A5A5
        self.b.flush(TEST_OUT, nbytes)

        ok1, _ = self.dma_pass(w, h, 0)                        # pass 1: statistics
        if ok1:
            z.proc.wr(SLOT_STAT_CTRL, 1)
            hw_st = self.slot_stats()
            n_hw = self.k.iters(hw_st)
            self.lle_load(n_hw)
            out[:] = 0xA5A5A5A5
            self.b.flush(TEST_OUT, nbytes)
            ok2, act = self.dma_pass(w, h, SLOT_MODE_LUT)      # pass 2: curve table
        z.proc.wr(SLOT_CTRL, self.live_hw_mode)
        self.lle_n_active = -1                                 # the live path rebuilds its table
        if not (ok1 and ok2):
            print("  FAIL: a test frame did not pass through proc_slot completely")
            self.debug_dump()
            self.reapply()
            return

        ref[:] = inp                                           # C golden model on the same input
        s0 = now()
        sw_st = self.k.stats(self.b.addr + TEST_REF, w, h, w)
        n_sw = self.k.iters(sw_st)
        self.k.apply(self.b.addr + TEST_REF, w, h, w, self.k.table(n_sw))
        sw_us = (now() - s0) * 1e6
        self.b.invalidate(TEST_OUT, nbytes)
        bad = int(((out & 0xFFFFFF) != (ref & 0xFFFFFF)).sum())
        st_ok = hw_st.key() == sw_st.key()
        print(f"  mean {self.k.mean(sw_st):.3f} -> {n_hw} iterations (HW statistics) / {n_sw} (C model); "
              f"statistics {'identical' if st_ok else 'DIFFER'}")
        print(f"  HW curve pass: {act / SLOT_CLK_HZ * 1e6:.1f} us in the slot; "
              f"C model (statistics + table + apply): {sw_us:.0f} us")
        print(f"  {'PASS' if (not bad and st_ok) else 'FAIL'}: {bad} of {n} pixels differ between HW and the C golden model")
        if w >= self.disp_w and h >= self.disp_h:
            off = (((h - self.disp_h) // 2) * w + (((w - self.disp_w) // 2) & ~7)) * 4
            z.fbr.configure(self.phys(TEST_OUT + off), self.disp_w, self.disp_h, w * 4)
            time.sleep(3)
        self.reapply()

    def run_mem_test(self, kernel, use_loaded):
        if kernel == 4:
            self.lle_mem_test(use_loaded)
            return
        if not self.test_ok:
            print("Memory test needs a u-dma-buf of >= 86 MB")
            return
        z = self.z
        w, h = TEST_SIZES[self.test_size]
        n, nbytes = w * h, w * h * 4
        hw_mode = kernel if kernel in (1, 2) else 0
        kname = ["none", "grayscale", "invert", "Canny(SW only -> HW pass)"]
        print(f"\nMemory test {w}x{h}, kernel {kname[kernel & 3]}, input "
              f"{'loaded from ' + self.load if use_loaded else 'generated'}")
        inp = self.pix(TEST_IN, w, h, w)
        out = self.pix(TEST_OUT, w, h, w)
        ref = self.pix(TEST_REF, w, h, w)
        if use_loaded:
            if not self.load_input(w, h):
                return
        else:
            inp[:] = self.test_image(w, h)
        self.b.flush(TEST_IN, nbytes)
        out[:] = 0xA5A5A5A5
        self.b.flush(TEST_OUT, nbytes)

        t0 = self.mem_dma_run(w, h, w, hw_mode)
        t_wr = t_rd = t_slot = 0.0
        px = lines = 0
        while now() - t0 < 1.0:
            t = now()
            if not t_wr and z.fbw.done():
                t_wr = t
            if not t_rd and z.fbr2.done():
                t_rd = t
            px, lines = z.proc.rd(SLOT_PIXELS_RUN), z.proc.rd(SLOT_LINES_RUN)
            if not t_slot and px == n and lines == h:
                t_slot = t
            if t_wr and t_rd and t_slot:
                break
        t1 = now()
        act, px, lines = z.proc.rd(SLOT_ACTIVE_CYCLES), z.proc.rd(SLOT_PIXELS_RUN), z.proc.rd(SLOT_LINES_RUN)
        us = lambda t: int((t - t0) * 1e6) if t else 0
        print(f"  done flags: write DMA {'at' if t_wr else 'NOT SEEN'} {us(t_wr)} us, read DMA "
              f"{'at' if t_rd else 'NOT SEEN'} {us(t_rd)} us; slot saw {px} of {n} px, {lines} of {h} lines"
              f"{'' if t_slot else ' (INCOMPLETE)'}")
        if t_wr and (not t_slot or t_wr < t_slot):
            print("  NOTE: write DMA reported done before the whole frame had passed proc_slot")
        z.proc.wr(SLOT_CTRL, self.live_hw_mode)
        if not t_slot:
            self.debug_dump()
            print("  FAIL: the test frame did not pass through proc_slot completely")
            self.reapply()
            return

        # software reference on the same input (C golden model through ctypes)
        self.b.invalidate(TEST_OUT, nbytes)
        ref[:] = inp
        self.b.flush(TEST_REF, nbytes)
        s0 = now()
        if hw_mode == 1:
            self.k.gray(self.b.addr + TEST_REF, w, h, w)
        elif hw_mode == 2:
            self.k.invert(self.b.addr + TEST_REF, w, h, w)
        s1 = now()
        canny_t = 0.0
        if kernel == 3:
            tmp = self.bgrx(TEST_REF, w, h, w)
            q0 = now()
            self.k.canny(tmp)
            canny_t = now() - q0
            ref[:] = inp

        o, r = out & 0xFFFFFF, ref & 0xFFFFFF
        diff = o != r
        bad = int(diff.sum())
        if bad:
            ys, xs = np.nonzero(diff)
            for yy, xx in list(zip(ys, xs))[:5]:
                print(f"  mismatch at ({xx},{yy}): HW {int(o[yy, xx]):06x} SW {int(r[yy, xx]):06x}")
            of, rf = o.ravel(), r.ravel()
            found = False
            for k in range(0, 65537):               # out[i] == ref[i + k] for the first 64 pixels
                if k + 64 > rf.size:
                    break
                if np.array_equal(of[:64], rf[k:k + 64]):
                    print(f"  Result = expected image shifted by {k} pixels along the stream "
                          f"({k // w} lines + {k % w} px)")
                    found = True
                    break
            if not found:
                print("  Result is not a shifted copy of the expected image (searched -64 .. 65536)")
        hw_us = act / SLOT_CLK_HZ * 1e6 if act else 0
        e2e_us = (t1 - t0) * 1e6
        mpix = px * SLOT_CLK_HZ / act / 1e6 if act else 0
        sw_us = (s1 - s0) * 1e6
        print(f"  HW: {px} px, {lines} lines, {act} active cycles = {hw_us:.1f} us in the slot "
              f"({mpix:.1f} Mpix/s), {e2e_us:.0f} us end to end")
        if hw_mode:
            print(f"  SW: {sw_us:.0f} us for the same kernel on the CPU "
                  f"({sw_us / hw_us if hw_us else 0:.1f}x slower than the slot)")
        if kernel == 3:
            print(f"  SW Canny: {canny_t * 1e6:.0f} us")
        print(f"  {'FAIL' if bad else 'PASS'}: {bad} of {n} pixels differ between HW and the C golden model")
        print(f"  Input 0x{self.phys(TEST_IN):08x}, HW result 0x{self.phys(TEST_OUT):08x}, "
              f"SW reference 0x{self.phys(TEST_REF):08x} ({nbytes} bytes each)")
        if w >= self.disp_w and h >= self.disp_h:
            off = (((h - self.disp_h) // 2) * w + (((w - self.disp_w) // 2) & ~7)) * 4
            z.fbr.configure(self.phys(TEST_OUT + off), self.disp_w, self.disp_h, w * 4)
            time.sleep(3)
        self.reapply()

    def run_gap_test(self):
        if not self.test_ok:
            print("Gap test needs a u-dma-buf of >= 86 MB")
            return
        GAP_PX, GAP_MARK, FILL, GUARD = 64, 0x00FF00FF, 0xA5A5A5A5, 1024
        z = self.z
        w, h = TEST_SIZES[self.test_size]
        S = w + GAP_PX
        nbytes = S * h * 4
        print(f"\nGap test {w}x{h}, stride {S} px ({GAP_PX} px gap after every line)")
        inp = self.pix(TEST_IN, S, h, S)
        out = self.pix(TEST_OUT, S, h, S)
        guard = self.u32[(TEST_OUT - GUARD) // 4: TEST_OUT // 4]
        inp[:, :w] = self.test_image(w, h)
        inp[:, w:] = GAP_MARK
        out[:] = FILL
        guard[:] = FILL
        self.b.flush(TEST_IN, nbytes)
        self.b.flush(TEST_OUT - GUARD, nbytes + GUARD)

        t0 = self.mem_dma_run(w, h, S, 0)
        complete = False
        while now() - t0 < 1.0:
            if z.proc.rd(SLOT_PIXELS_RUN) == w * h and z.proc.rd(SLOT_LINES_RUN) == h:
                complete = True
                break
        time.sleep(0.002)
        z.proc.wr(SLOT_CTRL, self.live_hw_mode)
        self.b.invalidate(TEST_OUT - GUARD, nbytes + GUARD)
        if not complete:
            print("  Frame did not pass proc_slot completely")
            self.debug_dump()

        diff = (out[:, :w] & 0xFFFFFF) != (inp[:, :w] & 0xFFFFFF)
        bad = int(diff.sum())
        guard_hit = int((guard != FILL).sum())
        if not bad:
            print(f"  PASS: all {w * h} pixels correct, gaps untouched check below, guard "
                  f"{'OVERWRITTEN' if guard_hit else 'untouched'}")
        else:
            k = 0
            for kk in range(1, 257):
                if np.array_equal(out[0, :16] & 0xFFFFFF, inp[0, kk:kk + 16] & 0xFFFFFF):
                    k = kk
                    break
            print(f"  FAIL: {bad} of {w * h} pixels differ; line 0 shifted by {k} px; guard "
                  f"{'OVERWRITTEN' if guard_hit else 'untouched'} ({guard_hit} words)")
            if k:
                for y in range(min(2, h - 1)):
                    seg = out[y, w - k:w]
                    n_mark = int((seg == GAP_MARK).sum())
                    n_fill = int((seg == FILL).sum())
                    n_next = int(((seg & 0xFFFFFF) == (inp[y + 1, :k] & 0xFFFFFF)).sum())
                    print(f"  line {y}, last {k} px: {n_mark} gap marker, {n_fill} untouched, "
                          f"{n_next} = start of next line")
                print(f"  -> gap marker: READ side fetched {k * 4} bytes ahead; untouched + guard hit: "
                      f"WRITE side {k * 4} bytes early;\n     next line: {k} px LOST from the stream between the DMAs")
        print(f"  output gap words written: {int((out[:, w:] != FILL).sum())} (expect 0)")
        self.reapply()

    # ---- user interface ------------------------------------------------------------------
    def hdmi_state(self):
        hd = self.z.hdmi
        return "absent" if not hd else ("monitor connected" if hd.connected() else "no monitor")

    def menu(self):
        print("\n---- zed_base jig (Python) --------------------------------------\n"
              " t  source: camera / test pattern      p  next test pattern\n"
              " d/D display mode                      c/C camera size\n"
              " 0-4 kernel: none, gray, invert, Canny, LLE (global curve)\n"
              " x  kernel in HW (proc_slot) / SW (CPU)  n  LLE iterations: auto / 2 / 4 / 8 / 12\n"
              " m  memory test (generated image)      l  memory test (--load file)\n"
              " r  memory-test size                   s  status   v  debug   h  menu\n"
              " g  gap test (where a memory-test shift arises: read, write or stream)\n"
              " H  HDMI status                        k  HDMI input clock delay (if sparkles)\n"
              " a  HDMI auto re-apply of lost setup on/off    q  quit\n"
              " Switches: SW0 = SW path, SW2:1 = kernel (last change wins)")
        src = PAT_NAME[self.pattern] if (self.src_pattern or not self.cam_ok) else "camera"
        cam = self.cam.size_name() if self.cam_ok else "not found"
        print(f" Now: source {src} | display {hw.MODES[self.disp_mode][0]}, HDMI {self.hdmi_state()} | "
              f"camera {cam} | kernel {KERN_NAME[self.kern]} in {'SW' if self.path_sw else 'HW'}"
              f"{' (LLE ' + self.lle_force_name() + ')' if self.kern == 4 else ''} | "
              f"memory test {TEST_SIZES[self.test_size][0]}x{TEST_SIZES[self.test_size][1]}")
        print("----------------------------------------------------------------")

    def lle_force_name(self):
        f = LLE_FORCE[self.lle_force]
        return f"{f} iterations" if f else "auto"

    def debug_dump(self):
        z = self.z

        def hls(name, f):
            r = f.r
            c = r.rd(hw.CTRL)
            print(f"  {name:8s} ctrl 0x{c & 0xFF:02x} [start {c & 1} done {(c >> 1) & 1} idle {(c >> 2) & 1} "
                  f"ready {(c >> 3) & 1} autorestart {(c >> 7) & 1}] width {r.rd(hw.WIDTH)} "
                  f"height {r.rd(hw.HEIGHT)} stride {r.rd(hw.STRIDE)}")
        print("Debug:")
        hls("vfb_rd", z.fbr)
        hls("vfb_wr", z.fbw)
        hls("vfb_rd2", z.fbr2)
        ga = z.vtc.rd(hw.VTC_GASIZE)
        print(f"  vtc      control 0x{z.vtc.rd(0x00):08x} status 0x{z.vtc.rd(0x04):08x} "
              f"active {ga & 0x1FFF}x{(ga >> 16) & 0x1FFF}")
        print(f"  clk_wiz  locked {z.clk.rd(hw.WZ_STATUS) & 1} cfg0 0x{z.clk.rd(hw.WZ_CFG0):08x} "
              f"cfg2 0x{z.clk.rd(hw.WZ_CFG2):08x}")
        self.hdmi_status()

    def hdmi_status(self):
        if not self.z.hdmi:
            print("  hdmi     ADV7511 not found")
            return
        try:
            print("  " + self.z.hdmi.status())
        except OSError as e:
            print(f"  hdmi status read failed ({e})")

    def run(self):
        z = self.z
        print("\nzed_base jig (Python)")
        sid = z.proc.rd(SLOT_ID)
        print(f"proc_slot ID = 0x{sid:08x} (expect 0x{SLOT_ID_EXPECTED:08x}), memory test DMA present")
        if sid != SLOT_ID_EXPECTED:
            print("WARNING: older proc_slot: kernel 4 (LLE) needs the v5 slot (shell v7)")
        z.proc.wr(SLOT_CTRL, 0)

        self.disp_mode = hw.mode_index(640, 480)            # start-up: 640x480 display, VGA camera
        m = hw.MODES[self.disp_mode]
        z.set_mode(m[1], m[2])
        if z.hdmi:
            z.hdmi.poll()
            print(f"HDMI: {self.hdmi_state()}")
        else:
            print("HDMI: no ADV7511 on I2C1 (VGA only)")
        try:
            self.cam_ok = self.cam.init(3)
        except OSError as e:
            print(f"Camera init failed ({e})")
            self.cam_ok = False
        if not self.cam_ok:
            print("No camera: test patterns and memory test only")
        self.src_pattern = int(not self.cam_ok)
        if not self.reapply():
            sys.exit("Config failed")
        self.menu()

        keys = Keys()
        cam_frames = disp_flips = sw_frames = 0
        sw_time = 0.0
        t_last = t_poll = t_hdmi = now()
        last_sw, led_tog, status_now = 0xFFFFFFFF, 0, False
        try:
            while True:
                if z.fbr.done():
                    self.disp_event()

                if self.cam_w >= 0 and z.fbw.done():
                    finished = self.cam_w
                    self.cam_w = self.cam_p
                    cam_frames += 1
                    led_tog ^= 0x80
                    if not self.path_sw:
                        self.display(finished)
                        disp_flips += 1
                    else:
                        self.raw_ready = finished
                    self.cam_p = self.pick_free()
                    z.fbw.set_addr(self.cam_addr(self.cam_p))

                # HW LLE: new statistics latched -> table for their iteration count (next frame)
                if (not self.path_sw and self.kern == 4 and z.proc.rd(SLOT_STAT_SEQ) != self.lle_seq_seen
                        and not self.lut_swap_pending()):
                    self.lle_seq_seen = z.proc.rd(SLOT_STAT_SEQ)
                    st = self.slot_stats()
                    self.lle_n_last, self.lle_mean_last = self.lle_n(st), self.k.mean(st)
                    if self.lle_n_last != self.lle_n_active:
                        self.lle_load(self.lle_n_last)

                if self.path_sw and self.raw_ready >= 0:
                    self.cpu_hold, self.raw_ready = self.raw_ready, -1
                    t0 = now()
                    if self.kern:
                        off = self.buf_off(self.cpu_hold) + self.cam_off
                        self.b.invalidate(off, self.cam_span())
                        if self.kern == 1:
                            self.k.gray(self.b.addr + off, self.cw, self.ch, self.canvas_w)
                        elif self.kern == 2:
                            self.k.invert(self.b.addr + off, self.cw, self.ch, self.canvas_w)
                        elif self.kern == 3:
                            self.k.canny(self.bgrx(off, self.cw, self.ch, self.canvas_w))
                        else:                                # LLE: statistics, table, apply
                            a = self.b.addr + off
                            st = self.k.stats(a, self.cw, self.ch, self.canvas_w)
                            self.lle_n_last, self.lle_mean_last = self.lle_n(st), self.k.mean(st)
                            self.k.apply(a, self.cw, self.ch, self.canvas_w, self.k.table(self.lle_n_last))
                        self.b.flush(off, self.cam_span())
                    sw_time += now() - t0
                    sw_frames += 1
                    self.display(self.cpu_hold)
                    disp_flips += 1
                    self.cpu_hold = -1

                t_now = now()
                if t_now - t_poll > 0.02:                       # switches every ~20 ms
                    t_poll = t_now
                    sw = self.io.switches()
                    self.io.set_leds((sw & 0x7F) | led_tog)
                    if (sw & 7) != (last_sw & 7):
                        self.set_proc(sw & 1, (sw >> 1) & 3)
                    last_sw = sw

                if t_now - t_hdmi > 0.5 and z.hdmi:              # monitor plugged / unplugged
                    t_hdmi = t_now
                    if z.hdmi.poll():
                        print(f"HDMI: {self.hdmi_state()}")

                c = keys.get()
                if c:
                    nm, ns, reset_stats = len(hw.MODES), len(self.cam.sizes) or 1, True
                    if c == "t":
                        if not self.cam_ok:
                            print("No camera: test patterns only")
                        else:
                            self.src_pattern ^= 1
                            self.reapply()
                    elif c == "p":
                        self.pattern = (self.pattern + 1) % 3
                        if self.src_pattern or not self.cam_ok:
                            self.reapply()
                        else:
                            print(f"Pattern set to {PAT_NAME[self.pattern]} (press t to show it)")
                    elif c in "dD":
                        step = 1 if c == "d" else nm - 1
                        self.apply_config((self.disp_mode + step) % nm, self.cam.size_idx, True, False)
                        if z.hdmi:
                            z.hdmi.set_aspect(self.disp_w, self.disp_h)
                    elif c in "cC":
                        if not self.cam_ok:
                            print("Size change needs the OV2640")
                        else:
                            step = 1 if c == "c" else ns - 1
                            self.apply_config(self.disp_mode, (self.cam.size_idx + step) % ns, False, True)
                    elif c == "n":
                        self.lle_force = (self.lle_force + 1) % len(LLE_FORCE)
                        print(f"LLE iterations: {self.lle_force_name()}")
                        if self.kern == 4 and not self.path_sw:
                            self.lle_seq_seen = -1               # rebuild on the next statistics
                        reset_stats = False
                    elif c in "01234":
                        self.set_proc(self.path_sw, int(c))
                        reset_stats = False
                    elif c == "x":
                        self.set_proc(1 - self.path_sw, self.kern)
                        reset_stats = False
                    elif c == "r":
                        self.test_size = (self.test_size + 1) % len(TEST_SIZES)
                        print("Memory test size %dx%d" % TEST_SIZES[self.test_size])
                        reset_stats = False
                    elif c == "m":
                        self.run_mem_test(self.kern, False)
                    elif c == "l":
                        self.run_mem_test(self.kern, True)
                    elif c == "g":
                        self.run_gap_test()
                    elif c == "s":
                        status_now, reset_stats = True, False
                    elif c == "v":
                        self.debug_dump()
                        reset_stats = False
                    elif c == "H":
                        print("HDMI:")
                        self.hdmi_status()
                        reset_stats = False
                    elif c == "a" and z.hdmi:
                        z.hdmi.auto = not z.hdmi.auto
                        print(f"HDMI auto re-apply {'on' if z.hdmi.auto else 'off'}")
                        reset_stats = False
                    elif c == "k":
                        if not z.hdmi:
                            print("No ADV7511")
                        else:
                            z.hdmi.set_clock_delay(z.hdmi.delay + 1)
                            print(f"ADV7511 input clock delay {z.hdmi.delay * 400 - 1200} ps")
                        reset_stats = False
                    elif c in "h?":
                        self.menu()
                        reset_stats = False
                    elif c == "q":
                        break
                    else:
                        reset_stats = False
                    if reset_stats:
                        cam_frames = disp_flips = sw_frames = self.disp_frames = 0
                        sw_time, t_last = 0.0, now()

                camera_src = not (self.src_pattern or not self.cam_ok)
                if (t_now - t_last > 2 and camera_src) or (status_now and t_now > t_last):
                    dt = t_now - t_last
                    status_now = False
                    cyc = z.proc.rd(SLOT_CYCLES_LAST)
                    src = PAT_NAME[self.pattern] if not camera_src else self.cam.size_name()
                    line = (f"[{hw.MODES[self.disp_mode][0]} | {src} | {KERN_NAME[self.kern]} "
                            f"{'SW' if self.path_sw else 'HW'}] cam {cam_frames / dt:.2f} fps | "
                            f"flips {disp_flips / dt:.2f}/s | display {self.disp_frames / dt:.2f} fps | "
                            f"slot: {SLOT_CLK_HZ / cyc if cyc else 0:.2f} fps, {z.proc.rd(SLOT_PIXELS_LAST)} px, "
                            f"{z.proc.rd(SLOT_LINES_LAST)} lines, {z.proc.rd(SLOT_STALLS_LAST)} stall cyc")
                    if self.kern == 4 and self.lle_n_last > 0:
                        line += f" | LLE mean {self.lle_mean_last:.3f}, {self.lle_n_last} iterations"
                    if self.path_sw and sw_frames:
                        line += f" | SW kernel {sw_time / sw_frames * 1000:.2f} ms/frame"
                    print(line)
                    cam_frames = disp_flips = sw_frames = self.disp_frames = 0
                    sw_time, t_last = 0.0, t_now
        except KeyboardInterrupt:
            pass
        finally:
            keys.restore()


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description="zed_base jig, Python port")
    ap.add_argument("--common", default=os.path.join(here, "..", "common"),
                    help="folder with ov2640_regs.h, sw_kernels.c, lle_global.c (default sw/common)")
    ap.add_argument("--shim", default=os.path.join(here, "..", "linux_c", "shim"),
                    help="folder with the BSP shim headers (default sw/linux_c/shim)")
    ap.add_argument("--load", default=None, help="raw XRGB8888 image for the 'l' memory test")
    args = ap.parse_args()
    if args.load:
        args.load = os.path.abspath(args.load)
    os.chdir(here)
    Jig(args).run()


if __name__ == "__main__":
    main()
