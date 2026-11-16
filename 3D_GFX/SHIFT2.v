module shift_reg2
    #(parameter BITS = 19)
    (
    input clk,
     input d,
	  output qo,
    output [BITS - 1 : 0] q
    );
     
    // internal signals
    reg [BITS - 1  : 0] rShiftReg;
    wire [BITS -1 : 0] output_next;
     
    // circuit is activated on clk or rst
    always @(posedge clk )
      
       rShiftReg <= output_next;
             
    // update the shift register contents by shifting left
    assign output_next = { rShiftReg[BITS - 2 : 0], d };
	  
	
    // update our output 
    assign q = rShiftReg;
                           
endmodule