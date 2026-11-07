`timescale 1ns/1ps
module tb;
  localparam W=8, H=4, FRAMES=3;
  reg pclk=0, aresetn=0; reg [7:0] d=0; reg href=0, vsync=0; reg tready=1;
  wire [23:0] tdata; wire tvalid, tuser, tlast;
  dvp_capture dut(.pclk(pclk),.aresetn(aresetn),.cam_d(d),.cam_href(href),.cam_vsync(vsync),
    .m_axis_tdata(tdata),.m_axis_tvalid(tvalid),.m_axis_tready(tready),.m_axis_tuser(tuser),.m_axis_tlast(tlast));
  always #20 pclk=~pclk;
  integer f,y,x,beats=0,lasts=0,users=0,errs=0,frame_beats; reg [15:0] px; reg [23:0] exp;
  // drive on falling edge like OV7670
  task send_frame(input integer fr); begin
    @(negedge pclk) vsync=1; repeat(6) @(negedge pclk); vsync=0; repeat(10) @(negedge pclk);
    for (y=0;y<H;y=y+1) begin
      for (x=0;x<W;x=x+1) begin
        px = {fr[3:0], y[3:0], x[7:0]};
        href=1; d=px[15:8]; @(negedge pclk); d=px[7:0]; @(negedge pclk);
      end
      href=0; d=8'hxx; repeat(12) @(negedge pclk);
    end
  end endtask
  // checker: expected pixel sequence
  integer cx=0, cy=0, cf=0, phase=0;
  function integer frame_of(input [23:0] t); frame_of = t[23:19] >> 1; endfunction
  function [23:0] conv(input [15:0] p); conv={p[15:11],p[15:13],p[4:0],p[4:2],p[10:5],p[10:9]}; endfunction
  always @(posedge pclk) if (tvalid && tready) begin
    beats=beats+1; if (tlast) lasts=lasts+1; if (tuser) users=users+1;
    if (tuser) begin cx=0; cy=0; end
    if (tuser) cf = frame_of(tdata);
    exp = conv({cf[3:0], cy[3:0], cx[7:0]});
    if (tdata!==exp || tuser!==(cx==0&&cy==0) || tlast!==(cx==W-1)) begin if (cf>=4 || phase==0) errs=errs+1;
      if (cf>=4 || phase==0) $display("ERR f%0d y%0d x%0d got %h u%b l%b exp %h", cf,cy,cx,tdata,tuser,tlast,exp); end
    cx=cx+1; if (cx==W) begin cx=0; cy=cy+1; if (cy==H) begin cy=0; cf=cf+1; end end
  end
  initial begin
    repeat(5) @(posedge pclk); aresetn=1;
    for (f=0;f<FRAMES;f=f+1) send_frame(f);
    repeat(20) @(posedge pclk);
    $display("beats=%0d (exp %0d) lasts=%0d (exp %0d) sof=%0d (exp %0d) errors=%0d",
      beats, W*H*FRAMES, lasts, H*FRAMES, users, FRAMES, errs);
    // overflow test: hold tready low mid-frame, expect frame drop then clean next frame
    beats=0; lasts=0; users=0; errs=0; phase=1;
    fork
      begin send_frame(3); send_frame(4); end
      begin repeat(40) @(posedge pclk); tready=0; repeat(30) @(posedge pclk); tready=1; end
    join
    repeat(20) @(posedge pclk);
    $display("after overflow: beats=%0d sof=%0d lasts=%0d errors=%0d (expect one clean frame 4: %0d beats)", beats, users, lasts, errs, W*H);
    $finish;
  end
endmodule
