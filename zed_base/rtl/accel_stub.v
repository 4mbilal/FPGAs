// accel_stub.v  (shell v7)
// Placeholder for a future accelerator (e.g. GEMM / attention engine) in the reserved shell
// window, so that the engine later replaces this module with the SAME ports and the PS7
// configuration, address map, interrupts and Linux device tree stay unchanged.
//
//   clock      FCLK3 (200 MHz in shell v7), reset from rst_acc
//   s_axi      AXI4-Lite slave, 64 KB window at 0x43E00000 (GP0)
//   m_axi      AXI4 master, 64-bit, to S_AXI_HP3 (DDR); idle in the stub
//   irq        level interrupt to IRQ_F2P[4]
//
// Registers of the stub:
//   0x0000 ID       0x41430000 ("AC", version 0 = empty stub); an engine uses its own ID
//   0x0004 CAPS     0
//   0x0008 SCRATCH  R/W, for bus tests
//   0x000C IRQ_TEST [0] R/W: drives irq directly (tests the Linux UIO interrupt path)
//   other offsets read 0

module accel_stub (
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 aclk CLK" *)
    (* X_INTERFACE_PARAMETER = "ASSOCIATED_BUSIF s_axi:m_axi, ASSOCIATED_RESET aresetn" *)
    input  wire        aclk,
    (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 aresetn RST" *)
    (* X_INTERFACE_PARAMETER = "POLARITY ACTIVE_LOW" *)
    input  wire        aresetn,

    // AXI4-Lite slave
    input  wire [15:0] s_axi_awaddr,
    input  wire [2:0]  s_axi_awprot,
    input  wire        s_axi_awvalid,
    output reg         s_axi_awready,
    input  wire [31:0] s_axi_wdata,
    input  wire [3:0]  s_axi_wstrb,
    input  wire        s_axi_wvalid,
    output reg         s_axi_wready,
    output wire [1:0]  s_axi_bresp,
    output reg         s_axi_bvalid,
    input  wire        s_axi_bready,
    input  wire [15:0] s_axi_araddr,
    input  wire [2:0]  s_axi_arprot,
    input  wire        s_axi_arvalid,
    output reg         s_axi_arready,
    output reg  [31:0] s_axi_rdata,
    output wire [1:0]  s_axi_rresp,
    output reg         s_axi_rvalid,
    input  wire        s_axi_rready,

    // AXI4 master to HP3 (idle)
    output wire [31:0] m_axi_awaddr,
    output wire [7:0]  m_axi_awlen,
    output wire [2:0]  m_axi_awsize,
    output wire [1:0]  m_axi_awburst,
    output wire [3:0]  m_axi_awcache,
    output wire [2:0]  m_axi_awprot,
    output wire        m_axi_awvalid,
    input  wire        m_axi_awready,
    output wire [63:0] m_axi_wdata,
    output wire [7:0]  m_axi_wstrb,
    output wire        m_axi_wlast,
    output wire        m_axi_wvalid,
    input  wire        m_axi_wready,
    input  wire [1:0]  m_axi_bresp,
    input  wire        m_axi_bvalid,
    output wire        m_axi_bready,
    output wire [31:0] m_axi_araddr,
    output wire [7:0]  m_axi_arlen,
    output wire [2:0]  m_axi_arsize,
    output wire [1:0]  m_axi_arburst,
    output wire [3:0]  m_axi_arcache,
    output wire [2:0]  m_axi_arprot,
    output wire        m_axi_arvalid,
    input  wire        m_axi_arready,
    input  wire [63:0] m_axi_rdata,
    input  wire [1:0]  m_axi_rresp,
    input  wire        m_axi_rlast,
    input  wire        m_axi_rvalid,
    output wire        m_axi_rready,

    (* X_INTERFACE_INFO = "xilinx.com:signal:interrupt:1.0 irq INTERRUPT" *)
    (* X_INTERFACE_PARAMETER = "SENSITIVITY LEVEL_HIGH" *)
    output wire        irq
);
    localparam [31:0] ID = 32'h4143_0000;

    // idle master: no requests, accepts any (unexpected) response
    assign m_axi_awaddr = 32'd0;  assign m_axi_awlen = 8'd0;  assign m_axi_awsize = 3'd3;
    assign m_axi_awburst = 2'd1;  assign m_axi_awcache = 4'd3; assign m_axi_awprot = 3'd0;
    assign m_axi_awvalid = 1'b0;
    assign m_axi_wdata = 64'd0;   assign m_axi_wstrb = 8'd0;  assign m_axi_wlast = 1'b0;
    assign m_axi_wvalid = 1'b0;   assign m_axi_bready = 1'b1;
    assign m_axi_araddr = 32'd0;  assign m_axi_arlen = 8'd0;  assign m_axi_arsize = 3'd3;
    assign m_axi_arburst = 2'd1;  assign m_axi_arcache = 4'd3; assign m_axi_arprot = 3'd0;
    assign m_axi_arvalid = 1'b0;  assign m_axi_rready = 1'b1;

    reg [31:0] scratch;
    reg        irq_test;
    assign irq = irq_test;
    assign s_axi_bresp = 2'b00;
    assign s_axi_rresp = 2'b00;

    always @(posedge aclk) begin
        if (!aresetn) begin
            s_axi_awready <= 1'b0; s_axi_wready <= 1'b0; s_axi_bvalid <= 1'b0;
            scratch <= 32'd0; irq_test <= 1'b0;
        end else begin
            if (!s_axi_bvalid && s_axi_awvalid && s_axi_wvalid && !s_axi_awready) begin
                s_axi_awready <= 1'b1; s_axi_wready <= 1'b1;
                if (s_axi_awaddr == 16'h0008) scratch  <= s_axi_wdata;
                if (s_axi_awaddr == 16'h000C) irq_test <= s_axi_wdata[0];
            end else begin
                s_axi_awready <= 1'b0; s_axi_wready <= 1'b0;
            end
            if (s_axi_awready)                     s_axi_bvalid <= 1'b1;
            else if (s_axi_bvalid && s_axi_bready) s_axi_bvalid <= 1'b0;
        end
    end

    always @(posedge aclk) begin
        if (!aresetn) begin
            s_axi_arready <= 1'b0; s_axi_rvalid <= 1'b0; s_axi_rdata <= 32'd0;
        end else begin
            if (!s_axi_rvalid && s_axi_arvalid && !s_axi_arready) begin
                s_axi_arready <= 1'b1;
                case (s_axi_araddr)
                    16'h0000: s_axi_rdata <= ID;
                    16'h0008: s_axi_rdata <= scratch;
                    16'h000C: s_axi_rdata <= {31'd0, irq_test};
                    default:  s_axi_rdata <= 32'd0;
                endcase
            end else begin
                s_axi_arready <= 1'b0;
            end
            if (s_axi_arready)                     s_axi_rvalid <= 1'b1;
            else if (s_axi_rvalid && s_axi_rready) s_axi_rvalid <= 1'b0;
        end
    end
endmodule
