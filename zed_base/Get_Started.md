# ZedBoard Bring-up with Vivado / Vitis 2024.2

## 1. Install the board files

ZedBoard is not listed in Vivado 2024.2 by default. Board files are no longer bundled with Vivado.

- Option A: Tools → Vivado Store → Boards → refresh → install ZedBoard.
- Option B: use the Avnet board files (`Avnet/bdf`, `zedboard/1.5`), from GitHub or the copy in the `bdf` folder.
  Copy the `zedboard/` folder into `<Vivado>/data/boards/board_files/`, or point Vivado at it with `set_param board.repoPaths <folder>`.

Use one board-file source only. Avnet `zedboard/1.5` is preferred over Digilent `zedboard/1.3`. They use different board IDs, so mixing them across projects causes confusion.

The board files provide:
- `preset.xml`: the PS7 configuration (DDR3 timing and board delays, MIO, clocks). This is the main reason to use them.
- `part0_pins.xml`: pin locations and I/O standards for the board interfaces (LEDs, switches, buttons, 100 MHz clock, FMC).

### 1.1 Why use the board files, and not the old BSP or a bare part

There are three ways to start, and only one is recommended:

- **Old ZedBoard BSP / reference designs (not recommended).**
  These date from the ISE/XPS and early PetaLinux era (2012–2014).
  Migrating them to 2024 tools costs more than building a fresh block design, and they hide the PS configuration.
- **Bare part `xc7z020clg484-1` without board files (possible, but risky).**
  Every PS7 setting must then be entered by hand in the ZYNQ7 GUI:
  - DDR3: Micron MT41K128M16 HA-15E, 32-bit bus, plus the per-lane DQS-to-CLK delays and board trace delays from Table 2 of the hardware user guide. DQS2 and DQS3 delays are slightly negative in the guide, so enter 0.
  - MIO bank 0 at 3.3 V and MIO bank 1 at 1.8 V.
  - Quad-SPI flash, Ethernet (RGMII), USB0, SD0, UART1 on MIO 48–49.
  - PS input clock 33.333 MHz.

  This is doable, but a wrong DDR parameter does not fail cleanly.
  It shows up as intermittent boot failures or memory corruption, which are very hard to debug.
- **New project with the board files (recommended).**
  "Apply Board Preset" loads all of the above from `preset.xml` in one click, and the board interfaces supply LED, switch, button and clock pins.
  The rest of the design is still built from scratch, so nothing is hidden and nothing in the board files restricts the design.

## 2. Create the project

Two equivalent routes: the GUI steps in sections 2–4, or the Tcl script in section 2.1.

### 2.1 Alternative: build everything with `zed_base.tcl`

`zed_base.tcl` replaces sections 2, 3 and 4 (project creation, block design, bitstream, XSA export).
It is reproducible and can be kept under git, which suits a lab setup.

Folder layout:
```
zed_base/
  zed_base.tcl          fresh build of the current version (v6); switches enable_vga / enable_hdmi
  add_camera.tcl        incremental: v2 (VGA) -> v3 (camera)
  add_processing.tcl    incremental: v3 -> v4 (processing slot)
  add_v5.tcl            incremental: v4 -> v5 (display modes, camera sizes, memory test)
  add_hdmi.tcl          incremental: v5 -> v6 (HDMI output through the ADV7511)
  refresh_rtl.tcl       rebuild an existing project after rtl/*.v changes
  tcl/bd_procs.tcl      block design procs (build_ps, build_gpio, build_video, build_camera,
                        build_vga, build_hdmi, build_processing, build_display_reconfig,
                        build_memtest, build_outputs)
  rtl/vga_pack.v        24-bit RGB -> 12-bit VGA
  rtl/hdmi_pack.v       24-bit RGB -> BT.709 YCbCr 4:2:2, 16-bit ADV7511 bus
  rtl/dvp_capture.v     DVP RGB565 -> AXI4-Stream RGB888 (PCLK domain)
  rtl/clk_forward.v     ODDR clock forwarding (camera XCLK, HDMI HD-CLK)
  rtl/proc_slot.v       stream processing slot: input select, kernels, counters, AXI-Lite
  constr/vga.xdc        VGA pins
  constr/hdmi.xdc       ADV7511 pins (data, syncs, clock, I2C)
  constr/camera.xdc     camera pins, PCLK clock, SCCB pull-ups
  sim/tb_dvp_capture.v  capture framing and overflow recovery
  sim/tb_proc_slot.v    slot kernels, backpressure, counters, AXI-Lite (dumps vectors)
  sim/tb_proc_slot_mem.v slot memory input: source select, draining, 1 pixel/clock
  sim/check_vectors.c   RTL vs C golden model cross-check
  sim/tb_hdmi_pack.v    hdmi_pack raster test (dumps vectors)
  sim/check_hdmi.c      hdmi_pack C golden model + colour round trip through the ADV7511 CSC
  sim/rotation_model.py buffer rotation model (both DMAs + polling loop)
  sw/jig.c              the only test app: one main(), everything from a UART menu
                        (test patterns, camera, kernels HW/SW, modes, memory test)
  sw/common/            zb_platform.c/.h (board bring-up), sw_kernels.c/.h (golden
                        model), ov2640_regs.h (all OV2640 sizes)
  sw/tools/rawimg.py    PNG <-> raw XRGB8888 for memory tests over JTAG
```

It creates:
- Zynq PS7 with the ZedBoard preset (DDR3, clocks, MIO), UART1 on MIO 48–49 (J14) at 115200 baud.
- FCLK0 100 MHz (AXI-Lite), FCLK1 150 MHz (video/DMA), S_AXI_HP0 64-bit, IRQ_F2P, one EMIO GPIO.
- `axi_gpio_ledsw`: dual-channel AXI GPIO, channel 1 = LEDs (8 out), channel 2 = switches (8 in).
- `axi_gpio_btns`: buttons (5 in).
- VGA frame buffer pipeline (section 9).
- OV7670 camera capture to DRAM (section 10).
- Bitstream, timing and utilization reports, then `system_wrapper.xsa` with the bitstream included.

Run it:
- GUI: Tools → Run Tcl Script → `zed_base.tcl`.
- CLI: `vivado -mode batch -source zed_base.tcl` (optionally `-tclargs <output_dir>`; default is `zed_base/zed_base/`).

Notes:
- If the board files are not under `<Vivado>/data/boards/board_files`, uncomment the `set_param board.repoPaths` line at the top of the script.
- Pixel clock (`pix_mhz`) and frame buffer limits are variables at the top of the script.
- v1 of the script (three separate GPIOs, no video) was verified on 2024.2. The v2 video additions have not yet been run; the IP `CONFIG.*` names and pin names of `v_frmbuf_rd`, `v_tc` and `v_axi4s_vid_out` are the most likely places for a first-run error.

