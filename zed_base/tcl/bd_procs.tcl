# ----------------------------------------------------------------------------
# bd_procs.tcl - block design building blocks for the zed_base platform.
# Sourced by zed_base.tcl (fresh build) and add_*.tcl (incremental changes).
# Each build_* proc adds one feature to the currently open block design.
# ----------------------------------------------------------------------------

# ---- helpers ----------------------------------------------------------------
proc find_zedboard {} {
    set bps [get_board_parts -quiet -latest_file_version *:zedboard:*]
    if {[llength $bps] == 0} {
        error "ZedBoard board files not found. Install from the Vivado Store or set board.repoPaths."
    }
    return [lindex $bps 0]
}

# Connect an AXI-Lite slave to PS M_AXI_GP0 (clocks chosen by automation).
proc axi_lite_to_gp0 {slave_intf} {
    apply_bd_automation -rule xilinx.com:bd_rule:axi4 -config [list \
        Clk_master {Auto} Clk_slave {Auto} Clk_xbar {Auto} \
        Master {/ps7/M_AXI_GP0} Slave "/$slave_intf" \
        ddr_seg {Auto} intc_ip {New AXI Interconnect} master_apm {0}] \
        [get_bd_intf_pins $slave_intf]
}

# Constant '1' driver (xlconstant, or inline-HDL constant if xlconstant is unavailable).
proc const_one {name} {
    if {[catch {create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant $name}]} {
        create_bd_cell -type inline_hdl -vlnv xilinx.com:inline_hdl:ilconstant $name
    }
    set_property -dict [list CONFIG.CONST_VAL {1} CONFIG.CONST_WIDTH {1}] [get_bd_cells $name]
    return [get_bd_pins $name/dout]
}

# ---- block design: PS -------------------------------------------------------
proc build_ps {} {
    create_bd_cell -type ip -vlnv xilinx.com:ip:processing_system7 ps7
    apply_bd_automation -rule xilinx.com:bd_rule:processing_system7 \
        -config {make_external "FIXED_IO, DDR" apply_board_preset "1" Master "Disable" Slave "Disable"} \
        [get_bd_cells ps7]
    set_property -dict [list \
        CONFIG.PCW_UART1_PERIPHERAL_ENABLE {1} \
        CONFIG.PCW_UART1_UART1_IO {MIO 48 .. 49} \
        CONFIG.PCW_UART1_BAUD_RATE {115200} \
        CONFIG.PCW_USE_M_AXI_GP0 {1} \
        CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ {100} \
        CONFIG.PCW_EN_CLK1_PORT {1} \
        CONFIG.PCW_FPGA1_PERIPHERAL_FREQMHZ {150} \
        CONFIG.PCW_USE_S_AXI_HP0 {1} \
        CONFIG.PCW_S_AXI_HP0_DATA_WIDTH {64} \
        CONFIG.PCW_USE_FABRIC_INTERRUPT {1} \
        CONFIG.PCW_IRQ_F2P_INTR {1} \
        CONFIG.PCW_GPIO_EMIO_GPIO_ENABLE {1} \
        CONFIG.PCW_GPIO_EMIO_GPIO_IO {1} \
    ] [get_bd_cells ps7]
}

# ---- block design: GPIO -----------------------------------------------------
proc build_gpio {} {
    # LEDs + switches share one dual-channel AXI GPIO
    create_bd_cell -type ip -vlnv xilinx.com:ip:axi_gpio axi_gpio_ledsw
    apply_board_connection -board_interface leds_8bits -ip_intf axi_gpio_ledsw/GPIO  -diagram [current_bd_design]
    apply_board_connection -board_interface sws_8bits  -ip_intf axi_gpio_ledsw/GPIO2 -diagram [current_bd_design]
    axi_lite_to_gp0 axi_gpio_ledsw/S_AXI

    # Buttons separate (room for an interrupt later)
    create_bd_cell -type ip -vlnv xilinx.com:ip:axi_gpio axi_gpio_btns
    apply_board_connection -board_interface btns_5bits -ip_intf axi_gpio_btns/GPIO -diagram [current_bd_design]
    axi_lite_to_gp0 axi_gpio_btns/S_AXI
}

