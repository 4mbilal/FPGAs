// vga_pack.v
// Converts the 24-bit RGB video bus from v_axi4s_vid_out to the ZedBoard
// 12-bit VGA connector (4 bits per colour), blanking outside active video.
// All outputs are registered on the pixel clock (packed into IOBs by vga.xdc).
//
// Xilinx AXI4-Stream video RGB component order (UG934):
//   vid_data[23:16] = R, vid_data[15:8] = B, vid_data[7:0] = G

module vga_pack (
    input  wire        pix_clk,
    input  wire [23:0] vid_data,
    input  wire        vid_active,
    input  wire        vid_hsync,
    input  wire        vid_vsync,
    output reg  [3:0]  vga_r,
    output reg  [3:0]  vga_g,
    output reg  [3:0]  vga_b,
    output reg         vga_hs,
    output reg         vga_vs
);

    always @(posedge pix_clk) begin
        vga_r  <= vid_active ? vid_data[23:20] : 4'd0;
        vga_b  <= vid_active ? vid_data[15:12] : 4'd0;
        vga_g  <= vid_active ? vid_data[7:4]   : 4'd0;
        vga_hs <= vid_hsync;
        vga_vs <= vid_vsync;
    end

endmodule
