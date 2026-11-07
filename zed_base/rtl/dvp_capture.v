// dvp_capture.v
// OV7670 (or any 8-bit DVP sensor) RGB565 capture -> AXI4-Stream video, PCLK domain.
//
// - Inputs are registered on the PCLK rising edge (OV7670 default: data changes on
//   the falling edge). camera.xdc packs these registers into the IOBs.
// - Two bytes per pixel: first = R[4:0] G[5:3], second = G[2:0] B[4:0].
// - Output: 24-bit RGB888 in Xilinx AXI4-Stream video order
//   tdata[23:16] = R, [15:8] = B, [7:0] = G; tuser = start of frame; tlast = end of line.
// - The camera cannot be stalled. If the downstream FIFO is full when a pixel must be
//   sent, the rest of the frame is dropped and capture re-arms at the next VSYNC, so
//   the DMA only ever sees whole frames (apart from the one that overflowed).

module dvp_capture (
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 pclk CLK" *)
    (* X_INTERFACE_PARAMETER = "ASSOCIATED_BUSIF m_axis, ASSOCIATED_RESET aresetn" *)
    input  wire        pclk,
    (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 aresetn RST" *)
    (* X_INTERFACE_PARAMETER = "POLARITY ACTIVE_LOW" *)
    input  wire        aresetn,

    input  wire [7:0]  cam_d,
    input  wire        cam_href,
    input  wire        cam_vsync,

    output reg  [23:0] m_axis_tdata,
    output reg         m_axis_tvalid,
    input  wire        m_axis_tready,
    output reg         m_axis_tuser,
    output reg         m_axis_tlast
);

    // ---- input stage (IOB registers) -------------------------------------
    reg [7:0] d_r;
    reg       href_r, vsync_r;
    always @(posedge pclk) begin
        d_r     <= cam_d;
        href_r  <= cam_href;
        vsync_r <= cam_vsync;
    end

    // ---- RGB565 -> RGB888 (bit replication), AXIS order {R, B, G} ----------
    function [23:0] rgb888;
        input [15:0] p;
        reg [4:0] r5, b5;
        reg [5:0] g6;
        begin
            r5 = p[15:11]; g6 = p[10:5]; b5 = p[4:0];
            rgb888 = {r5, r5[4:2], b5, b5[4:2], g6, g6[5:4]};
        end
    endfunction

    // ---- state ------------------------------------------------------------
    reg        href_q, vsync_q;
    reg        byte_sel;       // 0: current byte is the first of a pixel
    reg [7:0]  hi_byte;
    reg [23:0] pend;           // last completed pixel, sent when the next one completes
    reg        pend_valid, pend_sof;
    reg        sof_flag;       // next completed pixel is the first of the frame
    reg        enable;         // sending allowed in this frame

    wire vs_rise   = vsync_r & ~vsync_q;
    wire href_fall = ~href_r &  href_q;
    wire pix_done  = href_r & byte_sel;

    // A beat is produced when a new pixel completes (send the pending one) or at end of line.
    wire emit      = enable & pend_valid & (pix_done | href_fall);
    wire emit_last = href_fall;

    always @(posedge pclk) begin
        if (!aresetn) begin
            href_q        <= 1'b0;
            vsync_q       <= 1'b0;
            byte_sel      <= 1'b0;
            pend_valid    <= 1'b0;
            pend_sof      <= 1'b0;
            sof_flag      <= 1'b0;
            enable        <= 1'b0;
            m_axis_tvalid <= 1'b0;
            m_axis_tuser  <= 1'b0;
            m_axis_tlast  <= 1'b0;
        end else begin
            href_q  <= href_r;
            vsync_q <= vsync_r;

            // Frame start: re-arm
            if (vs_rise) begin
                sof_flag   <= 1'b1;
                enable     <= 1'b1;
                pend_valid <= 1'b0;
            end

            // Byte pairing
            if (href_r) begin
                if (!byte_sel) begin
                    hi_byte  <= d_r;
                    byte_sel <= 1'b1;
                end else begin
                    byte_sel   <= 1'b0;
                    pend       <= rgb888({hi_byte, d_r});
                    pend_valid <= 1'b1;
                    pend_sof   <= sof_flag;
                    sof_flag   <= 1'b0;
                end
            end else begin
                byte_sel <= 1'b0;
                if (href_fall) pend_valid <= 1'b0;
            end

            // Output register (1 deep; pixels arrive at most every 2 PCLKs)
            if (m_axis_tvalid && m_axis_tready) m_axis_tvalid <= 1'b0;
            if (emit) begin
                if (m_axis_tvalid && !m_axis_tready) begin
                    enable <= 1'b0;                  // overflow: drop rest of frame
                end else begin
                    m_axis_tdata  <= pend;
                    m_axis_tuser  <= pend_sof;
                    m_axis_tlast  <= emit_last;
                    m_axis_tvalid <= 1'b1;
                end
            end
        end
    end

endmodule
