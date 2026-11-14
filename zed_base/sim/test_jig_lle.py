#!/usr/bin/env python3
"""test_jig_lle.py - offline check of the low-light parts of sw/python/jig.py.

proc_slot v5 is replaced by a register-level model (curve-table banks with the swap at the
start of frame, statistics latched per frame or on STAT_CTRL, mode 3 applying the active bank),
the DMAs by instant copies. Checks: the two-pass memory test reports PASS with identical
statistics; the live HW loop loads the LiteIE iteration count, honours the forced count ('n')
and never writes a table while a swap is pending; the SW path matches the C model.
  python3 sim/test_jig_lle.py          (from the repository root; needs gcc and numpy)
"""
import ctypes
import os
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "sw", "python"))
sys.modules["hw"] = type(sys)("hw")                 # jig.py imports hw; not used here
import jig  # noqa: E402

J = jig


class Slot:
    """Register model of proc_slot v5 (rtl/proc_slot.v)."""

    def __init__(self):
        self.lut = np.zeros((2, 3, 256), np.uint8)
        self.bank, self.pend, self.ctrl, self.seq = 0, 0, 0, 0
        self.run = self.lat = None
        self.px = self.lines = 0
        self.writes_while_pending = 0

    def rd(self, off):
        if off == J.SLOT_LUT_BANK:
            return (self.bank << 1) | self.pend
        if off == J.SLOT_STAT_SEQ:
            return self.seq
        if off in (J.SLOT_PIXELS_RUN,):
            return self.px
        if off == J.SLOT_LINES_RUN:
            return self.lines
        if off == J.SLOT_ACTIVE_CYCLES:
            return self.px + 10
        if off == J.SLOT_ID:
            return J.SLOT_ID_EXPECTED
        if 0x1000 <= off < 0x1080 and self.lat:
            cnt, sums, hist = self.lat
            if off == J.SLOT_STAT_COUNT:
                return cnt
            if J.SLOT_SUM_R <= off < J.SLOT_SUM_R + 12:
                return sums[(off - J.SLOT_SUM_R) // 4]
            if J.SLOT_HIST0 <= off < J.SLOT_HIST0 + 64:
                return hist[(off - J.SLOT_HIST0) // 4]
        return 0

    def wr(self, off, v):
        if off == J.SLOT_CTRL:
            self.ctrl = v
        elif off == J.SLOT_LUT_BANK and v & 1:
            self.pend = 1
        elif off == J.SLOT_STAT_CTRL and v & 1:
            self.lat, self.seq = self.run, self.seq + 1
        elif 0x400 <= off < 0x1000:
            if self.pend:
                self.writes_while_pending += 1
            c, x = (off - 0x400) // 0x400, ((off - 0x400) % 0x400) // 4
            self.lut[1 - self.bank, c, x] = v & 0xFF

    def frame(self, img):
        """One frame through the slot (img: (h, w) uint32 XRGB). Returns the output."""
        if self.pend:                                   # swap at the start of frame
            self.bank ^= 1
            self.pend = 0
        if self.run is not None:                        # previous frame's set is latched
            self.lat, self.seq = self.run, self.seq + 1
        r, g, b = (img >> 16) & 0xFF, (img >> 8) & 0xFF, img & 0xFF
        y = (77 * r + 150 * g + 29 * b + 128) >> 8
        hist = np.bincount((y >> 4).ravel(), minlength=16)
        self.run = (img.size, [int(r.sum()), int(g.sum()), int(b.sum())], [int(k) for k in hist])
        self.px, self.lines = img.size, img.shape[0]
        mode = self.ctrl & 3
        if mode == 3:
            t = self.lut[self.bank]
            return (t[0][r].astype(np.uint32) << 16) | (t[1][g].astype(np.uint32) << 8) | t[2][b]
        if mode == 0:
            return img.copy()
        raise ValueError(mode)


class Buf:
    def __init__(self, size):
        self.arr = np.zeros(size, np.uint8)
        self.base, self.mm, self.size = self.arr, self.arr, size
        self.addr = self.arr.ctypes.data
        self.phys = 0x30000000

    def flush(self, off, n):
        pass

    invalidate = flush


class Dma:
    def __init__(self, test):
        self.t = test

    def stop(self):
        pass

    def done(self):
        return True


def make_jig(slot, w, h):
    j = object.__new__(J.Jig)
    j.k = J.Models(os.path.join(ROOT, "sw", "common"), os.path.join(ROOT, "sw", "linux_c", "shim"))
    j.b = Buf(J.TEST_END)
    j.u32 = np.frombuffer(j.b.arr, dtype=np.uint32)
    j.test_ok, j.load, j.test_size = True, None, 0
    j.live_hw_mode, j.path_sw, j.kern = 0, 0, 0
    j.lle_seq_seen, j.lle_n_active, j.lle_n_last, j.lle_mean_last, j.lle_force = 0, -1, -1, 0.0, 0
    j.disp_w = j.disp_h = 10 ** 6                      # no display of the result
    j.reapply = lambda: True
    j.debug_dump = lambda: None

    class Z:
        pass
    z = j.z = Z()
    z.proc = slot

    def mem_dma_run(ww, hh, stride, hw_mode):        # instant: TEST_IN -> slot -> TEST_OUT
        slot.wr(J.SLOT_CTRL, J.SLOT_SRC_MEM | hw_mode)
        inp = j.pix(J.TEST_IN, ww, hh, stride)
        j.pix(J.TEST_OUT, ww, hh, stride)[:] = slot.frame(np.array(inp))
        return J.now()
    j.mem_dma_run = mem_dma_run
    z.fbw = z.fbr2 = Dma(j)
    return j


def main():
    fails = 0
    w, h = J.TEST_SIZES[0]
    # 1. memory test, generated image and a brighter variant
    for name, scale in (("dark", None), ("mid", 1)):
        slot = Slot()
        j = make_jig(slot, w, h)
        if scale is not None:
            j.test_image = staticmethod(lambda ww, hh: (J.Jig.test_image(ww, hh) & 0x7F7F7F) << 1)
        import io
        import contextlib
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            j.lle_mem_test(False)
        out = buf.getvalue()
        ok = "PASS" in out and "identical" in out
        fails += not ok
        print(f"memory test ({name}): {'ok' if ok else 'FAIL'} | " + " | ".join(l.strip() for l in out.splitlines()[1:4]))

    # 2. live HW loop: frames of different brightness, auto and forced counts
    slot = Slot()
    j = make_jig(slot, w, h)
    rng = np.random.default_rng(1)
    j.set_proc(0, 4)
    assert slot.ctrl == 3
    seen = []
    for f in range(12):
        lvl = [20, 20, 60, 60, 120, 120, 200, 200, 30, 30, 30, 30][f]
        img = rng.integers(0, lvl, size=(h, w, 3)).astype(np.uint32)
        img = (img[..., 0] << 16) | (img[..., 1] << 8) | img[..., 2]
        out = slot.frame(img)
        if f == 8:
            j.lle_force = 3                              # 'n' -> 8 iterations
            j.lle_seq_seen = -1
        # the jig's per-loop step (as in run())
        if j.z.proc.rd(J.SLOT_STAT_SEQ) != j.lle_seq_seen and not j.lut_swap_pending():
            j.lle_seq_seen = j.z.proc.rd(J.SLOT_STAT_SEQ)
            st = j.slot_stats()
            j.lle_n_last, j.lle_mean_last = j.lle_n(st), j.k.mean(st)
            if j.lle_n_last != j.lle_n_active:
                j.lle_load(j.lle_n_last)
        seen.append(j.lle_n_last)
    print(f"live loop iteration counts per frame: {seen}")
    ok = slot.writes_while_pending == 0 and seen[-1] == 8 and len(set(seen[:8])) > 1
    fails += not ok
    print(f"live loop: {'ok' if ok else 'FAIL'} (table writes while a swap was pending: {slot.writes_while_pending}; "
          f"forced 8 honoured: {seen[-1] == 8})")

    # 3. table in the active bank matches the C model for the last count
    t = j.k.table(8)
    ok = all((slot.lut[slot.bank][c] == np.frombuffer(bytes(t[c]), np.uint8)).all() for c in range(3))
    fails += not ok
    print(f"active table = lle_build_table(8): {'ok' if ok else 'FAIL'}")

    # 4. SW path: apply through ctypes equals numpy table lookup
    img = rng.integers(0, 1 << 24, size=(h, w)).astype(np.uint32)
    a = img.copy()
    st = j.k.stats(a.ctypes.data, w, h, w)
    n = j.k.iters(st)
    tab = j.k.table(n)
    j.k.apply(a.ctypes.data, w, h, w, tab)
    T = [np.frombuffer(bytes(tab[c]), np.uint8).astype(np.uint32) for c in range(3)]
    exp = (T[0][(img >> 16) & 0xFF] << 16) | (T[1][(img >> 8) & 0xFF] << 8) | T[2][img & 0xFF]
    ok = ((a & 0xFFFFFF) == exp).all()
    fails += not ok
    print(f"SW path (stats, n={n}, apply): {'ok' if ok else 'FAIL'}")
    print("PASS" if not fails else f"FAIL ({fails})")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
