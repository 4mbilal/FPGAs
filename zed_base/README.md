# zed_base: ZedBoard video and vision test platform

A reproducible Vivado / Vitis 2024.2 base design for the ZedBoard (Zynq-7020) with a DRAM frame
buffer display (VGA and HDMI), a Pmod camera (OV2640), a stream processing slot for hardware
kernels and a reserved accelerator window. One test jig drives it, built from the same sources
for bare metal (UART menu) and for Linux (PetaLinux 2024.2 kernel + Ubuntu 22.04), in C and Python.
It is the platform for research on hardware kernels for real-time computer vision; the current
kernel is learned low-light enhancement (global curve, LiteIE rule).

Everything is built from Tcl and source files: no Vivado project or IP output is stored.

## Hardware (shell v7)

```
Camera (Pmod JA/JB, OV2640, RGB565) -> dvp_capture -> async FIFO -> proc_slot -> vfb_wr --+
                                                                                          | HP1
Memory test: vfb_rd2 (HP2) -> proc_slot memory input                                      v
                                                                                        DDR3
Display: vfb_rd (HP0) -> AXI4-Stream to video out (VTC timing) -> vga_pack  -> VGA      ^
                                                                -> hdmi_pack -> ADV7511 HDMI
Accelerator window: accel (0x43E00000, FCLK3 200 MHz, AXI master on HP3), stub for now
```
Block diagrams of every data flow: `doc/zed_base_arch.pdf` (TikZ source `doc/zed_base_arch.tex`).

| Block | What it does |
|---|---|
| `rtl/dvp_capture.v` | DVP RGB565 -> AXI4-Stream RGB888, whole frames only (drops on overflow) |
| `rtl/proc_slot.v` | processing slot v5 (ID 0x534C0005, 64 KB window, interrupt, 3-stage pipeline at 1 pixel/clock): pass, gray, invert, per-channel curve table (global low-light enhancement, double-banked, swap at frame start), frame statistics (16-bin luma histogram, channel sums), camera / memory input, counters |
| `rtl/accel_stub.v` | placeholder in the reserved accelerator window (ID 0x41430000, scratch, interrupt test) |
| `rtl/vga_pack.v` | 24-bit RGB -> 12-bit VGA |
| `rtl/hdmi_pack.v` | RGB -> BT.709 YCbCr 4:2:2 for the ADV7511 |
| `rtl/clk_forward.v` | ODDR clock forwarding (camera XCLK, HDMI clock) |

v7 on the xc7z020: timing met (WNS +0.293 ns), LUT 19.5 %, FF 15.6 %, BRAM 7.1 %, DSP 1.8 %;
proc_slot 810 LUT, 1,605 FF, 3 RAMB18, 0 DSP.

## Quick start

| Step | Command |
|---|---|
| Board files | Avnet `zedboard/1.5`, see `doc/Get_Started.md` section 1 |
| Hardware | `vivado -mode batch -source zed_base.tcl` -> `zed_base/system_wrapper.xsa` |
| Simulation | `sh sim/run_all.sh`, `python3 sim/test_jig_lle.py` |
| Bare metal | Vitis: platform from the XSA, empty app, everything in `sw/common/` flat in `src/`; UART 115200 |
| Linux image | once per shell: `doc/Linux_Guide.md` with `linux/system-user.dtsi` |
| Shell check | `python3 tools/shell_check.py check <xsa> tools/shell_v7.json` |
| New bitstream | `sh tools/make_bin.sh <bit>` (PC), `sudo sh tools/load_bitstream.sh <bin>` (board) |
| Linux C jig | `cd sw/linux_c && make && sudo ./jig` |
| Linux Python jig | `sudo python3 sw/python/jig.py` |
| Shell test | `sudo python3 tools/shell_test.py` |

## The test jig

Same keys everywhere (`h` prints the menu with the current state):

| Key | Action |
|---|---|
| `t`, `p` | camera / test pattern, next pattern |
| `d`/`D`, `c`/`C` | display mode, camera size |
| `0`..`4` | kernel: none, gray, invert, edges (Sobel bare metal, OpenCV Canny on Linux, CPU only), low-light enhancement |
| `x` | kernel in HW (proc_slot) or SW (CPU) |
| `n` | low-light iteration count: auto (LiteIE rule from the frame statistics) or forced 2 / 4 / 8 / 12 |
| `m`, `l`, `r` | memory test with a generated / loaded image (bit-exact against the C golden model), size |
| `g` | gap test (stride handling of the DMAs) |
| `s`, `v`, `H`, `k`, `a` | status, debug dump, HDMI status, HDMI clock delay, HDMI auto re-apply |
| switches | SW0 = SW path, SW2:1 = kernel 0..3 |

Low-light enhancement in HW: proc_slot latches the statistics of every frame; the CPU picks the
iteration count, builds the 3 x 256 table (`sw/common/lle_global.c`), writes it to the inactive
bank and requests the swap, which takes effect at the next start of frame.

## Repository layout

```
zed_base.tcl                 build script (calls tcl/bd_procs.tcl)
tcl/  rtl/  constr/          block design procs, RTL, constraints
sim/                         testbenches, C / Python models, run_all.sh, test_jig_lle.py
sw/common/                   ONE copy of the shared C: jig.c, zb_platform.*, ov2640_regs.h,
                             sw_kernels.* and lle_global.* (golden models, also build on a PC)
sw/linux_c/                  Linux C build of sw/common: Makefile, BSP shim, Xilinx drivers
sw/python/                   Linux Python: hw.py (hardware layer), jig.py, small tests
tools/                       shell_check.py, shell_test.py, make_bin.sh, load_bitstream.sh,
                             rawimg.py, batch.tcl (bare-metal datasets over xsct), shell_v7.json
linux/                       PetaLinux device tree fragment, udev rules
doc/                         Get_Started.md (bring-up log), Linux_Guide.md, zed_base_arch.tex/.pdf
```

## Status

- v6: verified on the board (bare metal and Linux): all display modes on VGA and HDMI, all OV2640
  sizes, kernels in HW and SW, memory test bit-exact.
- v7 shell: frozen and verified on the board under Linux (`shell_test.py` PASS); timing-clean build.
- Low-light enhancement (kernel 4): bit-exact in simulation and in the offline jig test; to be
  confirmed on the board with the merged jig.
- Next: the spatial mode (curve map at 1/8 resolution by a small CNN) inside proc_slot, and the
  accelerator in the reserved window, both bitstream-only updates.

Bugs found during bring-up and their causes are documented in `doc/Get_Started.md` and
`doc/Linux_Guide.md` section 8.
