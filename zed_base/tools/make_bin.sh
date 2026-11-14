#!/bin/sh
# make_bin.sh - bitstream (.bit) -> FPGA-manager image (.bin), on the PC with Vivado/Vitis sourced.
#   sh tools/make_bin.sh zed_base/zed_base.runs/impl_1/system_wrapper.bit
# Writes <name>.bit.bin next to the input. Copy it to the board and load it with
# load_bitstream.sh. Only valid when tools/shell_check.py reports SHELL MATCH.
set -e
bit=$(readlink -f "$1")
[ -f "$bit" ] || { echo "usage: make_bin.sh <file.bit>"; exit 1; }
dir=$(dirname "$bit")
printf 'all:\n{\n  %s\n}\n' "$bit" > "$dir/make_bin.bif"
bootgen -image "$dir/make_bin.bif" -arch zynq -process_bitstream bin -w on
ls -l "$bit.bin"