#### Why LEDs and switches share one AXI GPIO
Each AXI-Lite slave costs one `axi_gpio` instance plus one more port on the AXI interconnect.
Merging saves on the order of a few hundred LUTs/FFs, under 1% of the 7020's 53,200 LUTs.
The more practical gains are one address range and one driver instance.
Buttons stay on a separate GPIO so an interrupt can be added later without touching the LEDs/switches.

#### Vitis macro names come from the block design instance names
In the 2024 SDT flow, `xparameters.h` macros are derived from the BD instance names, not from creation order:

| BD instance | Macro |
|---|---|
| `axi_gpio_ledsw` | `XPAR_AXI_GPIO_LEDSW_BASEADDR` |
| `axi_gpio_btns` | `XPAR_AXI_GPIO_BTNS_BASEADDR` |
| `vfb_rd` | `XPAR_VFB_RD_BASEADDR` |
| `vtc` | `XPAR_VTC_BASEADDR` |

A GUI-built design with default names gives `XPAR_AXI_GPIO_0_BASEADDR`, `XPAR_AXI_GPIO_1_BASEADDR`, etc.
Always check the platform's `xparameters.h`.

After the script finishes, continue at section 5 (Vitis) with the generated `.xsa`.

### 2.2 GUI route

Create an RTL project without adding sources. Select ZedBoard on the Boards tab.

## 3. Block design

1. Create Block Design, add ZYNQ7 Processing System.
2. Run Block Automation with "Apply Board Preset" ticked.
3. From the Board tab, drag `leds_8bits`, `sws_8bits` and `btns_5bits` onto the design (or add AXI GPIO and use Connect Board Component).
4. Run Connection Automation.
5. Validate the design, then Create HDL Wrapper ("let Vivado manage").
6. Generate Bitstream.

## 4. Export hardware

File → Export → Export Hardware → tick "Include bitstream". This produces the `.xsa`.

## 5. Vitis (Unified IDE)

Vitis has been the Unified IDE since 2023.2. Older SDK / Vitis Classic tutorials will not match the menus.

### 5.1 Create Platform Component
- Hardware design: the exported `.xsa`. With the bitstream included, Vitis programs the PL for you.
- Operating system: `standalone`.
- Processor: `ps7_cortexa9_0`.
- Generate Boot artifacts: leave ticked. This builds the FSBL, needed later for SD boot.
- Finish, then Build the platform from the Flow panel (bottom left).

### 5.2 Create Embedded Application
- Choose the platform just built.
- Pick the Hello World template.
- Build the platform first, then the application.

If the build reports `xil_printf.h` not found, the platform BSP has not been built yet.
Build the platform, then Clean and Build the application.

### 5.3 Run on the board
- Boot mode: JTAG, i.e. MIO[5:3] jumpers JP8–JP10 = 000. At minimum remove the SD card, otherwise the SD image and the Vitis download fight each other.
- USB: connect both J17 (JTAG) and J14 (UART).
- Serial terminal: 115200 baud, 8N1 (Vitis terminal, PuTTY or Tera Term).
- Run: Flow panel → Run. Vitis programs the bitstream, runs `ps7_init` (DDR, clocks), loads the ELF and starts it.

When "Hello World" prints, DDR, UART and JTAG are working.

### 5.4 Test the PL GPIO (next step)

```c
#include "xparameters.h"
#include "xgpio.h"
#include "xil_printf.h"
#include "sleep.h"

XGpio ledsw;

int main(void)
{
    // 2024 SDT flow: Initialize takes the BASE ADDRESS, not a device ID.
    // Macro name = BD instance name (zed_base.tcl: axi_gpio_ledsw).
    XGpio_Initialize(&ledsw, XPAR_AXI_GPIO_LEDSW_BASEADDR);

    XGpio_SetDataDirection(&ledsw, 1, 0x00);  // channel 1: LEDs, outputs
    XGpio_SetDataDirection(&ledsw, 2, 0xFF);  // channel 2: switches, inputs

    while (1) {
        XGpio_DiscreteWrite(&ledsw, 1, XGpio_DiscreteRead(&ledsw, 2));
        usleep(10000);
    }
}
```

- Macro names follow BD instance names (see section 2.1). Check `xparameters.h` in the platform BSP include folder.
- Older tutorials use `XPAR_..._DEVICE_ID`. Those macros no longer exist in the 2024 SDT flow.
- `sw/vga_test.c` is the full test: LEDs follow switches, buttons cycle VGA test patterns.

### 5.5 SD boot (later)
Flow panel → Create Boot Image: FSBL + bitstream + application → `BOOT.BIN`. Copy to the SD card and set the boot jumpers back to SD mode.

## 6. Jumpers

| Jumper | Setting | Notes |
|---|---|---|
| JP7–JP11 | JTAG: JP8–JP10 = 000, JP7 and JP11 on GND | Boot mode and MIO strapping |
| JP6 | Shorted | MIO0 pull-down; matters for SD boot on CES silicon |
| JP2, JP3 | Leave default (open) | Only for USB host mode on J13 (both shorted). Not needed for JTAG/UART |
| J18 | Check physically | Sets Vadj for banks 34/35 (FMC, buttons, switches). See section 7 |

## 7. XDC concerns

### 7.1 When an XDC is needed
- Not needed: PS-only designs (DDR, UART, SD, Ethernet are fixed MIO pins), PL pins connected through board interfaces (constraints are generated inside the IP), the auto-generated HDL wrapper, PS FCLK and Clocking Wizard clocks.
- Needed: any port made external that is not a board interface, new ports added in a custom RTL top level (camera on FMC, Pmods, OLED, HDMI, VGA, audio I2S, XADC), external clocks entering the PL (`create_clock`), clock-domain crossings (`set_clock_groups`), and I/O delays.
- The original board files only cover LEDs, switches, buttons, the 100 MHz clock and the FMC pins. Everything else (Pmod JA–JD, OLED, HDMI, VGA, audio codec, XADC, USB-OTG reset) needs the master XDC.

### 7.2 The master XDC is not fully commented out
`zedboard_master.xdc` lines 362–375 are active, not commented:
- Bank 33 and bank 13 set to LVCMOS33 (harmless).
- Banks 34 and 35 set to LVCMOS18.

The Avnet 1.5 board files put buttons, switches and all FMC pins (banks 34/35) on LVCMOS25.
Adding the master XDC unchanged silently overrides those pins to LVCMOS18.
In a board-file project, comment out the bank-wide `get_iobanks` lines and set IOSTANDARD per port for the pins actually used.

### 7.3 Do not constrain a pin twice
Do not uncomment a pin in the XDC that is already connected through a board interface (LEDs, switches, buttons, sys clock, FMC). This causes critical warnings and conflicting constraints.

