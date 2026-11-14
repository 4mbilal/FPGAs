# batch.tcl - run a folder of raw images through the jig's batch mode (mailbox in DRAM).
#
#   xsct tools/batch.tcl <in_dir> <out_dir> [kernel]      (kernel 4 = LLE global curve)
#
# The jig (sw/common/jig.c) must be running on the board, started from Vitis or with xsct.
# Input files:  <name>_<W>x<H>.bin   packed XRGB8888, little endian (tools/rawimg.py)
# Output files: <name>_<W>x<H>.hw.bin   hardware result (proc_slot)
#               <name>_<W>x<H>.ref.bin  C golden model on the board (sw/common/lle_global.c)
#               results.csv             one line per image: iterations, mean, mismatches, ...
# Memory accesses stop the CPU briefly (stop / con), so they are coherent with its caches.

set in_dir  [lindex $argv 0]
set out_dir [lindex $argv 1]
set kernel  [expr {[llength $argv] > 2 ? [lindex $argv 2] : 4}]
if {$in_dir eq "" || $out_dir eq ""} { puts "usage: xsct batch.tcl <in_dir> <out_dir> \[kernel\]"; exit 1 }

set MB      0x13000000
set IN      0x10000000
set HW      0x11000000
set REF     0x12000000

connect
targets -set -nocase -filter {name =~ "*A9*#0"}
file mkdir $out_dir

proc rd {addr} { return [lindex [mrd -value $addr 1] 0] }
proc halted {} { return [string match "*Stopped*" [state]] }
proc pause {} { if {![halted]} { stop } }
proc resume {} { if {[halted]} { con } }

set csv [open [file join $out_dir results.csv] w]
puts $csv "name,w,h,status,iterations,mean_x1e6,mismatches,stats_identical,slot_cycles"
set files [lsort [glob -nocomplain -directory $in_dir *.bin]]
puts "[llength $files] images"
set n 0
foreach f $files {
    set base [file rootname [file tail $f]]
    if {![regexp {_(\d+)x(\d+)$} $base -> w h]} { puts "skip $f (name must end in _WxH.bin)"; continue }
    pause
    dow -data $f $IN
    mwr [expr {$MB + 4}]  $w
    mwr [expr {$MB + 8}]  $h
    mwr [expr {$MB + 12}] $kernel
    set seq0 [rd [expr {$MB + 20}]]
    mwr $MB 1
    resume
    set t0 [clock milliseconds]
    while {1} {
        after 200
        pause
        set st  [rd [expr {$MB + 16}]]
        set seq [rd [expr {$MB + 20}]]
        if {$seq != $seq0 && ($st == 2 || $st == 3)} break
        resume
        if {[clock milliseconds] - $t0 > 30000} { puts "timeout on $base"; set st -1; pause; break }
    }
    set px [expr {$w * $h}]
    mrd -bin -file [file join $out_dir $base.hw.bin]  $HW  $px
    mrd -bin -file [file join $out_dir $base.ref.bin] $REF $px
    set it  [rd [expr {$MB + 24}]]
    set mn  [rd [expr {$MB + 28}]]
    set mm  [rd [expr {$MB + 32}]]
    set sok [rd [expr {$MB + 36}]]
    set cyc [rd [expr {$MB + 40}]]
    resume
    puts $csv "$base,$w,$h,$st,$it,$mn,$mm,$sok,$cyc"
    flush $csv
    incr n
    puts "$n/[llength $files] $base: status $st, $it iterations, $mm mismatches"
}
close $csv
puts "done: $n images, results in [file join $out_dir results.csv]"
