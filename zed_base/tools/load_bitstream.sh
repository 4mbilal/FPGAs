#!/bin/sh
# load_bitstream.sh - load a new PL bitstream at run time (Linux on the ZedBoard, as root).
#   sudo sh load_bitstream.sh system_wrapper.bit.bin
# Stop every program that uses the PL (jig, jig.py, lle_batch.py) first: reprogramming
# while a DMA runs leaves the HP ports in an undefined state. The UIO devices stay valid
# because a shell-compatible bitstream keeps every address and interrupt.
set -e
f=$(readlink -f "$1")
[ -f "$f" ] || { echo "usage: load_bitstream.sh <file.bit.bin>"; exit 1; }
if pgrep -f "jig|lle_batch" >/dev/null; then echo "a PL user is still running (jig / lle_batch): stop it first"; exit 1; fi
cp "$f" /lib/firmware/
echo 0 > /sys/class/fpga_manager/fpga0/flags
echo "$(basename "$f")" > /sys/class/fpga_manager/fpga0/firmware
echo "FPGA manager state: $(cat /sys/class/fpga_manager/fpga0/state)"