### 7.4 Recommended practice
- Add the master XDC to the project with every pin line commented, plus the bank-wide lines commented.
- Uncomment only pins used by the custom top level, and rename `get_ports` to match port names.
- Keep timing constraints in a separate `timing.xdc`.

### 7.5 Bank 34/35 voltage (J18 / Vadj)
- The hardware guide is inconsistent: it calls 1.8 V the default in one place and 2.5 V in another. The board files assume 2.5 V (LVCMOS25), the master XDC assumes 1.8 V.
- Check the J18 jumper physically before driving any bank 34/35 or FMC pin, and make the I/O standard match it.
- If J18 is at 1.8 V, the board-file buttons, switches and FMC pins are mis-specified at LVCMOS25. Either move J18 to 2.5 V or override those pins to LVCMOS18.
- Check J18 before plugging in any FMC card. A wrong Vadj can damage the card.

### 7.6 Errors in the hardware user guide
- HDMI: the guide says bank 35, but its pin list (Y13, AA14, W18 …) and the XDC put it on bank 33 (3.3 V). Trust the XDC.
- Pmod JA3: the guide lists Y19, which is VGA vsync. The XDC (Y10) is correct.
- Master XDC typo: HDMI data bit 15 is named `HS_D15` instead of `HD_D15`. Rename when uncommenting.

### 7.7 Other pins worth knowing
- USB-OTG reset (`OTG_RESETN`, G17, bank 35) must be driven high (or from an EMIO GPIO) for USB to work under Linux.
- K16 (`PUDC_B`) is a configuration pin. Leave it unused.
- HDMI clock (W18) should be driven through an ODDR, not directly from fabric logic.

## 8. Other notes
- Check the chip marking for CES vs production silicon (relevant to JP6 and SD boot).
- On Windows, keep the Vitis workspace path short, without spaces or non-ASCII characters. Long paths are the most common cause of BSP build failures.
- After re-exporting the XSA, update the platform (Switch XSA / Update Hardware Specification) and rebuild.
- Suggested bring-up order: bare-metal Hello World over JTAG → PL GPIO test → FSBL + `BOOT.BIN` from SD → PetaLinux / Yocto only when Linux is needed.

## 9. VGA output from a DRAM frame buffer

### 9.1 Architecture
```
DRAM frame buffer (32-bit XRGB)
   │  S_AXI_HP0 (64-bit, 150 MHz)
   ▼
Video Frame Buffer Read (vfb_rd) ──AXI4-Stream RGB──► AXI4-Stream to Video Out (vid_out) ──► vga_pack ──► VGA (12-bit)
        ▲ AXI-Lite (GP0)                                ▲ timing                    ▲ pixel clock
        │ reset: EMIO GPIO 54                          VTC (vtc, AXI-Lite)         clk_wiz_pix (65 MHz from FCLK0)
```
- Software writes pixels to an ordinary DRAM buffer. The DMA rescans it every frame, so any write appears on screen at the next refresh.
- Default mode 1024x768@60 (65 MHz pixel clock). Frame buffer IP is sized for up to 1920x1080, so a new resolution only needs a different pixel clock and VTC timing.
- VGA DAC is 4 bits per colour; `vga_pack` takes the top 4 bits of each 8-bit component and blanks outside active video.
- Bandwidth at 1024x768@60, 32 bpp: about 190 MB/s, well within HP0. 1080p60 would be about 500 MB/s, which is workable but takes a noticeable share of the 32-bit DDR3.

### 9.2 Why Video Frame Buffer Read and not AXI VDMA
Both work for bare metal. Frame Buffer Read is the IP used by the Xilinx Linux DRM driver for PL displays (`xlnx,pl-disp` with the `xilinx_frmbuf` DMA driver).
Choosing it now avoids redesigning the hardware when moving to PetaLinux.
The Linux driver needs a reset GPIO for the IP, which is why its reset comes from EMIO GPIO 54 rather than from a reset block.
In bare metal, software must release that reset before using the IP (done in `vga_test.c`).

### 9.3 Bare-metal test (`sw/vga_test.c`, now merged into `sw/jig.c`)
- Pixel format: 32-bit `0x00RRGGBB` (memory byte order B,G,R,X = Xilinx `BGRX8` = DRM `XRGB8888`). If red and blue are swapped, change to `XVIDC_CSF_MEM_RGBX8`.
- Flush the data cache (`Xil_DCacheFlushRange`) after drawing; the DMA reads DRAM, not the CPU cache.
- The buffer is a 3 MB static array in DDR; no special linker changes are needed.

### 9.4 Path to PetaLinux / Ubuntu desktop
- In Linux, the kernel DRM driver allocates the frame buffer from CMA (contiguous DMA memory). This is the "DMA-allocated buffer in DRAM" approach, similar in spirit to Xillinux.
  Reserve enough CMA in the bootargs, e.g. `cma=64M`.
- Expected stack: `xlnx,pl-disp` DRM driver + `xilinx_frmbuf` + VTC bridge → `/dev/dri/card0` and `/dev/fb0` (fbdev emulation) → Xorg → desktop.
- Open point: there is no encoder IP between `vid_out` and the VGA connector, so the device-tree display graph needs a VGA connector node at its end. This must be verified during the PetaLinux bring-up.
  Fallback: a `simple-framebuffer` node over a reserved buffer, with the hardware started by U-Boot or a small driver.
- Fixed pixel clock means a single display mode. For Linux mode switching, enable dynamic reconfiguration on `clk_wiz_pix` (Linux driver `xlnx,clocking-wizard`).
- Realistic expectation: the 7020 has no GPU, so the desktop is software-rendered on two Cortex-A9 cores. A lightweight desktop (LXDE/XFCE) is usable; GNOME is not.

### 9.5 Adding features without rebuilding from scratch
- Block designs synthesize each IP out of context. Adding a block only re-synthesizes new or changed IPs (the AXI interconnects usually change), plus the top level. Place-and-route always reruns.
- `zed_base.tcl` sets a shared IP cache in `ip_cache/` next to the script, so a full rebuild from Tcl reuses unchanged IP results.
- Each feature is a `build_*` proc in `tcl/bd_procs.tcl`. An `add_<feature>.tcl` script opens the existing project, sources the procs, runs one proc and rebuilds (e.g. `add_camera.tcl`). The fresh-build script calls the same procs, so both routes give the same design.

## 10. Camera (Muse Lab PMOD-CAMERA v1.0, OV2640)

### 10.1 Connection
- Camera board plugs into JA (control) and JB (data).
- Pin mapping from the earlier `zeboard.xdc` for this module; it matches the silkscreen if the second label of each pair is on the top row (pins 1-4):

