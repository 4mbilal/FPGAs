#!/usr/bin/env python3
"""shell_test.py - checks the shell v7 hardware from Linux (run on the ZedBoard, user in group fpga).

  python3 shell_test.py

  * finds proc_slot and accel by UIO name and physical address, checks map sizes (64 KB)
  * proc_slot: ID 0x534C0005, CAPS, interrupt through UIO (statistics latch via STAT_CTRL,
    no video needed), write-1-to-clear
  * accel stub: ID 0x41430000, scratch register, interrupt through UIO (IRQ_TEST)
Standard library only (mmap, os, select).
"""
import glob
import mmap
import os
import select
import struct
import sys


def find_uio(name, addr):
    for d in glob.glob("/sys/class/uio/uio*"):
        try:
            n = open(f"{d}/name").read().strip()
            a = int(open(f"{d}/maps/map0/addr").read(), 16)
            size = int(open(f"{d}/maps/map0/size").read(), 16)
        except OSError:
            continue
        if a == addr:                     # the address is the key; the UIO name follows the
            if not n.startswith(name):    # device-tree node (e.g. accel_stub), so it is only reported
                print(f"  note  UIO name at 0x{addr:08x} is '{n}'")
            return "/dev/" + os.path.basename(d), size
    return None, 0


class Uio:
    def __init__(self, name, addr):
        self.dev, self.size = find_uio(name, addr)
        if not self.dev:
            raise SystemExit(f"no UIO device '{name}' at 0x{addr:08x}: check the device tree (generic-uio)")
        self.fd = os.open(self.dev, os.O_RDWR | os.O_SYNC)
        self.m = mmap.mmap(self.fd, self.size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)

    def rd(self, off):
        return struct.unpack_from("<I", self.m, off)[0]

    def wr(self, off, v):
        struct.pack_into("<I", self.m, off, v)

    def irq_arm(self):
        os.write(self.fd, struct.pack("<I", 1))           # re-enable the interrupt in the kernel

    def irq_wait(self, timeout=1.0):
        r, _, _ = select.select([self.fd], [], [], timeout)
        return struct.unpack("<I", os.read(self.fd, 4))[0] if r else None


def check(cond, msg, fails):
    print(("  ok    " if cond else "  FAIL  ") + msg)
    if not cond:
        fails.append(msg)


def main():
    fails = []
    print("proc_slot (0x43D00000)")
    s = Uio("proc_slot", 0x43D00000)
    check(s.size == 0x10000, f"{s.dev} map size 0x{s.size:x} (exp 0x10000)", fails)
    check(s.rd(0x04) == 0x534C0005, f"ID 0x{s.rd(0x04):08x} (exp 0x534c0005)", fails)
    check(s.rd(0x28) == 0xF, f"CAPS 0x{s.rd(0x28):x} (exp 0xf)", fails)
    s.wr(0x3C, 3)                                         # clear stale status
    s.wr(0x38, 1)                                         # IRQ_EN: statistics latched
    s.irq_arm()
    s.wr(0x34, 1)                                         # STAT_CTRL: latch now -> interrupt
    n = s.irq_wait()
    check(n is not None, f"interrupt via UIO after STAT_CTRL (count {n})", fails)
    check(s.rd(0x3C) & 1 == 1, "IRQ_STATUS bit 0 set", fails)
    s.wr(0x3C, 1)
    check(s.rd(0x3C) & 1 == 0, "IRQ_STATUS bit 0 cleared by write-1", fails)
    s.wr(0x38, 0)

    print("accel stub (0x43E00000)")
    a = Uio("accel", 0x43E00000)
    check(a.size == 0x10000, f"{a.dev} map size 0x{a.size:x} (exp 0x10000)", fails)
    check(a.rd(0x00) == 0x41430000, f"ID 0x{a.rd(0x00):08x} (exp 0x41430000)", fails)
    a.wr(0x08, 0x12345678)
    check(a.rd(0x08) == 0x12345678, "scratch register", fails)
    a.irq_arm()
    a.wr(0x0C, 1)
    n = a.irq_wait()
    a.wr(0x0C, 0)
    check(n is not None, f"interrupt via UIO from IRQ_TEST (count {n})", fails)
    print("PASS" if not fails else f"FAIL ({len(fails)})")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
