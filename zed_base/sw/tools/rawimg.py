#!/usr/bin/env python3
"""rawimg.py - convert between PNG/JPG and the jig's raw XRGB8888 frame format.

Raw format: width*height 32-bit little-endian words, 0x00RRGGBB per pixel (bytes B, G, R, 0),
rows packed (stride = width*4). This is what vfb_rd2 reads at 0x10000000 and what the
jig writes to 0x11000000 (HW result) and 0x12000000 (SW reference).

  python rawimg.py to-raw   photo.png  input.bin  --size 1920x1080
  python rawimg.py from-raw result.bin result.png --size 1920x1080
  python rawimg.py diff     result.bin ref.bin    --size 1920x1080   (count differing pixels)

Load / read back with xsct (Vitis):
  xsct% connect; targets -set -filter {name =~ "*A9*#0"}
  xsct% dow -data input.bin 0x10000000
  xsct% mrd -bin -file result.bin 0x11000000 [expr 1920*1080]
Requires Pillow and NumPy: pip install pillow numpy
"""
import argparse
import numpy as np
from PIL import Image


def size(s):
    w, h = s.lower().split("x")
    return int(w), int(h)


def to_raw(src, dst, wh):
    img = Image.open(src).convert("RGB").resize(wh, Image.BILINEAR)
    a = np.asarray(img, dtype=np.uint8)
    out = np.zeros((wh[1], wh[0], 4), dtype=np.uint8)
    out[..., 0] = a[..., 2]   # B
    out[..., 1] = a[..., 1]   # G
    out[..., 2] = a[..., 0]   # R
    out.tofile(dst)


def load_raw(path, wh):
    a = np.fromfile(path, dtype=np.uint8, count=wh[0] * wh[1] * 4).reshape(wh[1], wh[0], 4)
    return np.stack([a[..., 2], a[..., 1], a[..., 0]], axis=-1)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("cmd", choices=["to-raw", "from-raw", "diff"])
    p.add_argument("a")
    p.add_argument("b")
    p.add_argument("--size", type=size, required=True, help="WxH, e.g. 1920x1080")
    args = p.parse_args()
    if args.cmd == "to-raw":
        to_raw(args.a, args.b, args.size)
    elif args.cmd == "from-raw":
        Image.fromarray(load_raw(args.a, args.size)).save(args.b)
    else:
        x, y = load_raw(args.a, args.size), load_raw(args.b, args.size)
        n = int(np.any(x != y, axis=-1).sum())
        print(f"{n} of {args.size[0] * args.size[1]} pixels differ")


if __name__ == "__main__":
    main()