| Pmod pin | Signal | FPGA pin | Pmod pin | Signal | FPGA pin |
|---|---|---|---|---|---|
| JA2 | PCLK | AA11 | JA8 | XCLK | AB10 |
| JA3 | HREF | Y10 | JA9 | VSYNC | AB9 |
| JA4 | SCL | AA9 | JA10 | SDA | AA8 |
| JB1 | D7 | W12 | JB7 | D6 | V12 |
| JB2 | D5 | W11 | JB8 | D4 | W10 |
| JB3 | D3 | V10 | JB9 | D2 | V9 |
| JB4 | D1 | W8 | JB10 | D0 | V8 |

- JA1/JA7 are NC on this module (power-down and reset are fixed on the camera board).
- Errors in the old project constraint files:
  - `zeboard.xdc`: HREF on Y19 (the hardware-guide typo for JA3). Y19 is VGA vsync. Correct pin is Y10.
  - `zed_board.xdc`: PCLK assigned twice (W10 and V12), V12 also used for D[7]. That file is for a different wiring and should not be reused.

### 10.2 Hardware
```
FCLK2 25 MHz -> clk_forward (ODDR) -> XCLK
PS I2C0 (EMIO) -> SCCB (SCL/SDA, internal pull-ups)
D[7:0], HREF, VSYNC, PCLK -> dvp_capture (PCLK domain) -> axis_data_fifo (async) -> vfb_wr (150 MHz) -> HP1 -> DRAM
```
- `dvp_capture` pairs bytes into RGB565 pixels, expands to RGB888, and emits AXI4-Stream with start-of-frame (tuser) and end-of-line (tlast).
- The camera cannot be stalled. If the FIFO is full (DMA not running), the rest of that frame is dropped and capture re-arms at the next VSYNC, so the DMA only sees whole frames.
- PCLK is constrained at 25 MHz. The OV2640's actual PCLK in this mode has not been measured; the capture logic is small enough to run well above that, but tighten `create_clock` if PCLK proves faster.
- PCLK enters on a Pmod pin that is not clock-capable: `CLOCK_DEDICATED_ROUTE FALSE` in `camera.xdc` (same approach as the old project).
- EMIO GPIO 54 = `vfb_rd` reset, 55 = `vfb_wr` reset (the Linux frame buffer drivers each need their own reset GPIO).
- Interrupts: `irq_concat` In0 = `vfb_rd`, In1 = `vfb_wr` -> IRQ_F2P[1:0].
- Verified in simulation (`sim/tb_dvp_capture.v`, Icarus): pixel order, colour expansion, tuser/tlast placement over several frames, and frame-drop recovery after FIFO overflow.

### 10.3 Software (`sw/cam_test.c`, now merged into `sw/jig.c`; `sw/common/ov2640_regs.h`)
- The module turned out to carry an OV2640 (SCCB address 0x30), not an OV7670 (0x21). The bus scan found it; the hardware is sensor-independent, so no rebuild was needed.
- `cam_test.c` probes 0x30 then 0x21 and checks the chip ID (OV2640: 0x26 0x42, read from register bank 1; OV7670: 0x76 0x73).
- OV2640 table (`ov2640_regs.h`) is generated by compiling the Linux driver's tables and flattening the `ov2640_set_params()` sequence for VGA + RGB565 big-endian. If colours look byte-swapped, set 0xDA = 0x09.
- If nothing answers, run the I2C bus scan and check XCLK on JA8 (about 1.6 V DC on a multimeter when running).
- Configures VGA 640x480 RGB565.
- Zero-copy preview: `vfb_wr` writes 640x480 directly into a centred window of the 1024x768 display buffer (base = window origin, stride = 4096 bytes). No CPU copying; tearing is possible because the two DMAs are not synchronised.
- The register table is a minimal VGA/RGB565 set (window values as in the Linux ov7670 driver). Colour and exposure may need tuning; the Linux driver's default table is the fuller reference.

### 10.4 Next steps
1. Software processing path: camera writes to its own 2-3 buffers rotated on the `vfb_wr` interrupt; CPU processes the latest and writes to the display buffer. Invalidate the cache before reading a DMA-written buffer, flush after writing a buffer the display reads.
2. Hardware processing path: insert HLS/RTL operators between `cam_fifo` and `vfb_wr`; the software path serves as the golden model.
3. Linux: OV7670 has a mainline sensor driver; capture through `vfb_wr` with the Xilinx video framework gives `/dev/video0`. `dvp_capture` has no registers, so its device-tree node must act as a transparent link (open point, like the VGA connector).

## 11. Processing slot and buffer rotation (v4)

### 11.1 Hardware
```
cam_fifo -> proc_slot (150 MHz) -> vfb_wr -> HP1 -> DRAM
               ^ AXI-Lite 0x43D00000 (GP0)
```
- `proc_slot.v`: one pipeline stage, 1 pixel/clock. Kernels selected by `CTRL.mode`, applied from the next start of frame: 0 pass, 1 grayscale, 2 invert.
- Counters, measured at the slot output: frames, cycles between start-of-frame beats (frame rate = 150 MHz / cycles), pixels and lines per frame, stall cycles (downstream backpressure).
- New kernels go into the slot; the register map and counters stay the same, so every variant is measured identically. This is the insertion point for custom-arithmetic kernels.
- Address is fixed at 0x43D00000 in `build_processing` so software does not depend on generated macros.
- v5 adds CTRL[4] (input select) and registers 0x1C-0x24; see section 12.

| Offset | Register | Access |
|---|---|---|
| 0x00 | CTRL [1:0] mode | R/W |
| 0x04 | ID = 0x534C0001 | R |
| 0x08 | FRAMES | R |
| 0x0C | CYCLES_LAST | R |
| 0x10 | PIXELS_LAST (expect 307200) | R |
| 0x14 | LINES_LAST (expect 480) | R |
| 0x18 | STALLS_LAST | R |

### 11.2 Verification (Icarus, before hardware)
- `sim/tb_proc_slot.v`: random input gaps and random output backpressure; modes switched mid-frame (take effect at the next frame only); 480/480 beats bit-exact against the reference model; all counters correct; AXI-Lite read/write.
- `sim/check_vectors.c`: the testbench dumps every beat; the C golden model (`sw_kernels`) recomputes it. 0 mismatches over all modes, i.e. the HW and SW grayscale/invert are bit-exact.

