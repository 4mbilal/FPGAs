# ----------------------------------------------------------------------------
# camera.xdc - Muse Lab PMOD-CAMERA v1.0 (OV7670) on ZedBoard JA + JB, Bank 13 (3.3 V)
#
# Mapping from the earlier zeboard.xdc for this module. It agrees with the
# camera silkscreen if the SECOND label of each pair is on the top row (pins 1-4):
#   JA: JA1/JA7 = NC/NC, JA2/JA8 = PCLK/XCLK, JA3/JA9 = HREF/VSYNC, JA4/JA10 = SCL/SDA
#   JB: JB1/JB7 = D7/D6, JB2/JB8 = D5/D4,   JB3/JB9 = D3/D2,      JB4/JB10 = D1/D0
# Correction vs zeboard.xdc: HREF is JA3 = Y10. The old file used Y19 (a typo copied
# from the hardware user guide), which is the VGA vsync pin.
# ----------------------------------------------------------------------------

# Control / timing (JA)
set_property -dict {PACKAGE_PIN AB10 IOSTANDARD LVCMOS33} [get_ports cam_xclk]
set_property -dict {PACKAGE_PIN AA11 IOSTANDARD LVCMOS33} [get_ports cam_pclk]
set_property -dict {PACKAGE_PIN Y10  IOSTANDARD LVCMOS33} [get_ports cam_href]
set_property -dict {PACKAGE_PIN AB9  IOSTANDARD LVCMOS33} [get_ports cam_vsync]
set_property -dict {PACKAGE_PIN AA9  IOSTANDARD LVCMOS33} [get_ports cam_iic_scl_io]
set_property -dict {PACKAGE_PIN AA8  IOSTANDARD LVCMOS33} [get_ports cam_iic_sda_io]

# Data (JB)
set_property -dict {PACKAGE_PIN V8   IOSTANDARD LVCMOS33} [get_ports {cam_d[0]}]
set_property -dict {PACKAGE_PIN W8   IOSTANDARD LVCMOS33} [get_ports {cam_d[1]}]
set_property -dict {PACKAGE_PIN V9   IOSTANDARD LVCMOS33} [get_ports {cam_d[2]}]
set_property -dict {PACKAGE_PIN V10  IOSTANDARD LVCMOS33} [get_ports {cam_d[3]}]
set_property -dict {PACKAGE_PIN W10  IOSTANDARD LVCMOS33} [get_ports {cam_d[4]}]
set_property -dict {PACKAGE_PIN W11  IOSTANDARD LVCMOS33} [get_ports {cam_d[5]}]
set_property -dict {PACKAGE_PIN V12  IOSTANDARD LVCMOS33} [get_ports {cam_d[6]}]
set_property -dict {PACKAGE_PIN W12  IOSTANDARD LVCMOS33} [get_ports {cam_d[7]}]

# SCCB pull-ups (weak internal; fine at 100 kHz. Remove if the module has its own.)
set_property PULLUP true [get_ports {cam_iic_scl_io cam_iic_sda_io}]

# XCLK: slow edges are enough at 25 MHz and reduce ringing on the Pmod cable
set_property SLEW SLOW [get_ports cam_xclk]

# PCLK: constrained for up to ~66 MHz so larger OV2640 output sizes (up to 1600x1200) are covered.
# The Pmod pin is not a clock-capable input, so allow general routing to the BUFG.
create_clock -name cam_pclk -period 15.000 [get_ports cam_pclk]
set_property CLOCK_DEDICATED_ROUTE FALSE [get_nets cam_pclk_IBUF]

# Capture registers in the IOBs for consistent input timing
set_property IOB TRUE [get_ports {cam_d[*] cam_href cam_vsync}]
