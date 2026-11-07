// proc_slot.v
// Processing slot in the camera stream (cam_fifo -> proc_slot -> vfb_wr), 150 MHz domain.
//
// Video:  AXI4-Stream RGB888, Xilinx order tdata[23:16]=R, [15:8]=B, [7:0]=G,
//         tuser = start of frame, tlast = end of line. One pipeline stage, full
//         throughput (1 pixel/clock) when the output is ready.
// Kernels (CTRL.mode, applied from the next start of frame):
//   0 = pass-through
//   1 = grayscale  Y = (77 R + 150 G + 29 B + 128) >> 8   (same integer formula as SW)
//   2 = invert
//   3 = pass-through (reserved)
// New kernels go in the "kernel" function / pipeline below; the counters and
// register map stay the same so every variant is measured the same way.
//
// AXI-Lite registers (byte offsets):
//   0x00 CTRL         [1:0] mode, [4] source (R/W)
//   0x04 ID           0x534C0002 ("SL", version 2)
//   0x08 FRAMES       output frames (start-of-frame beats) since reset
//   0x0C CYCLES_LAST  aclk cycles between the last two output start-of-frame beats
//   0x10 PIXELS_LAST  output beats in the last complete frame
//   0x14 LINES_LAST   output tlast beats in the last complete frame
//   0x18 STALLS_LAST  cycles in the last frame with output valid but not ready
//   0x1C ACTIVE_CYCLES cycles from the current/last frame's start-of-frame beat to its
//                     most recent end-of-line beat (inclusive): processing time of a
//                     single frame, e.g. a memory-to-memory test
//   0x20 PIXELS_RUN   output beats so far in the current/last frame
//   0x24 LINES_RUN    output end-of-line beats so far in the current/last frame
//
// CTRL[4] selects the input: 0 = camera (s_axis), 1 = memory test source (s_axis_mem).
// The unselected camera input is drained (tready = 1) so the camera FIFO never fills;
// the unselected memory input is held off (tready = 0). Switch only between frames,
// with the frame buffer write DMA stopped (software sequencing).