### 11.3 Software (`sw/proc_test.c`, now merged into `sw/jig.c`)
- Six display-size buffers rotate between the camera DMA, the display DMA and the CPU. Frame-done flags of both DMAs are polled (no interrupt controller). No buffer is ever written while it is scanned out or processed, so there is no tearing.
- SW0 = 0, HW path: the slot runs the kernel; the CPU only swaps buffer addresses.
- SW0 = 1, SW path: the slot passes through; the CPU runs the same kernel from `sw_kernels.c` (plus Sobel) on each frame and reports ms/frame.
- SW2:1 select the kernel. UART prints camera fps, display flips/s, slot counters and SW kernel time every 2 s.
- Assumption to confirm on hardware: the frame buffer IPs latch a new buffer address at the start of their next frame (auto-restart). If frames ever appear out of order or torn, this is the first thing to check.

### 11.4 Vitis setup
- Update the platform to the new XSA (Switch XSA / Update Hardware Specification) and rebuild it.
- New application `proc_test`: source files `proc_test.c` plus all of `sw/common/` (flat in `src/`).

### 11.5 Display bookkeeping fix (applies to jig.c and proc_test.c)
- Found with `sim/rotation_model.py`: in the SW path with slow kernels, a display frame-done event could stay unread while the CPU was busy, then be credited to the NEW display address after a buffer change. The buffer still on screen lost its protection and the camera could write into it (tearing, SW path at 30/60 fps only).
- Fix: a pending display event is consumed before the address changes, and the previous display buffer stays protected for two display frames. The model reports 0 collisions for HW and SW paths at 15/30/60 fps, including an injected race between polling and writing the address.

## 12. Version 5: run-time modes and memory-to-memory testing

### 12.1 What can change at run time
| Item | Range | How |
|---|---|---|
| Display mode | 640x480, 800x600, 1024x768, 1280x720, 1280x1024, 1920x1080 (all @60) | pixel clock reprogrammed through the clocking wizard (dynamic reconfiguration), VTC timing, display DMA format |
| Camera size (OV2640) | QCIF 176x144, QVGA, CIF, VGA, SVGA, XGA, SXGA, UXGA 1600x1200 | sensor tables from the Linux driver, write DMA format |
| Kernel / path | none, gray, invert (HW + SW), Sobel (SW) | switches |

- Capture and the processing slot follow HREF/VSYNC and tuser/tlast, so they adapt to any frame size without reconfiguration.
- The clocking wizard is built for 148.5 MHz (highest mode) so timing closure covers every mode; the application sets the actual mode at start-up. All v5 apps call `zb_display_set_mode()` (or `zb_vtc_init()` for 1024x768).
- The split OV2640 tables reproduce the VGA sequence verified on the board register for register (195 registers, identical).
- PCLK is now constrained at 15 ns (about 66 MHz) to cover UXGA output.

### 12.2 Canvas layout
- Each of the 6 live buffers is a canvas of max(camera, display) size, up to 1920x1200.
- The camera writes centred in the canvas; the display scans a centred display-size region. Camera smaller than the display gives a window with a border, larger gives a crop; no copying either way.
- All 48 camera x display combinations were checked: buffers fit, offsets are 32-byte aligned (cache lines and DMA).

### 12.3 Memory-to-memory test
```
TEST_IN 0x10000000 -> vfb_rd2 (HP2) -> proc_slot (CTRL[4]=1) -> vfb_wr -> TEST_OUT 0x11000000
                                    C golden model on the CPU -> TEST_REF 0x12000000
```
- Key `m`: generated test image (gradients + hash, every byte value, blocks); key `l`: image already loaded at TEST_IN; key `r`: size (640x480, 1280x720, 1600x1200, 1920x1080, 1920x1200).
- Reported: pixels, lines, ACTIVE_CYCLES (slot busy time at 150 MHz), throughput in Mpixel/s, end-to-end time, the same kernel's CPU time, and the number of pixels that differ from the C golden model (PASS = 0).
- The HW result is shown on screen for 3 s when it is at least the display size, then live video resumes.
- In simulation the slot processes a gap-free frame in exactly W x H cycles (1 pixel/clock), i.e. up to 150 Mpixel/s.
- Real images over JTAG (xsct): `dow -data input.bin 0x10000000`, then `mrd -bin -file result.bin 0x11000000 <W*H>`. `sw/tools/rawimg.py` converts PNG <-> raw (`to-raw`, `from-raw`, `diff`).

| proc_slot register (v2) | Meaning |
|---|---|
| 0x00 CTRL | [1:0] kernel, [4] input: 0 camera, 1 memory |
| 0x04 ID | 0x534C0002 |
| 0x1C ACTIVE_CYCLES | start-of-frame beat to latest end-of-line beat of the current/last frame |
| 0x20 PIXELS_RUN | output pixels so far in the current/last frame |
| 0x24 LINES_RUN | output lines so far in the current/last frame |

### 12.4 Hardware additions (v4 -> v5, `add_v5.tcl`)
- `clk_wiz_pix`: dynamic reconfiguration, AXI-Lite on GP0, static 148.5 MHz.
- `vfb_rd2`: frame buffer read on HP2, up to 1920x1200, reset on EMIO GPIO 56, interrupt IRQ_F2P[2].
- `vfb_wr`: maximum raised to 1920x1200.
- `proc_slot` v2 (module reference refreshed by the script).

### 12.5 Running it
1. With the v4 project open, run `add_v5.tcl` (or build fresh with `zed_base.tcl`).
2. Update the Vitis platform to the new XSA and rebuild it.
3. One Vitis app, `jig`: `jig.c` plus everything in `sw/common/`, flat in `src/`. Serial terminal at 115200; the jig reads single key presses.
4. `vga_test.c`, `cam_test.c` and `proc_test.c` are merged into `jig.c` (single `main()`); delete the old apps from the Vitis workspace.

### 12.6 Things to confirm on hardware
- Each display mode on your monitor (VGA monitors differ in which modes they accept; 1920x1080 over the resistor-ladder DAC may look soft).
- Timing closure at 148.5 MHz for the pixel domain (check `timing_summary.txt`).
- Frame rates at the larger OV2640 sizes, and that PCLK at UXGA stays within the 15 ns constraint.
- Macro names `XPAR_VFB_RD2_BASEADDR` and `XPAR_CLK_WIZ_PIX_BASEADDR` in `xparameters.h`. If the clocking wizard macro has another name, mode changes print an error and only 1024x768 works.

### 12.7 UART menu (`jig.c`)
| Key | Action |
|---|---|
| `t` | source: camera / test pattern (pattern only, if no camera is found) |
| `p` | next test pattern: colour bars, checkerboard, grey ramp (red 1-pixel frame to check the edges) |
| `d` / `D` | next / previous display mode |
| `c` / `C` | next / previous camera size (OV2640) |
| `0` `1` `2` `3` | kernel: none, grayscale, invert, Sobel (Sobel on the CPU only) |
| `x` | run the kernel in HW (proc_slot) or SW (CPU) |
| `m` / `l` | memory test with a generated image / with the image loaded at 0x10000000 |
| `r` | next memory-test size |
| `g` | gap test: locates a memory-test shift (read side, write side or stream) |
| `s` / `h` | status line now / menu with the current state |
| `v` | debug: DMA, VTC, pixel clock and ADV7511 state |
| `H` | HDMI status (v6) |
| `k` | next ADV7511 input clock delay, -1200 .. +1600 ps in 400 ps steps (v6) |
| `a` | HDMI: automatic re-apply of a lost ADV7511 setup on / off (default off; v6, diagnostics) |

