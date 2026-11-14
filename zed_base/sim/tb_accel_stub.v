// tb_accel_stub.v - AXI-Lite access and interrupt test of the shell's accelerator stub
//   iverilog -g2005 -o tb_acc sim/tb_accel_stub.v rtl/accel_stub.v && vvp -n tb_acc
`timescale 1ns/1ps
module tb;
    reg aclk = 0, aresetn = 0;
    always #2.5 aclk = ~aclk;
    reg  [15:0] awaddr = 0, araddr = 0; reg awvalid = 0, wvalid = 0, bready = 1, arvalid = 0, rready = 1;
    reg  [31:0] wdata = 0;
    wire awready, wready, bvalid, arready, rvalid, irq; wire [1:0] bresp, rresp; wire [31:0] rdata;
    wire awv, arv, wv;
    accel_stub dut (.aclk(aclk), .aresetn(aresetn),
        .s_axi_awaddr(awaddr), .s_axi_awprot(3'd0), .s_axi_awvalid(awvalid), .s_axi_awready(awready),
        .s_axi_wdata(wdata), .s_axi_wstrb(4'hF), .s_axi_wvalid(wvalid), .s_axi_wready(wready),
        .s_axi_bresp(bresp), .s_axi_bvalid(bvalid), .s_axi_bready(bready),
        .s_axi_araddr(araddr), .s_axi_arprot(3'd0), .s_axi_arvalid(arvalid), .s_axi_arready(arready),
        .s_axi_rdata(rdata), .s_axi_rresp(rresp), .s_axi_rvalid(rvalid), .s_axi_rready(rready),
        .m_axi_awaddr(), .m_axi_awlen(), .m_axi_awsize(), .m_axi_awburst(), .m_axi_awcache(), .m_axi_awprot(),
        .m_axi_awvalid(awv), .m_axi_awready(1'b0), .m_axi_wdata(), .m_axi_wstrb(), .m_axi_wlast(),
        .m_axi_wvalid(wv), .m_axi_wready(1'b0), .m_axi_bresp(2'd0), .m_axi_bvalid(1'b0), .m_axi_bready(),
        .m_axi_araddr(), .m_axi_arlen(), .m_axi_arsize(), .m_axi_arburst(), .m_axi_arcache(), .m_axi_arprot(),
        .m_axi_arvalid(arv), .m_axi_arready(1'b0), .m_axi_rdata(64'd0), .m_axi_rresp(2'd0), .m_axi_rlast(1'b0),
        .m_axi_rvalid(1'b0), .m_axi_rready(), .irq(irq));

    task wr(input [15:0] a, input [31:0] d); begin
        @(negedge aclk); awaddr = a; wdata = d; awvalid = 1; wvalid = 1;
        while (!(awready && wready)) @(negedge aclk);
        @(posedge aclk); @(negedge aclk); awvalid = 0; wvalid = 0;
        while (!bvalid) @(negedge aclk);
        @(posedge aclk);
    end endtask
    task rd(input [15:0] a, output [31:0] d); begin
        @(negedge aclk); araddr = a; arvalid = 1;
        while (!arready) @(negedge aclk);
        @(posedge aclk); @(negedge aclk); arvalid = 0;
        while (!rvalid) @(negedge aclk);
        d = rdata; @(posedge aclk);
    end endtask

    integer errs = 0; reg [31:0] r;
    initial begin
        repeat (4) @(posedge aclk); aresetn = 1;
        rd(16'h0000, r); if (r != 32'h41430000) errs = errs + 1;
        wr(16'h0008, 32'hDEADBEEF); rd(16'h0008, r); if (r != 32'hDEADBEEF) errs = errs + 1;
        rd(16'h1234, r); if (r != 0) errs = errs + 1;
        if (irq !== 0) errs = errs + 1;
        wr(16'h000C, 1); @(posedge aclk); if (irq !== 1) errs = errs + 1;
        wr(16'h000C, 0); @(posedge aclk); if (irq !== 0) errs = errs + 1;
        if (awv || arv || wv) errs = errs + 1;
        $display("accel_stub: ID, scratch, unmapped read, interrupt test, idle master: %0d errors -> %s",
                 errs, errs ? "FAIL" : "PASS");
        $finish;
    end
endmodule