module proc_slot (
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 aclk CLK" *)
    (* X_INTERFACE_PARAMETER = "ASSOCIATED_BUSIF s_axis:s_axis_mem:m_axis:s_axi, ASSOCIATED_RESET aresetn" *)
    input  wire        aclk,
    (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 aresetn RST" *)
    (* X_INTERFACE_PARAMETER = "POLARITY ACTIVE_LOW" *)
    input  wire        aresetn,

    // AXI4-Stream in: camera
    input  wire [23:0] s_axis_tdata,
    input  wire        s_axis_tvalid,
    output wire        s_axis_tready,
    input  wire        s_axis_tuser,
    input  wire        s_axis_tlast,

    // AXI4-Stream in: memory test source (frame buffer read DMA)
    input  wire [23:0] s_axis_mem_tdata,
    input  wire        s_axis_mem_tvalid,
    output wire        s_axis_mem_tready,
    input  wire        s_axis_mem_tuser,
    input  wire        s_axis_mem_tlast,

    // AXI4-Stream out
    output reg  [23:0] m_axis_tdata,
    output reg         m_axis_tvalid,
    input  wire        m_axis_tready,
    output reg         m_axis_tuser,
    output reg         m_axis_tlast,

    // AXI4-Lite slave
    input  wire [7:0]  s_axi_awaddr,
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
    input  wire [7:0]  s_axi_araddr,
    input  wire [2:0]  s_axi_arprot,
    input  wire        s_axi_arvalid,
    output reg         s_axi_arready,
    output reg  [31:0] s_axi_rdata,
    output wire [1:0]  s_axi_rresp,
    output reg         s_axi_rvalid,
    input  wire        s_axi_rready
);

    localparam [31:0] ID = 32'h534C_0002;

    // ---- registers ----------------------------------------------------------
    reg [1:0]  mode_reg;          // written by software
    reg        src_mem;           // CTRL[4]: 0 camera, 1 memory test source
    reg [31:0] act_cycles;
    reg [1:0]  mode_act;          // active for the current frame
    reg [31:0] frames, cycles_last, pixels_last, lines_last, stalls_last;
    reg [31:0] cycles_run, pixels_run, lines_run, stalls_run;
    reg        seen_sof;

    // ---- kernel -------------------------------------------------------------
    function [23:0] kernel;
        input [1:0]  mode;
        input [23:0] p;
        reg   [7:0]  r, g, b, y;
        reg   [17:0] acc;
        begin
            r = p[23:16]; b = p[15:8]; g = p[7:0];
            acc = 18'd77 * r + 18'd150 * g + 18'd29 * b + 18'd128;
            y = acc[15:8];
            case (mode)
                2'd1:    kernel = {y, y, y};
                2'd2:    kernel = ~p;
                default: kernel = p;
            endcase
        end
    endfunction

    // ---- input select -----------------------------------------------------------
    wire [23:0] in_tdata  = src_mem ? s_axis_mem_tdata  : s_axis_tdata;
    wire        in_tvalid = src_mem ? s_axis_mem_tvalid : s_axis_tvalid;
    wire        in_tuser  = src_mem ? s_axis_mem_tuser  : s_axis_tuser;
    wire        in_tlast  = src_mem ? s_axis_mem_tlast  : s_axis_tlast;
    wire        in_tready = ~m_axis_tvalid | m_axis_tready;
    assign s_axis_tready     = src_mem ? 1'b1 : in_tready;      // drain camera when not selected
    assign s_axis_mem_tready = src_mem ? in_tready : 1'b0;

    // ---- stream: one register stage ------------------------------------------
    wire in_hs  = in_tvalid & in_tready;
    wire out_hs = m_axis_tvalid & m_axis_tready;
    // mode for this beat: a start-of-frame beat picks up the latest software setting
    wire [1:0] mode_now = in_tuser ? mode_reg : mode_act;

    always @(posedge aclk) begin
        if (!aresetn) begin
            m_axis_tvalid <= 1'b0;
            m_axis_tuser  <= 1'b0;
            m_axis_tlast  <= 1'b0;
            mode_act      <= 2'd0;
        end else begin
            if (in_hs) begin
                m_axis_tdata  <= kernel(mode_now, in_tdata);
                m_axis_tuser  <= in_tuser;
                m_axis_tlast  <= in_tlast;
                m_axis_tvalid <= 1'b1;
                if (in_tuser) mode_act <= mode_reg;
            end else if (out_hs) begin
                m_axis_tvalid <= 1'b0;
            end
        end
    end

    // ---- counters (measured at the output) ------------------------------------
    always @(posedge aclk) begin
        if (!aresetn) begin
            frames <= 0; cycles_last <= 0; pixels_last <= 0; lines_last <= 0; stalls_last <= 0;
            cycles_run <= 0; pixels_run <= 0; lines_run <= 0; stalls_run <= 0;
            act_cycles <= 0;
            seen_sof <= 1'b0;
        end else begin
            if (out_hs && m_axis_tuser) begin
                // close the previous frame, start a new one (this beat counts in the new frame)
                if (seen_sof) begin
                    cycles_last <= cycles_run + 1;
                    pixels_last <= pixels_run;
                    lines_last  <= lines_run;
                    stalls_last <= stalls_run;
                end
                seen_sof   <= 1'b1;
                frames     <= frames + 1;
                cycles_run <= 0;
                pixels_run <= 1;
                lines_run  <= m_axis_tlast ? 1 : 0;
                stalls_run <= 0;
                act_cycles <= m_axis_tlast ? 1 : 0;
            end else begin
                cycles_run <= cycles_run + 1;
                if (out_hs)                          pixels_run <= pixels_run + 1;
                if (out_hs && m_axis_tlast)          lines_run  <= lines_run + 1;
                if (out_hs && m_axis_tlast)          act_cycles <= cycles_run + 2;   // SOF cycle .. this cycle
                if (m_axis_tvalid && !m_axis_tready) stalls_run <= stalls_run + 1;
            end
        end
    end

    // ---- AXI4-Lite slave (single outstanding transaction per channel) --------
    assign s_axi_bresp = 2'b00;
    assign s_axi_rresp = 2'b00;

    always @(posedge aclk) begin
        if (!aresetn) begin
            s_axi_awready <= 1'b0;
            s_axi_wready  <= 1'b0;
            s_axi_bvalid  <= 1'b0;
            mode_reg      <= 2'd0;
            src_mem       <= 1'b0;
        end else begin
            // accept address and data together
            if (!s_axi_bvalid && s_axi_awvalid && s_axi_wvalid && !s_axi_awready) begin
                s_axi_awready <= 1'b1;
                s_axi_wready  <= 1'b1;
                if (s_axi_awaddr[7:2] == 6'd0 && s_axi_wstrb[0]) begin
                    mode_reg <= s_axi_wdata[1:0];
                    src_mem  <= s_axi_wdata[4];
                end
            end else begin
                s_axi_awready <= 1'b0;
                s_axi_wready  <= 1'b0;
            end
            if (s_axi_awready)                  s_axi_bvalid <= 1'b1;
            else if (s_axi_bvalid && s_axi_bready) s_axi_bvalid <= 1'b0;
        end
    end

    always @(posedge aclk) begin
        if (!aresetn) begin
            s_axi_arready <= 1'b0;
            s_axi_rvalid  <= 1'b0;
            s_axi_rdata   <= 32'd0;
        end else begin
            if (!s_axi_rvalid && s_axi_arvalid && !s_axi_arready) begin
                s_axi_arready <= 1'b1;
                case (s_axi_araddr[7:2])
                    6'd0:    s_axi_rdata <= {27'd0, src_mem, 2'd0, mode_reg};
                    6'd1:    s_axi_rdata <= ID;
                    6'd2:    s_axi_rdata <= frames;
                    6'd3:    s_axi_rdata <= cycles_last;
                    6'd4:    s_axi_rdata <= pixels_last;
                    6'd5:    s_axi_rdata <= lines_last;
                    6'd6:    s_axi_rdata <= stalls_last;
                    6'd7:    s_axi_rdata <= act_cycles;
                    6'd8:    s_axi_rdata <= pixels_run;
                    6'd9:    s_axi_rdata <= lines_run;
                    default: s_axi_rdata <= 32'd0;
                endcase
            end else begin
                s_axi_arready <= 1'b0;
            end
            if (s_axi_arready)                     s_axi_rvalid <= 1'b1;
            else if (s_axi_rvalid && s_axi_rready) s_axi_rvalid <= 1'b0;
        end
    end

endmodule
