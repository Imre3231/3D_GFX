`timescale 1ns / 1ps
//=============================================================================
//  SDRAM_ctrl  --  SDR-SDRAM controller: frame buffers and 3D data
//                  Spartan-6, 2 x IS42S16320D in parallel (32 bit), CL3, BL1
//
//  Read port   RdClk domain (the display clock): RdData is the word at RdAddr
//              one RdClk edge later (DVI_TOP: RD_LATENCY = 2).  No request.
//  Write port  clk domain: WR / WrAddr / WrData into a write FIFO, WrFull =
//              back-pressure (4 words early: a word per clock is safe),
//              WrIdle = every queued word issued.
//  DMA port    clk domain: DmaReq with DmaAddr / DmaLen (1..256) while DmaRdy
//              (no burst left to issue: the next may start while the reads of
//              the last are in flight); the words come back in order, one per
//              DmaValid.  Reads do not see words still in the write FIFO: wait
//              for WrIdle.
//
//  Reads come from a line buffer (slot = address mod 2**LB_AW) whose read
//  port runs on RdClk.  The controller follows RdAddr through a two-register
//  synchronizer: up to LB_HIST words behind it are kept, the rest is
//  read-ahead.  A jump reloads the buffer; the display must present the new
//  address ahead of use (DVI_TOP does, from HSYNC on).
//  The display has priority while the read-ahead is below LB_LO: writes and
//  DMA reads wait and up to four refreshes are postponed.  From LB_HI on, DMA
//  reads and writes run, taking turns when both wait (a DMA burst, then up to
//  64 written words); display read-ahead gets the rest.  Writes are written
//  through into the line buffer as they reach the SDRAM.  Every read in flight
//  carries a tag: its data goes to the line buffer or to the DMA port.
//
//  Timing: decisions use only registered status (buffer levels, open-row
//  match, timers); line-buffer writes are applied one clock after they are
//  decided; the SDRAM pins are driven from a register stage of their own.
//  No path carries arithmetic into a pin or a RAM port.
//
//  Board: CS#=0, CKE=1, DQM=0, BA=0 strapped -> NOP driven in every idle cycle.
//  Debug: TEST_PAT=1 returns the address pattern; POST_FAIL=0 = address test ok.
//=============================================================================

module SDRAM_ctrl #(
    // ---- geometry ----------------------------------------------------------
    parameter ADDR_W    = 23,                // word address (8M x 32, the whole bank)
    parameter COL_W     = 10,                // 1024 columns per row
    // ---- timing (clocks) ---------------------------------------------------
    parameter CLK_MHZ   = 50,                // must match clk
    parameter T_INIT    = 200*CLK_MHZ,       // 200 us power-up wait
    parameter T_REFI    = 7*CLK_MHZ,           // refresh every 7 us (max 7.81)
    parameter T_RP      = 3,
    parameter T_RCD     = 3,
    parameter T_RAS     = 5,
    parameter T_RFC     = 9,
    parameter T_MRD     = 3,
    parameter T_WR      = 3,
    parameter CL        = 3,
    parameter RD_LAT    = 6,                 // used if CAL_EN=0 or calibration fails
    parameter CAL_EN    = 1,
    // ---- buffers -----------------------------------------------------------
    parameter LB_AW     = 10,                // line buffer    2**LB_AW words
    parameter WB_AW     = 9,                 // write FIFO     2**WB_AW words
    parameter LB_HIST   = (1 << LB_AW) / 2,  // words kept behind the display
    parameter LB_LO     = ((1 << LB_AW) - LB_HIST) / 2,     // read-ahead below: display first
    parameter LB_HI     = ((1 << LB_AW) - LB_HIST) / 2 + 64, // read-ahead from: DMA and writes run
    parameter JUMP_WIN  = 64,                // wait, not reload, this far past the fill
    parameter RD_SYNC   = 2,                 // 0: RdClk is clk, else RdAddr is synchronized
    parameter CMD_GAP   = 0,                 // idle clocks between column commands
    parameter CMD_PHASE = 0,                 // 1: cmd/addr/data half a clock later
    parameter POST_EN   = 1,                 // power-up address test
    parameter TEST_PAT  = 0,                 // 1: SDRAM bypassed, RdData = address pattern
    parameter WB_LINGER = 64,                // keep the write row open this long
    parameter WR_SYNC   = 1,                 // 1: WR/WrAddr/WrData registered first
    parameter WR_EDGE   = 0                  // 1: one word per rising edge of WR
)(
    input  wire              clk,
    input  wire              RdClk,          // display clock (DVI pix_clk)
    input  wire              RdReq,          // unused, kept for pin compatibility
    input  wire [ADDR_W-1:0] RdAddr,
    output wire [31:0]       RdData,
    input  wire              WR,
    input  wire [ADDR_W-1:0] WrAddr,
    input  wire [31:0]       WrData,
    output wire              SDRAM_BUSY,     // = ~RdRdy
    output wire              SDRAM_WEn,
    output wire              SDRAM_CASn,
    output wire              SDRAM_RASn,
    output wire [12:0]       SDRAM_A,
    inout  wire [31:0]       SDRAM_DQ,
    output wire              RdRdy,          // clk domain: display address is buffered
    output wire              WrAck,          // word taken this cycle
    output wire              WrFull,         // do not offer a word
    output wire [3:0]        CAL_LAT,        // measured read latency
    output wire [4:0]        POST_FAIL,      // 0 = address test passed
    output wire              WR_LOST,        // sticky: word offered while full
    output wire              WrIdle,         // write FIFO empty, last word issued
    input  wire              DmaReq,         // clk domain: take a burst read while DmaRdy
    input  wire [ADDR_W-1:0] DmaAddr,
    input  wire [8:0]        DmaLen,         // words, 1..256
    output wire              DmaRdy,         // no burst pending or in flight
    output wire [31:0]       DmaData,
    output wire              DmaValid        // one word, in address order
);

//-----------------------------------------------------------------------------
localparam ROW_W   = ADDR_W - COL_W;
localparam LB_DEP  = 1 << LB_AW;
localparam WB_DEP  = 1 << WB_AW;
localparam WB_W    = ADDR_W + 32;
localparam WAIT_W  = JUMP_WIN;

// {RAS#,CAS#,WE#}
localparam [2:0] CMD_NOP = 3'b111, CMD_ACT = 3'b011, CMD_RD  = 3'b101,
                 CMD_WR  = 3'b100, CMD_PRE = 3'b010, CMD_REF = 3'b001,
                 CMD_MRS = 3'b000;

localparam [2:0]  CL_BITS  = CL;
localparam [12:0] MODE_REG = {3'b000, 1'b0, 2'b00, CL_BITS, 1'b0, 3'b000}; // BL1
localparam [12:0] A_PALL   = 13'h0400;       // A10 = 1: precharge all

localparam [ROW_W-1:0] CAL_ROW = {ROW_W{1'b1}};
localparam [COL_W-1:0] CAL_C0  = {{(COL_W-1){1'b1}}, 1'b0};
localparam [COL_W-1:0] CAL_C1  = {COL_W{1'b1}};
localparam [31:0]      CAL_P0  = 32'h5A5AA5A5;
localparam [31:0]      CAL_P1  = 32'h1234ABCD;

localparam [2:0] S_RESET = 3'd0, S_INIT_PRE = 3'd1, S_INIT_REF = 3'd2,
                 S_INIT_MRS = 3'd3, S_CAL = 3'd4, S_IDLE = 3'd5, S_ROW = 3'd6,
                 S_POST = 3'd7;

//-----------------------------------------------------------------------------
// registers (each written by one always block)
//-----------------------------------------------------------------------------
// sequencer: the decided command, address and write data (cmd_i, a_i, ...)
reg  [2:0]        state    = S_RESET;
reg  [15:0]       init_cnt = 16'd0;           // power-up wait
reg  [3:0]        tmr      = 4'd0;            // command spacing
reg  [3:0]        init_ref = 4'd0;
reg  [3:0]        cal_step = 4'd0;
reg  [2:0]        cmd_i    = CMD_NOP;
reg  [12:0]       a_i      = 13'd0;
reg  [31:0]       dqo_i    = 32'd0;
reg               dqoe_i   = 1'b0;
reg  [ROW_W-1:0]  open_row = {ROW_W{1'b0}};
reg               pf_in_row = 1'b0;           // pf_addr lies in open_row
reg  [3:0]        ras_cnt  = 4'd0;
reg  [3:0]        wrec_cnt = 4'd0;
reg  [3:0]        gap_cnt  = 4'd0;
reg               busy_r   = 1'b1;
reg  [3:0]        rd_lat   = RD_LAT;
reg  [15:0]       cal_m0   = 16'd0;
reg  [31:0]       cal_pat  = 32'd0;
reg               cal_go   = 1'b0;
reg               ref_ack  = 1'b0;
reg  [4:0]        po_i     = 5'd0;
reg  [4:0]        po_fail  = 5'd0;
reg  [3:0]        po_step  = 4'd0;

// pin stage: copies of the decision one clock later, in the IOBs
(* IOB = "TRUE" *) reg [2:0]  cmd    = CMD_NOP;
(* IOB = "TRUE" *) reg [12:0] a_r    = 13'd0;
(* IOB = "TRUE" *) reg [31:0] dq_out = 32'd0;
reg               dq_oe    = 1'b0;
(* IOB = "TRUE" *) reg [31:0] dq_r   = 32'd0;

// read tokens, calibration
reg  [9:0]        tok      = 10'd0;          // reads in flight, by age
reg  [9:0]        dtag     = 10'd0;          // ... of which DMA reads
reg  [9:0]        busy_msk = 10'h3FF;        // tok bits [rd_lat:0]
reg  [9:0]        ret_msk  = 10'h020;        // tok bit  rd_lat-1
reg  [15:0]       cal_mask = 16'd0;
reg  [3:0]        cal_cnt  = 4'd0;
reg               cal_run  = 1'b0;

// refresh
reg  [15:0]       ref_tick = 16'd0;
reg  [2:0]        ref_pend = 3'd0;

// RdAddr synchronizer: loads only a value held on two clocks, from a register
(* ASYNC_REG = "TRUE", SHREG_EXTRACT = "NO" *)
reg  [ADDR_W-1:0] ra_m     = {ADDR_W{1'b0}};
reg  [ADDR_W-1:0] ra_s     = {ADDR_W{1'b0}};
reg  [ADDR_W-1:0] ra_q     = {ADDR_W{1'b0}};

// line buffer: holds addresses lb_base .. lb_base+lb_cnt-1, slot = address mod LB_DEP
(* ram_style = "block" *)
reg  [31:0]       lb_mem [0:LB_DEP-1];
reg  [31:0]       rd_q     = 32'd0;           // RdClk domain
reg  [31:0]       pat_q    = 32'd0;           // RdClk domain, TEST_PAT
reg  [LB_AW:0]    lb_wp    = {(LB_AW+1){1'b0}};
reg  [LB_AW:0]    lb_rp    = {(LB_AW+1){1'b0}};
reg  [ADDR_W-1:0] lb_base  = {ADDR_W{1'b0}};
reg  [ADDR_W-1:0] fl_addr  = {ADDR_W{1'b0}};  // address of the next fill
reg  [ADDR_W-1:0] pf_addr  = {ADDR_W{1'b0}};  // next address to read
reg  [4:0]        pf_fly   = 5'd0;            // reads in flight
reg  [4:0]        pf_disc  = 5'd0;            // in-flight reads to discard
reg               lb_fl_r  = 1'b0;            // reload request
reg               fr_v     = 1'b0;            // free pending
reg  [LB_AW:0]    fr_n     = {(LB_AW+1){1'b0}};  // words to free

// registered line-buffer status
reg               lb_space_r = 1'b0;
reg               disp_low_r = 1'b1;
reg               ahead_hi_r = 1'b0;
reg               turn_dma   = 1'b0;          // DMA and writes both waiting: DMA's turn
reg  [5:0]        wturn      = 6'd0;          // words written in this write turn
reg               rd_rdy_r   = 1'b0;

// line-buffer write stage: a fill or a write-through, applied next clock
reg               lbw_v    = 1'b0;
reg               lbw_fill = 1'b0;
reg  [LB_AW-1:0]  lbw_a    = {LB_AW{1'b0}};
reg  [31:0]       lbw_d    = 32'd0;
reg               wt_v     = 1'b0;            // a WRITE was taken last clock
reg  [ADDR_W-1:0] wt_a     = {ADDR_W{1'b0}};
reg  [31:0]       wt_d     = 32'd0;

// write FIFO
(* ram_style = "block" *)
reg  [WB_W-1:0]   wb_mem [0:WB_DEP-1];
reg  [WB_W-1:0]   wb_dout  = {WB_W{1'b0}};
reg  [WB_AW:0]    wb_wp    = {(WB_AW+1){1'b0}};
reg  [WB_AW:0]    wb_rp    = {(WB_AW+1){1'b0}};
reg               wb_hd    = 1'b0;            // wb_dout holds a queued word
reg               wr_d     = 1'b0;
reg               wr_d2    = 1'b0;
reg  [ADDR_W-1:0] wa_d     = {ADDR_W{1'b0}};
reg  [31:0]       wd_d     = 32'd0;
reg  [7:0]        wl_cnt   = 8'd0;            // write-row linger
reg               wlost_r  = 1'b0;
reg               wmode    = 1'b0;            // writes may use the bus

// DMA burst read
reg  [ADDR_W-1:0] dma_addr = {ADDR_W{1'b0}};  // next address to read
reg  [8:0]        dma_left = 9'd0;            // words still to read
reg               dma_in_row = 1'b0;          // dma_addr lies in open_row
reg               dmode    = 1'b0;            // DMA reads may use the bus
reg  [31:0]       dma_q    = 32'd0;
reg               dma_v    = 1'b0;

// falling-edge output stage (CMD_PHASE)
reg  [2:0]        cmd_n    = CMD_NOP;
reg  [12:0]       a_n      = 13'd0;
reg  [31:0]       dqo_n    = 32'd0;
reg               dqoe_n   = 1'b0;

//-----------------------------------------------------------------------------
// write FIFO status (look-ahead read: the head is available every clock)
//-----------------------------------------------------------------------------
wire [WB_AW:0]    wb_cnt   = wb_wp - wb_rp;
wire              wb_full  = wb_cnt[WB_AW];
wire              wb_afull = (wb_cnt >= WB_DEP - 4);          // WrFull: room for words in flight
wire              wr_lvl   = (WR_SYNC != 0) ? wr_d  : WR;
wire              wr_prev  = (WR_SYNC != 0) ? wr_d2 : wr_d;
wire              wr_req   = (WR_EDGE != 0) ? (wr_lvl & ~wr_prev) : wr_lvl;
wire              wb_push  = wr_req & ~wb_full;
wire [ADDR_W-1:0] wa_in    = (WR_SYNC != 0) ? wa_d : WrAddr;
wire [31:0]       wd_in    = (WR_SYNC != 0) ? wd_d : WrData;
wire [ADDR_W-1:0] wf_addr  = wb_dout[WB_W-1:32];
wire [31:0]       wf_data  = wb_dout[31:0];
wire              wr_pend  = wb_hd;

//-----------------------------------------------------------------------------
// line-buffer status (rd_d = distance of ra from the oldest buffered word);
// everything here is registered before the sequencer uses it
//   rd_d <  lb_cnt          : word is in the line buffer
//   rd_d - lb_cnt <= WAIT_W : just past the fill, wait for it
//   otherwise               : jump, reload from ra
//-----------------------------------------------------------------------------
wire [ADDR_W-1:0] ra       = (RD_SYNC == 0) ? RdAddr : ra_q;
wire [LB_AW:0]    lb_cnt   = lb_wp - lb_rp;
wire [ADDR_W-1:0] lb_cnt_x = {{(ADDR_W-LB_AW-1){1'b0}}, lb_cnt};
wire [ADDR_W-1:0] rd_d     = ra - lb_base;
wire              rd_here  = (rd_d <  lb_cnt_x);
wire              rd_in    = (rd_d <= lb_cnt_x);
wire [ADDR_W-1:0] rd_past  = rd_d - lb_cnt_x;              // words past the fill
wire              lb_flush = ~lb_fl_r & ~rd_in & (rd_past > WAIT_W);
wire              lb_free  = ~lb_fl_r & ~fr_v & rd_in & (rd_d > LB_HIST);
wire [LB_AW:0]    lb_ahead = rd_in ? (lb_cnt - rd_d[LB_AW:0]) : {(LB_AW+1){1'b0}};
wire [LB_AW+1:0]  lb_used  = {1'b0, lb_cnt} + {{(LB_AW-3){1'b0}}, pf_fly} + lbw_fill;
wire [ADDR_W-1:0] wt_off   = wt_a - lb_base;
wire              wt_hit   = wt_v & (wt_off < lb_cnt_x);  // written word is buffered

//-----------------------------------------------------------------------------
// SDRAM side decode: registered inputs and short logic only
//-----------------------------------------------------------------------------
wire [ROW_W-1:0]  pf_row   = pf_addr[ADDR_W-1:COL_W];
wire [ROW_W-1:0]  wr_row   = wf_addr[ADDR_W-1:COL_W];
wire [ROW_W-1:0]  dma_row  = dma_addr[ADDR_W-1:COL_W];

wire idle_rdy  = (state == S_IDLE) & (tmr == 4'd0);
wire row_rdy   = (state == S_ROW ) & (tmr == 4'd0);
wire ras_ok    = (ras_cnt  == 4'd0);
wire wrec_ok   = (wrec_cnt == 4'd0);
wire gap_ok    = (gap_cnt  == 4'd0);
wire dq_busy   = |(tok & busy_msk);                 // SDRAM may drive DQ
wire ret       = |(tok & ret_msk);                  // read data is in dq_r
wire ret_dma   = |(dtag & ret_msk);                 // ... and it belongs to the DMA port
wire ret_lb    = ret & ~ret_dma;
wire ref_hold  = (ref_pend != 3'd0);
wire ref_now   = ref_hold & (~disp_low_r | ref_pend[2]);   // postpone at most 4

wire wr_want   = wr_pend & wmode;
wire rd_want   = lb_space_r & ~lb_fl_r;
wire dma_want  = (dma_left != 9'd0) & dmode;
wire wr_in_row = (wr_row == open_row);
wire lb_hold   = (wl_cnt != 8'd0) & ~disp_low_r;    // write row linger

wire ref_go   = row_rdy & ref_now & ras_ok & wrec_ok & ~dq_busy;
wire ref_idle = idle_rdy & ref_now;
wire dma_win  = dma_want & (~wr_want | turn_dma);          // both waiting: take turns
wire wr_win   = wr_want & ~dma_win;
wire wr_go    = row_rdy & ~ref_now & wr_win & wr_in_row & ~dq_busy & gap_ok;
wire dma_go   = row_rdy & ~ref_now & dma_win & dma_in_row & gap_ok;
wire rd_go    = row_rdy & ~ref_now & ~wr_want & ~dma_want & rd_want & pf_in_row & gap_ok;
wire pre_go   = row_rdy & ~ref_now & ~wr_go & ~dma_go & ~rd_go & ras_ok & wrec_ok & ~dq_busy &
                ( (dma_win & ~dma_in_row) |
                  (wr_win & ~wr_in_row) |
                  (~wr_want & ~dma_want & rd_want & ~pf_in_row & ~lb_hold) );
wire use_wr   = wr_win | (wr_pend & ~rd_want & ~dma_want);
wire act_go   = idle_rdy & ~ref_now & (use_wr | dma_want | rd_want);
wire [ROW_W-1:0] act_row   = use_wr ? wr_row : (dma_want ? dma_row : pf_row);
wire [ROW_W-1:0] next_open = act_go ? act_row : open_row;
wire             pf_last   = (pf_addr[COL_W-1:0] == {COL_W{1'b1}});
wire             dma_last  = (dma_addr[COL_W-1:0] == {COL_W{1'b1}});
wire             dma_take  = DmaReq & DmaRdy;

wire [4:0]    fly_next = pf_fly + (rd_go ? 5'd1 : 5'd0) - (ret_lb ? 5'd1 : 5'd0);
wire [WB_AW:0] wb_rp_n = wb_rp + (wr_go ? 1'b1 : 1'b0);

// fills need a read in flight and wr_go needs none, and a write-through
// trails its WRITE by one clock: a fill and a write-through never meet
wire lb_fill = ret_lb & (pf_disc == 5'd0) & ~lb_fl_r;

// power-on self test: walking-one addresses, index-derived data
wire [ADDR_W-1:0] po_adr = (po_i == 5'd0) ? {ADDR_W{1'b0}}
                                          : ({{(ADDR_W-1){1'b0}}, 1'b1} << (po_i-5'd1));
wire [31:0]       po_dat = {po_i, ~po_i, po_i, ~po_i, po_i, ~po_i, 2'b01};

// row address on A[12:0], zero-extended
function [12:0] pad13;
    input [12:0] v;
    pad13 = v;
endfunction

//-----------------------------------------------------------------------------
// outputs
//-----------------------------------------------------------------------------
wire [2:0]  cmd_o  = (CMD_PHASE != 0) ? cmd_n  : cmd;
wire [12:0] a_o    = (CMD_PHASE != 0) ? a_n    : a_r;
wire [31:0] dqo_o  = (CMD_PHASE != 0) ? dqo_n  : dq_out;
wire        dqoe_o = (CMD_PHASE != 0) ? dqoe_n : dq_oe;

assign {SDRAM_RASn, SDRAM_CASn, SDRAM_WEn} = cmd_o;
assign SDRAM_A    = a_o;
assign SDRAM_DQ   = dqoe_o ? dqo_o : 32'hZZZZZZZZ;

assign RdData     = (TEST_PAT != 0) ? pat_q : rd_q;
assign RdRdy      = (TEST_PAT != 0) ? 1'b1 : (rd_rdy_r & ~busy_r);
assign SDRAM_BUSY = ~RdRdy;
assign WrAck      = wb_push;
assign WrFull     = wb_afull;
assign CAL_LAT    = rd_lat;
assign POST_FAIL  = po_fail;
assign WR_LOST    = wlost_r;
assign WrIdle     = ~wb_hd & (wb_cnt == {(WB_AW+1){1'b0}}) & ~wr_req;
assign DmaRdy     = (dma_left == 9'd0) & ~busy_r;
assign DmaData    = dma_q;
assign DmaValid   = dma_v;

//-----------------------------------------------------------------------------
// pin stage and falling-edge output stage
//-----------------------------------------------------------------------------
always @(posedge clk) begin
    cmd    <= cmd_i;
    a_r    <= a_i;
    dq_out <= dqo_i;
    dq_oe  <= dqoe_i;
end

always @(negedge clk) begin
    cmd_n  <= cmd;
    a_n    <= a_r;
    dqo_n  <= dq_out;
    dqoe_n <= dq_oe;
end

//-----------------------------------------------------------------------------
// RdAddr into clk: two registered samples must agree, so a half-changed
// address is never taken
//-----------------------------------------------------------------------------
always @(posedge clk) begin
    ra_m <= RdAddr;
    ra_s <= ra_m;
    if (ra_m == ra_s) ra_q <= ra_s;
end

//-----------------------------------------------------------------------------
// pin capture, read tokens, latency calibration
//   cal_mask[j] = 1: the pattern was in dq_r j clocks after the READ decision
//-----------------------------------------------------------------------------
always @(posedge clk) begin
    busy_msk <= 10'h3FF >> (4'd9 - rd_lat);
    ret_msk  <= 10'd1   << (rd_lat - 4'd1);
    dq_r     <= SDRAM_DQ;
    tok      <= {tok[8:0], rd_go | dma_go};
    dtag     <= {dtag[8:0], dma_go};

    if (cal_go) begin
        cal_mask <= 16'd0;
        cal_cnt  <= 4'd1;
        cal_run  <= 1'b1;
    end
    else if (cal_run) begin
        cal_cnt <= cal_cnt + 4'd1;
        if (cal_cnt == 4'd12) cal_run <= 1'b0;
        if (dq_r == cal_pat)  cal_mask[cal_cnt + 4'd1] <= 1'b1;
    end
end

//-----------------------------------------------------------------------------
// refresh requests
//-----------------------------------------------------------------------------
wire ref_tick_end = (ref_tick == T_REFI - 1);

always @(posedge clk) begin
    ref_tick <= ref_tick_end ? 16'd0 : ref_tick + 16'd1;

    case ({ref_tick_end, ref_ack})
        2'b10  : if (ref_pend != 3'd7) ref_pend <= ref_pend + 3'd1;
        2'b01  : if (ref_pend != 3'd0) ref_pend <= ref_pend - 3'd1;
        default: ;
    endcase
end

//-----------------------------------------------------------------------------
// write FIFO block RAM
//-----------------------------------------------------------------------------
always @(posedge clk) begin
    if (wb_push) wb_mem[wb_wp[WB_AW-1:0]] <= {wa_in, wd_in};
    wb_dout <= wb_mem[wb_rp_n[WB_AW-1:0]];
end

//-----------------------------------------------------------------------------
// write FIFO pointers, write mode
//-----------------------------------------------------------------------------
always @(posedge clk) begin
    wr_d  <= WR;
    wr_d2 <= wr_d;
    wa_d  <= WrAddr;
    wd_d  <= WrData;
    if (wr_req & wb_full) wlost_r <= 1'b1;

    if (wb_push) wb_wp <= wb_wp + 1'b1;
    wb_rp <= wb_rp_n;
    wb_hd <= ((wb_wp - wb_rp_n) != {(WB_AW+1){1'b0}});   // word written before this edge

    if (wr_go)               wl_cnt <= WB_LINGER;
    else if (wl_cnt != 8'd0) wl_cnt <= wl_cnt - 8'd1;

    // display first; a full FIFO may drain early once the read-ahead is safe
    if (~wr_pend | disp_low_r)                wmode <= 1'b0;
    else if (ahead_hi_r | dmode)              wmode <= 1'b1;      // display refilled to LB_HI first
end

//-----------------------------------------------------------------------------
// line buffer block RAM   A (clk): write stage   B (RdClk): display
//-----------------------------------------------------------------------------
always @(posedge clk)
    if (lbw_v) lb_mem[lbw_a] <= lbw_d;

always @(posedge RdClk) begin
    rd_q  <= lb_mem[RdAddr[LB_AW-1:0]];
    pat_q <= {RdAddr[15:0], ~RdAddr[15:0]};
end

//-----------------------------------------------------------------------------
// line-buffer write stage and registered status
//-----------------------------------------------------------------------------
always @(posedge clk) begin
    lbw_v    <= lb_fill | wt_hit;
    lbw_fill <= lb_fill;
    lbw_a    <= lb_fill ? fl_addr[LB_AW-1:0] : wt_a[LB_AW-1:0];
    lbw_d    <= lb_fill ? dq_r : wt_d;

    wt_v     <= wr_go;
    wt_a     <= wf_addr;
    wt_d     <= wf_data;

    lb_space_r <= (lb_used < LB_DEP - 4);
    disp_low_r <= (lb_ahead < LB_LO);
    ahead_hi_r <= (lb_ahead >= LB_HI);
    rd_rdy_r   <= rd_here;
end

//-----------------------------------------------------------------------------
// line buffer pointers: a word counts once its write stage has written it
//-----------------------------------------------------------------------------
always @(posedge clk) begin
    lb_fl_r <= lb_flush;
    pf_fly  <= fly_next;
    if (rd_go)                   pf_addr <= pf_addr + 1'b1;
    if (lb_fill)                 fl_addr <= fl_addr + 1'b1;
    if (ret_lb & (pf_disc != 5'd0)) pf_disc <= pf_disc - 5'd1;
    if (lbw_fill)                lb_wp   <= lb_wp + 1'b1;

    if (lb_fl_r) begin                          // reload from ra
        lb_rp   <= {1'b0, ra[LB_AW-1:0]};
        lb_wp   <= {1'b0, ra[LB_AW-1:0]};
        lb_base <= ra;
        fl_addr <= ra;
        pf_addr <= ra;
        pf_disc <= fly_next;
        fr_v    <= 1'b0;
    end
    else if (fr_v) begin                        // keep LB_HIST words behind ra
        lb_rp   <= lb_rp + fr_n;
        lb_base <= lb_base + fr_n;
        fr_v    <= 1'b0;
    end
    else if (lb_free) begin
        fr_n    <= rd_d[LB_AW:0] - LB_HIST;
        fr_v    <= 1'b1;
    end
end

//-----------------------------------------------------------------------------
// prefetch address lies in the open row: set on ACT or reload, cleared when
// the last column of the row has been read
//-----------------------------------------------------------------------------
always @(posedge clk)
    if (lb_fl_r)              pf_in_row <= (ra[ADDR_W-1:COL_W] == next_open);
    else if (act_go)          pf_in_row <= (pf_row == act_row);
    else if (rd_go & pf_last) pf_in_row <= 1'b0;

//-----------------------------------------------------------------------------
// DMA burst read: address, words left and in flight; data out one clock
// after it reaches dq_r
//-----------------------------------------------------------------------------
always @(posedge clk) begin
    dma_v <= ret_dma;
    // like writes: run once the display is well ahead, stop when it runs low
    if ((dma_left == 9'd0) | disp_low_r)          dmode <= 1'b0;
    else if (ahead_hi_r | ~lb_space_r | wmode)     dmode <= 1'b1;   // with writes: turns decide
    dma_q <= dq_r;
    // both waiting: a DMA burst, then up to 64 written words
    if (turn_dma) begin
        if (~dma_want | (dma_go & (dma_left == 9'd1))) turn_dma <= 1'b0;
    end else begin
        if (wr_go) wturn <= wturn + 6'd1;
        if (dma_want & (~wr_want | (wr_go & (wturn == 6'd63)))) begin
            turn_dma <= 1'b1;
            wturn    <= 6'd0;
        end
    end
    if (dma_take) begin
        dma_addr   <= DmaAddr;
        dma_left   <= DmaLen;
        dma_in_row <= (DmaAddr[ADDR_W-1:COL_W] == next_open);
    end
    else begin
        if (dma_go) begin
            dma_addr <= dma_addr + 1'b1;
            dma_left <= dma_left - 9'd1;
        end
        if (act_go)                 dma_in_row <= (dma_row == act_row);
        else if (dma_go & dma_last) dma_in_row <= 1'b0;
    end
end

//-----------------------------------------------------------------------------
// main sequencer: decides cmd_i / a_i / dqo_i / dqoe_i, the pin stage copies
//-----------------------------------------------------------------------------
always @(posedge clk) begin
    cmd_i   <= CMD_NOP;
    dqoe_i  <= 1'b0;
    ref_ack <= 1'b0;
    cal_go  <= 1'b0;

    if (init_cnt != 16'd0) init_cnt <= init_cnt - 16'd1;
    if (tmr      !=  4'd0) tmr      <= tmr      -  4'd1;
    if (ras_cnt  !=  4'd0) ras_cnt  <= ras_cnt  -  4'd1;
    if (wrec_cnt !=  4'd0) wrec_cnt <= wrec_cnt -  4'd1;
    if (gap_cnt  !=  4'd0) gap_cnt  <= gap_cnt  -  4'd1;
    if (rd_go | wr_go)     gap_cnt  <= CMD_GAP;

    case (state)

    //---------------------------------------------------------- power-up
    S_RESET: begin
        busy_r   <= 1'b1;
        a_i      <= 13'd0;
        init_cnt <= T_INIT;
        state    <= S_INIT_PRE;
    end

    S_INIT_PRE: if (init_cnt == 16'd0) begin
        cmd_i    <= CMD_PRE;
        a_i      <= A_PALL;
        tmr      <= T_RP - 1;
        init_ref <= 4'd8;
        state    <= S_INIT_REF;
    end

    S_INIT_REF: if (tmr == 4'd0) begin
        if (init_ref != 4'd0) begin
            cmd_i    <= CMD_REF;
            tmr      <= T_RFC - 1;
            init_ref <= init_ref - 4'd1;
        end
        else begin
            cmd_i <= CMD_MRS;
            a_i   <= MODE_REG;
            tmr   <= T_MRD - 1;
            state <= S_INIT_MRS;
        end
    end

    S_INIT_MRS: if (tmr == 4'd0) begin
        if (CAL_EN != 0) begin
            cal_step <= 4'd0;
            state    <= S_CAL;
        end
        else begin
            busy_r <= 1'b0;
            state  <= S_IDLE;
        end
    end

    //---------------------------------------------------------- read latency
    S_CAL: if (tmr == 4'd0) case (cal_step)
        4'd0: begin
            cmd_i    <= CMD_ACT;
            a_i      <= pad13(CAL_ROW);
            ras_cnt  <= T_RAS - 1;
            tmr      <= T_RCD - 1;
            cal_step <= 4'd1;
        end
        4'd1: begin
            cmd_i    <= CMD_WR;
            a_i      <= {{(13-COL_W){1'b0}}, CAL_C0};
            dqo_i    <= CAL_P0;
            dqoe_i   <= 1'b1;
            cal_step <= 4'd2;
        end
        4'd2: begin
            cmd_i    <= CMD_WR;
            a_i      <= {{(13-COL_W){1'b0}}, CAL_C1};
            dqo_i    <= CAL_P1;
            dqoe_i   <= 1'b1;
            wrec_cnt <= T_WR;
            cal_step <= 4'd3;
        end
        4'd3: if (wrec_ok & ras_ok) begin
            cmd_i    <= CMD_PRE;
            a_i      <= A_PALL;
            tmr      <= T_RP - 1;
            cal_step <= 4'd4;
        end
        4'd4: begin
            cmd_i    <= CMD_ACT;
            a_i      <= pad13(CAL_ROW);
            ras_cnt  <= T_RAS - 1;
            tmr      <= T_RCD - 1;
            cal_step <= 4'd5;
        end
        4'd5: begin                                   // read back pattern 0
            cmd_i    <= CMD_RD;
            a_i      <= {{(13-COL_W){1'b0}}, CAL_C0};
            cal_pat  <= CAL_P0;
            cal_go   <= 1'b1;
            tmr      <= 4'd14;
            cal_step <= 4'd6;
        end
        4'd6: begin                                   // read back pattern 1
            cal_m0   <= cal_mask;
            cmd_i    <= CMD_RD;
            a_i      <= {{(13-COL_W){1'b0}}, CAL_C1};
            cal_pat  <= CAL_P1;
            cal_go   <= 1'b1;
            tmr      <= 4'd14;
            cal_step <= 4'd7;
        end
        4'd7: begin
            cal_m0   <= cal_m0 & cal_mask;            // both patterns must agree
            cal_step <= 4'd8;
        end
        4'd8: begin
            if      (cal_m0[4]) rd_lat <= 4'd4;
            else if (cal_m0[5]) rd_lat <= 4'd5;
            else if (cal_m0[6]) rd_lat <= 4'd6;
            else if (cal_m0[7]) rd_lat <= 4'd7;
            else if (cal_m0[8]) rd_lat <= 4'd8;
            else if (cal_m0[9]) rd_lat <= 4'd9;
            else                rd_lat <= RD_LAT;
            cmd_i    <= CMD_PRE;
            a_i      <= A_PALL;
            tmr      <= T_RP - 1;
            cal_step <= 4'd9;
        end
        default: begin
            if (POST_EN != 0) begin
                po_i    <= 5'd0;
                po_step <= 4'd0;
                state   <= S_POST;
            end
            else begin
                busy_r <= 1'b0;
                state  <= S_IDLE;
            end
        end
    endcase

    //---------------------------------------------------------- address self test
    // writes then reads back addresses 0,1,2,4,..; overwrites those words
    S_POST: if (tmr == 4'd0) case (po_step)
        4'd0: begin
            cmd_i   <= CMD_ACT;
            a_i     <= pad13(po_adr[ADDR_W-1:COL_W]);
            ras_cnt <= T_RAS - 1;
            tmr     <= T_RCD - 1;
            po_step <= 4'd1;
        end
        4'd1: begin
            cmd_i    <= CMD_WR;
            a_i      <= {{(13-COL_W){1'b0}}, po_adr[COL_W-1:0]};
            dqo_i    <= po_dat;
            dqoe_i   <= 1'b1;
            wrec_cnt <= T_WR;
            po_step  <= 4'd2;
        end
        4'd2: if (wrec_ok & ras_ok) begin
            cmd_i   <= CMD_PRE;
            a_i     <= A_PALL;
            tmr     <= T_RP - 1;
            po_step <= 4'd3;
        end
        4'd3: begin
            if (po_i == ADDR_W) begin po_i <= 5'd0; po_step <= 4'd4; end
            else                begin po_i <= po_i + 5'd1; po_step <= 4'd0; end
        end
        4'd4: begin
            cmd_i   <= CMD_ACT;
            a_i     <= pad13(po_adr[ADDR_W-1:COL_W]);
            ras_cnt <= T_RAS - 1;
            tmr     <= T_RCD - 1;
            po_step <= 4'd5;
        end
        4'd5: begin
            cmd_i   <= CMD_RD;
            a_i     <= {{(13-COL_W){1'b0}}, po_adr[COL_W-1:0]};
            tmr     <= rd_lat - 4'd1;                 // dq_r holds it at tmr = 0
            po_step <= 4'd6;
        end
        4'd6: begin
            if ((dq_r != po_dat) && (po_fail == 5'd0)) po_fail <= po_i + 5'd1;
            cmd_i   <= CMD_PRE;
            a_i     <= A_PALL;
            tmr     <= T_RP - 1;
            po_step <= 4'd7;
        end
        4'd7: begin
            if (po_i == ADDR_W) po_step <= 4'd8;
            else begin po_i <= po_i + 5'd1; po_step <= 4'd4; end
        end
        default: begin
            busy_r <= 1'b0;
            state  <= S_IDLE;
        end
    endcase

    //---------------------------------------------------------- all precharged
    S_IDLE: begin
        if (ref_idle) begin
            cmd_i   <= CMD_REF;
            ref_ack <= 1'b1;
            tmr     <= T_RFC - 1;
        end
        else if (act_go) begin
            cmd_i    <= CMD_ACT;
            a_i      <= pad13(act_row);
            open_row <= act_row;
            ras_cnt  <= T_RAS - 1;
            tmr      <= T_RCD - 1;
            state    <= S_ROW;
        end
    end

    //---------------------------------------------------------- row open
    S_ROW: begin
        if (ref_go | pre_go) begin
            cmd_i <= CMD_PRE;
            a_i   <= A_PALL;
            tmr   <= T_RP - 1;
            state <= S_IDLE;
        end
        else if (wr_go) begin
            cmd_i    <= CMD_WR;
            a_i      <= {{(13-COL_W){1'b0}}, wf_addr[COL_W-1:0]};
            dqo_i    <= wf_data;
            dqoe_i   <= 1'b1;
            wrec_cnt <= T_WR;
        end
        else if (rd_go) begin
            cmd_i <= CMD_RD;
            a_i   <= {{(13-COL_W){1'b0}}, pf_addr[COL_W-1:0]};
        end
        else if (dma_go) begin
            cmd_i <= CMD_RD;
            a_i   <= {{(13-COL_W){1'b0}}, dma_addr[COL_W-1:0]};
        end
    end

    default: state <= S_RESET;
    endcase
end

endmodule