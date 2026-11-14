#!/usr/bin/env python3
"""display_test.py - test pattern on VGA + HDMI through vfb_rd (1024x768@60).

  python3 display_test.py          colour bars + grey ramp + border
  python3 display_test.py --anim   also rotate 6 buffers with a moving square (tear-free check)
"""
import sys
import time

import numpy as np

import hw


def pattern(f, square_x=None):
    h, w = f.shape
    bars = np.array([0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00, 0xFF00FF, 0xFF0000, 0x0000FF, 0x000000],
                    dtype=np.uint32)
    img = np.empty((h, w), dtype=np.uint32)
    img[: h * 2 // 3] = bars[(np.arange(w) * 8) // w]
    ramp = (np.arange(w) * 255 // (w - 1)).astype(np.uint32)
    img[h * 2 // 3:] = ramp * 0x010101
    img[:4, :] = img[-4:, :] = img[:, :4] = img[:, -4:] = 0xFFFFFF
    if square_x is not None:
        img[h // 3: h // 3 + 96, square_x: square_x + 96] = 0xFF8000
    f[:] = img                    # one bulk write into the uncached buffer


def main():
    z = hw.Zed()
    z.set_mode(1024, 768)
    f0 = z.frame(0)
    pattern(f0)
    z.fbr.configure(z.frame_phys(0), hw.DISP_W, hw.DISP_H, hw.DISP_STRIDE)
    time.sleep(0.2)
    z.dump()

    if "--anim" in sys.argv:
        nbuf = 6
        print("Animating across 6 buffers; Ctrl-C to stop")
        x, n, frames, t0 = 0, 0, 0, time.monotonic()
        try:
            while True:
                n = (n + 1) % nbuf
                pattern(z.frame(n), square_x=x)
                while not z.fbr.done():          # wait for a frame boundary
                    time.sleep(0.0005)
                z.fbr.set_addr(z.frame_phys(n))   # shown from the next frame
                x = (x + 8) % (hw.DISP_W - 96)
                frames += 1
                if frames % 60 == 0:
                    print(f"  {frames / (time.monotonic() - t0):.1f} buffer swaps/s")
        except KeyboardInterrupt:
            pass
    else:
        # Stay alive: the EMIO reset lines are held by this process, and the frame buffer
        # cores go back into reset when it exits.
        print("Pattern on screen. Ctrl-C to exit")
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
