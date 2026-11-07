# ----------------------------------------------------------------------------
# hdmi.xdc - ZedBoard ADV7511 HDMI transmitter, Bank 33 (fixed 3.3 V)
# Pins from Avnet zedboard_master.xdc, identical to the Analog Devices ZedBoard
# reference design (projects/common/zed/zed_system_constr.xdc). The hardware user
# guide's table lists HD-D1 on AA12 (OLED pin) and bank 35: both wrong, AA13 is right.
# HD-INT (W16) and the S/PDIF pins are not used.
# ----------------------------------------------------------------------------
set_property -dict {PACKAGE_PIN W18  IOSTANDARD LVCMOS33} [get_ports hd_clk]
set_property -dict {PACKAGE_PIN U16  IOSTANDARD LVCMOS33} [get_ports hd_de]
set_property -dict {PACKAGE_PIN V17  IOSTANDARD LVCMOS33} [get_ports hd_hsync]
set_property -dict {PACKAGE_PIN W17  IOSTANDARD LVCMOS33} [get_ports hd_vsync]

set_property -dict {PACKAGE_PIN Y13  IOSTANDARD LVCMOS33} [get_ports {hd_d[0]}]
set_property -dict {PACKAGE_PIN AA13 IOSTANDARD LVCMOS33} [get_ports {hd_d[1]}]
set_property -dict {PACKAGE_PIN AA14 IOSTANDARD LVCMOS33} [get_ports {hd_d[2]}]
set_property -dict {PACKAGE_PIN Y14  IOSTANDARD LVCMOS33} [get_ports {hd_d[3]}]
set_property -dict {PACKAGE_PIN AB15 IOSTANDARD LVCMOS33} [get_ports {hd_d[4]}]
set_property -dict {PACKAGE_PIN AB16 IOSTANDARD LVCMOS33} [get_ports {hd_d[5]}]
set_property -dict {PACKAGE_PIN AA16 IOSTANDARD LVCMOS33} [get_ports {hd_d[6]}]
set_property -dict {PACKAGE_PIN AB17 IOSTANDARD LVCMOS33} [get_ports {hd_d[7]}]
set_property -dict {PACKAGE_PIN AA17 IOSTANDARD LVCMOS33} [get_ports {hd_d[8]}]
set_property -dict {PACKAGE_PIN Y15  IOSTANDARD LVCMOS33} [get_ports {hd_d[9]}]
set_property -dict {PACKAGE_PIN W13  IOSTANDARD LVCMOS33} [get_ports {hd_d[10]}]
set_property -dict {PACKAGE_PIN W15  IOSTANDARD LVCMOS33} [get_ports {hd_d[11]}]
set_property -dict {PACKAGE_PIN V15  IOSTANDARD LVCMOS33} [get_ports {hd_d[12]}]
set_property -dict {PACKAGE_PIN U17  IOSTANDARD LVCMOS33} [get_ports {hd_d[13]}]
set_property -dict {PACKAGE_PIN V14  IOSTANDARD LVCMOS33} [get_ports {hd_d[14]}]
set_property -dict {PACKAGE_PIN V13  IOSTANDARD LVCMOS33} [get_ports {hd_d[15]}]

# ADV7511 I2C (PS I2C1 on EMIO)
set_property -dict {PACKAGE_PIN AA18 IOSTANDARD LVCMOS33} [get_ports hdmi_iic_scl_io]
set_property -dict {PACKAGE_PIN Y16  IOSTANDARD LVCMOS33} [get_ports hdmi_iic_sda_io]
# weak internal pull-ups in addition to the board's (harmless, as for the camera bus)
set_property PULLUP TRUE [get_ports {hdmi_iic_scl_io hdmi_iic_sda_io}]

# Output registers in the IOBs: data, DE and syncs leave the FPGA with the same delay as
# the forwarded clock (ODDR in the clock pin's OLOGIC). As in the ADI reference design,
# there are no output-delay constraints: the ADV7511 input clock delay (register 0xBA,
# jig key 'k') is the adjustment if the picture shows sparkles at 1080p.
set_property IOB TRUE [get_ports {hd_d[*] hd_de hd_hsync hd_vsync}]