The switches still work (SW0 = SW path, SW2:1 = kernel). The jig acts on switch changes only, so keys and switches can be mixed; the last change wins.

### 12.8 Final state of v5 and the resolved image-split bug

**Start-up configuration:** display 640x480@60, camera 640x480 (VGA). The camera image fills the screen.

**Resolved bug (fixed 2026-11-07): camera image split after camera size changes.**
- Symptom: after some camera size changes (first seen QCIF -> QVGA), the camera image shows a vertical seam: the left part of the scene appears on the right, one line lower. It persists through further changes, including going back to VGA with the keys. Reprogramming the FPGA from Vitis clears it, without power-cycling the camera.
- Not the cause: the frames reaching the write DMA have exactly W x H pixels between start-of-frame markers (checked with the proc_slot counters on the board). The camera gets the same soft reset and register tables in both the failing and the working case.
- Workaround: after camera size experiments, reprogram the FPGA (Vitis Run) to return to a clean state.

Tried and removed (none fixed the issue on the board; each was based on a hypothesis, not on a measurement):
1. proc_slot frame resynchronisation (CTRL[5], drop input until the next start of frame after a DMA stop). Fixed the effect in simulation, not on the board.
2. dvp_capture start of frame on the first line after the VSYNC falling edge (assumed VSYNC edges inside scaled output lines). Fixed the effect in simulation, not on the board.
3. Per-frame geometry check with automatic camera DMA restart in jig.c (never triggered: frame sizes were always correct).
4. PL reset (FCLK_RESET0..3_N through the SLCR) before every reconfiguration.

Kept, because each fixed a problem observed on the board or a race shown in the model:
- Reset pulse of each frame buffer DMA (EMIO) before it is restarted, and a 5 ms wait after the pixel clock relocks before the VTC is programmed. Fixed the blank screen after every mode change.
- Display bookkeeping fix (11.5), verified with `sim/rotation_model.py`.
- `v` debug key (DMA, VTC and pixel clock state) and `refresh_rtl.tcl` (rebuild after RTL changes).

**Root cause and fix (verified on the board 2026-11-07):**
- The memory test reproduced the problem without the camera: after camera size changes (seen after QCIF) the memory-to-memory result was the input shifted by exactly 32 pixels, and it stayed that way for every later test, although the write DMA was confirmed reset (width register 0, idle) before each one.
- Gap test (`g`): 64-pixel gap after every line, guard area before the output. Result: line ends contain the start of the next line, no gap marker, guard and output gaps untouched. So read addresses and write addresses were correct, and the data stream was offset by 32 px = 128 bytes = one 16-beat burst on the 64-bit HP port.
- Explanation consistent with every observation: the write DMA was stopped with the Xilinx l2 `Stop()` (a flush that aborts the frame) and then immediately reset with the EMIO pulse. That can leave a write address in the PS HP1 port without its data. The next frame's first burst of data then completes the old address, and every later data burst is paired with the address one burst ahead: the image moves left by 32 px and the last burst is always left pending, so the offset carries over. It survives the DMA reset and the PL reset (the HP port is in the PS) and is cleared only by a system reset (Vitis Run). Several such events add up: the camera seam of about 290 px = 9 bursts. The seam first appeared right after the DMA reset pulse was introduced (the blank-screen fix).
- Fix (software): `hls_graceful_stop()` in `zb_platform.c` clears auto-restart, lets the frame in progress finish (all bursts complete), waits for the core to go idle plus 5 ms, and only then pulses the reset. The flush is used only as a fallback if the core does not go idle within 250 ms (then 50 ms wait, as in the Linux driver).
- Verified: after a system reset, cycling through all camera sizes (including QCIF) and all display modes, the memory test (`m`) passed every time and the camera image showed no seam.
- Rule for all future DMA code (bare metal and any custom driver): never abort an AXI master (flush) and reset it straight away. Let it finish its frame and go idle, then reset. If a flush is unavoidable, wait (50 ms, as the Linux driver) before the reset. A wrong offset in the PS HP port needs a system reset to clear.

Kept for reference only (not needed after the fix; sensor-side causes reported by others, web search 2026-11-07):

1. **Settle-and-skip sequence (OpenMV, OV7725).** Very similar symptom: after resolution switches, a section of the right side of the frame appeared at the far left (about 1 in 100 switches, cleared by the next switch). Cause given: changing output-related sensor registers while frames are being captured produces incorrect frames. Their remedy: stop DMA, apply the sensor registers, wait about 100 ms, then skip the first frames before using the image (`sensor.skip_frames(time = 100)`).
   To try here: in `apply_config()` the DMA is already stopped and the code waits 200 ms, but the first frames after the change go straight to the write DMA. Change the sequence to: stop DMA -> write the size tables -> wait -> discard N complete frames (count VSYNCs with the camera DMA still stopped, e.g. through proc_slot FRAMES with the slot draining, or simply wait N frame periods) -> start the DMA on a frame boundary. Try N = 2, 5, 10.
   Source: https://forums.openmv.io/t/ov7725-based-on-version-4-4-2-the-image-became-misaligned/10700

2. **Sensor output FIFO read offset (Linux kernel patch, OV2659).** Same OmniVision family. Image wrap-around in the lower, scaled resolutions (640x480, 320x240), with scaling and binning enabled. Cause: the read start offset of the sensor's internal output FIFO was too small. Fix: REG_VFIFO_READ_START_L 0x80 -> 0xa0 (REG_VFIFO_READ_START_H stays 0x00) in the VGA and QVGA tables.
   To try here: find the equivalent OV2640 DSP/output FIFO register in the OV2640 datasheet or in other OV2640 drivers (esp32-camera, OpenMV, ArduCAM tables) and compare their values for QCIF/QVGA/CIF/VGA with the Linux tables used in `ov2640_regs.h`. A wrong offset that only bites after certain size changes would fit a sensor-side cause.
   Source: https://lkml.iu.edu/hypermail/linux/kernel/1909.3/03420.html

3. **XCLK interruption.** A Vitis Run also re-runs `ps7_init`, restarting all PS clocks, including FCLK2 = the camera XCLK; the jig never interrupts it. Briefly stopping XCLK during a size change (SLCR FPGA2 clock control) would show whether the sensor needs a clock interruption to recover.

