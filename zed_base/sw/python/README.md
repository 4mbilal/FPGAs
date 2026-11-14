# sw/python: Python access to zed_base under Linux

Userspace port of `zb_platform.c`: UIO for the PL IP, u-dma-buf for frame buffers,
libgpiod for the EMIO resets, `/dev/i2c-0` (OV2640) and `/dev/i2c-1` (ADV7511).
Shared C sources come from `../common` (OV2640 tables, golden models), found relative to
these files, so the scripts run from anywhere in the repository.

## Setup (on the board)
```bash
sudo apt install -y python3-numpy python3-opencv python3-libgpiod python3-smbus2 gcc || pip3 install smbus2
```
The `zed` user must be in the `fpga` group (`linux/99-fpga.rules`); otherwise run with `sudo`.

## Run
```bash
sudo python3 sw/python/jig.py                   # full test jig, same keys as the C jig ('h')
sudo python3 sw/python/jig.py --load img.raw    # 'l' memory test with a raw XRGB8888 image
sudo python3 sw/python/display_test.py          # colour bars on VGA + HDMI (--anim: buffer rotation)
sudo python3 sw/python/cam_loop.py              # camera window, fps printout (--snap a.png)
```
Low-light enhancement is kernel 4: `4` on, `0` off, `x` HW (proc_slot) / SW (CPU), `n`
iteration count auto (LiteIE rule) / forced 2 / 4 / 8 / 12, `m`/`l` memory test bit-exact against
`lle_global.c`, `s` status with the frame mean and iteration count.

## Files
| File | Content |
|---|---|
| `hw.py` | All hardware access (port of `zb_platform.c`): `Zed`, `FrameBuf`, `Camera`, `Hdmi`, display modes |
| `jig.py` | Full port of `jig.c`; `sw_kernels.c` + `lle_global.c` built once into `libzbmodels.so` (ctypes, bit-exact); kernel 3 is OpenCV Canny |
| `display_test.py` | Display path test |
| `cam_loop.py` | Camera to display loop (zero-copy, as `jig.c`) |

`sim/test_jig_lle.py` checks the low-light parts of `jig.py` offline against a register model of
proc_slot v5 (no board needed).

Do not run together with the C `jig`: both drive the same hardware.
