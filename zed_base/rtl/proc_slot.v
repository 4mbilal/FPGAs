// proc_slot.v  (v5, shell v7)
// Processing slot in the camera stream (cam_fifo -> proc_slot -> vfb_wr), 150 MHz domain.
//
// Video:  AXI4-Stream RGB888, Xilinx order tdata[23:16]=R, [15:8]=B, [7:0]=G,
//         tuser = start of frame, tlast = end of line. One pipeline stage, full
//         throughput (1 pixel/clock) when the output is ready.
// Kernels (CTRL.mode, applied from the next start of frame):
//   0 = pass-through
//   1 = grayscale  Y = (77 R + 150 G + 29 B + 128) >> 8   (same integer formula as SW)
//   2 = invert
//   3 = curve table: out_c = LUT_c[in_c] per channel (global tone curve, e.g. LiteIE;
//       the table is computed by software from the frame statistics below)
//
// AXI-Lite window: 64 KB at 0x43D00000 (fixed by the v7 shell; everything this slot ever
// gets, including the spatial-mode network, lives inside it). Byte offsets:
//   0x0000 CTRL          [1:0] mode, [4] source (R/W)
//   0x0004 ID            0x534C0005 ("SL", version 5)
//   0x0008 FRAMES        output frames (start-of-frame beats) since reset
//   0x000C CYCLES_LAST   aclk cycles between the last two output start-of-frame beats
//   0x0010 PIXELS_LAST   output beats in the last complete frame
//   0x0014 LINES_LAST    output tlast beats in the last complete frame
//   0x0018 STALLS_LAST   cycles in the last frame with output valid but not ready
//   0x001C ACTIVE_CYCLES cycles from the current/last frame's start-of-frame beat to its
//                        most recent end-of-line beat (inclusive): processing time of a
//                        single frame, e.g. a memory-to-memory test
//   0x0020 PIXELS_RUN    output beats so far in the current/last frame
//   0x0024 LINES_RUN     output end-of-line beats so far in the current/last frame
//   0x0028 CAPS          (R) features: [0] kernels 0-2, [1] curve table, [2] statistics,
//                        [3] interrupt, [4] spatial mode (not in v5)
//   0x002C LUT_BANK      W: [0] = 1 requests a bank swap at the next input start of frame;
//                        R: [0] swap pending, [1] active bank. Write the inactive bank only
//                        while no swap is pending.
//   0x0030 STAT_SEQ      (R) number of statistics sets latched since reset
//   0x0034 STAT_CTRL     (W) [0] = 1 latches the running statistics now (single-frame tests)
//   0x0038 IRQ_EN        (R/W) [0] statistics latched, [1] output start of frame
//   0x003C IRQ_STATUS    (R, write 1 to clear) same bits; irq = |(IRQ_STATUS & IRQ_EN), level
//   0x0400..0x0FFF LUT   (W) curve table, inactive bank, one entry per word, [7:0]:
//                        0x0400 + 4 x, 0x0800 + 4 x, 0x0C00 + 4 x  for R, G, B, x = 0..255
//   0x1000 STAT_COUNT    (R) pixels in the latched set
//   0x1004/08/0C SUM_R/G/B (R) channel sums of the latched set
//   0x1040..0x107C HIST[0..15] (R) luma histogram of the latched set, bin = Y >> 4 with the
//                        grayscale formula above
//   0x2000..0x3FFF       reserved: spatial mode (network control, curve-map read-back)
//   0x8000..0xFFFF       reserved: network weight memory
//   Unmapped offsets read 0 and ignore writes.
// Statistics are taken on the INPUT pixels (whatever the mode) and latched at every input
// start of frame (previous frame) or on STAT_CTRL. They run one pipeline stage behind the
// stream, independent of output backpressure.
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

    // AXI4-Lite slave, 64 KB window
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

    // interrupt (level, active high) to IRQ_F2P[3]
    (* X_INTERFACE_INFO = "xilinx.com:signal:interrupt:1.0 irq INTERRUPT" *)
    (* X_INTERFACE_PARAMETER = "SENSITIVITY LEVEL_HIGH" *)
    output wire        irq
);

    localparam [31:0] ID   = 32'h534C_0005;
    localparam [31:0] CAPS = 32'h0000_000F;

    // ---- registers ----------------------------------------------------------
    reg [1:0]  mode_reg;          // written by software
    reg        src_mem;           // CTRL[4]: 0 camera, 1 memory test source
    reg [31:0] act_cycles;
    reg [1:0]  mode_act;          // active for the current frame
    reg [31:0] frames, cycles_last, pixels_last, lines_last, stalls_last;
    reg [31:0] cycles_run, pixels_run, lines_run, stalls_run;
    reg        seen_sof;

    // curve table: two banks per channel, distributed RAM, asynchronous read
    reg [7:0]  lut_r [0:511];
    reg [7:0]  lut_g [0:511];
    reg [7:0]  lut_b [0:511];
    reg        bank_act, swap_pend;
    reg        lut_we, swap_req, latch_req;   // one-cycle pulses from the AXI-Lite write side
    reg [7:0]  lut_wdata;
    reg [9:0]  lut_widx;          // {channel 0..2, code}

    // statistics: running (current frame) and latched sets
    reg [23:0] hist_run [0:15];
    reg [23:0] hist_lat [0:15];
    reg [31:0] sum_run_r, sum_run_g, sum_run_b, cnt_run;
    reg [31:0] sum_lat_r, sum_lat_g, sum_lat_b, cnt_lat, stat_seq;
    reg        st_seen_sof;

    // interrupt
    reg [1:0]  irq_en, irq_status;
    reg [1:0]  irq_clr;           // write-1-to-clear pulse
    assign irq = |(irq_status & irq_en);

    // ---- input select -----------------------------------------------------------
    wire [23:0] in_tdata  = src_mem ? s_axis_mem_tdata  : s_axis_tdata;
    wire        in_tvalid = src_mem ? s_axis_mem_tvalid : s_axis_tvalid;
    wire        in_tuser  = src_mem ? s_axis_mem_tuser  : s_axis_tuser;
    wire        in_tlast  = src_mem ? s_axis_mem_tlast  : s_axis_tlast;
    // three-stage pipeline with one global enable: every stage advances when the output
    // register is empty or being read (1 pixel/clock, latency 3)
    wire        en        = ~m_axis_tvalid | m_axis_tready;
    wire        in_tready = en;
    assign s_axis_tready     = src_mem ? 1'b1 : in_tready;      // drain camera when not selected
    assign s_axis_mem_tready = src_mem ? in_tready : 1'b0;

    wire in_hs  = in_tvalid & in_tready;
    wire out_hs = m_axis_tvalid & m_axis_tready;
    // mode for this beat: a start-of-frame beat picks up the latest software setting
    wire [1:0] mode_now = in_tuser ? mode_reg : mode_act;
    // table bank for this beat: a pending swap takes effect at the start of frame
    wire       bank_now = (in_tuser && swap_pend) ? ~bank_act : bank_act;

    // stage 1: input register
    reg        s1_v, s1_user, s1_last, s1_bank;
    reg [1:0]  s1_mode;
    reg [23:0] s1_p;
    // stage 2: table read, luminance products
    reg        s2_v, s2_user, s2_last;
    reg [1:0]  s2_mode;
    reg [23:0] s2_p, s2_lut;
    reg [15:0] s2_pr, s2_pg, s2_pb;
    wire [17:0] ysum = s2_pr + s2_pg + s2_pb + 18'd128;   // Y = ysum[15:8]
    wire [7:0]  y2   = ysum[15:8];

    always @(posedge aclk) begin
        if (!aresetn) begin
            s1_v <= 1'b0; s2_v <= 1'b0;
        end else if (en) begin
            s1_v <= in_tvalid;
            s2_v <= s1_v;
        end
    end
    always @(posedge aclk) begin
        if (en) begin
            s1_p <= in_tdata; s1_user <= in_tuser; s1_last <= in_tlast;
            s1_mode <= mode_now; s1_bank <= bank_now;
            s2_p <= s1_p; s2_user <= s1_user; s2_last <= s1_last; s2_mode <= s1_mode;
            s2_lut <= {lut_r[{s1_bank, s1_p[23:16]}],    // R
                       lut_b[{s1_bank, s1_p[15:8]}],     // B
                       lut_g[{s1_bank, s1_p[7:0]}]};     // G
            s2_pr <= 8'd77  * s1_p[23:16];
            s2_pg <= 8'd150 * s1_p[7:0];
            s2_pb <= 8'd29  * s1_p[15:8];
        end
    end

    // table writes (inactive bank) and bank swap
    always @(posedge aclk) begin
        if (lut_we) begin
            case (lut_widx[9:8])
                2'd0: lut_r[{~bank_act, lut_widx[7:0]}] <= lut_wdata;
                2'd1: lut_g[{~bank_act, lut_widx[7:0]}] <= lut_wdata;
                2'd2: lut_b[{~bank_act, lut_widx[7:0]}] <= lut_wdata;
                default: ;
            endcase
        end
    end
    always @(posedge aclk) begin
        if (!aresetn) begin
            bank_act  <= 1'b0;
            swap_pend <= 1'b0;
        end else begin
            if (in_hs && in_tuser && swap_pend) begin
                bank_act  <= ~bank_act;
                swap_pend <= swap_req;             // a request in the same cycle waits for the next frame
            end else if (swap_req) begin
                swap_pend <= 1'b1;
            end
        end
    end

    always @(posedge aclk) begin
        if (!aresetn) begin
            m_axis_tvalid <= 1'b0;
            m_axis_tuser  <= 1'b0;
            m_axis_tlast  <= 1'b0;
            mode_act      <= 2'd0;
        end else begin
            if (in_hs && in_tuser) mode_act <= mode_reg;
            if (en) begin                                  // stage 3: output register
                m_axis_tvalid <= s2_v;
                m_axis_tuser  <= s2_user;
                m_axis_tlast  <= s2_last;
                case (s2_mode)
                    2'd1:    m_axis_tdata <= {y2, y2, y2};
                    2'd2:    m_axis_tdata <= ~s2_p;
                    2'd3:    m_axis_tdata <= s2_lut;
                    default: m_axis_tdata <= s2_p;
                endcase
            end
        end
    end

    // ---- frame statistics (input pixels, taken as they leave stage 2) ----------
    reg        st_v, st_sof;
    reg [7:0]  st_r, st_g, st_b;
    reg [3:0]  st_bin;
    integer k;
    always @(posedge aclk) begin
        if (!aresetn) begin
            st_v <= 1'b0; st_sof <= 1'b0;
        end else begin
            st_v   <= en & s2_v;
            st_sof <= en & s2_v & s2_user;
            st_r   <= s2_p[23:16];
            st_b   <= s2_p[15:8];
            st_g   <= s2_p[7:0];
            st_bin <= ysum[15:12];
        end
    end
    always @(posedge aclk) begin
        if (!aresetn) begin
            for (k = 0; k < 16; k = k + 1) begin hist_run[k] <= 0; hist_lat[k] <= 0; end
            sum_run_r <= 0; sum_run_g <= 0; sum_run_b <= 0; cnt_run <= 0;
            sum_lat_r <= 0; sum_lat_g <= 0; sum_lat_b <= 0; cnt_lat <= 0;
            stat_seq <= 0; st_seen_sof <= 1'b0;
        end else if (st_v && st_sof) begin
            // close the previous frame (if any), start the new one with this pixel
            if (st_seen_sof) begin
                for (k = 0; k < 16; k = k + 1) hist_lat[k] <= hist_run[k];
                sum_lat_r <= sum_run_r; sum_lat_g <= sum_run_g; sum_lat_b <= sum_run_b;
                cnt_lat <= cnt_run;
                stat_seq <= stat_seq + 1;
            end
            st_seen_sof <= 1'b1;
            for (k = 0; k < 16; k = k + 1) hist_run[k] <= (k == st_bin) ? 24'd1 : 24'd0;
            sum_run_r <= st_r; sum_run_g <= st_g; sum_run_b <= st_b; cnt_run <= 1;
        end else begin
            if (st_v) begin
                hist_run[st_bin] <= hist_run[st_bin] + 1;
                sum_run_r <= sum_run_r + st_r;
                sum_run_g <= sum_run_g + st_g;
                sum_run_b <= sum_run_b + st_b;
                cnt_run   <= cnt_run + 1;
            end
            if (latch_req) begin
                // snapshot of the running set (includes the beat accumulated in this cycle
                // only from the next cycle on; software latches long after the frame ends)
                for (k = 0; k < 16; k = k + 1) hist_lat[k] <= hist_run[k];
                sum_lat_r <= sum_run_r; sum_lat_g <= sum_run_g; sum_lat_b <= sum_run_b;
                cnt_lat <= cnt_run;
                stat_seq <= stat_seq + 1;
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

    // ---- interrupt status: set by events, cleared by software (set wins) ----------
    wire ev_stats = (st_v && st_sof && st_seen_sof) || latch_req;
    wire ev_osof  = out_hs && m_axis_tuser;
    always @(posedge aclk) begin
        if (!aresetn) irq_status <= 2'b00;
        else          irq_status <= (irq_status & ~irq_clr) | {ev_osof, ev_stats};
    end

    // ---- AXI4-Lite slave (single outstanding transaction per channel) --------
    assign s_axi_bresp = 2'b00;
    assign s_axi_rresp = 2'b00;
    wire [15:0] wa = s_axi_awaddr;
    wire [15:0] ra = s_axi_araddr;

    always @(posedge aclk) begin
        if (!aresetn) begin
            s_axi_awready <= 1'b0;
            s_axi_wready  <= 1'b0;
            s_axi_bvalid  <= 1'b0;
            mode_reg      <= 2'd0;
            src_mem       <= 1'b0;
            lut_we        <= 1'b0;
            swap_req      <= 1'b0;
            latch_req     <= 1'b0;
            irq_en        <= 2'b00;
            irq_clr       <= 2'b00;
        end else begin
            lut_we    <= 1'b0;
            swap_req  <= 1'b0;
            latch_req <= 1'b0;
            irq_clr   <= 2'b00;
            // accept address and data together
            if (!s_axi_bvalid && s_axi_awvalid && s_axi_wvalid && !s_axi_awready) begin
                s_axi_awready <= 1'b1;
                s_axi_wready  <= 1'b1;
                if (s_axi_wstrb[0]) begin
                    if (wa[15:12] == 4'h0 && wa[11:10] != 2'b00) begin       // 0x0400..0x0FFF table
                        lut_we    <= 1'b1;
                        lut_widx  <= {wa[11:10] - 2'd1, wa[9:2]};
                        lut_wdata <= s_axi_wdata[7:0];
                    end else if (wa[15:8] == 8'h00) begin
                        case (wa[7:2])
                            6'h00: begin mode_reg <= s_axi_wdata[1:0]; src_mem <= s_axi_wdata[4]; end
                            6'h0B: swap_req  <= s_axi_wdata[0];               // 0x2C LUT_BANK
                            6'h0D: latch_req <= s_axi_wdata[0];               // 0x34 STAT_CTRL
                            6'h0E: irq_en    <= s_axi_wdata[1:0];             // 0x38 IRQ_EN
                            6'h0F: irq_clr   <= s_axi_wdata[1:0];             // 0x3C IRQ_STATUS
                            default: ;
                        endcase
                    end
                end
            end else begin
                s_axi_awready <= 1'b0;
                s_axi_wready  <= 1'b0;
            end
            if (s_axi_awready)                     s_axi_bvalid <= 1'b1;
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
                s_axi_rdata   <= 32'd0;
                if (ra[15:8] == 8'h00) begin
                    case (ra[7:2])
                        6'h00: s_axi_rdata <= {27'd0, src_mem, 2'd0, mode_reg};
                        6'h01: s_axi_rdata <= ID;
                        6'h02: s_axi_rdata <= frames;
                        6'h03: s_axi_rdata <= cycles_last;
                        6'h04: s_axi_rdata <= pixels_last;
                        6'h05: s_axi_rdata <= lines_last;
                        6'h06: s_axi_rdata <= stalls_last;
                        6'h07: s_axi_rdata <= act_cycles;
                        6'h08: s_axi_rdata <= pixels_run;
                        6'h09: s_axi_rdata <= lines_run;
                        6'h0A: s_axi_rdata <= CAPS;
                        6'h0B: s_axi_rdata <= {30'd0, bank_act, swap_pend};
                        6'h0C: s_axi_rdata <= stat_seq;
                        6'h0E: s_axi_rdata <= {30'd0, irq_en};
                        6'h0F: s_axi_rdata <= {30'd0, irq_status};
                        default: ;
                    endcase
                end else if (ra[15:8] == 8'h10) begin                     // statistics
                    if (ra[7:6] == 2'b01)                                 // 0x1040..0x107C
                        s_axi_rdata <= {8'd0, hist_lat[ra[5:2]]};
                    else case (ra[7:2])
                        6'h00: s_axi_rdata <= cnt_lat;
                        6'h01: s_axi_rdata <= sum_lat_r;
                        6'h02: s_axi_rdata <= sum_lat_g;
                        6'h03: s_axi_rdata <= sum_lat_b;
                        default: ;
                    endcase
                end
            end else begin
                s_axi_arready <= 1'b0;
            end
            if (s_axi_arready)                     s_axi_rvalid <= 1'b1;
            else if (s_axi_rvalid && s_axi_rready) s_axi_rvalid <= 1'b0;
        end
    end

endmodule
