// clk_forward.v
// Forwards a fabric clock to an output pin through an ODDR (the standard 7-series
// way to drive a clock off-chip; avoids routing a clock net into general logic).
// Used for the camera XCLK.

module clk_forward (
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 clk_in CLK" *)
    input  wire clk_in,
    output wire clk_out
);

    ODDR #(
        .DDR_CLK_EDGE ("SAME_EDGE"),
        .INIT         (1'b0),
        .SRTYPE       ("SYNC")
    ) u_oddr (
        .Q  (clk_out),
        .C  (clk_in),
        .CE (1'b1),
        .D1 (1'b1),
        .D2 (1'b0),
        .R  (1'b0),
        .S  (1'b0)
    );

endmodule
