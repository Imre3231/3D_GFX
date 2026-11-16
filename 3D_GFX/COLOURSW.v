 
module COLOURSW(
input   mode  ,
input   clk  ,

output reg  [7:0] RED,
output reg  [7:0] GREEN,
output reg  [7:0] BLUE,
input [31:0]  Datain32,
input [15:0]  Datain16

    );
	 
 
 always@(posedge clk)
 
 
  if (mode == 1'b1) begin
        // RGB888: 0xRRGGBB
        RED   <=  Datain32[23:16];
        GREEN <=  Datain32[15:8];
        BLUE  <=  Datain32[7:0];
 
 
	end else begin
			// RGB565 
    {BLUE[7:3]}   <=  Datain32[4:0];
    {GREEN[7:2]}  <= Datain32[10:5];
    {RED[7:3]}    <=  Datain32[15:11];
 
	end 
  
endmodule




 