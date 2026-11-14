// tb_proc_slot_lle.v - curve table (mode 3) and frame statistics of proc_slot
//   iverilog -g2005 -o tb_lle sim/tb_proc_slot_lle.v rtl/proc_slot.v && vvp -n tb_lle
// Checks, under random input gaps and random output backpressure:
//   * mode 3 output = per-channel table of the bank active for that frame
//   * a swap requested mid-frame takes effect only at the next start of frame
//   * table writes during a frame go to the inactive bank (the active one is untouched)
//   * interrupt: status bits, level output only when enabled, write-1-to-clear
//   * statistics latched at each start of frame (previous frame) and on STAT_CTRL:
//     16-bin luma histogram, channel sums, pixel count, sequence number
// Writes lle_vectors.txt (pixels and latched statistics) for sim/check_lle.c, which
// recomputes the statistics with the C golden model in sw/common/lle_global.c.
`timescale 1ns/1ps
module tb;
    localparam W = 32, H = 8, NF = 4;

    reg aclk = 0, aresetn = 0;
    always #3.333 aclk = ~aclk;

    reg  [23:0] s_tdata; reg s_tvalid = 0, s_tuser = 0, s_tlast = 0; wire s_tready;
    wire [23:0] m_tdata; wire m_tvalid, m_tuser, m_tlast; reg m_tready = 1;
    reg  [15:0] awaddr = 0, araddr = 0; reg awvalid = 0, wvalid = 0, bready = 1, arvalid = 0, rready = 1;
    reg  [31:0] wdata = 0;
    wire awready, wready, bvalid, arready, rvalid, irq; wire [1:0] bresp, rresp; wire [31:0] rdata;

    proc_slot dut (
        .aclk(aclk), .aresetn(aresetn),
        .s_axis_tdata(s_tdata), .s_axis_tvalid(s_tvalid), .s_axis_tready(s_tready),
        .s_axis_tuser(s_tuser), .s_axis_tlast(s_tlast),
        .s_axis_mem_tdata(24'd0), .s_axis_mem_tvalid(1'b0), .s_axis_mem_tready(),
        .s_axis_mem_tuser(1'b0), .s_axis_mem_tlast(1'b0),
        .m_axis_tdata(m_tdata), .m_axis_tvalid(m_tvalid), .m_axis_tready(m_tready),
        .m_axis_tuser(m_tuser), .m_axis_tlast(m_tlast),
        .s_axi_awaddr(awaddr), .s_axi_awprot(3'd0), .s_axi_awvalid(awvalid), .s_axi_awready(awready),
        .s_axi_wdata(wdata), .s_axi_wstrb(4'hF), .s_axi_wvalid(wvalid), .s_axi_wready(wready),
        .s_axi_bresp(bresp), .s_axi_bvalid(bvalid), .s_axi_bready(bready),
        .s_axi_araddr(araddr), .s_axi_arprot(3'd0), .s_axi_arvalid(arvalid), .s_axi_arready(arready),
        .s_axi_rdata(rdata), .s_axi_rresp(rresp), .s_axi_rvalid(rvalid), .s_axi_rready(rready), .irq(irq));

    task axi_write(input [15:0] a, input [31:0] d); begin
        @(negedge aclk); awaddr = a; wdata = d; awvalid = 1; wvalid = 1;
        while (!(awready && wready)) @(negedge aclk);
        @(posedge aclk); @(negedge aclk); awvalid = 0; wvalid = 0;
        while (!bvalid) @(negedge aclk);
        @(posedge aclk);
    end endtask
    task axi_read(input [15:0] a, output [31:0] d); begin
        @(negedge aclk); araddr = a; arvalid = 1;
        while (!arready) @(negedge aclk);
        @(posedge aclk); @(negedge aclk); arvalid = 0;
        while (!rvalid) @(negedge aclk);
        d = rdata;
        @(posedge aclk);
    end endtask

    // ---- tables: A (set 0) and B (set 1), channel c = 0 R, 1 G, 2 B --------------
    function [7:0] tab(input integer set, input integer c, input integer x);
        tab = set == 0 ? ((x * 7 + c * 50 + 13) & 255) : ((255 - x) ^ (c * 37));
    endfunction
    task load_table(input integer set); integer c, x; begin
        for (c = 0; c < 3; c = c + 1)
            for (x = 0; x < 256; x = x + 1) axi_write(16'h0400 + 16'h0400 * c + 4 * x, tab(set, c, x));
    end endtask

    // ---- frames ------------------------------------------------------------------
    integer seed = 11;
    function [23:0] pix(input integer f, input integer y, input integer x);
        pix = ((f * 24'h3A1F07) ^ (y * 24'h0B5D33) ^ (x * 24'h17E2C9)) + {f[7:0], y[7:0], x[7:0]};
    endfunction
    // what each output frame should be: mode and table set (-1 = no table)
    integer f_mode [0:NF-1];
    integer f_set  [0:NF-1];

    task send_frame(input integer f); integer x, y; begin
        for (y = 0; y < H; y = y + 1)
            for (x = 0; x < W; x = x + 1) begin
                @(negedge aclk);
                s_tvalid = 0;
                while (($random(seed) & 3) == 0) @(negedge aclk);
                s_tdata = pix(f, y, x); s_tuser = (x == 0 && y == 0); s_tlast = (x == W - 1);
                s_tvalid = 1;
                while (!s_tready) @(negedge aclk);
                @(posedge aclk);
            end
        @(negedge aclk); s_tvalid = 0;
    end endtask

    // ---- sink and checker ----------------------------------------------------------
    function [23:0] expect_px(input integer f, input [23:0] p);
        begin
            if (f_mode[f] == 3)
                expect_px = {tab(f_set[f], 0, p[23:16]), tab(f_set[f], 2, p[15:8]), tab(f_set[f], 1, p[7:0])};
            else
                expect_px = p;
        end
    endfunction
    integer cf = -1, cx = 0, cy = 0, errs = 0, beats = 0;
    integer vf;
    always @(posedge aclk) m_tready <= #0.5 (($random(seed) % 3) != 0);
    always @(posedge aclk) if (m_tvalid && m_tready) begin
        if (m_tuser) begin cf = cf + 1; cx = 0; cy = 0; end
        if (m_tdata !== expect_px(cf, pix(cf, cy, cx)) || m_tuser !== (cx == 0 && cy == 0) || m_tlast !== (cx == W - 1)) begin
            errs = errs + 1;
            if (errs < 10) $display("ERR f%0d y%0d x%0d mode%0d got %h exp %h", cf, cy, cx, f_mode[cf], m_tdata,
                                    expect_px(cf, pix(cf, cy, cx)));
        end
        $fwrite(vf, "P %0d %h\n", cf, pix(cf, cy, cx));
        beats = beats + 1;
        cx = cx + 1; if (cx == W) begin cx = 0; cy = cy + 1; end
    end

    // ---- expected statistics of one frame (same formula as the RTL and sw/common/lle_global.c)
    integer e_hist [0:15];
    integer e_sum [0:2];
    task model_stats(input integer f); integer x, y, k, yy; reg [23:0] p; begin
        for (k = 0; k < 16; k = k + 1) e_hist[k] = 0;
        e_sum[0] = 0; e_sum[1] = 0; e_sum[2] = 0;
        for (y = 0; y < H; y = y + 1)
            for (x = 0; x < W; x = x + 1) begin
                p = pix(f, y, x);
                yy = (77 * p[23:16] + 150 * p[7:0] + 29 * p[15:8] + 128) >> 8;
                e_hist[yy >> 4] = e_hist[yy >> 4] + 1;
                e_sum[0] = e_sum[0] + p[23:16]; e_sum[1] = e_sum[1] + p[7:0]; e_sum[2] = e_sum[2] + p[15:8];
            end
    end endtask
    integer serrs = 0;
    task check_stats(input integer f, input integer seq_exp); integer k; reg [31:0] r; begin
        model_stats(f);
        axi_read(16'h0030, r); if (r != seq_exp) begin serrs = serrs + 1; $display("STAT_SEQ %0d exp %0d", r, seq_exp); end
        axi_read(16'h1000, r); if (r != W * H) begin serrs = serrs + 1; $display("COUNT %0d exp %0d", r, W * H); end
        $fwrite(vf, "S %0d %0d", f, r);
        axi_read(16'h1004, r); if (r != e_sum[0]) begin serrs = serrs + 1; $display("SUM_R %0d exp %0d", r, e_sum[0]); end
        $fwrite(vf, " %0d", r);
        axi_read(16'h1008, r); if (r != e_sum[1]) begin serrs = serrs + 1; $display("SUM_G %0d exp %0d", r, e_sum[1]); end
        $fwrite(vf, " %0d", r);
        axi_read(16'h100C, r); if (r != e_sum[2]) begin serrs = serrs + 1; $display("SUM_B %0d exp %0d", r, e_sum[2]); end
        $fwrite(vf, " %0d", r);
        for (k = 0; k < 16; k = k + 1) begin
            axi_read(16'h1040 + 4 * k, r);
            if (r != e_hist[k]) begin serrs = serrs + 1; $display("HIST[%0d] %0d exp %0d", k, r, e_hist[k]); end
            $fwrite(vf, " %0d", r);
        end
        $fwrite(vf, "\n");
    end endtask

    reg [31:0] r;
    integer i;
    initial begin
        vf = $fopen("lle_vectors.txt", "w");
        // frame plan: 0 = A (swap requested before the frame), 1 = A (swap to B requested
        // mid-frame), 2 = B (garbage written into the inactive bank during the frame),
        // 3 = pass-through
        f_mode[0] = 3; f_set[0] = 0;
        f_mode[1] = 3; f_set[1] = 0;
        f_mode[2] = 3; f_set[2] = 1;
        f_mode[3] = 0; f_set[3] = -1;
        repeat (5) @(posedge aclk); aresetn <= 1; repeat (5) @(posedge aclk);
        axi_read(16'h0004, r); $display("ID = %h (exp 534c0005)", r); if (r != 32'h534C0005) serrs = serrs + 1;
        axi_read(16'h0028, r); $display("CAPS = %h (exp 0000000f)", r); if (r != 32'hF) serrs = serrs + 1;

        load_table(0);                       // A into bank 1 (inactive after reset)
        axi_write(16'h002C, 1);              // swap at the next start of frame
        axi_write(16'h0000, 3);
        axi_read(16'h002C, r); $display("LUT_BANK before frame 0 = %b (exp pending=1, bank 0)", r[1:0]);
        send_frame(0);
        load_table(1);                       // B into bank 0 (inactive), between frames
        fork
            send_frame(1);
            begin repeat (W * H / 2) @(posedge aclk); axi_write(16'h002C, 1); end   // mid-frame swap request
        join
        check_stats(0, 1);                   // latched at the start of frame 1
        fork
            send_frame(2);
            begin                            // garbage into the inactive bank (A) during frame 2
                repeat (20) @(posedge aclk);
                for (i = 0; i < 64; i = i + 1) axi_write(16'h0400 + 4 * i, 8'hA5 ^ i);
            end
        join
        axi_write(16'h0000, 0);              // frame 3 pass-through
        send_frame(3);
        repeat (40) @(posedge aclk);
        check_stats(2, 3);                   // latched at the start of frame 3
        axi_write(16'h0034, 1);              // snapshot of the running set = frame 3
        repeat (3) @(posedge aclk);
        check_stats(3, 4);
        axi_read(16'h002C, r); $display("LUT_BANK at end = %b (exp pending=0, bank 0 after two swaps)", r[1:0]);
        if (r[1:0] != 2'b00) serrs = serrs + 1;
        // interrupt: both sources fired during the test; enable, check level, clear
        if (irq !== 1'b0) begin serrs = serrs + 1; $display("irq high while disabled"); end
        axi_read(16'h003C, r); $display("IRQ_STATUS = %b (exp 11)", r[1:0]); if (r[1:0] != 2'b11) serrs = serrs + 1;
        axi_write(16'h0038, 1);
        repeat (2) @(posedge aclk);
        if (irq !== 1'b1) begin serrs = serrs + 1; $display("irq low with stats enabled and pending"); end
        axi_write(16'h003C, 1);                                    // clear bit 0
        repeat (2) @(posedge aclk);
        if (irq !== 1'b0) begin serrs = serrs + 1; $display("irq still high after clear"); end
        axi_read(16'h003C, r); if (r[1:0] != 2'b10) begin serrs = serrs + 1; $display("IRQ_STATUS after clear %b", r[1:0]); end
        axi_write(16'h0034, 1);                                    // a new latch sets it again
        repeat (3) @(posedge aclk);
        if (irq !== 1'b1) begin serrs = serrs + 1; $display("irq not raised by STAT_CTRL latch"); end
        axi_write(16'h003C, 3); axi_write(16'h0038, 0);
        repeat (2) @(posedge aclk);
        if (irq !== 1'b0) begin serrs = serrs + 1; $display("irq high after clear/disable"); end
        $display("interrupt checks done");

        $display("beats=%0d (exp %0d) pixel errors=%0d statistics errors=%0d", beats, NF * W * H, errs, serrs);
        $display("%s", (errs == 0 && serrs == 0 && beats == NF * W * H) ? "PASS" : "FAIL");
        $fclose(vf);
        $finish;
    end
endmodule
