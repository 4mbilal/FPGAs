# ----------------------------------------------------------------------------
# vga.xdc - ZedBoard VGA connector, Bank 33 (fixed 3.3 V)
# Pins from Avnet zedboard_master.xdc. Index 3 = schematic R4/G4/B4 (MSB).
# If colours look inverted in intensity, the MSB/LSB order is reversed.
# ----------------------------------------------------------------------------
set_property -dict {PACKAGE_PIN V20  IOSTANDARD LVCMOS33} [get_ports {vga_r[0]}]
set_property -dict {PACKAGE_PIN U20  IOSTANDARD LVCMOS33} [get_ports {vga_r[1]}]
set_property -dict {PACKAGE_PIN V19  IOSTANDARD LVCMOS33} [get_ports {vga_r[2]}]
set_property -dict {PACKAGE_PIN V18  IOSTANDARD LVCMOS33} [get_ports {vga_r[3]}]

set_property -dict {PACKAGE_PIN AB22 IOSTANDARD LVCMOS33} [get_ports {vga_g[0]}]
set_property -dict {PACKAGE_PIN AA22 IOSTANDARD LVCMOS33} [get_ports {vga_g[1]}]
set_property -dict {PACKAGE_PIN AB21 IOSTANDARD LVCMOS33} [get_ports {vga_g[2]}]
set_property -dict {PACKAGE_PIN AA21 IOSTANDARD LVCMOS33} [get_ports {vga_g[3]}]

set_property -dict {PACKAGE_PIN Y21  IOSTANDARD LVCMOS33} [get_ports {vga_b[0]}]
set_property -dict {PACKAGE_PIN Y20  IOSTANDARD LVCMOS33} [get_ports {vga_b[1]}]
set_property -dict {PACKAGE_PIN AB20 IOSTANDARD LVCMOS33} [get_ports {vga_b[2]}]
set_property -dict {PACKAGE_PIN AB19 IOSTANDARD LVCMOS33} [get_ports {vga_b[3]}]

set_property -dict {PACKAGE_PIN AA19 IOSTANDARD LVCMOS33} [get_ports vga_hs]
set_property -dict {PACKAGE_PIN Y19  IOSTANDARD LVCMOS33} [get_ports vga_vs]

# Pack the vga_pack output registers into the IOBs so all 14 pins switch together.
set_property IOB TRUE [get_ports {vga_r[*] vga_g[*] vga_b[*] vga_hs vga_vs}]
