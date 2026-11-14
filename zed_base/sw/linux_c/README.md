# zed_base jig on Linux (C, same code as bare metal)

Runs the bare-metal `jig.c` + `zb_platform.c` from `../common` as a Linux process, together with
the **unmodified Xilinx drivers** (VTC, Video Frame Buffer Read/Write, video_common) from
embeddedsw `xilinx_v2024.2`. A small shim replaces the standalone BSP:

| Bare-metal BSP | Linux back end (`shim/zb_linux.c`) |
|---|---|
| `Xil_In32/Out32` (physical address) | UIO mappings of the PL IP, found by address in `/sys/class/uio` |
| frame buffer address registers | CPU pointer into `/dev/udmabuf0` converted to the physical address |
| `Xil_DCacheFlushRange/InvalidateRange` | u-dma-buf `sync_for_device` / `sync_for_cpu` (buffer mapped cached) |
| A9 global timer (`zb_time`) | emulated from `CLOCK_MONOTONIC` |
| `XGpio`, `XGpioPs` (EMIO resets 54-56) | libgpiod v1 (`41200000.gpio`, `41210000.gpio`, `zynq_gpio`) |
| `XIicPs` (OV2640, ADV7511) | `/dev/i2c-0`, `/dev/i2c-1` (repeated start kept for ADV7511 reads) |
| UART menu | terminal keys (serial console or SSH) |

`jig.c` is the same file as on bare metal. The Makefile defines `ZB_LINUX`, which selects the
Linux parts: buffers in the u-dma-buf region (live buffers at the start, memory-test buffers after
them), OpenCV Canny as the CPU edge kernel, `ZB_IMAGE` as the input of the `l` memory test, and no
DRAM mailbox. (`__linux__` cannot be used: the Xilinx drivers are compiled with `-U__linux__`.)

## Layout
```
Makefile  README.md
shim/     BSP replacement headers, zb_linux.c, zb_xlnx_cfg.c (driver config tables), zb_cv.cpp (Canny)
xlnx/     Xilinx drivers, embeddedsw xilinx_v2024.2 (MIT licence, unmodified)
../common jig.c zb_platform.c/.h ov2640_regs.h sw_kernels.c/.h lle_global.c/.h (shared with bare metal)
```

## Build and run (on the board)
```bash
sudo apt install -y build-essential libgpiod-dev libopencv-dev
cd ~/zed_base/sw/linux_c && make
sudo ./jig            # keys as on the UART: h menu, t source, d/D display mode, c/C camera size, ...
sudo ZB_IMAGE=$HOME/img_1920x1080.raw ./jig     # 'l': memory test with a raw XRGB8888 image
```
Kernel 4 is low-light enhancement (global curve): `4` on, `0` off, `x` HW / SW, `n` iteration
count auto / forced. `make libzbmodels.so` builds the golden models for Python (the Python jig
also builds it on demand).

Do not run the Python jig or `hw.py` scripts at the same time (both drive the same hardware).

## Software kernel 3: OpenCV Canny
Kernel 3 on the CPU path is OpenCV `cv::Canny` (`shim/zb_cv.cpp`) instead of the C Sobel:
key `3` then `x` (or SW2:1 = 11, SW0 = 1). Grey conversion, 3x3 Gaussian blur, Canny,
written back in place. Thresholds without rebuilding:
`sudo ZB_CANNY_LO=30 ZB_CANNY_HI=90 ./jig`; `ZB_CANNY_BLUR=0` skips the blur.
proc_slot has no Canny, so the HW path passes the image through for kernel 3 (as for Sobel).

## Without sudo
The `fpga` group already covers `/dev/uio*`, `/dev/udmabuf0`, `/dev/gpiochip*`, `/dev/i2c-*`.
The u-dma-buf cache-sync attributes in sysfs also need write access: install
`linux/99-fpga.rules` in `/etc/udev/rules.d/`, then reboot (or `sudo udevadm trigger`).

## u-dma-buf size
- 64 MB: live video (6 buffers of up to 1920x1200) works; memory and gap tests are disabled.
- 128 MB: everything, including the memory/gap tests. In `system-user.dtsi`:
  `size = <0x08000000>;` then `petalinux-build` and copy the new `image.ub` (and `BOOT.BIN`).
  CMA is 256 MB (`cma=256M`), so this fits.

## Not carried over
- DRAM mailbox batch mode (bare metal, xsct): under Linux, datasets are read from files.
- Timing numbers: `zb_time()` is now Linux time; Linux scheduling adds jitter to end-to-end
  times. The proc_slot cycle counters are unaffected.