4. **Read back the sensor state.** Read the OV2640 DSP window and output registers over SCCB in a working and in a split state and compare.

5. **Measure.** Capture HREF/VSYNC/PCLK and the capture/slot outputs with an ILA in both states.

Background (not a direct match):
- espressif/esp32-camera issue 224: OV2640 window offsets above 255 also shifted the image horizontally because of a wrong bit field in register VHYX (0x55). Not used by the Linux tables here, but it shows that the DSP window registers can shift the image sideways. https://github.com/espressif/esp32-camera/issues/224
- No report found of exactly this symptom with an OV2640 on an FPGA, or with the Muse Lab PMOD-CAMERA (https://www.tindie.com/products/johnnywu/pmod-camera-expansion-board).

## 13. Planned work

### 13.1 Next phase: HDMI output (ADV7511) - implemented in v6, see section 14

**Board facts (ZedBoard hardware user guide, section 2.4.1, Table 7):**
- ADV7511 HDMI 1.4 / DVI transmitter, 1080p60 supported, wired for **16-bit YCbCr 4:2:2** (HD_D[15:0]), plus HD-CLK, HD-DE, HD-HSYNC, HD-VSYNC, I2C (HD-SCL AA18, HD-SDA Y16), interrupt (HD-INT W16) and S/PDIF in/out. I2S audio is not connected.
- Hot-plug detect (HPD) is **not connected to the FPGA**: the ADV7511 sees it itself; software reads the plug state from the ADV7511 over I2C (or uses HD-INT).
- Pins: use `zedboard_master.xdc` (bank 33, 3.3 V). The guide's table has two errors: it says bank 35, and lists HD-D1 on AA12 (the OLED pin); the XDC's AA13 is correct.
- Analog Devices provides Linux drivers and reference designs for this board.

**One design with both outputs, or two versions?** One design with both, from a single source:
- VGA and HDMI share everything up to the pixel bus: frame buffer DMA, VTC, `vid_out`, pixel clock. HDMI adds only a small output block next to `vga_pack`. Both monitors show the same mode at the same time.
- Two separate HW/SW versions would duplicate the Tcl, the RTL top level, the constraints and the software for no saving: the shared part is all of the cost.
- Build-time switches in `zed_base.tcl` (`enable_vga`, `enable_hdmi`) can still produce a VGA-only or HDMI-only bitstream from the same files if that is ever needed (for example, to free pins).
- One `jig.c`: HDMI is set up if the ADV7511 answers on its I2C bus, otherwise skipped.

**Utilisation:** HDMI adds roughly (estimates, to be confirmed by synthesis):

| Block | Estimate |
|---|---|
| RGB -> YCbCr 3x3 matrix, pipelined | 3-9 DSP48 + about 100 LUT/FF (or 300-400 LUT, about 200 FF without DSPs) |
| 4:4:4 -> 4:2:2 chroma subsampling | 30-60 LUT/FF |
| 16-bit bus, DE/HS/VS output registers, ODDR clock | in the IOBs |
| ADV7511 I2C on PS I2C1 via EMIO | no PL logic (an AXI IIC block would be about 400-500 LUT) |
| **Total** | **a few hundred LUT/FF, under 1% of the Z-7020 (53,200 LUT, 106,400 FF, 220 DSP)** |

Keeping VGA costs only `vga_pack` (14 registers). So keeping both does not raise utilisation in any meaningful way. The large blocks are the three frame buffer DMAs, the VTC and the AXI interconnects; the current figures are in `utilization.txt` in the project folder (written by every build) and should be checked before the research kernels are added.

Measured v5 (routed, 2026-11-07, `utilization.txt`): LUT 9201 (17.3 %), FF 14453 (13.6 %), slices 4774 (35.9 %), BRAM 8.5 tiles (6.1 %), DSP 3 (1.4 %), BUFG 6, MMCM 1. HDMI adds well under 1 % on top; most of the device is free for the research kernels (DSP and BRAM especially). Avoid the Video Processing Subsystem for the colour conversion (several thousand LUT for the same job).

**Plan:**
1. Read the ADV7511 programming guide for the input format matching the ZedBoard wiring (16-bit YCbCr 4:2:2, separate syncs) and its required register settings (power-up, input ID/style, colour space conversion to RGB output, HDMI/DVI mode); cross-check with the Analog Devices ZedBoard reference settings.
2. RTL `hdmi_pack.v`: RGB888 -> YCbCr (BT.601 or BT.709, chosen per mode) -> 4:2:2, 16-bit bus, DE/HSYNC/VSYNC registered in the IOBs, HD-CLK through an ODDR. Simulate against a C model, as for proc_slot.
3. Block design: `hdmi_pack` fed from `vid_out` in parallel with `vga_pack`; PS I2C1 on EMIO for the ADV7511; `hdmi.xdc` with pins and output timing. New proc `build_hdmi` in `tcl/bd_procs.tcl`, `add_hdmi.tcl` for the existing project.
4. Software: ADV7511 register table and hot-plug polling in `zb_platform`; jig shows HDMI status in the menu; display modes unchanged (now including 1920x1080 on a monitor that supports it).
5. Check timing at 148.5 MHz with both outputs, and the on-screen colour against the memory-test results.

### 13.2 Image-split bug: closed
Fixed by the graceful DMA stop (12.8). Keep the memory test (`m`) and gap test (`g`) as regression checks after any change to the DMA start/stop code or the HP port wiring. The OpenMV settle-and-skip idea is still worth adding later, only to hide the one or two bad frames right after a camera size change.

### 13.3 Other items
- Interrupt-driven buffer rotation (GIC + handlers) instead of polling; matches the Linux driver model and avoids missed frame events during slow SW kernels.
- OV2640 800x600 readout mode for about 30 fps (new register table, no FPGA change); white balance / colour tuning (purple skin tones).
- First real kernel in proc_slot (3x3 convolution / Sobel with line buffers), bit-exact against `sw_kernels.c` via `check_vectors.c` and the memory test; then the custom-arithmetic variants.
- PetaLinux: DRM display (`xlnx,pl-disp` + frame buffer read), V4L2 capture (frame buffer write), open device-tree points for the VGA connector and the capture link.

## 14. Version 6: HDMI output (ADV7511)

### 14.1 Design
- One design with both outputs (section 13.1): `hdmi_pack` sits next to `vga_pack` on the `vid_out` pixel bus, so VGA and HDMI show the same image in the same mode. `zed_base.tcl` has `enable_vga` / `enable_hdmi` to build either one alone (the matching XDC is then left out).
- `rtl/hdmi_pack.v`: RGB -> YCbCr BT.709 limited range (10-bit coefficients) -> 4:2:2 with a [1 2 1]/4 chroma filter, co-sited with the even pixel. Bus: `hd_d[15:8]` = Cb (even pixel) / Cr (odd pixel), `hd_d[7:0]` = Y, the same format as the Analog Devices `axi_hdmi_tx` core on this board. 6 register stages, outputs in the IOBs.
- HD-CLK = pixel clock through `clk_forward` (ODDR, same phase), as the ADI reference design (`OUT_CLK_POLARITY = 0`).
- ADV7511 on PS I2C1 via EMIO (AA18 / Y16), 7-bit address 0x39. HD-INT is not used; software polls.
- Pins: `constr/hdmi.xdc`, cross-checked against the ADI ZedBoard constraints (identical).

### 14.2 ADV7511 setup (`zb_hdmi_*` in `zb_platform.c`)
Values from the Linux `adv7511` driver with the ZedBoard device tree of the ADI reference design (`input-colorspace = "yuv422"`, `input-clock = "1x"`, `input-style = <1>`, `input-depth = <8>`, `input-justification = "right"`, `clock-delay = <0>`):

| Register | Value | Meaning |
|---|---|---|
| 0xD6[7:6] | 11 | ignore hot-plug for power control: the chip stays configured with no monitor |
| 0x41[6] | 0 | power up |
| 0x98 0x9A 0x9C 0x9D 0xA2 0xA3 0xE0 0xF9 | 03 E0 30 61 A4 A4 D0 00 | fixed values required by the programming guide |
| 0x15[3:0] | 1 | input ID 1: YCbCr 4:2:2, separate syncs |
| 0x16 | 0x38 | output 4:4:4 RGB, 8-bit input, style 1 (register code 2: the guide's numbering differs), rising edge |
| 0x48 | 0x08 | right justified |
| 0xD0 | 0x0C | sync pulse: none |
| 0xBA | 0x60 | input clock delay 0 ps (key `k` changes it) |
| 0x17 | 0x00 / 0x02 | syncs passed through; aspect 16:9 for 1280x720 and 1920x1080 |
| 0x18..0x2F | Linux matrix | CSC YCbCr (BT.709) -> RGB full range, scaling x4 |
| 0xAF[1] | 0 | DVI mode: no infoframes, full-range RGB; works on HDMI and DVI monitors |

The jig checks the setup every 0.5 s and re-applies it if the chip was reset; it reports monitor plug / unplug (HPD from register 0x42).

### 14.3 Verification before hardware
- `sim/tb_hdmi_pack.v` + `sim/check_hdmi.c`: bit-exact against the C model on a small raster with random and extreme (0x00 / 0xFF) colours, including DE/HSYNC/VSYNC alignment: PASS. Three deliberately injected bugs (Cb/Cr swapped, one coefficient off by 1, sync one stage early) were all caught.
- Colour round trip RGB -> YCbCr (hdmi_pack) -> RGB (ADV7511 matrix as loaded): max error 3 LSB, mean 0.5-0.65 LSB per channel, without the chroma subsampling.
- ADV7511 register sequence checked against a mock I2C bus (the writes above, in this order).
- Software compiles against v4, v5 and v6 `xparameters.h` (HDMI is detected at run time by probing I2C1).

### 14.4 Running it
1. With the v5 project open: Tools -> Run Tcl Script -> `add_hdmi.tcl` (or fresh build with `zed_base.tcl`).
2. Vitis: update the platform with the new XSA, rebuild, copy the new `zb_platform.c/.h` and `jig.c` into `src/`.
3. Connect a monitor to the HDMI OUT connector, VGA can stay connected. Start-up prints `ADV7511 found: revision ..`, and `HDMI: monitor connected` once a monitor is plugged in.
4. `t` / `p` test patterns: colour bars show colour correctness (order white, yellow, cyan, green, magenta, red, blue, black), the grey ramp shows range (no clipped black or white at the ends).
5. `d` through all modes, `H` in each: TMDS PLL locked, input VIC as expected (1 = 640x480, 4 = 1280x720, 16 = 1920x1080).

### 14.5 Things to confirm on hardware
- Colours: if red/blue appear swapped or the image is green/purple, the Cb/Cr order or byte lanes differ from the ADI core (one-line change in `hdmi_pack.v`).
- Sparkles or unstable picture at 1080p only: input clock timing. Press `k` to step the ADV7511 clock delay; note the working range and keep its centre as the default (`hdmi_delay` in `zb_platform.c`).
- Range: black should be black (not dark grey) and white not clipped; if not, the monitor is treating the signal as limited range (DVI mode should prevent that).
- Utilisation and timing at 148.5 MHz in `utilization.txt` / `timing_summary.txt`; expected: a few hundred LUT/FF more than v5.
- First run (2026-11-07, Samsung TV): picture OK, but a purple tint comes and goes (photo: purple band over part of the frame, moving), and the TV occasionally loses sync and reconnects. Purple is what the TV shows when the ADV7511's colour conversion is off (YCbCr Y=16, Cb=Cr=128 shown as RGB 128,16,128); sync loss fits the chip losing its setup or its TMDS PLL losing lock. Diagnostics added: every detected loss of setup is printed with the register values, `H` shows the CSC registers and counters, `a` turns the automatic re-apply on / off.
- Result: with the re-apply off (`a`) the picture is perfect, although "setup lost" is still reported now and then. So the chip never lost its setup: the check misread the registers, and the re-apply itself caused the purple tint (colour conversion held while the coefficients are reloaded) and the sync losses (power and PLL registers rewritten). Fix: automatic re-apply off by default; the check still logs. Logged: `41=14 15=14 16=14 18=14 42=14`, the same value for five different registers, so the reads were wrong, not the chip. Reads changed from write-stop-read to one transaction with a repeated start (as the Linux driver); a read where all five registers agree is now counted as an I2C error ("suspect I2C read") and ignored. Confirmed on the board (2026-11-07): after several minutes `H` shows 0 re-applies, 0 HPD changes, 0 I2C read errors; monitor connected, TMDS PLL locked, input VIC 1 (640x480), CSC on (18=C7), DVI mode; picture correct. This also matters at set-up, where register updates are read-modify-write.
- v6 confirmed on the board (2026-11-07, Samsung TV + VGA monitor): all display modes and camera sizes work on both outputs at the same time; colour bars in the right order with correct hues, checkerboard sharp, grey ramp from black to white; no sparkles at 1080p with the default clock delay (0 ps), so `k` was not needed. The ripple patterns in phone photos of the TV are moire between the camera sensor and the TV's pixel grid, not in the signal. If the red 1-pixel frame is missing on some edges, the TV is overscanning (set the TV's picture size to "Screen Fit" / "Just Scan").
- Not done: audio, EDID reading (the mode list stays fixed), CEC, HDMI mode with infoframes.
