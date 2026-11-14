# CLAUDE.md: map of zed_base for agents

ZedBoard (xc7z020) video platform: OV2640 camera -> proc_slot -> DDR -> VGA/HDMI, plus a reserved
accelerator window. One repository for hardware, simulation, bare-metal, Linux C and Python.
Read `README.md` first; this file says where things are and which rules keep them consistent.

## Where to look

| Task | Files |
|---|---|
| Change a hardware block | `rtl/<block>.v`, its testbench `sim/tb_<block>.v`, then `sh sim/run_all.sh` |
| Change the block design | `tcl/bd_procs.tcl` (one proc per subsystem; `build_shell_v7` is the frozen shell) |
| proc_slot register map | header comment of `rtl/proc_slot.v`; mirrored in `sw/common/jig.c` (SLOT_*) and `sw/python/jig.py` |
| Test app logic | `sw/common/jig.c` (C, bare metal + Linux), `sw/python/jig.py` (Python port, keep in step) |
| Board support | `sw/common/zb_platform.c` (C), `sw/python/hw.py` (Python port) |
| Golden models | `sw/common/sw_kernels.c`, `sw/common/lle_global.c` (also used by sim/, Python via ctypes, llie_fpga) |
| Linux C build | `sw/linux_c/Makefile`, `sw/linux_c/shim/` (BSP replacement), `sw/linux_c/xlnx/` (unmodified drivers) |
| Linux image | `linux/system-user.dtsi`, `doc/Linux_Guide.md` |
| Shell / rebuild decisions | `tools/shell_check.py` + `tools/shell_v7.json`; `doc/Get_Started.md` section 16 |
| Architecture figures | `doc/zed_base_arch.tex` (TikZ) |

## Rules

1. One copy of every shared source: C used by more than one target lives in `sw/common/`.
   Never copy it into `sw/linux_c` or `sw/python`; they reference `../common`.
2. `jig.c` selects Linux-only code with `ZB_LINUX` (set by `sw/linux_c/Makefile`), never `__linux__`
   (the Linux build compiles the Xilinx drivers with `-U__linux__`). `xil_printf` is a macro on
   Linux: no `#if` inside its arguments.
3. The shell is frozen (PS7 settings, address map, IRQ_F2P order). New hardware goes inside
   proc_slot's 64 KB window (0x2000 and 0x8000 reserved for the spatial mode) or the accel window.
   Every new XSA: `python3 tools/shell_check.py check <xsa> tools/shell_v7.json` must report
   SHELL MATCH, otherwise a PetaLinux rebuild is needed.
4. RTL changes keep 1 pixel/clock and the AXI4-Stream handshake; check timing (WNS >= 0 on all
   clocks) in `zed_base/zed_base.runs/impl_1/system_wrapper_timing_summary_routed.rpt`.
5. Bit-exactness is the acceptance test: RTL vs C model in simulation (`sim/run_all.sh`), board vs
   C model in the jig memory test (`m`/`l`), Python jig vs model offline (`sim/test_jig_lle.py`).
6. When the C jig gains a feature, add it to `sw/python/jig.py` too (same key, same output).
7. UIO devices are found by physical address; names follow the device-tree node.

## Verification commands

```bash
sh sim/run_all.sh                 # all RTL testbenches + C model checks (Icarus, gcc)
python3 sim/test_jig_lle.py       # Python jig LLE logic against a proc_slot register model
cd sw/linux_c && make             # on the board (needs libgpiod v1, OpenCV)
sudo python3 tools/shell_test.py  # on the board: UIO windows, IDs, interrupts
```

## Related

- `llie_fpga` repository: the low-light study (software, bit-true models, Phase 3 board scripts);
  `lle/hwglobal.py` there is the Python twin of `sw/common/lle_global.c`.