# ---- block design: video ----------------------------------------------------
proc build_video {pix_mhz max_cols max_rows} {
    set fclk1 [get_bd_pins ps7/FCLK_CLK1]

    # Pixel clock from PS FCLK0 (100 MHz). FCLK0 is already on a global buffer,
    # so the MMCM input takes it without an input buffer. (Board-flow connection
    # of clk_wiz to sys_clock/Y9 failed in 2024.2 with the Avnet 1.5 board files.)
    create_bd_cell -type ip -vlnv xilinx.com:ip:clk_wiz clk_wiz_pix
    set_property -dict [list \
        CONFIG.PRIM_SOURCE {No_buffer} \
        CONFIG.PRIM_IN_FREQ {100.000} \
        CONFIG.CLKOUT1_REQUESTED_OUT_FREQ $pix_mhz \
        CONFIG.USE_RESET {false} \
    ] [get_bd_cells clk_wiz_pix]
    connect_bd_net [get_bd_pins ps7/FCLK_CLK0] [get_bd_pins clk_wiz_pix/clk_in1]
    set pclk [get_bd_pins clk_wiz_pix/clk_out1]

    # Resets: 150 MHz video domain and pixel domain
    create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset rst_vid
    connect_bd_net $fclk1 [get_bd_pins rst_vid/slowest_sync_clk]
    connect_bd_net [get_bd_pins ps7/FCLK_RESET0_N] [get_bd_pins rst_vid/ext_reset_in]

    create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset rst_pix
    connect_bd_net $pclk [get_bd_pins rst_pix/slowest_sync_clk]
    connect_bd_net [get_bd_pins ps7/FCLK_RESET0_N] [get_bd_pins rst_pix/ext_reset_in]
    connect_bd_net [get_bd_pins clk_wiz_pix/locked] [get_bd_pins rst_pix/dcm_locked]

    # Frame buffer read DMA (DRAM -> AXI4-Stream). Reset held by EMIO GPIO 54
    # (the Linux xilinx_frmbuf driver requires a reset GPIO; bare metal releases it).
    create_bd_cell -type ip -vlnv xilinx.com:ip:v_frmbuf_rd vfb_rd
    set_property -dict [list \
        CONFIG.MAX_COLS $max_cols \
        CONFIG.MAX_ROWS $max_rows \
        CONFIG.SAMPLES_PER_CLOCK {1} \
        CONFIG.MAX_DATA_WIDTH {8} \
        CONFIG.AXIMM_DATA_WIDTH {64} \
        CONFIG.HAS_BGRX8 {1} \
        CONFIG.HAS_RGBX8 {1} \
    ] [get_bd_cells vfb_rd]
    connect_bd_net $fclk1 [get_bd_pins vfb_rd/ap_clk]
    connect_bd_net [get_bd_pins ps7/GPIO_O] [get_bd_pins vfb_rd/ap_rst_n]
    connect_bd_net [get_bd_pins vfb_rd/interrupt] [get_bd_pins ps7/IRQ_F2P]

    # Video timing generator (programmed by software over AXI-Lite)
    create_bd_cell -type ip -vlnv xilinx.com:ip:v_tc vtc
    set_property -dict [list CONFIG.enable_detection {false}] [get_bd_cells vtc]
    connect_bd_net $pclk [get_bd_pins vtc/clk]
    connect_bd_net [get_bd_pins rst_pix/peripheral_aresetn] [get_bd_pins vtc/resetn]

    # AXI4-Stream to Video Out (async FIFO between 150 MHz and pixel clock)
    create_bd_cell -type ip -vlnv xilinx.com:ip:v_axi4s_vid_out vid_out
    set_property -dict [list CONFIG.C_HAS_ASYNC_CLK {1} CONFIG.C_ADDR_WIDTH {10}] [get_bd_cells vid_out]
    connect_bd_net $fclk1 [get_bd_pins vid_out/aclk]
    connect_bd_net [get_bd_pins rst_vid/peripheral_aresetn] [get_bd_pins vid_out/aresetn]
    connect_bd_net $pclk [get_bd_pins vid_out/vid_io_out_clk]
    connect_bd_net [get_bd_pins rst_pix/peripheral_reset] [get_bd_pins vid_out/vid_io_out_reset]
    connect_bd_intf_net [get_bd_intf_pins vfb_rd/m_axis_video] [get_bd_intf_pins vid_out/video_in]
    connect_bd_intf_net [get_bd_intf_pins vtc/vtiming_out]     [get_bd_intf_pins vid_out/vtiming_in]
    connect_bd_net [get_bd_pins vid_out/vtg_ce] [get_bd_pins vtc/gen_clken]

    # Clock enables tied high
    set one [const_one const_one]
    connect_bd_net $one [get_bd_pins vtc/clken] [get_bd_pins vtc/s_axi_aclken] \
                        [get_bd_pins vid_out/aclken] [get_bd_pins vid_out/vid_io_out_ce]

    # AXI connections: control registers on GP0, frame buffer reads on HP0
    axi_lite_to_gp0 vfb_rd/s_axi_CTRL
    axi_lite_to_gp0 vtc/ctrl
    apply_bd_automation -rule xilinx.com:bd_rule:axi4 -config [list \
        Clk_master {Auto} Clk_slave {Auto} Clk_xbar {Auto} \
        Master {/vfb_rd/m_axi_mm_video} Slave {/ps7/S_AXI_HP0} \
        ddr_seg {Auto} intc_ip {New AXI Interconnect} master_apm {0}] \
        [get_bd_intf_pins ps7/S_AXI_HP0]
}

