module mode_switch_1bit (
    input  wire MODE,
    input  wire WRITE,
    input  wire DATA,
    input  wire CLOCK,
    output wire WRITE_OUT1,
    output wire DATA_OUT1,
    output wire CLOCK_OUT1,
    output wire WRITE_OUT2,
    output wire DATA_OUT2,
    output wire CLOCK_OUT2
);

assign WRITE_OUT1 = (MODE == 1'b0) ? WRITE : 1'b0;
assign DATA_OUT1  = (MODE == 1'b0) ? DATA  : 1'b0;
assign CLOCK_OUT1 = (MODE == 1'b0) ? CLOCK : 1'b0;

assign WRITE_OUT2 = (MODE == 1'b1) ? WRITE : 1'b0;
assign DATA_OUT2  = (MODE == 1'b1) ? DATA  : 1'b0;
assign CLOCK_OUT2 = (MODE == 1'b1) ? CLOCK : 1'b0;

endmodule

