# ----------------------------------------------------------------------------
# zed_base.tcl  (v6: v5 + HDMI)
# ZedBoard base platform for Vivado 2024.2 (Avnet zedboard 1.5 board files)
#
#   PS7       : board preset (DDR3, clocks, MIO), UART1 on MIO 48-49 @115200
#               FCLK0 100 MHz (AXI-Lite), FCLK1 150 MHz (video/DMA)
#               S_AXI_HP0 64-bit (frame buffer reads), IRQ_F2P, 1 EMIO GPIO
#   GPIO      : axi_gpio_ledsw  ch1 = LEDs (8 out), ch2 = switches (8 in)
#               axi_gpio_btns   ch1 = buttons (5 in)
#   Video     : DRAM frame buffer -> Video Frame Buffer Read (vfb_rd)
#               -> AXI4-Stream to Video Out (vid_out) <- timing from VTC (vtc)
#               -> vga_pack (RTL module reference) -> VGA connector
#               -> hdmi_pack (RTL, YCbCr 4:2:2) -> ADV7511 HDMI (I2C1 on EMIO)
#               Both outputs show the same image; build switches enable_vga / enable_hdmi
#               Pixel clock from clk_wiz_pix (FCLK0 input), set per display mode by software
#   Camera    : OV7670 (Muse Lab PMOD-CAMERA, JA/JB) -> dvp_capture -> async FIFO
#               -> Video Frame Buffer Write (vfb_wr) -> HP1 -> DRAM
#               XCLK 25 MHz (FCLK2), SCCB on PS I2C0 (EMIO)
#   Processing: proc_slot (RTL) between cam_fifo and vfb_wr: pass/gray/invert +
#               frame/cycle/pixel/stall counters, AXI-Lite at 0x43D00000
#   Modes     : display mode switchable at run time (clk_wiz dynamic reconfiguration,
#               640x480 .. 1920x1080); camera size switchable (176x144 .. 1600x1200)
#   Mem test  : DRAM -> vfb_rd2 (HP2) -> proc_slot (CTRL[4]=1) -> vfb_wr -> DRAM
#   Output    : bitstream + XSA (bitstream included) for Vitis / PetaLinux
#
# Files (relative to this script):
#   tcl/bd_procs.tcl   block design procs (shared with add_camera.tcl)
#   rtl/vga_pack.v  rtl/hdmi_pack.v  rtl/dvp_capture.v  rtl/clk_forward.v  rtl/proc_slot.v
#   constr/vga.xdc  constr/hdmi.xdc  constr/camera.xdc
#   sw/jig.c (test app), sw/common/ (shared code)
#
# Usage:
#   GUI : Tools -> Run Tcl Script -> zed_base.tcl
#   CLI : vivado -mode batch -source zed_base.tcl [-tclargs <output_dir>]
#
# Vitis (SDT) macros follow the instance names:
#   XPAR_AXI_GPIO_LEDSW_BASEADDR  XPAR_AXI_GPIO_BTNS_BASEADDR
#   XPAR_VFB_RD_BASEADDR          XPAR_VTC_BASEADDR
#   XPAR_VFB_WR_BASEADDR          XPAR_XIICPS_0_BASEADDR (camera SCCB)
#   XPAR_VFB_RD2_BASEADDR         XPAR_CLK_WIZ_PIX_BASEADDR
#   XPAR_XIICPS_1_BASEADDR (ADV7511 I2C)
# ----------------------------------------------------------------------------

# ---- user settings ----------------------------------------------------------
set proj_name   zed_base
set bd_name     system
set part        xc7z020clg484-1
set jobs        4
set pix_mhz     65.000      ;# 1024x768@60. 74.25 = 1280x720@60, 108 = 1280x1024@60
set fb_max_cols 1920        ;# frame buffer IP limits; resolution itself is set in software
set fb_max_rows 1080
set pix_max_mhz 148.5       ;# highest display mode (1920x1080@60); timing is closed at this rate
set test_max_cols 1920      ;# camera / memory-test frame limits (vfb_wr, vfb_rd2)
set test_max_rows 1200
set enable_vga  1           ;# VGA output (vga_pack, 14 pins)
set enable_hdmi 1           ;# HDMI output (hdmi_pack + ADV7511 on I2C1, 22 pins)
set script_dir  [file dirname [file normalize [info script]]]
set proj_dir    [expr {[info exists argv] && [llength $argv] > 0 \
                       ? [file normalize [lindex $argv 0]] \
                       : [file join $script_dir $proj_name]}]
# Shared IP cache outside the project folder: unchanged IPs are not re-synthesized
# on a fresh rebuild (the -force project re-creation deletes the in-project cache).
set ip_cache_dir [file join $script_dir ip_cache]
# Uncomment if the board files are not under <Vivado>/data/boards/board_files:
# set_param board.repoPaths [list /path/to/bdf]

source [file join $script_dir tcl bd_procs.tcl]

# ---- main -------------------------------------------------------------------
create_project $proj_name $proj_dir -part $part -force
set_property board_part [find_zedboard] [current_project]
set_property target_language Verilog [current_project]
if {[catch {config_ip_cache -use_cache_location $ip_cache_dir} err]} { puts "WARNING: IP cache not set: $err" }

add_files -norecurse [glob [file join $script_dir rtl *.v]]
if {!$enable_vga && !$enable_hdmi} { error "enable_vga and enable_hdmi are both 0: no display output" }
set xdc_files {}
foreach x [glob [file join $script_dir constr *.xdc]] {
    set n [file tail $x]
    if {($n eq "vga.xdc" && !$enable_vga) || ($n eq "hdmi.xdc" && !$enable_hdmi)} continue
    lappend xdc_files $x
}
add_files -fileset constrs_1 -norecurse $xdc_files
update_compile_order -fileset sources_1

create_bd_design $bd_name
build_ps
build_gpio
build_video $pix_mhz $fb_max_cols $fb_max_rows
if {$enable_vga}  { build_vga }
if {$enable_hdmi} { build_hdmi }
build_camera
build_processing
build_display_reconfig $pix_max_mhz
build_memtest $test_max_cols $test_max_rows
assign_bd_address
regenerate_bd_layout
validate_bd_design
save_bd_design

build_outputs $proj_dir $bd_name $jobs
