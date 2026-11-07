# zed_base: ZedBoard video and vision test platform

A reproducible Vivado / Vitis 2024.2 base design for the ZedBoard (Zynq-7020) with a
DRAM frame buffer display (VGA and HDMI), a Pmod camera (OV2640), a stream processing slot
for hardware kernels, and a bare-metal test jig driven from a UART menu. It is the
platform for research on hardware kernels for real-time computer vision.

Everything is built from Tcl and source files: no Vivado project or IP output is stored in
the repository.

## Hardware (version 6)

```
Camera (Pmod JA/JB, OV2640, RGB565) -> dvp_capture -> async FIFO -> proc_slot -> vfb_wr --+
                                                                                          | HP1
Memory test: vfb_rd2 (HP2) -> proc_slot memory input                                      v
                                                                                        DDR3
Display: vfb_rd (HP0) -> AXI4-Stream to video out (VTC timing) -> vga_pack  -> VGA      ^
                                                                -> hdmi_pack -> ADV7511 HDMI
Pixel clock: clocking wizard, reprogrammed at run time (640x480 .. 1920x1080)
```

| Block | What it does |
|---|---|
| `rtl/dvp_capture.v` | DVP RGB565 -> AXI4-Stream RGB888, whole frames only (drops on overflow) |
| `rtl/proc_slot.v` | processing slot: kernels (pass, gray, invert), input select camera / memory, frame counters and timing over AXI-Lite |
| `rtl/vga_pack.v` | 24-bit RGB -> 12-bit VGA |
| `rtl/hdmi_pack.v` | RGB -> BT.709 YCbCr 4:2:2 for the ADV7511 |
| `rtl/clk_forward.v` | ODDR clock forwarding (camera XCLK, HDMI clock) |

Measured utilisation (v5, before HDMI, which adds well under 1 %): LUT 17 %, FF 14 %,
slices 36 %, BRAM 6 %, DSP 1 %.

## Quick start

1. Install the Avnet ZedBoard board files (bdf `zedboard/1.5`), see `Get_Started.md` section 1.
2. Build: `vivado -mode batch -source zed_base.tcl` (or Tools -> Run Tcl Script). Produces
   `zed_base/system_wrapper.xsa` with the bitstream. Switches at the top of the script:
   `enable_vga`, `enable_hdmi`, `jobs`, IP cache location.
3. Vitis Unified IDE: platform from the XSA, empty application, copy `sw/jig.c` and
   `sw/common/*` flat into `src/`, build, run. UART 115200 8N1.
4. Press `h` in the terminal for the menu: test patterns, camera sizes, display modes,
   kernels in HW or SW, memory-to-memory test, HDMI status.

Incremental upgrades of an existing project (keep synthesized IPs): `add_camera.tcl`,
`add_processing.tcl`, `add_v5.tcl`, `add_hdmi.tcl`; `refresh_rtl.tcl` after RTL edits.

## Verification

`sh sim/run_all.sh` runs every testbench (Icarus Verilog) and model check: capture framing
and overflow recovery, proc_slot bit-exact against the C golden model used by the software
kernels, the memory-test input, hdmi_pack bit-exact against its C model plus the colour
round trip through the ADV7511 matrix, and the buffer rotation model.

## Repository layout

```
zed_base.tcl, add_*.tcl, refresh_rtl.tcl   build scripts
tcl/bd_procs.tcl                           block design procs (one per subsystem)
rtl/  constr/                              RTL and constraints
sim/                                       testbenches, C / Python models, run_all.sh
sw/jig.c                                   test application (single main, UART menu)
sw/common/                                 board support, camera tables, SW kernels
sw/tools/rawimg.py                         raw frame dump <-> image conversion
Get_Started.md                             full bring-up notes, design decisions, bugs found
```

## Status

Version 6 is complete and verified on the board: all display modes on VGA and HDMI at the
same time, all OV2640 sizes, kernels in HW and SW, memory-to-memory test passing.
Bugs found during bring-up and their causes are documented in `Get_Started.md`
(section 12.8: AXI write stream offset after aborting the write DMA; section 14.5: false
ADV7511 register reads without a repeated start).

Known limitations: no interrupts (polling), no EDID (fixed mode list), no HDMI audio,
camera at 15 fps with the Linux driver tables.

Next: research kernels in the processing slot, starting with low-light enhancement.