# ---- block design: VGA output -------------------------------------------------
#   vid_out -> vga_pack (RTL, 12-bit VGA, registered in the IOBs) -> VGA connector
proc build_vga {} {
    set pclk [get_bd_pins clk_wiz_pix/clk_out1]
    # RTL: 24-bit RGB -> 12-bit VGA, registered on the pixel clock
    create_bd_cell -type module -reference vga_pack vga_pack_0
    connect_bd_net $pclk [get_bd_pins vga_pack_0/pix_clk]
    connect_bd_net [get_bd_pins vid_out/vid_data]         [get_bd_pins vga_pack_0/vid_data]
    connect_bd_net [get_bd_pins vid_out/vid_active_video] [get_bd_pins vga_pack_0/vid_active]
    connect_bd_net [get_bd_pins vid_out/vid_hsync]        [get_bd_pins vga_pack_0/vid_hsync]
    connect_bd_net [get_bd_pins vid_out/vid_vsync]        [get_bd_pins vga_pack_0/vid_vsync]
    foreach c {r g b} {
        create_bd_port -dir O -from 3 -to 0 vga_$c
        connect_bd_net [get_bd_pins vga_pack_0/vga_$c] [get_bd_ports vga_$c]
    }
    foreach s {hs vs} {
        create_bd_port -dir O vga_$s
        connect_bd_net [get_bd_pins vga_pack_0/vga_$s] [get_bd_ports vga_$s]
    }
}

# ---- block design: HDMI output (ADV7511) ----------------------------------------
#   vid_out -> hdmi_pack (RTL: RGB -> BT.709 YCbCr 4:2:2, 16 bit, separate syncs)
#   -> ADV7511 HD_D[15:0], HD-DE, HD-HSYNC, HD-VSYNC; HD-CLK = pixel clock through an ODDR.
#   ADV7511 configuration over PS I2C1 on EMIO (address 0x39), done by software
#   (zb_hdmi_* in sw/common/zb_platform.c). Same pixel bus as VGA: both show the same image.
proc build_hdmi {} {
    set pclk [get_bd_pins clk_wiz_pix/clk_out1]

    set_property -dict [list \
        CONFIG.PCW_I2C1_PERIPHERAL_ENABLE {1} \
        CONFIG.PCW_I2C1_I2C1_IO {EMIO} \
    ] [get_bd_cells ps7]
    # the wrapper inserts IOBUFs: hdmi_iic_scl_io / hdmi_iic_sda_io
    create_bd_intf_port -mode Master -vlnv xilinx.com:interface:iic_rtl:1.0 hdmi_iic
    connect_bd_intf_net [get_bd_intf_pins ps7/IIC_1] [get_bd_intf_ports hdmi_iic]

    create_bd_cell -type module -reference hdmi_pack hdmi_pack_0
    connect_bd_net $pclk [get_bd_pins hdmi_pack_0/pix_clk]
    connect_bd_net [get_bd_pins vid_out/vid_data]         [get_bd_pins hdmi_pack_0/vid_data]
    connect_bd_net [get_bd_pins vid_out/vid_active_video] [get_bd_pins hdmi_pack_0/vid_active]
    connect_bd_net [get_bd_pins vid_out/vid_hsync]        [get_bd_pins hdmi_pack_0/vid_hsync]
    connect_bd_net [get_bd_pins vid_out/vid_vsync]        [get_bd_pins hdmi_pack_0/vid_vsync]
    create_bd_port -dir O -from 15 -to 0 hd_d
    connect_bd_net [get_bd_pins hdmi_pack_0/hd_d] [get_bd_ports hd_d]
    foreach s {de hsync vsync} {
        create_bd_port -dir O hd_$s
        connect_bd_net [get_bd_pins hdmi_pack_0/hd_$s] [get_bd_ports hd_$s]
    }

    create_bd_cell -type module -reference clk_forward hdmi_clk_fwd
    connect_bd_net $pclk [get_bd_pins hdmi_clk_fwd/clk_in]
    create_bd_port -dir O hd_clk
    connect_bd_net [get_bd_pins hdmi_clk_fwd/clk_out] [get_bd_ports hd_clk]
}

