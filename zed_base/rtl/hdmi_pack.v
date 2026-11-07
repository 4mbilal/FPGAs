// hdmi_pack.v
// 24-bit RGB video bus from v_axi4s_vid_out -> ZedBoard ADV7511 input:
// 16-bit YCbCr 4:2:2, separate syncs (ADV7511 input ID 1, style 1, 8 bit, right justified;
// the same settings as the Analog Devices ZedBoard reference design and its Linux device tree).
//
//   hd_d[15:8] = chroma: Cb on even pixels, Cr on odd pixels (counted from the start of DE)
//   hd_d[7:0]  = Y
//   Outside DE: hd_d = {0x80, 0x10} (black)
//
// Colour conversion: BT.709, limited range (Y 16..235, Cb/Cr 16..240), 10-bit coefficients:
//   Y  = 16  + ( 187 R + 629 G +  63 B + 512) >> 10
//   Cb = 128 + (-103 R - 347 G + 450 B + 512) >> 10      (arithmetic shift)
//   Cr = 128 + ( 450 R - 409 G -  41 B + 512) >> 10
// The ADV7511 converts back to RGB with the BT.709 matrix that software loads (the matrix
// of the Linux adv7511 driver), so the round trip is consistent.
//
// Chroma subsampling (4:4:4 -> 4:2:2): co-sited with the even pixel, [1 2 1]/4 filter,
// edge pixels of a line repeat themselves as the missing neighbour:
//   even pixel n:   {Cb_f(n), Y(n)}     odd pixel n+1: {Cr_f(n), Y(n+1)}
//
// Latency: 6 register stages (input register .. output register), the same for data, DE,
// HSYNC and VSYNC. The VGA output (vga_pack, 1 stage) leads by 5 pixels; irrelevant, as the
// two outputs go to different monitors.
// Checked bit-exact against sim/check_hdmi.c (C golden model) with sim/tb_hdmi_pack.v.
// Outputs are registered (packed into the IOBs by hdmi.xdc). The pixel clock goes to the
// HD-CLK pin through clk_forward (ODDR, same phase), as in the ADI reference design.
//
// Xilinx AXI4-Stream video RGB order: vid_data[23:16] = R, [15:8] = B, [7:0] = G.

module hdmi_pack (
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 pix_clk CLK" *)
    input  wire        pix_clk,
    input  wire [23:0] vid_data,
    input  wire        vid_active,
    input  wire        vid_hsync,
    input  wire        vid_vsync,
    output reg  [15:0] hd_d,
    output reg         hd_de,
    output reg         hd_hsync,
    output reg         hd_vsync
);

    // ---- stage 0: input registers ------------------------------------------------
    reg [7:0] r0, g0, b0;
    reg [2:0] s0;                                   // {vsync, hsync, de}
    always @(posedge pix_clk) begin
        r0 <= vid_data[23:16]; b0 <= vid_data[15:8]; g0 <= vid_data[7:0];
        s0 <= {vid_vsync, vid_hsync, vid_active};
    end

    // ---- stage 1: products (constant multiplies, signed 19 bit) -------------------
    reg signed [18:0] yr, yg, yb, ur, ug, ub, vr, vg, vb;
    reg [2:0] s1;
    wire signed [9:0] R = {2'b00, r0}, G = {2'b00, g0}, B = {2'b00, b0};
    always @(posedge pix_clk) begin
        yr <=  187 * R;  yg <=  629 * G;  yb <=   63 * B;
        ur <= -103 * R;  ug <= -347 * G;  ub <=  450 * B;
        vr <=  450 * R;  vg <= -409 * G;  vb <=  -41 * B;
        s1 <= s0;
    end

    // ---- stage 2: sums with rounding ----------------------------------------------
    reg signed [19:0] ys, us, vs;
    reg [2:0] s2;
    always @(posedge pix_clk) begin
        ys <= yr + yg + yb + 20'sd512;
        us <= ur + ug + ub + 20'sd512;
        vs <= vr + vg + vb + 20'sd512;
        s2 <= s1;
    end

    // ---- stage 3: scale, offset, clamp -> sample A (newest) ------------------------
    function [7:0] clamp;
        input signed [11:0] v;  input [7:0] lo, hi;
        begin
            if (v < $signed({4'd0, lo}))      clamp = lo;
            else if (v > $signed({4'd0, hi})) clamp = hi;
            else                              clamp = v[7:0];
        end
    endfunction
    wire signed [11:0] y_sc = (ys >>> 10) + 12'sd16;
    wire signed [11:0] u_sc = (us >>> 10) + 12'sd128;
    wire signed [11:0] v_sc = (vs >>> 10) + 12'sd128;

    // three consecutive samples: A = n+1, B = n (the one being output), C = n-1
    reg [7:0] yA, uA, vA, yB, uB, vB, uC, vC;
    reg [2:0] sA, sB;
    reg       deC;
    reg       phA, phB;                             // 0 = even pixel of the line
    always @(posedge pix_clk) begin
        yA  <= clamp(y_sc, 8'd16, 8'd235);
        uA  <= clamp(u_sc, 8'd16, 8'd240);
        vA  <= clamp(v_sc, 8'd16, 8'd240);
        sA  <= s2;
        phA <= (s2[0] && sA[0]) ? ~phA : 1'b0;      // first DE pixel of a line is even
        yB <= yA; uB <= uA; vB <= vA; sB <= sA; phB <= phA;
        uC <= uB; vC <= vB; deC <= sB[0];
    end

    // ---- stage 4: [1 2 1]/4 chroma filter around B, 4:2:2 packing ----------------
    wire [7:0] uL = deC   ? uC : uB,  uR = sA[0] ? uA : uB;
    wire [7:0] vL = deC   ? vC : vB,  vR = sA[0] ? vA : vB;
    wire [9:0] u_f = {2'b0, uL} + {1'b0, uB, 1'b0} + {2'b0, uR} + 10'd2;
    wire [9:0] v_f = {2'b0, vL} + {1'b0, vB, 1'b0} + {2'b0, vR} + 10'd2;
    reg  [7:0] cr_hold;

    always @(posedge pix_clk) begin
        hd_de    <= sB[0];
        hd_hsync <= sB[1];
        hd_vsync <= sB[2];
        if (!sB[0]) begin
            hd_d <= {8'h80, 8'h10};
        end else if (!phB) begin
            hd_d    <= {u_f[9:2], yB};              // even: Cb, Y
            cr_hold <= v_f[9:2];                    // Cr of the same (even) position
        end else begin
            hd_d    <= {cr_hold, yB};               // odd: Cr, Y
        end
    end

endmodule
