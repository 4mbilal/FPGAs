// tb_hdmi_pack.v - Icarus testbench for hdmi_pack
//   iverilog -g2005 -o tb_hdmi sim/tb_hdmi_pack.v rtl/hdmi_pack.v && vvp -n tb_hdmi
//   gcc -O2 -o check_hdmi sim/check_hdmi.c && ./check_hdmi hdmi_vectors.txt
// Drives a small video raster (DE, HSYNC, VSYNC like the VTC) with random and extreme
// RGB values and writes every clock's input and output to hdmi_vectors.txt; check_hdmi.c
// (C golden model) checks the output bit-exact, including latency and sync alignment.
`timescale 1ns/1ps
module tb;
    localparam W = 12, HB = 5, H = 4, VB = 2, FRAMES = 3;

    reg clk = 0;
    always #3.367 clk = ~clk;                      // 148.5 MHz

    reg  [23:0] data = 0;
    reg  de = 0, hs = 0, vs = 0;
    wire [15:0] d;  wire o_de, o_hs, o_vs;

    hdmi_pack dut (.pix_clk(clk), .vid_data(data), .vid_active(de), .vid_hsync(hs), .vid_vsync(vs),
                   .hd_d(d), .hd_de(o_de), .hd_hsync(o_hs), .hd_vsync(o_vs));

    integer f, seed = 11, x, y, fr, n = 0;
    initial f = $fopen("hdmi_vectors.txt", "w");

    // one line per clock: inputs as applied in this cycle, outputs as seen in this cycle
    always @(posedge clk) begin
        #0.1;
        $fwrite(f, "%0d %b %b %b %h  %b %b %b %h\n", n, de, hs, vs, data, o_de, o_hs, o_vs, d);
        n = n + 1;
    end

    function [7:0] val(input integer k);
        case (k % 6) 0: val = 8'h00; 1: val = 8'hFF; default: val = $random(seed); endcase
    endfunction

    task pix(input a, input h_s, input v_s); begin
        @(negedge clk);
        de = a; hs = h_s; vs = v_s;
        data = a ? {val($random(seed) & 255), val($random(seed) & 255), val($random(seed) & 255)} : 24'h0;
    end endtask

    initial begin
        repeat (4) pix(0, 0, 0);
        for (fr = 0; fr < FRAMES; fr = fr + 1) begin
            for (y = 0; y < H + VB; y = y + 1)
                for (x = 0; x < W + HB; x = x + 1)
                    pix(y < H && x < W, x == W + 1 || x == W + 2, y == H + 1);
        end
        repeat (10) pix(0, 0, 0);
        $fclose(f);
        $display("vectors written: %0d clocks", n);
        $finish;
    end
endmodule