# Single-bit slice of a bus (xlslice, or inline-HDL slice if unavailable).
proc slice_bit {name width bit} {
    if {[catch {create_bd_cell -type ip -vlnv xilinx.com:ip:xlslice $name}]} {
        create_bd_cell -type inline_hdl -vlnv xilinx.com:inline_hdl:ilslice $name
    }
    set_property -dict [list CONFIG.DIN_WIDTH $width CONFIG.DIN_FROM $bit CONFIG.DIN_TO $bit] [get_bd_cells $name]
    return $name
}

# N-input concat (xlconcat, or inline-HDL concat if unavailable).
proc concat_n {name n} {
    if {[catch {create_bd_cell -type ip -vlnv xilinx.com:ip:xlconcat $name}]} {
        create_bd_cell -type inline_hdl -vlnv xilinx.com:inline_hdl:ilconcat $name
    }
    set_property -dict [list CONFIG.NUM_PORTS $n] [get_bd_cells $name]
    return $name
}

# Remove whatever net currently drives/loads a pin (used when re-wiring an existing design).
proc disconnect_pin {pin} {
    set n [get_bd_nets -quiet -of_objects [get_bd_pins $pin]]
    if {$n ne ""} { delete_bd_objs $n }
}

# ---- block design: OV7670 camera (Muse Lab PMOD-CAMERA on JA/JB) -------------
#   XCLK 25 MHz (FCLK2 -> ODDR), SCCB on PS I2C0 via EMIO,
#   dvp_capture (PCLK domain, RGB565 -> RGB888 AXI4-Stream)
#   -> async AXIS FIFO -> Video Frame Buffer Write (vfb_wr) -> HP1 -> DRAM
#   EMIO GPIO 54 = vfb_rd reset, 55 = vfb_wr reset. IRQ_F2P[0] = vfb_rd, [1] = vfb_wr.
# Works on a fresh design (after build_video) and on the existing v2 design.
proc build_camera {} {
    set fclk1 [get_bd_pins ps7/FCLK_CLK1]

    # PS: FCLK2 for XCLK, I2C0 on EMIO, HP1 for camera writes, 2 EMIO GPIOs
    disconnect_pin ps7/GPIO_O
    disconnect_pin ps7/IRQ_F2P
    set_property -dict [list \
        CONFIG.PCW_EN_CLK2_PORT {1} \
        CONFIG.PCW_FPGA2_PERIPHERAL_FREQMHZ {25} \
        CONFIG.PCW_I2C0_PERIPHERAL_ENABLE {1} \
        CONFIG.PCW_I2C0_I2C0_IO {EMIO} \
        CONFIG.PCW_USE_S_AXI_HP1 {1} \
        CONFIG.PCW_S_AXI_HP1_DATA_WIDTH {64} \
        CONFIG.PCW_GPIO_EMIO_GPIO_IO {2} \
    ] [get_bd_cells ps7]

    # SCCB (I2C) to the camera; the wrapper inserts IOBUFs: cam_iic_scl_io / cam_iic_sda_io
    create_bd_intf_port -mode Master -vlnv xilinx.com:interface:iic_rtl:1.0 cam_iic
    connect_bd_intf_net [get_bd_intf_pins ps7/IIC_0] [get_bd_intf_ports cam_iic]

    # EMIO resets for both frame buffer DMAs
    slice_bit emio_rst_rd 2 0
    slice_bit emio_rst_wr 2 1
    connect_bd_net [get_bd_pins ps7/GPIO_O] [get_bd_pins emio_rst_rd/Din] [get_bd_pins emio_rst_wr/Din]
    connect_bd_net [get_bd_pins emio_rst_rd/Dout] [get_bd_pins vfb_rd/ap_rst_n]

    # XCLK out through an ODDR
    create_bd_cell -type module -reference clk_forward xclk_fwd
    connect_bd_net [get_bd_pins ps7/FCLK_CLK2] [get_bd_pins xclk_fwd/clk_in]
    create_bd_port -dir O cam_xclk
    connect_bd_net [get_bd_pins xclk_fwd/clk_out] [get_bd_ports cam_xclk]

    # Camera inputs
    create_bd_port -dir I -type clk -freq_hz 25000000 cam_pclk
    create_bd_port -dir I -from 7 -to 0 cam_d
    create_bd_port -dir I cam_href
    create_bd_port -dir I cam_vsync

    # PCLK-domain reset
    create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset rst_cam
    connect_bd_net [get_bd_ports cam_pclk] [get_bd_pins rst_cam/slowest_sync_clk]
    connect_bd_net [get_bd_pins ps7/FCLK_RESET0_N] [get_bd_pins rst_cam/ext_reset_in]

    # Capture
    create_bd_cell -type module -reference dvp_capture cam_cap
    connect_bd_net [get_bd_ports cam_pclk]  [get_bd_pins cam_cap/pclk]
    connect_bd_net [get_bd_pins rst_cam/peripheral_aresetn] [get_bd_pins cam_cap/aresetn]
    connect_bd_net [get_bd_ports cam_d]     [get_bd_pins cam_cap/cam_d]
    connect_bd_net [get_bd_ports cam_href]  [get_bd_pins cam_cap/cam_href]
    connect_bd_net [get_bd_ports cam_vsync] [get_bd_pins cam_cap/cam_vsync]

    # PCLK -> 150 MHz
    create_bd_cell -type ip -vlnv xilinx.com:ip:axis_data_fifo cam_fifo
    set_property -dict [list CONFIG.FIFO_DEPTH {2048} CONFIG.IS_ACLK_ASYNC {1}] [get_bd_cells cam_fifo]
    connect_bd_net [get_bd_ports cam_pclk] [get_bd_pins cam_fifo/s_axis_aclk]
    connect_bd_net [get_bd_pins rst_cam/peripheral_aresetn] [get_bd_pins cam_fifo/s_axis_aresetn]
    connect_bd_net $fclk1 [get_bd_pins cam_fifo/m_axis_aclk]
    connect_bd_intf_net [get_bd_intf_pins cam_cap/m_axis] [get_bd_intf_pins cam_fifo/S_AXIS]

    # Frame buffer write DMA
    create_bd_cell -type ip -vlnv xilinx.com:ip:v_frmbuf_wr vfb_wr
    set_property -dict [list \
        CONFIG.MAX_COLS {1024} \
        CONFIG.MAX_ROWS {768} \
        CONFIG.SAMPLES_PER_CLOCK {1} \
        CONFIG.MAX_DATA_WIDTH {8} \
        CONFIG.AXIMM_DATA_WIDTH {64} \
        CONFIG.HAS_BGRX8 {1} \
        CONFIG.HAS_RGBX8 {1} \
    ] [get_bd_cells vfb_wr]
    connect_bd_net $fclk1 [get_bd_pins vfb_wr/ap_clk]
    connect_bd_net [get_bd_pins emio_rst_wr/Dout] [get_bd_pins vfb_wr/ap_rst_n]
    connect_bd_intf_net [get_bd_intf_pins cam_fifo/M_AXIS] [get_bd_intf_pins vfb_wr/s_axis_video]

    # Interrupts
    concat_n irq_concat 2
    connect_bd_net [get_bd_pins vfb_rd/interrupt] [get_bd_pins irq_concat/In0]
    connect_bd_net [get_bd_pins vfb_wr/interrupt] [get_bd_pins irq_concat/In1]
    connect_bd_net [get_bd_pins irq_concat/dout]  [get_bd_pins ps7/IRQ_F2P]

    # AXI: control on GP0, pixel writes on HP1
    axi_lite_to_gp0 vfb_wr/s_axi_CTRL
    apply_bd_automation -rule xilinx.com:bd_rule:axi4 -config [list \
        Clk_master {Auto} Clk_slave {Auto} Clk_xbar {Auto} \
        Master {/vfb_wr/m_axi_mm_video} Slave {/ps7/S_AXI_HP1} \
        ddr_seg {Auto} intc_ip {New AXI Interconnect} master_apm {0}] \
        [get_bd_intf_pins ps7/S_AXI_HP1]
}

