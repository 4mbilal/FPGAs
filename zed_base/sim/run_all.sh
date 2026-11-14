#!/bin/sh
# run_all.sh - runs every RTL testbench and C/Python model check (Icarus Verilog, gcc, python3).
#   sh sim/run_all.sh          (from the repository root)
# Outputs go to sim/out/ (ignored by git).
set -e
cd "$(dirname "$0")/.."
O=sim/out; mkdir -p $O

echo "== dvp_capture";    iverilog -g2005 -o $O/tb_dvp  sim/tb_dvp_capture.v rtl/dvp_capture.v && vvp -n $O/tb_dvp | tail -3
echo "== proc_slot";      iverilog -g2005 -o $O/tb_slot sim/tb_proc_slot.v rtl/proc_slot.v && (cd $O && vvp -n tb_slot | tail -7)
gcc -I sw/common -o $O/check_vectors sim/check_vectors.c sw/common/sw_kernels.c && $O/check_vectors $O/slot_vectors.txt
echo "== proc_slot memory input"; iverilog -g2005 -o $O/tb_mem sim/tb_proc_slot_mem.v rtl/proc_slot.v && vvp -n $O/tb_mem | tail -6
echo "== hdmi_pack";      iverilog -g2005 -o $O/tb_hdmi sim/tb_hdmi_pack.v rtl/hdmi_pack.v && (cd $O && vvp -n tb_hdmi | tail -1)
gcc -O2 -o $O/check_hdmi sim/check_hdmi.c && $O/check_hdmi $O/hdmi_vectors.txt
echo "== buffer rotation model"; python3 sim/rotation_model.py | tail -4
echo "== proc_slot curve table and statistics"; iverilog -g2005 -o $O/tb_lle sim/tb_proc_slot_lle.v rtl/proc_slot.v && (cd $O && vvp -n tb_lle | tail -3)
gcc -Wall -O2 -I sw/common -o $O/check_lle sim/check_lle.c sw/common/lle_global.c && $O/check_lle $O/lle_vectors.txt
echo "== accel_stub (shell v7 reserved window)"; iverilog -g2005 -o $O/tb_acc sim/tb_accel_stub.v rtl/accel_stub.v && (cd $O && vvp -n tb_acc | grep accel_stub)
echo "== Python jig, low-light logic (register model of proc_slot)"; python3 sim/test_jig_lle.py | tail -4
