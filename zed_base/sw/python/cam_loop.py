#!/usr/bin/env python3
"""cam_loop.py - OV2640 640x480 -> proc_slot -> vfb_wr -> DRAM -> vfb_rd -> VGA/HDMI.

Zero-copy, as jig.c: the camera DMA writes straight into a 640x480 window of the display
buffer (display stride), so no CPU work is needed per frame.

  python3 cam_loop.py                 run and print camera frame rate
  python3 cam_loop.py --snap out.png  also save one camera frame with OpenCV
"""
import os
import sys
import time

import numpy as np

import hw

TABLE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "common", "ov2640_regs.h")


def main():
    z = hw.Zed()
    z.set_mode(1024, 768)

    disp = z.frame(0)
    disp[:] = 0x202020                                       # dark grey background
    z.fbr.configure(z.frame_phys(0), hw.DISP_W, hw.DISP_H, hw.DISP_STRIDE)

    cam = hw.Camera(bus=0)
    cam.init_vga(TABLE)

    z.fbw.configure(z.frame_phys(0) + hw.CAM_WIN_OFF, hw.CAM_W, hw.CAM_H, hw.DISP_STRIDE)
    time.sleep(0.3)
    z.dump()

    if "--snap" in sys.argv:
        import cv2
        path = sys.argv[sys.argv.index("--snap") + 1]
        win = z.buf.bgrx(z.frame_off(0) + hw.CAM_WIN_OFF, hw.CAM_W, hw.CAM_H, hw.DISP_STRIDE)
        cv2.imwrite(path, np.array(win[..., :3]))           # copy out of the uncached buffer
        print("saved", path)

    print("Camera frames/s (Ctrl-C to stop; the video keeps running after exit):")
    n, t0 = 0, time.monotonic()
    try:
        while True:
            if z.fbw.done():
                n += 1
            else:
                time.sleep(0.001)
            dt = time.monotonic() - t0
            if dt >= 2.0:
                print(f"  {n / dt:.1f} fps")
                n, t0 = 0, time.monotonic()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