# ---- block design: processing slot in the camera stream ----------------------
#   cam_fifo -> proc_slot (RTL, 150 MHz, AXI-Lite on GP0) -> vfb_wr
#   Fixed AXI-Lite address so software does not depend on generated macros.
set PROC_SLOT_ADDR 0x43D00000
proc build_processing {} {
    global PROC_SLOT_ADDR
    set fclk1 [get_bd_pins ps7/FCLK_CLK1]

    # Break the direct camera FIFO -> frame buffer write link
    set n [get_bd_intf_nets -quiet -of_objects [get_bd_intf_pins vfb_wr/s_axis_video]]
    if {$n ne ""} { delete_bd_objs $n }

    create_bd_cell -type module -reference proc_slot proc_slot
    connect_bd_net $fclk1 [get_bd_pins proc_slot/aclk]
    connect_bd_net [get_bd_pins rst_vid/peripheral_aresetn] [get_bd_pins proc_slot/aresetn]
    connect_bd_intf_net [get_bd_intf_pins cam_fifo/M_AXIS]   [get_bd_intf_pins proc_slot/s_axis]
    connect_bd_intf_net [get_bd_intf_pins proc_slot/m_axis]  [get_bd_intf_pins vfb_wr/s_axis_video]

    axi_lite_to_gp0 proc_slot/s_axi
    set seg [get_bd_addr_segs -quiet -of_objects [get_bd_intf_pins proc_slot/s_axi]]
    set addr_seg [get_bd_addr_segs -quiet ps7/Data/SEG_proc_slot_*]
    if {$addr_seg ne ""} {
        set_property offset $PROC_SLOT_ADDR $addr_seg
        set_property range 4K $addr_seg
    } elseif {$seg ne ""} {
        assign_bd_address -offset $PROC_SLOT_ADDR -range 4K $seg
    }
}

