// tb_proc_slot.v - Icarus testbench for proc_slot
//   iverilog -g2005 -o tb_slot sim/tb_proc_slot.v rtl/proc_slot.v && vvp -n tb_slot
// Checks: bit-exact output for modes 0/1/2 against a reference model, framing
// (tuser/tlast) under random input gaps and random output backpressure, mode
// switching only at frame start, counters and AXI-Lite access.
`timescale 1ns/1ps
module tb;
    localparam W = 16, H = 6;

    reg aclk = 0, aresetn = 0;
    always #3.333 aclk = ~aclk;

    reg  [23:0] s_tdata; reg s_tvalid = 0, s_tuser = 0, s_tlast = 0; wire s_tready;
    wire [23:0] m_tdata; wire m_tvalid, m_tuser, m_tlast; reg m_tready = 1;

    reg  [7:0] awaddr = 0, araddr = 0; reg awvalid = 0, wvalid = 0, bready = 1, arvalid = 0, rready = 1;
    reg  [31:0] wdata = 0;
    wire awready, wready, bvalid, arready, rvalid; wire [1:0] bresp, rresp; wire [31:0] rdata;

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
        .s_axi_rdata(rdata), .s_axi_rresp(rresp), .s_axi_rvalid(rvalid), .s_axi_rready(rready));

    // ---- AXI-Lite tasks ------------------------------------------------------
    // Drive on the falling edge, check ready on the falling edge (stable), and let the
    // handshake complete on the following rising edge.
    task axi_write(input [7:0] a, input [31:0] d); begin
        @(negedge aclk); awaddr = a; wdata = d; awvalid = 1; wvalid = 1;
        while (!(awready && wready)) @(negedge aclk);
        @(posedge aclk); @(negedge aclk); awvalid = 0; wvalid = 0;
        while (!bvalid) @(negedge aclk);
        @(posedge aclk);
    end endtask
    task axi_read(input [7:0] a, output [31:0] d); begin
        @(negedge aclk); araddr = a; arvalid = 1;
        while (!arready) @(negedge aclk);
        @(posedge aclk); @(negedge aclk); arvalid = 0;
        while (!rvalid) @(negedge aclk);
        d = rdata;
        @(posedge aclk);
    end endtask

    // ---- reference model -----------------------------------------------------
    function [23:0] model(input [1:0] mode, input [23:0] p);
        reg [7:0] r, g, b, y; integer acc;
        begin
            r = p[23:16]; b = p[15:8]; g = p[7:0];
            acc = (77 * r + 150 * g + 29 * b + 128) >> 8; y = acc[7:0];
            case (mode) 1: model = {y, y, y}; 2: model = ~p; default: model = p; endcase
        end
    endfunction

    // expected mode per frame index (frame f uses frame_mode[f])
    reg [1:0] frame_mode [0:15];
    integer seed = 7;

    // ---- source: random gaps -------------------------------------------------
    function [23:0] pix(input integer f, input integer y, input integer x);
        pix = {f[3:0], y[3:0], x[7:0], 8'h00} ^ (x * 24'h010203) ^ (y * 24'h0A0B0C);
    endfunction
    task send_frame(input integer f); integer x, y; begin
        for (y = 0; y < H; y = y + 1)
            for (x = 0; x < W; x = x + 1) begin
                @(negedge aclk);
                s_tvalid = 0;
                while (($random(seed) & 3) == 0) @(negedge aclk);       // random input gaps
                s_tdata = pix(f, y, x); s_tuser = (x == 0 && y == 0); s_tlast = (x == W - 1);
                s_tvalid = 1;
                while (!s_tready) @(negedge aclk);                      // ready is stable here
                @(posedge aclk);                                        // handshake
            end
        @(negedge aclk); s_tvalid = 0;
    end endtask

    // ---- sink: random backpressure + checker ---------------------------------
    integer cf = -1, cx = 0, cy = 0, errs = 0, beats = 0;
    integer vf; initial vf = $fopen("slot_vectors.txt", "w");   // mode in out (hex, AXIS order) for sw/common cross-check
    always @(posedge aclk) m_tready <= #0.5 (($random(seed) % 3) != 0);   // changes just after the rising edge
    always @(posedge aclk) if (m_tvalid && m_tready) begin
        if (m_tuser) begin cf = cf + 1; cx = 0; cy = 0; end
        if (m_tdata !== model(frame_mode[cf], pix(cf, cy, cx)) ||
            m_tuser !== (cx == 0 && cy == 0) || m_tlast !== (cx == W - 1)) begin
            errs = errs + 1;
            if (errs < 10) $display("ERR f%0d y%0d x%0d mode%0d got %h exp %h u%b l%b",
                cf, cy, cx, frame_mode[cf], m_tdata, model(frame_mode[cf], pix(cf, cy, cx)), m_tuser, m_tlast);
        end
        $fwrite(vf, "%0d %h %h\n", frame_mode[cf], pix(cf, cy, cx), m_tdata);
        beats = beats + 1;
        cx = cx + 1; if (cx == W) begin cx = 0; cy = cy + 1; end
    end

    integer f; reg [31:0] r;
    initial begin
        frame_mode[0] = 0; frame_mode[1] = 1; frame_mode[2] = 2; frame_mode[3] = 1; frame_mode[4] = 0;
        repeat (5) @(posedge aclk); aresetn <= 1; repeat (5) @(posedge aclk);

        axi_read(8'h04, r); $display("ID = %h (exp 534c0005)", r);
        for (f = 0; f < 5; f = f + 1) begin
            // set the mode for frame f before its start of frame; for f>0, write it
            // mid-way through frame f-1 to check it only takes effect at the next frame
            if (f == 0) axi_write(8'h00, frame_mode[0]);
            fork
                send_frame(f);
                if (f < 4) begin repeat (W * H) @(posedge aclk); axi_write(8'h00, frame_mode[f + 1]); end
            join
        end
        repeat (50) @(posedge aclk);

        $display("beats=%0d (exp %0d) errors=%0d", beats, 5 * W * H, errs);
        axi_read(8'h00, r); $display("CTRL        = %0d (exp 0)", r);
        axi_read(8'h08, r); $display("FRAMES      = %0d (exp 5)", r);
        axi_read(8'h10, r); $display("PIXELS_LAST = %0d (exp %0d)", r, W * H);
        axi_read(8'h14, r); $display("LINES_LAST  = %0d (exp %0d)", r, H);
        axi_read(8'h0C, r); $display("CYCLES_LAST = %0d (>= %0d)", r, W * H);
        axi_read(8'h18, r); $display("STALLS_LAST = %0d (> 0 with random backpressure)", r);
        $finish;
    end
endmodule
