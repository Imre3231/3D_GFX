module mode_mux_mem (
    input  wire        MODE,
    input  wire        WE1,
    input  wire        WE2,
    input  wire [18:0] ADDR1,
    input  wire [22:0] ADDR2,
    input  wire [15:0] DATA1,
    input  wire [31:0] DATA2,
    output wire        WE_OUT,
    output wire [22:0] ADDR_OUT,
    output wire [31:0] DATA_OUT
);

assign WE_OUT   = (MODE == 1'b0) ? WE1   : WE2;
assign ADDR_OUT = (MODE == 1'b0) ? ADDR1 : ADDR2;
assign DATA_OUT = (MODE == 1'b0) ? DATA1 : DATA2;

endmodule