# ---- block design: display modes switchable at run time ----------------------
#   Enables dynamic reconfiguration on clk_wiz_pix (AXI-Lite on GP0).
#   The static output frequency is set to the HIGHEST mode so that timing closure
#   covers every mode; software programs the actual mode at start-up
#   (zb_display_set_mode in sw/common/zb_platform.c).
proc build_display_reconfig {max_pix_mhz} {
    set_property -dict [list \
        CONFIG.USE_DYN_RECONFIG {true} \
        CONFIG.CLKOUT1_REQUESTED_OUT_FREQ $max_pix_mhz \
    ] [get_bd_cells clk_wiz_pix]
    axi_lite_to_gp0 clk_wiz_pix/s_axi_lite
}

# ---- block design: memory-to-memory test path ----------------------------------
#   DRAM -> vfb_rd2 (HP2) -> proc_slot s_axis_mem -> vfb_wr -> DRAM
#   proc_slot CTRL[4] selects camera or memory input.
#   EMIO GPIO 56 = vfb_rd2 reset. IRQ_F2P[2] = vfb_rd2.
#   Also raises vfb_wr to max_cols x max_rows (camera up to 1600x1200, tests up to max).
proc build_memtest {max_cols max_rows} {
    set fclk1 [get_bd_pins ps7/FCLK_CLK1]

    # PS: HP2 for test-image reads, a third EMIO GPIO for the new DMA reset
    disconnect_pin ps7/GPIO_O
    set_property -dict [list \
        CONFIG.PCW_USE_S_AXI_HP2 {1} \
        CONFIG.PCW_S_AXI_HP2_DATA_WIDTH {64} \
        CONFIG.PCW_GPIO_EMIO_GPIO_IO {3} \
    ] [get_bd_cells ps7]
    set_property CONFIG.DIN_WIDTH {3} [get_bd_cells emio_rst_rd]
    set_property CONFIG.DIN_WIDTH {3} [get_bd_cells emio_rst_wr]
    slice_bit emio_rst_rd2 3 2
    connect_bd_net [get_bd_pins ps7/GPIO_O] [get_bd_pins emio_rst_rd/Din] \
                   [get_bd_pins emio_rst_wr/Din] [get_bd_pins emio_rst_rd2/Din]

    # Larger frames for the write DMA
    set_property -dict [list CONFIG.MAX_COLS $max_cols CONFIG.MAX_ROWS $max_rows] [get_bd_cells vfb_wr]

    # Test-image source
    create_bd_cell -type ip -vlnv xilinx.com:ip:v_frmbuf_rd vfb_rd2
    set_property -dict [list \
        CONFIG.MAX_COLS $max_cols \
        CONFIG.MAX_ROWS $max_rows \
        CONFIG.SAMPLES_PER_CLOCK {1} \
        CONFIG.MAX_DATA_WIDTH {8} \
        CONFIG.AXIMM_DATA_WIDTH {64} \
        CONFIG.HAS_BGRX8 {1} \
        CONFIG.HAS_RGBX8 {1} \
    ] [get_bd_cells vfb_rd2]
    connect_bd_net $fclk1 [get_bd_pins vfb_rd2/ap_clk]
    connect_bd_net [get_bd_pins emio_rst_rd2/Dout] [get_bd_pins vfb_rd2/ap_rst_n]
    connect_bd_intf_net [get_bd_intf_pins vfb_rd2/m_axis_video] [get_bd_intf_pins proc_slot/s_axis_mem]

    # Third interrupt
    set_property CONFIG.NUM_PORTS {3} [get_bd_cells irq_concat]
    connect_bd_net [get_bd_pins vfb_rd2/interrupt] [get_bd_pins irq_concat/In2]

    # AXI: control on GP0, reads on HP2
    axi_lite_to_gp0 vfb_rd2/s_axi_CTRL
    apply_bd_automation -rule xilinx.com:bd_rule:axi4 -config [list \
        Clk_master {Auto} Clk_slave {Auto} Clk_xbar {Auto} \
        Master {/vfb_rd2/m_axi_mm_video} Slave {/ps7/S_AXI_HP2} \
        ddr_seg {Auto} intc_ip {New AXI Interconnect} master_apm {0}] \
        [get_bd_intf_pins ps7/S_AXI_HP2]
}

