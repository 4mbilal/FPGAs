// tb_proc_slot_mem.v - memory-test path of proc_slot (CTRL[4] = 1)
//   iverilog -g2005 -o tb_mem sim/tb_proc_slot_mem.v rtl/proc_slot.v && vvp -n tb_mem
// Checks: camera input drained (tready = 1) while the memory source is selected;
// memory frames bit-exact for grayscale; full throughput (ACTIVE_CYCLES = W*H with no
// gaps/backpressure); PIXELS_RUN / LINES_RUN after a single frame; switching back to camera.
`timescale 1ns/1ps
module tb;
    localparam W = 40, H = 12;
    reg aclk = 0, aresetn = 0;
    always #3.333 aclk = ~aclk;

    reg  [23:0] c_tdata = 0, m_tdata_in = 0; reg c_tvalid = 0, c_tuser = 0, c_tlast = 0;
    reg  m_tvalid = 0, m_tuser = 0, m_tlast = 0;
    wire c_tready, m_tready_in;
    wire [23:0] o_tdata; wire o_tvalid, o_tuser, o_tlast; reg o_tready = 1;
    reg  [7:0] awaddr = 0, araddr = 0; reg awvalid = 0, wvalid = 0, bready = 1, arvalid = 0, rready = 1;
    reg  [31:0] wdata = 0;
    wire awready, wready, bvalid, arready, rvalid; wire [1:0] bresp, rresp; wire [31:0] rdata;

    proc_slot dut (
        .aclk(aclk), .aresetn(aresetn),
        .s_axis_tdata(c_tdata), .s_axis_tvalid(c_tvalid), .s_axis_tready(c_tready),
        .s_axis_tuser(c_tuser), .s_axis_tlast(c_tlast),
        .s_axis_mem_tdata(m_tdata_in), .s_axis_mem_tvalid(m_tvalid), .s_axis_mem_tready(m_tready_in),
        .s_axis_mem_tuser(m_tuser), .s_axis_mem_tlast(m_tlast),
        .m_axis_tdata(o_tdata), .m_axis_tvalid(o_tvalid), .m_axis_tready(o_tready),
        .m_axis_tuser(o_tuser), .m_axis_tlast(o_tlast),
        .s_axi_awaddr(awaddr), .s_axi_awprot(3'd0), .s_axi_awvalid(awvalid), .s_axi_awready(awready),
        .s_axi_wdata(wdata), .s_axi_wstrb(4'hF), .s_axi_wvalid(wvalid), .s_axi_wready(wready),
        .s_axi_bresp(bresp), .s_axi_bvalid(bvalid), .s_axi_bready(bready),
        .s_axi_araddr(araddr), .s_axi_arprot(3'd0), .s_axi_arvalid(arvalid), .s_axi_arready(arready),
        .s_axi_rdata(rdata), .s_axi_rresp(rresp), .s_axi_rvalid(rvalid), .s_axi_rready(rready));

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

    function [23:0] gray(input [23:0] p);
        reg [7:0] r, g, b; integer y;
        begin r = p[23:16]; b = p[15:8]; g = p[7:0];
              y = (77 * r + 150 * g + 29 * b + 128) >> 8; gray = {y[7:0], y[7:0], y[7:0]}; end
    endfunction
    function [23:0] pix(input integer y, input integer x); pix = (x * 24'h030507) ^ (y * 24'h110D0B); endfunction

    // memory source: back-to-back, no gaps
    task mem_frame; integer x, y; begin
        for (y = 0; y < H; y = y + 1)
            for (x = 0; x < W; x = x + 1) begin
                @(negedge aclk);
                m_tdata_in = pix(y, x); m_tuser = (x == 0 && y == 0); m_tlast = (x == W - 1); m_tvalid = 1;
                while (!m_tready_in) @(negedge aclk);
                @(posedge aclk);
            end
        @(negedge aclk); m_tvalid = 0;
    end endtask

    // camera keeps streaming garbage while the memory source is selected
    integer cam_accepted = 0;
    always @(negedge aclk) begin c_tvalid = 1; c_tdata = $random; c_tuser = 0; c_tlast = 0; end
    always @(posedge aclk) if (c_tvalid && c_tready) cam_accepted = cam_accepted + 1;

    integer cx = 0, cy = 0, errs = 0, beats = 0, collecting = 0;
    always @(posedge aclk) if (collecting && o_tvalid && o_tready) begin
        if (o_tuser) begin cx = 0; cy = 0; end
        if (o_tdata !== gray(pix(cy, cx)) || o_tuser !== (cx == 0 && cy == 0) || o_tlast !== (cx == W - 1)) begin
            errs = errs + 1;
            if (errs < 5) $display("ERR y%0d x%0d got %h exp %h", cy, cx, o_tdata, gray(pix(cy, cx)));
        end
        beats = beats + 1; cx = cx + 1; if (cx == W) begin cx = 0; cy = cy + 1; end
    end

    reg [31:0] r; integer cam_before;
    initial begin
        repeat (5) @(posedge aclk); aresetn = 1; repeat (5) @(posedge aclk);
        axi_read(8'h04, r); $display("ID = %h (exp 534c0002)", r);
        // camera selected: camera tready follows the output (always ready here), mem held off
        repeat (10) @(posedge aclk);
        $display("camera selected: mem tready = %b (exp 0)", m_tready_in);

        axi_write(8'h00, 32'h11);                 // source = memory, mode = gray
        axi_read(8'h00, r); $display("CTRL = %h (exp 11)", r);
        cam_before = cam_accepted;
        collecting = 1;
        mem_frame;
        repeat (5) @(posedge aclk);
        collecting = 0;
        $display("memory frame: beats=%0d (exp %0d) errors=%0d", beats, W * H, errs);
        axi_read(8'h1C, r); $display("ACTIVE_CYCLES = %0d (exp %0d = W*H at 1 pixel/clock)", r, W * H);
        axi_read(8'h20, r); $display("PIXELS_RUN    = %0d (exp %0d)", r, W * H);
        axi_read(8'h24, r); $display("LINES_RUN     = %0d (exp %0d)", r, H);
        $display("camera beats drained while memory selected: %0d (> 0 expected)", cam_accepted - cam_before);

        // with output backpressure: active cycles grow, data still exact
        beats = 0; errs = 0; collecting = 1;
        fork
            mem_frame;
            begin : bp repeat (W * H * 3) begin @(posedge aclk); o_tready <= #0.5 ($random % 2) != 0; end end
        join
        o_tready = 1; repeat (5) @(posedge aclk); collecting = 0;
        $display("memory frame with backpressure: beats=%0d errors=%0d", beats, errs);
        axi_read(8'h1C, r); $display("ACTIVE_CYCLES = %0d (> %0d with backpressure)", r, W * H);

        axi_write(8'h00, 32'h00);                 // back to camera
        repeat (5) @(posedge aclk);
        $display("camera selected again: mem tready = %b (exp 0)", m_tready_in);
        $finish;
    end
endmodule