# ---- outputs: wrapper, bitstream, reports, XSA ------------------------------
proc build_outputs {proj_dir bd_name jobs} {
    set bd_file [get_files $bd_name.bd]
    generate_target all $bd_file
    if {[catch {set wrapper [make_wrapper -files $bd_file -top -force]}]} {
        set wrapper [make_wrapper -files $bd_file -top]
    }
    if {[llength [get_files -quiet $wrapper]] == 0} { add_files -norecurse $wrapper }
    set_property top ${bd_name}_wrapper [current_fileset]
    update_compile_order -fileset sources_1

    # Only top-level synthesis is reset; per-IP (out-of-context) runs are reused if unchanged.
    reset_run synth_1
    launch_runs impl_1 -to_step write_bitstream -jobs $jobs
    wait_on_run impl_1
    if {[get_property PROGRESS [get_runs impl_1]] ne "100%"} {
        error "Implementation failed. See [get_property DIRECTORY [get_runs impl_1]]"
    }
    open_run impl_1
    report_timing_summary -file [file join $proj_dir timing_summary.txt]
    report_utilization    -file [file join $proj_dir utilization.txt]
    set xsa [file join $proj_dir ${bd_name}_wrapper.xsa]
    write_hw_platform -fixed -include_bit -force -file $xsa
    puts "DONE. XSA for Vitis: $xsa"
}
