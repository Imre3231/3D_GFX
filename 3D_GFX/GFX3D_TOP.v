`timescale 1ns/1ps
`default_nettype none

// ============================================================================
// GFX3D_TOP (GFX4, stage 1) -- .g3d files into SDRAM, the 640 x 480 frame at
// SDRAM address 0 (one pixel a word, {8'h00, R, G, B}), debug console on top.
//   SPI bytes -> async_fifo -> g3d_loader -> write arbiter -> SDRAM_ctrl WR port
//   events (boot, SDRAM, chunks, GO, errors) -> gfx_console -> gfx_frame
//   gfx_frame: the latest picture (PI chunk, read through the DMA port) or the
//   background, the console over it -> write arbiter
// After GO the file is read back through the DMA port and checked chunk by
// chunk. The console goes off CON_HOLD_S seconds after a checked file with a
// descriptor (SC) and comes back on for any new chunk or error, or while the
// scene program holds it on (CON_SHOW).
// Same name, parameters and ports as before: a drop-in replacement.
// ============================================================================
module GFX3D_TOP #(
    parameter CLK_FREQ_HZ       = 50_000_000,
    parameter FIFO_ADDR_BITS    = 8,
    parameter FIFO_AFULL_MARGIN = 128,
    parameter FIFO_AFULL_HYST   = 1,
    parameter FIFO_LATENCY      = 2,
    parameter CON_HOLD_S        = 3,
    parameter STATUS_HZ         = 10,      // status bar redraws per second
    parameter FRAME_HZ          = 60       // animation steps per second (the renderer, stage 3)
)(
    input  wire        clk,
    input  wire [7:0]  data_in,
    input  wire        data_wr,
    output wire        dreq,
    output wire        WR,
    output wire [22:0] WrAddr,
    output wire [31:0] WrData,
    input  wire        WrFull,
    input  wire        WrIdle,
    output wire        DmaReq,
    output wire [22:0] DmaAddr,
    output wire [8:0]  DmaLen,
    input  wire        DmaRdy,
    input  wire [31:0] DmaData,
    input  wire        DmaValid,
    input  wire [3:0]  CAL_LAT,
    input  wire [4:0]  POST_FAIL,
    input  wire        WR_LOST
);

// message ids and template start addresses (con_rom.py)
localparam [3:0] M_BANNER = 4'd0;
localparam [3:0] M_SDRAM_OK = 4'd1;
localparam [3:0] M_SDRAM_BAD = 4'd2;
localparam [3:0] M_WAIT = 4'd3;
localparam [3:0] M_CHUNK_OK = 4'd4;
localparam [3:0] M_CHUNK_BAD = 4'd5;
localparam [3:0] M_GO = 4'd6;
localparam [3:0] M_CPU_HALT = 4'd7;
localparam [3:0] M_STATUS = 4'd8;
localparam [3:0] M_WRLOST = 4'd9;
localparam [3:0] M_RB_OK = 4'd10;
localparam [3:0] M_RB_BAD = 4'd11;
localparam [3:0] M_RB_DONE = 4'd12;
localparam [3:0] M_RUN = 4'd13;
localparam [3:0] M_NORUN = 4'd14;
localparam [3:0] M_CPU = 4'd15;

function [9:0] msg_start;
    input [3:0] id;
    case (id)
    4'd0:    msg_start = 10'd0;
    4'd1:    msg_start = 10'd83;
    4'd2:    msg_start = 10'd167;
    4'd3:    msg_start = 10'd235;
    4'd4:    msg_start = 10'd297;
    4'd5:    msg_start = 10'd336;
    4'd6:    msg_start = 10'd379;
    4'd7:    msg_start = 10'd427;
    4'd8:    msg_start = 10'd486;
    4'd9:    msg_start = 10'd557;
    4'd10:    msg_start = 10'd634;
    4'd11:    msg_start = 10'd672;
    4'd12:    msg_start = 10'd710;
    4'd13:    msg_start = 10'd773;
    4'd14:    msg_start = 10'd803;
    4'd15:    msg_start = 10'd848;
    default: msg_start = 10'd0;
    endcase
endfunction

reg    run3d = 1'b0;                       // file loaded and checked, with a descriptor

// ---------------------------------------------------------------- reset, uptime
reg [31:0] rst_cnt = 32'd0;
reg        rst     = 1'b1;
reg [31:0] sec_cnt = 32'd0;
reg [31:0] uptime  = 32'd0;

always @(posedge clk) begin
    if (rst_cnt < CLK_FREQ_HZ / 10) rst_cnt <= rst_cnt + 1'b1;
    rst <= (rst_cnt < CLK_FREQ_HZ / 10);
    if (sec_cnt == CLK_FREQ_HZ - 1) begin sec_cnt <= 32'd0; uptime <= uptime + 1'b1; end
    else sec_cnt <= sec_cnt + 1'b1;
end

// ---------------------------------------------------------------- input FIFO
wire [7:0]              fifo_out;
wire                    fifo_empty, fifo_rd_afull, fifo_read_req;
wire [FIFO_ADDR_BITS:0] fifo_level;

assign dreq = ~fifo_rd_afull;

async_fifo #(
    .WIDTH           (8),
    .ADDR_BITS       (FIFO_ADDR_BITS),
    .AFULL_MARGIN    (FIFO_AFULL_MARGIN),
    .AFULL_HYSTERESIS(FIFO_AFULL_HYST)
) fifo_u (
    .wr_clk  (data_wr),
    .wr_rst  (rst),
    .wr_en   (1'b1),
    .din     (data_in),
    .full    (),
    .rd_clk  (clk),
    .rd_rst  (rst),
    .rd_en   (fifo_read_req),
    .dout    (fifo_out),
    .empty   (fifo_empty),
    .level   (),
    .rd_level(fifo_level),
    .rd_afull(fifo_rd_afull)
);

// ---------------------------------------------------------------- loader
reg         sd_ready = 1'b0;               // SDRAM calibrated and tested
wire        ld_wreq, ld_wack, go, ev_head, ev_chunk, ev_ok;
wire [22:0] ld_addr, go_addr, ev_addr;
wire [31:0] ld_data, n_rx, ev_len, ev_sum, ev_exp;
wire [15:0] n_ok, n_bad, ev_type;

g3d_loader #(.FIFO_LATENCY(FIFO_LATENCY), .AW(23)) ld_u (
    .clk(clk), .rst(rst | ~sd_ready),
    .fifo_dout(fifo_out), .fifo_empty(fifo_empty), .fifo_rd(fifo_read_req),
    .wreq(ld_wreq), .wr_addr(ld_addr), .wr_data(ld_data), .wack(ld_wack), .wr_idle(WrIdle),
    .go(go), .go_addr(go_addr), .n_ok(n_ok), .n_bad(n_bad), .n_rx(n_rx),
    .ev_head(ev_head), .ev_chunk(ev_chunk), .ev_ok(ev_ok), .ev_type(ev_type), .ev_addr(ev_addr),
    .ev_len(ev_len), .ev_sum(ev_sum), .ev_exp(ev_exp));

// ---------------------------------------------------------------- SDRAM write port: loader first, then frames
wire        w_req, w_ack;
wire [18:0] w_addr;
wire [23:0] w_data;

wire        c_wreq;                                              // the CPU's DMA writes
wire [22:0] c_waddr;
wire [31:0] c_wdata;
// Port b: the frame writer and the CPU take turns of up to 64 words; a turn also ends after 8
// clocks with nothing to write. Word-by-word turns would change SDRAM rows at almost every word.
reg         wb_cpu = 1'b0;
reg  [5:0]  wb_n = 6'd0;                                         // words in this turn
reg  [3:0]  wb_idle = 4'd0;                                      // clocks without a request
wire        b_req = wb_cpu ? c_wreq : w_req;
wire        b_other = wb_cpu ? w_req : c_wreq;
wire        b_ack;

always @(posedge clk)
    if (!b_req && b_other && (wb_n == 6'd63 || wb_idle == 4'd7)) begin
        wb_cpu <= !wb_cpu; wb_n <= 6'd0; wb_idle <= 4'd0;
    end else begin
        if (b_ack && wb_n != 6'd63) wb_n <= wb_n + 1'b1;
        wb_idle <= b_req ? 4'd0 : wb_idle + (wb_idle != 4'd7);
    end

assign w_ack = b_ack && !wb_cpu;

gfx_wr_arb #(.AW(23)) arb_u (
    .clk(clk),
    .a_req(ld_wreq), .a_addr(ld_addr), .a_data(ld_data), .a_ack(ld_wack),
    .b_req(b_req), .b_addr(wb_cpu ? c_waddr : {4'd0, w_addr}), .b_data(wb_cpu ? c_wdata : {8'd0, w_data}),
    .b_ack(b_ack),
    .WR(WR), .WrAddr(WrAddr), .WrData(WrData), .WrFull(WrFull));

// ---------------------------------------------------------------- file read-back through the DMA port
wire         sd_ev_v, sd_busy, sd_o_v, sd_done, sd_ok;
wire         sd_want, sd_req, sd_rdy, sd_valid;
wire [22:0]  sd_addr;
wire [8:0]   sd_len;
wire [131:0] sd_ev;
reg          sd_take = 1'b0;
wire [15:0]  sd_o_type;
wire [23:0]  sd_o_idx;
wire [31:0]  sd_o_data;

gfx_scene_dma #(.AW(23), .CLK_MHZ((CLK_FREQ_HZ < 2_000_000) ? 1 : CLK_FREQ_HZ / 1_000_000)) sd_u (
    .clk(clk), .rst(rst),
    .ch_v(ev_chunk & ev_ok), .ch_type(ev_type), .ch_addr(ev_addr), .ch_len(ev_len), .ch_sum(ev_sum),
    .go(go),
    .dma_want(sd_want), .dma_req(sd_req), .dma_addr(sd_addr), .dma_len(sd_len), .dma_rdy(sd_rdy),
    .dma_data(DmaData), .dma_valid(sd_valid),
    .o_v(sd_o_v), .o_type(sd_o_type), .o_idx(sd_o_idx), .o_data(sd_o_data),
    .done(sd_done), .ok(sd_ok),
    .ev_v(sd_ev_v), .ev(sd_ev), .ev_take(sd_take), .busy(sd_busy));

always @(posedge clk)                                            // stop as a new file starts, run once checked
    if (rst || ev_head) run3d <= 1'b0;
    else if (sd_done) run3d <= sd_ok;

// ---------------------------------------------------------------- scene CPU: the program from a PG chunk, run once the file is checked
localparam [22:0] PG_BASE = 23'h1F8000;                         // 16K words: the CPU's RAM image
localparam [15:0] T_PG = {8'h50, 8'h47};

wire        c_want, c_req, c_rdy, c_valid, c_line_v, c_line_b, c_halt, c_pic_set, tx_done;
wire [22:0] c_addr, c_pic_addr;
wire [8:0]  c_len;
wire [31:0] c_halt_pc, c_halt_ir, c_instret;
wire [7:0]  tx_a, tx_q;
wire        pg_we = ld_wack && (ld_addr[22:14] == PG_BASE[22:14]);
reg         pg_seen = 1'b0, cpu_prog = 1'b0, c_line_take = 1'b0;
wire        cpu_run = run3d && cpu_prog;

always @(posedge clk)                                            // this file brought a program
    if (rst) begin
        pg_seen <= 1'b0; cpu_prog <= 1'b0;
    end else begin
        if (ev_chunk && ev_ok && ev_type == T_PG) pg_seen <= 1'b1;
        if (go) begin cpu_prog <= pg_seen; pg_seen <= 1'b0; end
    end

// ---------------------------------------------------------------- the renderer (gfx_gpu, at the end)
wire        g_want, g_req, g_rdy, g_valid, g_busy, band_rd, band_buf, c_gpu_go, c_tex_we;
wire [1:0]  c_gpu_mode;
wire [8:0]  c_tex_a;
wire [31:0] c_tex_d;
wire [22:0] g_addr, c_gpu_table;
wire [8:0]  g_len;
wire [1:0]  band_ready, band_free;
wire [13:0] band_a;
wire [23:0] band_q, c_gpu_bg;
wire        band_cov;
wire [2:0]  c_probe_sel;                                         // the renderer's setup probe, read by the CPU
wire [31:0] g_probe;
wire        c_con_show;
reg  [23:0] gpu_bg_d = 24'd0;
always @(posedge clk) gpu_bg_d <= c_gpu_bg;
reg         gpu_on = 1'b0;                                       // frames come from the renderer

wire        f_want, f_req, f_rdy, f_valid;
wire [22:0] f_addr;
wire [8:0]  f_len;

gfx_dma_arb dma_u (                                              // picture lines, CPU, read-back, renderer
    .clk(clk), .rst(rst),
    .w0(f_want), .r0(f_req), .a0(f_addr), .n0(f_len), .y0(f_rdy), .v0(f_valid),
    .w1(c_want), .r1(c_req), .a1(c_addr), .n1(c_len), .y1(c_rdy), .v1(c_valid),
    .w2(sd_want), .r2(sd_req), .a2(sd_addr), .n2(sd_len), .y2(sd_rdy), .v2(sd_valid),
    .w3(g_want), .r3(g_req), .a3(g_addr), .n3(g_len), .y3(g_rdy), .v3(g_valid),
    .DmaReq(DmaReq), .DmaAddr(DmaAddr), .DmaLen(DmaLen), .DmaRdy(DmaRdy), .DmaValid(DmaValid));

// ---------------------------------------------------------------- the picture: the latest PI chunk once written, or the CPU's
localparam [15:0] T_PI = {8'h50, 8'h49};

reg         pic_on   = 1'b0;
reg         pic_pend = 1'b0;
reg         pic_new  = 1'b0;
reg  [22:0] pic_addr = 23'd0, pic_next = 23'd0;

always @(posedge clk) begin
    pic_new <= 1'b0;
    if (rst) begin
        pic_on <= 1'b0; pic_pend <= 1'b0;
    end else if (ev_chunk && ev_ok && ev_type == T_PI) begin
        pic_next <= ev_addr; pic_pend <= 1'b1;
    end else if (pic_pend && WrIdle) begin
        pic_addr <= pic_next; pic_on <= 1'b1; pic_pend <= 1'b0; pic_new <= 1'b1;
    end else if (c_pic_set) begin                                // the CPU shows its own picture
        pic_addr <= c_pic_addr; pic_on <= 1'b1; pic_new <= 1'b1;
    end
end

// ---------------------------------------------------------------- events: one pending slot per source
// slot 0 boot messages, 1 chunk result, 2 GO, 3 write lost, 4 CPU text, 5 CPU halt, then the read-back
reg  [5:0]   pend = 6'd0;
reg  [131:0] slot0 = 0, slot1 = 0, slot2 = 0, slot3 = 0, slot4 = 0, slot5 = 0;
reg          halt_d = 1'b0, halt_d2 = 1'b0;
reg  [1:0]   boot = 2'd0;
reg          lost_d = 1'b0;
reg          ev_v = 1'b0;
reg  [131:0] ev = 132'd0;
wire         ev_full;

always @(posedge clk) begin
    ev_v    <= 1'b0;
    sd_take <= 1'b0;
    c_line_take <= 1'b0;
    lost_d  <= WR_LOST;
    halt_d  <= c_halt;
    halt_d2 <= halt_d;
    if (rst) begin
        pend <= 6'd0; boot <= 2'd0; sd_ready <= 1'b0;
    end else begin
        if (!pend[0]) case (boot)                                // banner, SDRAM result, waiting
        2'd0: begin slot0 <= {M_BANNER, 128'd0}; pend[0] <= 1'b1; boot <= 2'd1; end
        2'd1: if (DmaRdy) begin
            slot0 <= (POST_FAIL == 5'd0) ? {M_SDRAM_OK, 28'd0, CAL_LAT, 96'd0}
                                         : {M_SDRAM_BAD, 27'd0, POST_FAIL, 28'd0, CAL_LAT, 64'd0};
            pend[0] <= 1'b1; boot <= 2'd2;
        end
        2'd2: begin slot0 <= {M_WAIT, 128'd0}; pend[0] <= 1'b1; sd_ready <= 1'b1; boot <= 2'd3; end
        default: ;
        endcase
        if (ev_chunk) begin
            slot1 <= {ev_ok ? M_CHUNK_OK : M_CHUNK_BAD, n_ok + n_bad, ev_type, 9'd0, ev_addr,
                      ev_ok ? ev_len : ev_sum, ev_ok ? ev_sum : ev_exp};
            pend[1] <= 1'b1;
        end
        if (go) begin
            slot2 <= {M_GO, 9'd0, go_addr, n_rx, 16'd0, n_ok, 16'd0, n_bad};
            pend[2] <= 1'b1;
        end
        if (WR_LOST && !lost_d) begin slot3 <= {M_WRLOST, 128'd0}; pend[3] <= 1'b1; end
        if (!pend[4] && c_line_v && !c_line_take) begin
            slot4 <= {M_CPU, 31'd0, c_line_b, 96'd0}; pend[4] <= 1'b1; c_line_take <= 1'b1;
        end
        if (halt_d && !halt_d2) begin                            // halt_ir is caught a clock after halt
            slot5 <= {M_CPU_HALT, c_halt_pc, c_halt_ir, c_instret, 32'd0}; pend[5] <= 1'b1;
        end
        if (!ev_full && !ev_v) begin                             // one event a clock into the console
            if      (pend[0]) begin ev <= slot0; ev_v <= 1'b1; pend[0] <= 1'b0; end
            else if (pend[1]) begin ev <= slot1; ev_v <= 1'b1; pend[1] <= 1'b0; end
            else if (pend[2]) begin ev <= slot2; ev_v <= 1'b1; pend[2] <= 1'b0; end
            else if (pend[3]) begin ev <= slot3; ev_v <= 1'b1; pend[3] <= 1'b0; end
            else if (pend[4]) begin ev <= slot4; ev_v <= 1'b1; pend[4] <= 1'b0; end
            else if (pend[5]) begin ev <= slot5; ev_v <= 1'b1; pend[5] <= 1'b0; end
            else if (sd_ev_v && !sd_take) begin ev <= sd_ev; ev_v <= 1'b1; sd_take <= 1'b1; end
        end
    end
end

// ---------------------------------------------------------------- console on/off
localparam [31:0] HOLD = CLK_FREQ_HZ * CON_HOLD_S;

reg        con_on = 1'b1;
reg [31:0] hold   = 32'd0;

always @(posedge clk)
    if (!run3d || ev_chunk || (WR_LOST && !lost_d) || (halt_d && !halt_d2) || c_con_show) begin
        con_on <= 1'b1; hold <= HOLD;
    end else if (hold != 32'd0) hold <= hold - 1'b1;
    else con_on <= 1'b0;

// ---------------------------------------------------------------- console and frames
wire [4:0]  cr_row;
wire [6:0]  cr_col;
wire [9:0]  cr_q;
wire        f_end;
reg  [31:0] fps = 32'd0, fcount = 32'd0;

gfx_console #(.STATUS_PERIOD(CLK_FREQ_HZ / STATUS_HZ)) con_u (
    .clk(clk), .rst(rst),
    .ev_v(ev_v), .ev_id(ev[131:128]), .ev_a0(ev[127:96]), .ev_a1(ev[95:64]), .ev_a2(ev[63:32]),
    .ev_a3(ev[31:0]), .ev_full(ev_full),
    .s0(n_rx), .s1({16'd0, n_ok}), .s2({16'd0, n_bad}), .s3({31'd0, dreq}),
    .s4({{(31 - FIFO_ADDR_BITS){1'b0}}, fifo_level}), .s5({28'd0, CAL_LAT}), .s6(fps), .s7(uptime),
    .rd_row(cr_row), .rd_col(cr_col), .rd_q(cr_q), .tx_a(tx_a), .tx_q(tx_q), .tx_done(tx_done));

gfx_frame frm_u (
    .clk(clk), .rst(rst), .con_on(con_on), .bg(24'h000021),
    .pic_on(pic_on), .pic_addr(pic_addr), .pic_new(pic_new),
    .cr_row(cr_row), .cr_col(cr_col), .cr_q(cr_q),
    .dma_want(f_want), .dma_req(f_req), .dma_addr(f_addr), .dma_len(f_len), .dma_rdy(f_rdy),
    .dma_data(DmaData), .dma_valid(f_valid),
    .req(w_req), .addr(w_addr), .data(w_data), .ack(w_ack), .frame_end(f_end),
    .gpu_on(gpu_on), .band_ready(band_ready), .band_rd(band_rd), .band_buf(band_buf), .band_a(band_a),
    .band_q(band_q), .band_cov(band_cov), .bg_new(gpu_bg_d != c_gpu_bg), .band_free(band_free));

gfx_cpu cpu_u (
    .clk(clk), .run(cpu_run), .ld_we(pg_we), .ld_a(ld_addr[13:0]), .ld_d(ld_data),
    .halt(c_halt), .halt_pc(c_halt_pc), .halt_ir(c_halt_ir), .instret(c_instret), .frame_end(f_end),
    .line_v(c_line_v), .line_b(c_line_b), .line_take(c_line_take), .tx_a(tx_a), .tx_q(tx_q), .tx_done(tx_done),
    .pic_set(c_pic_set), .pic_addr(c_pic_addr),
    .dma_want(c_want), .dma_req(c_req), .dma_addr(c_addr), .dma_len(c_len), .dma_rdy(c_rdy),
    .dma_data(DmaData), .dma_valid(c_valid),
    .w_req(c_wreq), .w_addr(c_waddr), .w_data(c_wdata), .w_ack(b_ack && wb_cpu),
    .gpu_table(c_gpu_table), .gpu_go(c_gpu_go), .gpu_bg(c_gpu_bg), .gpu_mode(c_gpu_mode),
    .tex_we(c_tex_we), .tex_a(c_tex_a), .tex_d(c_tex_d), .gpu_busy(g_busy),
    .probe_sel(c_probe_sel), .probe_q(g_probe), .con_show(c_con_show));

always @(posedge clk)                                            // 3D until a picture, a halt or a new file
    if (rst || !cpu_run || c_pic_set || (halt_d && !halt_d2)) gpu_on <= 1'b0;
    else if (c_gpu_go) gpu_on <= 1'b1;

gfx_gpu gpu_u (
    .clk(clk), .rst(rst || !cpu_run), .rec12(c_gpu_mode[0]), .fog_on(c_gpu_mode[1]), .d_we(c_tex_we), .d_a(c_tex_a), .d_d(c_tex_d),
    .go(c_gpu_go), .table_addr(c_gpu_table), .bg(c_gpu_bg), .busy(g_busy),
    .wr_idle(WrIdle), .dma_want(g_want), .dma_req(g_req), .dma_addr(g_addr), .dma_len(g_len), .dma_rdy(g_rdy),
    .dma_data(DmaData), .dma_valid(g_valid), .ready(band_ready), .rd(band_rd), .rd_buf(band_buf), .rd_a(band_a),
    .rd_q(band_q), .rd_cov(band_cov), .free(band_free), .pr_sel(c_probe_sel), .pr_q(g_probe));

always @(posedge clk)                                            // frames per second
    if (sec_cnt == CLK_FREQ_HZ - 1) begin fps <= fcount + f_end; fcount <= 32'd0; end
    else if (f_end) fcount <= fcount + 1'b1;

endmodule

`timescale 1ns/1ps
`default_nettype none

// ============================================================================
// gfx_frame -- writes the 640 x 480 frame buffer at SDRAM address 0, one pixel
// a word ({8'h00, R, G, B}): the renderer's bands (gpu_on), the picture at
// pic_addr (read a line ahead through the DMA port, 128-word bursts) or the
// background, with the console on top while con_on: text in its colour, the
// rest at half brightness.
// Renderer frames are drawn band by band as they are finished; a band's pixels
// are cleared as they are read and its buffer freed after its last line.
// Otherwise frames are drawn while con_on, then once more after it goes off,
// the picture changes or the renderer stops; then it waits.
// ============================================================================
module gfx_frame (
    input  wire        clk,
    input  wire        rst,
    input  wire        con_on,
    input  wire [23:0] bg,
    input  wire        pic_on,                // show the picture at pic_addr
    input  wire [22:0] pic_addr,
    input  wire        pic_new,               // the picture changed: draw again
    output reg  [4:0]  cr_row = 5'd0,         // console read port
    output reg  [6:0]  cr_col = 7'd0,
    input  wire [9:0]  cr_q,
    output wire        dma_want,              // picture lines
    output reg         dma_req  = 1'b0,
    output reg  [22:0] dma_addr = 23'd0,
    output wire [8:0]  dma_len,
    input  wire        dma_rdy,
    input  wire [31:0] dma_data,
    input  wire        dma_valid,
    output reg         req  = 1'b0,           // frame buffer words (write arbiter)
    output reg  [18:0] addr = 19'd0,
    output reg  [23:0] data = 24'd0,
    input  wire        ack,
    output reg         frame_end = 1'b0,
    input  wire        gpu_on,                // frames come from the renderer
    input  wire [1:0]  band_ready,            // band buffer holds the next band
    output wire        band_rd,               // read a band pixel (and clear it)
    output wire        band_buf,
    output reg  [13:0] band_a = 14'd0,
    input  wire [23:0] band_q,                // a clock after band_rd
    input  wire        band_cov,              // with band_q: the pixel was drawn
    input  wire        bg_new,                // the 3D background colour changed
    output reg  [1:0]  band_free = 2'b00      // done with that buffer
);

// ---------------------------------------------------------------- frame control
reg         need = 1'b1;                      // a frame must be drawn
reg         con_d = 1'b0;
reg         busy = 1'b0;                      // drawing a frame
reg         f_pic = 1'b0;                     // this frame shows the picture
reg         f_gpu = 1'b0;                     // this frame comes from the renderer
reg         gpu_d = 1'b0;
reg  [22:0] f_base = 23'd0;

// ---------------------------------------------------------------- line buffer: two lines
reg  [23:0] lb [0:2047];                      // {line[0], x}
reg  [10:0] lb_ra = 11'd0;
reg  [23:0] lb_q = 24'd0;
reg         lb_we = 1'b0;
reg  [10:0] lb_wa = 11'd0;
reg  [23:0] lb_wd = 24'd0;

always @(posedge clk) begin
    if (lb_we) lb[lb_wa] <= lb_wd;
    lb_q <= lb[lb_ra];
end

// ---------------------------------------------------------------- fetch: picture lines, one ahead of the writer
reg  [8:0]  fy = 9'd0;                        // lines complete
reg  [9:0]  fk = 10'd0;                       // words of line fy received
reg  [2:0]  fb = 3'd0;                        // bursts of line fy requested
reg         fwant = 1'b0, fpend = 1'b0;       // a burst wanted, in flight
reg  [22:0] frow = 23'd0;                     // address of line fy
reg  [8:0]  wy = 9'd0;                        // line being written

assign dma_want = fwant;
assign dma_len  = 9'd128;

always @(posedge clk) begin
    dma_req <= 1'b0;
    lb_we   <= 1'b0;
    if (rst || !busy) begin
        fy <= 9'd0; fk <= 10'd0; fb <= 3'd0; fwant <= 1'b0; fpend <= 1'b0; frow <= pic_addr;
    end else if (f_pic) begin
        if (!fwant && !fpend && fb != 3'd5 && fy != 9'd480 && fy <= wy + 1'b1) fwant <= 1'b1;
        if (fwant && dma_rdy) begin
            dma_req <= 1'b1; dma_addr <= frow + {fb, 7'd0};
            fwant <= 1'b0; fpend <= 1'b1; fb <= fb + 1'b1;
        end
        if (dma_valid) begin
            lb_we <= 1'b1; lb_wa <= {fy[0], fk}; lb_wd <= dma_data[23:0];
            if (fk[6:0] == 7'd127) fpend <= 1'b0;
            if (fk == 10'd639) begin
                fk <= 10'd0; fb <= 3'd0; fy <= fy + 1'b1; frow <= frow + 23'd640;
            end else
                fk <= fk + 1'b1;
        end
    end
end

// ---------------------------------------------------------------- writer: console lookups every 8 pixels
localparam [2:0] W_IDLE = 3'd0, W_WAIT = 3'd1, W_CELL = 3'd2, W_FONT = 3'd3, W_GLYPH = 3'd4,
                 W_RD = 3'd5, W_WR = 3'd6, W_ACK = 3'd7;
reg  [2:0]  ws = W_IDLE;
reg  [9:0]  wx = 10'd0;
reg  [1:0]  fw = 2'd0;                        // font read wait
reg  [10:0] font_a = 11'd0;
reg  [2:0]  gcol = 3'd0;
reg  [7:0]  glyph = 8'd0;
wire [7:0]  font_q;

gfx_font_rom font_u (.clk(clk), .ra(font_a), .rd(font_q));

function [23:0] fg;                           // text colours (the old RGB565 values, expanded)
    input [2:0] c;
    reg   [15:0] p;
    begin
        case (c)
        3'd2:    p = 16'h4FE9;                // green
        3'd3:    p = 16'hFA69;                // red
        3'd4:    p = 16'hFF26;                // yellow
        3'd5:    p = 16'h5E7F;                // cyan
        default: p = 16'hEF7D;                // white
        endcase
        fg = {p[15:11], p[15:13], p[10:5], p[10:9], p[4:0], p[4:2]};
    end
endfunction

// 3D frames write only what changed: a pixel that is background now and was background in the
// last frame is already in the frame buffer. Full frames after the console, a background colour
// change, or a frame from elsewhere, and every 64th.
reg         cov [0:307199];                   // drawn in the last 3D frame
reg  [18:0] cov_ra = 19'd0;
reg         cov_q = 1'b0;
reg         full = 1'b1;                      // the next 3D frame writes every pixel
reg         f_skip = 1'b0;                    // this frame may skip
reg  [5:0]  gcnt = 6'd0;
wire        skip = f_gpu && f_skip && !con_on && !band_cov && !cov_q;

always @(posedge clk) begin
    cov_q <= cov[cov_ra];
    if (ws == W_ACK && !req && f_gpu) cov[addr] <= band_cov;
end

wire [23:0] base = f_gpu ? band_q : f_pic ? lb_q : bg;
assign band_rd  = f_gpu && (ws == W_WR);
assign band_buf = wy[4];                      // band wy / 16 is in buffer (wy / 16) & 1
wire        tbit = con_on & glyph[~wx[2:0]];  // bit 7 is the leftmost pixel
wire [23:0] pix  = tbit ? fg(gcol) : con_on ? {1'b0, base[23:17], 1'b0, base[15:9], 1'b0, base[7:1]} : base;

always @(posedge clk) begin
    frame_end <= 1'b0;
    band_free <= 2'b00;
    con_d <= con_on;
    gpu_d <= gpu_on;
    if (pic_new || (con_d && !con_on) || (gpu_d && !gpu_on)) need <= 1'b1;   // draw once more
    if (rst) begin
        ws <= W_IDLE; req <= 1'b0; busy <= 1'b0; need <= 1'b1;
    end else case (ws)
    W_IDLE: if (gpu_on ? band_ready[0] : (need || con_on)) begin
        need <= 1'b0; busy <= 1'b1; f_gpu <= gpu_on; f_pic <= pic_on && !gpu_on; f_base <= pic_addr;
        f_skip <= gpu_on && !full && (gcnt != 6'd0);
        if (gpu_on) begin full <= 1'b0; gcnt <= gcnt + 1'b1; end
        wy <= 9'd0; wx <= 10'd0; addr <= 19'd0;
        ws <= W_WAIT;
    end
    W_WAIT: if (f_gpu && !gpu_on) begin                          // the renderer stopped
                busy <= 1'b0; ws <= W_IDLE;
            end else if (f_gpu ? band_ready[wy[4]] : (!f_pic || fy > wy)) begin
                if (con_on) ws <= W_CELL;                        // the line is there
                else begin glyph <= 8'd0; ws <= W_RD; end        // no console: no glyphs
            end
    W_CELL: begin cr_row <= wy[8:4]; cr_col <= wx[9:3]; fw <= 2'd1; ws <= W_FONT; end
    W_FONT: if (fw != 2'd0) fw <= fw - 1'b1;                     // console read
            else begin font_a <= {cr_q[6:0], wy[3:0]}; gcol <= cr_q[9:7]; fw <= 2'd1; ws <= W_GLYPH; end
    W_GLYPH: if (fw != 2'd0) fw <= fw - 1'b1;                    // font read
             else begin glyph <= font_q; ws <= W_RD; end
    W_RD:   begin
                lb_ra  <= {wy[0], wx};
                cov_ra <= addr;
                band_a <= {wy[3:0], 9'd0} + {wy[3:0], 7'd0} + {4'd0, wx};
                ws <= W_WR;
            end
    W_WR:   ws <= W_ACK;                                         // read: lb_q, band_q next
    W_ACK:  if (!req && !skip) begin
                req <= 1'b1; data <= pix;
            end else if (ack || !req) begin                      // written, or skipped
                req <= 1'b0; addr <= addr + 1'b1;
                if (wx == 10'd639) begin
                    wx <= 10'd0;
                    if (f_gpu && wy[3:0] == 4'd15) band_free[wy[4]] <= 1'b1;
                    if (wy == 9'd479) begin busy <= 1'b0; frame_end <= 1'b1; ws <= W_IDLE; end
                    else begin wy <= wy + 1'b1; ws <= W_WAIT; end
                end else begin
                    wx <= wx + 1'b1;
                    if (wx[2:0] != 3'd7) ws <= W_RD;
                    else if (con_on) ws <= W_CELL;
                    else begin glyph <= 8'd0; ws <= W_RD; end
                end
            end
    endcase
    if (con_on) f_skip <= 1'b0;                                  // the console came on
    if (bg_new || con_d != con_on || (gpu_on && !gpu_d) || !f_gpu && busy) full <= 1'b1;
end

endmodule

`timescale 1ns/1ps
`default_nettype none

// ============================================================================
// gfx_cpu -- the scene CPU: rv32_cpu, 64 KB of RAM and its devices.
// The program is written into the RAM while the CPU is held (ld_*), then run
// starts it at address 0. RAM: 0x00000000-0x0000FFFF. Devices, word registers
// at 0x10000000 +:
//   0x04 PUTC     w: a character for the console; '\n' or the 80th ends the line
//                 r: bit 0 set when a character can be written
//   0x08 CYCLES   r: clocks since the CPU started
//   0x0C FRAMES   r: frames finished by the frame writer
//   0x10 RANDOM   r: a new 32-bit random number each read
//   0x14 PICTURE  w: show the picture at this SDRAM word address
//   0x20 DMA_SD   SDRAM word address
//   0x24 DMA_RAM  RAM byte address (word aligned)
//   0x28 DMA_LEN  words, 1..8192
//   0x2C DMA_GO   w: starts a transfer, bit 0: 0 SDRAM to RAM ("in"), 1 RAM to SDRAM ("out");
//                 the two run apart, each takes the three registers above as it starts (it must
//                 be idle). r: bit 0 set while either is busy, bit 1 in, bit 2 out; bit 31 set
//                 (two channels: older ones read 0 there)
//   0x40 GPU_TABLE  SDRAM word address of the band table (30 x {list address, triangles})
//   0x44 GPU_GO     w: render a frame; r: bit 0 set while the lists are being drawn
//   0x48 GPU_BG     background colour
//   0x4C GPU_MODE   bit 0: lists of 12-word triangles (textures); bit 1: haze; r: bits 31:16 0x3541
//                   ("5A": modes, alpha textures and haze are here)
//   0x50 FOG_COLOUR the haze's colour
//   0x54 FOG_PTR    w: the haze table's word to be written next (0..255)
//   0x58 FOG_DATA   w: four table bytes there (the first the lowest); the pointer steps on
//   0x200-0x3FC     texture table: 0x200 + 8 slot the base, + 4 {alpha, levels, lh, lw} (gfx_tex)
//   0x80-0xD0       vertex engine (gfx_vtx): 0x80 + 4i register i; 0xD0 r: bit 0 set while busy.
//                   Register 20 (0xD0, the count) starts it; written while DMA in is busy, the
//                   start waits for that transfer to end (no engine register may be written meanwhile)
//   0xD4            r: "VTX1" (0x56545831), the vertex engine is here
//   0xD8 BUILD      r: 0x3541nnnn, this GFX3D_TOP's stage 5a build (older ones read 0)
//   0x100 PROBE_SEL w: which word of the renderer's setup probe (gfx_raster: the record last set up)
//   0x104 PROBE     r: that word
//   0x108 CON_SHOW  w: bit 0 keeps the console on
//   0x400-0x7FC     vertex engine materials
// On the RAM's data port DMA in goes first, then DMA out, then the vertex engine; the CPU's loads
// and stores wait.
// ============================================================================
module gfx_cpu (
    input  wire        clk,
    input  wire        run,                   // CPU out of reset
    input  wire        ld_we,                 // program words, while not running
    input  wire [13:0] ld_a,
    input  wire [31:0] ld_d,
    output wire        halt,
    output wire [31:0] halt_pc,
    output reg  [31:0] halt_ir = 32'd0,
    output wire [31:0] instret,
    input  wire        frame_end,
    output wire        line_v,                // a text line waits: buffer line_b
    output wire        line_b,
    input  wire        line_take,             // the line's event is taken
    input  wire [7:0]  tx_a,                  // the console reads the line: {buffer, character}
    output reg  [7:0]  tx_q = 8'd0,
    input  wire        tx_done,
    output reg         pic_set = 1'b0,
    output reg  [22:0] pic_addr = 23'd0,
    output wire        dma_want,              // SDRAM reads, 256-word bursts
    output reg         dma_req = 1'b0,
    output reg  [22:0] dma_addr = 23'd0,
    output reg  [8:0]  dma_len = 9'd0,
    input  wire        dma_rdy,
    input  wire [31:0] dma_data,
    input  wire        dma_valid,
    output reg         w_req = 1'b0,          // SDRAM writes
    output reg  [22:0] w_addr = 23'd0,
    output reg  [31:0] w_data = 32'd0,
    input  wire        w_ack,
    output reg  [22:0] gpu_table = 23'd0,     // the renderer
    output reg         gpu_go = 1'b0,
    output reg  [23:0] gpu_bg = 24'd0,
    output reg  [1:0]  gpu_mode = 2'b00,      // {haze, 12-word triangles}
    output wire        tex_we,                // the pixel unit's registers: texture table, haze
    output wire [8:0]  tex_a,
    output wire [31:0] tex_d,
    input  wire        gpu_busy,
    output reg  [2:0]  probe_sel = 3'd0,      // the renderer's setup probe
    input  wire [31:0] probe_q,
    output reg         con_show = 1'b0        // keep the console on
);

localparam [31:0] BUILD = 32'h3541_0004;     // "5A", build 4

// ---------------------------------------------------------------- the CPU
wire [31:0] i_addr, c_addr, c_wdata;
wire [3:0]  c_be;
wire        c_rd, c_wr, d_busy;
wire [31:0] c_rdata;
wire [31:0] ra_q;

rv32_cpu cpu_u (
    .clk(clk), .rst(!run), .i_addr(i_addr), .i_data(ra_q),
    .d_rd(c_rd), .d_wr(c_wr), .d_addr(c_addr), .d_wdata(c_wdata), .d_be(c_be), .d_rdata(c_rdata),
    .d_busy(d_busy), .halt(halt), .halt_pc(halt_pc), .retired(instret));
always @(posedge clk) if (halt) halt_ir <= ra_q;             // the halted instruction, fetched again

wire dev   = (c_addr[31:28] == 4'h1);
wire dev_w = c_wr && dev;

// ---------------------------------------------------------------- DMA state: the next transfer, channels in and out
reg  [22:0] st_sd = 23'd0;                   // DMA_SD, DMA_RAM, DMA_LEN: taken by a channel as it starts
reg  [13:0] st_ra = 14'd0, st_n = 14'd0;
reg         di_busy = 1'b0;                  // in
reg  [22:0] di_sd = 23'd0;
reg  [13:0] di_ra = 14'd0;
reg  [13:0] di_n = 14'd0;                    // words to request
reg  [13:0] di_left = 14'd0;                 // words still to arrive
reg         di_want = 1'b0, di_fly = 1'b0;
reg  [8:0]  di_bn = 9'd0;                    // words of the burst in flight
reg         dv_v = 1'b0;                     // an arrived word, into the RAM this clock
reg  [31:0] dv_d = 32'd0;
reg         do_busy = 1'b0;                  // out
reg  [22:0] do_sd = 23'd0;
reg  [13:0] do_ra = 14'd0;
reg  [13:0] do_n = 14'd0;                    // words to move
reg  [1:0]  dw_st = 2'd0;                    // 0 read, 1 offer, 2 wait for the write

wire dm_rd = do_busy && dw_st == 2'd0 && do_n != 14'd0 && !dv_v;   // out's RAM read, when in's word is not written

// vertex engine: its RAM accesses after DMA's, before the CPU's; a start held while DMA in is busy
wire        ve_req, ve_we, ve_busy;
wire [13:0] ve_a;
wire [31:0] ve_d;
wire        ve_gnt = ve_req && !(!run && ld_we) && !dv_v && !dm_rd;
reg         ve_hold = 1'b0;                  // register 20 written while DMA in was busy
reg  [31:0] ve_hd = 32'd0;
assign d_busy   = dv_v || dm_rd || ve_req;
assign dma_want = di_want;

// ---------------------------------------------------------------- RAM: four byte lanes; port A fetch, port B data
wire        b_ld   = !run && ld_we;
wire [13:0] b_addr = b_ld ? ld_a : dv_v ? di_ra : dm_rd ? do_ra : ve_gnt ? ve_a : c_addr[15:2];
wire [3:0]  b_we   = (b_ld || dv_v) ? 4'hF : ve_gnt ? {4{ve_we}} : (c_wr && !dev && !d_busy) ? c_be : 4'h0;
wire [31:0] b_d    = b_ld ? ld_d : dv_v ? dv_d : ve_gnt ? ve_d : c_wdata;
wire [31:0] rb_q;

genvar g;
generate for (g = 0; g < 4; g = g + 1) begin : lane
    reg [7:0] m [0:16383];
    reg [7:0] qa = 8'd0, qb = 8'd0;
    always @(posedge clk) qa <= m[i_addr[15:2]];
    always @(posedge clk) begin
        if (b_we[g]) m[b_addr] <= b_d[8 * g +: 8];
        qb <= m[b_addr];
    end
    assign ra_q[8 * g +: 8] = qa;
    assign rb_q[8 * g +: 8] = qb;
end endgenerate

// ---------------------------------------------------------------- text lines: two buffers of 80 characters
reg  [7:0]  lb [0:255];                      // {buffer, character}
reg         cur = 1'b0;                      // buffer being filled
reg  [6:0]  cnt = 7'd0;
reg  [1:0]  full = 2'b00;                    // written, until the console has shown it
reg  [1:0]  wait_ev = 2'b00;                 // its event not yet taken
reg         lb_we = 1'b0;
reg  [7:0]  lb_wa = 8'd0, lb_wd = 8'd0;

assign line_v = wait_ev[0] || wait_ev[1];
assign line_b = (wait_ev == 2'b11) ? cur : wait_ev[1];   // both: cur has come back to the older

always @(posedge clk) begin
    if (lb_we) lb[lb_wa] <= lb_wd;
    tx_q <= lb[tx_a];
end

// ---------------------------------------------------------------- devices
reg  [31:0] cycles = 32'd0, frames = 32'd0, rnd = 32'h2545F491;
wire [31:0] x1 = rnd ^ (rnd << 13), x2 = x1 ^ (x1 >> 17), x3 = x2 ^ (x2 << 5);   // xorshift32
reg  [31:0] dev_q = 32'd0;
reg         dev_r = 1'b0;
assign c_rdata = dev_r ? dev_q : rb_q;

always @(posedge clk) begin
    lb_we   <= 1'b0;
    pic_set <= 1'b0;
    cycles  <= run ? cycles + 1'b1 : 32'd0;
    if (frame_end) frames <= frames + 1'b1;
    if (line_take) wait_ev[line_b] <= 1'b0;
    if (tx_done) full[tx_a[7]] <= 1'b0;

    dev_r <= dev;
    case (c_addr[10:2])
    9'h001:    dev_q <= {31'd0, !full[cur]};
    9'h002:    dev_q <= cycles;
    9'h003:    dev_q <= frames;
    9'h004:    dev_q <= rnd;
    9'h00B:    dev_q <= {1'b1, 28'd0, do_busy, di_busy, di_busy || do_busy};
    9'h011:    dev_q <= {31'd0, gpu_busy};
    9'h013:    dev_q <= {16'h3541, 14'd0, gpu_mode};     // "5A": the stage 5a renderer is here
    9'h034:    dev_q <= {31'd0, ve_busy || ve_hold};
    9'h035:    dev_q <= 32'h5654_5831;                       // "VTX1": the vertex engine is here
    9'h036:    dev_q <= BUILD;
    9'h041:    dev_q <= probe_q;
    default: dev_q <= 32'd0;
    endcase
    if (c_rd && dev && c_addr[10:2] == 9'h004) rnd <= x3;

    if (dev_w && c_addr[10:2] == 9'h001 && !full[cur]) begin    // PUTC
        lb_we <= 1'b1; lb_wa <= {cur, cnt};
        lb_wd <= (c_wdata[7:0] == 8'h0A) ? 8'd0 : c_wdata[7:0];
        if (c_wdata[7:0] == 8'h0A || cnt == 7'd79) begin
            full[cur] <= 1'b1; wait_ev[cur] <= 1'b1; cur <= !cur; cnt <= 7'd0;
        end else
            cnt <= cnt + 1'b1;
    end
    if (dev_w && c_addr[10:2] == 9'h005) begin pic_addr <= c_wdata[22:0]; pic_set <= 1'b1; end
    gpu_go <= dev_w && c_addr[10:2] == 9'h011;
    if (dev_w && c_addr[10:2] == 9'h010) gpu_table <= c_wdata[22:0];
    if (dev_w && c_addr[10:2] == 9'h012) gpu_bg <= c_wdata[23:0];
    if (dev_w && c_addr[10:2] == 9'h013) gpu_mode <= c_wdata[1:0];
    if (dev_w && c_addr[10:2] == 9'h040) probe_sel <= c_wdata[2:0];
    if (dev_w && c_addr[10:2] == 9'h042) con_show <= c_wdata[0];
    if (!run) begin gpu_mode <= 2'b00; con_show <= 1'b0; end
    if (!run) begin
        cur <= 1'b0; cnt <= 7'd0; full <= 2'b00; wait_ev <= 2'b00;
    end
end

// ---------------------------------------------------------------- DMA
always @(posedge clk) begin
    dma_req <= 1'b0;
    dv_v    <= 1'b0;
    if (dev_w) case (c_addr[10:2])
        9'h008: st_sd <= c_wdata[22:0];
        9'h009: st_ra <= c_wdata[15:2];
        9'h00A: st_n  <= c_wdata[13:0];
        default: ;
    endcase
    if (!run) begin
        di_busy <= 1'b0; di_want <= 1'b0; di_fly <= 1'b0; do_busy <= 1'b0; dw_st <= 2'd0; w_req <= 1'b0;
    end else begin
        if (dev_w && c_addr[10:2] == 9'h00B) begin              // DMA_GO: the channel named takes the transfer
            if (!c_wdata[0] && !di_busy) begin
                di_sd <= st_sd; di_ra <= st_ra; di_n <= st_n; di_left <= st_n; di_busy <= (st_n != 14'd0);
            end
            if (c_wdata[0] && !do_busy) begin
                do_sd <= st_sd; do_ra <= st_ra; do_n <= st_n; do_busy <= (st_n != 14'd0);
            end
        end
        if (di_busy) begin                                      // in: SDRAM to RAM, a burst at a time
            if (!di_want && !di_fly && di_n != 14'd0) di_want <= 1'b1;
            if (di_want && dma_rdy) begin
                dma_req <= 1'b1; dma_addr <= di_sd;
                dma_len <= (di_n > 14'd256) ? 9'd256 : di_n[8:0];
                di_bn   <= (di_n > 14'd256) ? 9'd256 : di_n[8:0];
                di_sd   <= di_sd + ((di_n > 14'd256) ? 23'd256 : {9'd0, di_n});
                di_n    <= (di_n > 14'd256) ? di_n - 14'd256 : 14'd0;
                di_want <= 1'b0; di_fly <= 1'b1;
            end
            if (dma_valid) begin
                dv_v <= 1'b1; dv_d <= dma_data;
                di_bn <= di_bn - 1'b1;
                if (di_bn == 9'd1) di_fly <= 1'b0;
            end
            if (dv_v) begin
                di_ra <= di_ra + 1'b1; di_left <= di_left - 1'b1;
                if (di_left == 14'd1) di_busy <= 1'b0;
            end
        end
        if (do_busy) case (dw_st)                               // out: RAM to SDRAM, a word at a time
            2'd0: if (do_n == 14'd0) do_busy <= 1'b0; else if (!dv_v) dw_st <= 2'd1;
            2'd1: begin w_req <= 1'b1; w_addr <= do_sd; w_data <= rb_q; dw_st <= 2'd2; end
            default: if (w_ack) begin
                w_req <= 1'b0; do_sd <= do_sd + 1'b1; do_ra <= do_ra + 1'b1; do_n <= do_n - 1'b1;
                dw_st <= 2'd0;
            end
        endcase
    end
end

// a vertex engine start written while DMA in is busy: given to the engine once the transfer is in
wire        ve_go = dev_w && c_addr[10:2] == 9'h034;            // register 20
wire        ve_rel = ve_hold && !di_busy;
always @(posedge clk)
    if (!run) ve_hold <= 1'b0;
    else if (ve_go && di_busy) begin ve_hold <= 1'b1; ve_hd <= c_wdata; end
    else if (ve_rel) ve_hold <= 1'b0;

// ---------------------------------------------------------------- vertex engine
gfx_vtx vtx_u (
    .clk(clk), .rst(!run),
    .reg_we((dev_w && c_addr[10:7] == 4'b0001 && !(ve_go && di_busy)) || ve_rel),
    .reg_a(ve_rel ? 5'd20 : c_addr[6:2]), .reg_d(ve_rel ? ve_hd : c_wdata),
    .mat_we(dev_w && c_addr[10]), .mat_a(c_addr[9:2]), .busy(ve_busy),
    .ram_req(ve_req), .ram_we(ve_we), .ram_a(ve_a), .ram_d(ve_d), .ram_gnt(ve_gnt), .ram_q(rb_q));

assign tex_we = dev_w && (c_addr[10:9] == 2'b01 || c_addr[10:2] == 9'h014 || c_addr[10:2] == 9'h015 ||
                          c_addr[10:2] == 9'h016);
assign tex_a  = c_addr[10:2];
assign tex_d  = c_wdata;

endmodule
`default_nettype wire

`timescale 1ns/1ps
`default_nettype none

// ============================================================================
// rv32_cpu -- RV32IM core for the GFX4 scene CPU.
// Fetch and execute: the next PC goes straight to the instruction RAM, so a
// taken branch costs nothing. Register writes land a clock later (W); ALU
// results are forwarded from there, and an instruction that needs the result
// of the load just before it waits one clock. Waiting means fetching the same
// PC again. MUL takes 3 clocks (DSP), DIV/REM 34.
// ECALL, EBREAK and unknown instructions halt the core (halt, halt_pc).
// CSRs, read only: cycle, instret and their high halves (also as mcycle...).
// ============================================================================
module rv32_cpu #(
    parameter [31:0] RESET_PC = 32'h0000_0000
)(
    input  wire        clk,
    input  wire        rst,
    output wire [31:0] i_addr,                // instruction RAM, read every clock
    input  wire [31:0] i_data,                // the word at i_addr of the clock before
    output wire        d_rd,                  // load
    output wire        d_wr,                  // store, lanes in d_be
    output wire [31:0] d_addr,
    output wire [31:0] d_wdata,
    output wire [3:0]  d_be,
    input  wire [31:0] d_rdata,               // the loaded word, the clock after d_rd
    input  wire        d_busy,                // data port taken: loads and stores wait
    output reg         halt    = 1'b0,
    output reg  [31:0] halt_pc = 32'd0,
    output wire [31:0] retired                // instructions completed, low 32 bits
);

localparam [6:0] OP_LUI = 7'b0110111, OP_AUIPC = 7'b0010111, OP_JAL  = 7'b1101111,
                 OP_JALR = 7'b1100111, OP_BR   = 7'b1100011, OP_LOAD = 7'b0000011,
                 OP_STORE = 7'b0100011, OP_IMM = 7'b0010011, OP_REG  = 7'b0110011,
                 OP_FENCE = 7'b0001111, OP_SYS = 7'b1110011;

// ---------------------------------------------------------------- the instruction in execute
wire [31:0] ir     = i_data;
wire [6:0]  opcode = ir[6:0];
wire [4:0]  rd     = ir[11:7];
wire [2:0]  f3     = ir[14:12];
wire [4:0]  rs1    = ir[19:15];
wire [4:0]  rs2    = ir[24:20];
wire        f7_5   = ir[30];
wire        muldiv = (opcode == OP_REG) && (ir[31:25] == 7'b0000001);

wire [31:0] imm_i = {{20{ir[31]}}, ir[31:20]};
wire [31:0] imm_s = {{20{ir[31]}}, ir[31:25], ir[11:7]};
wire [31:0] imm_b = {{19{ir[31]}}, ir[31], ir[7], ir[30:25], ir[11:8], 1'b0};
wire [31:0] imm_u = {ir[31:12], 12'd0};
wire [31:0] imm_j = {{11{ir[31]}}, ir[31], ir[19:12], ir[20], ir[30:21], 1'b0};

reg  [31:0] pc = RESET_PC;                    // address of ir

// ---------------------------------------------------------------- registers; W: the write a clock later
reg  [31:0] rf [0:31];
integer     ri;
initial for (ri = 0; ri < 32; ri = ri + 1) rf[ri] = 32'd0;

reg         w_we = 1'b0, w_load = 1'b0;
reg  [4:0]  w_rd = 5'd0;
reg  [31:0] w_val = 32'd0;
reg  [2:0]  w_f3 = 3'd0;
reg  [1:0]  w_alo = 2'd0;

wire [31:0] a = (w_we && !w_load && w_rd == rs1) ? w_val : rf[rs1];
wire [31:0] b = (w_we && !w_load && w_rd == rs2) ? w_val : rf[rs2];

wire [7:0]  ld_b = d_rdata >> {w_alo, 3'd0};
wire [15:0] ld_h = w_alo[1] ? d_rdata[31:16] : d_rdata[15:0];
wire [31:0] ld_v = (w_f3 == 3'd0) ? {{24{ld_b[7]}}, ld_b} :
                   (w_f3 == 3'd1) ? {{16{ld_h[15]}}, ld_h} :
                   (w_f3 == 3'd4) ? {24'd0, ld_b} :
                   (w_f3 == 3'd5) ? {16'd0, ld_h} : d_rdata;

always @(posedge clk)
    if (w_we) rf[w_rd] <= w_load ? ld_v : w_val;

// ---------------------------------------------------------------- ALU
wire [31:0] op_b  = (opcode == OP_REG) ? b : imm_i;
wire [4:0]  shamt = op_b[4:0];
wire signed [31:0] a_s = a;
wire [31:0] sra_v = a_s >>> shamt;           // alone: in a ?: with unsigned it would shift in zeros
reg  [31:0] alu;
always @* begin
    case (f3)
    3'd0:    alu = (opcode == OP_REG && f7_5) ? a - op_b : a + op_b;
    3'd1:    alu = a << shamt;
    3'd2:    alu = {31'd0, $signed(a) < $signed(op_b)};
    3'd3:    alu = {31'd0, a < op_b};
    3'd4:    alu = a ^ op_b;
    3'd5:    alu = f7_5 ? sra_v : a >> shamt;
    3'd6:    alu = a | op_b;
    default: alu = a & op_b;
    endcase
end

reg taken;
always @* begin
    case (f3)
    3'd0:    taken = (a == b);
    3'd1:    taken = (a != b);
    3'd4:    taken = ($signed(a) < $signed(b));
    3'd5:    taken = ($signed(a) >= $signed(b));
    3'd6:    taken = (a < b);
    3'd7:    taken = (a >= b);
    default: taken = 1'b0;
    endcase
end

// ---------------------------------------------------------------- MUL: operands, product, result
reg  [1:0]  m_st = 2'd0;
reg  [32:0] m_a = 33'd0, m_b = 33'd0;
reg  [65:0] m_p = 66'd0;
always @(posedge clk) begin
    m_a <= {(f3 == 3'd1 || f3 == 3'd2) & a[31], a};             // MULH, MULHSU: signed a
    m_b <= {(f3 == 3'd1) & b[31], b};                            // MULH: signed b
    m_p <= $signed(m_a) * $signed(m_b);
end
wire [31:0] mul_r = (f3 == 3'd0) ? m_p[31:0] : m_p[63:32];

// ---------------------------------------------------------------- DIV/REM: one quotient bit a clock
reg  [5:0]  dv_n = 6'd0;                      // 0 idle, 33 start, 1 done
reg  [31:0] dv_q = 32'd0, dv_r = 32'd0, dv_d = 32'd0;
reg         dv_nq = 1'b0, dv_nr = 1'b0, dv_z = 1'b0;
wire        dv_s = !f3[0];                    // DIV, REM: signed
wire [32:0] dv_t = {dv_r, dv_q[31]};
wire        dv_ge = (dv_t >= {1'b0, dv_d});
wire [31:0] dv_qo = dv_z ? 32'hFFFF_FFFF : dv_nq ? -dv_q : dv_q;
wire [31:0] dv_ro = dv_nr ? -dv_r : dv_r;
wire [31:0] div_r = f3[1] ? dv_ro : dv_qo;

// ---------------------------------------------------------------- CSR reads
reg  [63:0] cycles = 64'd0, instret = 64'd0;
assign retired = instret[31:0];
reg  [31:0] csr;
always @* begin
    case (ir[31:20])
    12'hC00, 12'hB00: csr = cycles[31:0];
    12'hC80, 12'hB80: csr = cycles[63:32];
    12'hC02, 12'hB02: csr = instret[31:0];
    12'hC82, 12'hB82: csr = instret[63:32];
    default:          csr = 32'd0;
    endcase
end

// ---------------------------------------------------------------- execute: what the instruction does
wire is_load  = (opcode == OP_LOAD);
wire is_store = (opcode == OP_STORE);
wire legal = (opcode == OP_LUI) || (opcode == OP_AUIPC) || (opcode == OP_JAL) ||
             (opcode == OP_JALR && f3 == 3'd0) ||
             (opcode == OP_BR && f3 != 3'd2 && f3 != 3'd3) ||
             (is_load && (f3 == 3'd0 || f3 == 3'd1 || f3 == 3'd2 || f3 == 3'd4 || f3 == 3'd5)) ||
             (is_store && (f3 == 3'd0 || f3 == 3'd1 || f3 == 3'd2)) ||
             (opcode == OP_IMM) || (opcode == OP_REG) || (opcode == OP_FENCE) ||
             (opcode == OP_SYS && f3 != 3'd0 && f3 != 3'd4);

wire load_use = w_we && w_load && (w_rd == rs1 || w_rd == rs2);
wire md_wait  = muldiv && (f3[2] ? (dv_n != 6'd1) : (m_st != 2'd2));
wire stall    = load_use || md_wait || (d_busy && (is_load || is_store));
wire run      = !rst && !halt && legal && !stall;

reg  [31:0] res;
always @* begin
    case (opcode)
    OP_LUI:            res = imm_u;
    OP_AUIPC:          res = pc + imm_u;
    OP_JAL, OP_JALR:   res = pc + 32'd4;
    OP_SYS:            res = csr;
    default:           res = muldiv ? (f3[2] ? div_r : mul_r) : alu;
    endcase
end

wire [31:0] ls_addr = a + (is_store ? imm_s : imm_i);
wire [31:0] pc_next = !run                          ? pc :
                      (opcode == OP_JAL)            ? pc + imm_j :
                      (opcode == OP_JALR)           ? {ls_addr[31:1], 1'b0} :
                      (opcode == OP_BR && taken)    ? pc + imm_b : pc + 32'd4;

assign i_addr  = rst ? RESET_PC : pc_next;
assign d_rd    = run && is_load;
assign d_wr    = run && is_store;
assign d_addr  = ls_addr;
assign d_wdata = (f3[1:0] == 2'd0) ? {4{b[7:0]}} : (f3[1:0] == 2'd1) ? {2{b[15:0]}} : b;
assign d_be    = (f3[1:0] == 2'd0) ? (4'b0001 << ls_addr[1:0]) :
                 (f3[1:0] == 2'd1) ? (ls_addr[1] ? 4'b1100 : 4'b0011) : 4'b1111;

always @(posedge clk) begin
    cycles <= cycles + 1'b1;
    if (rst) begin
        pc <= RESET_PC; halt <= 1'b0; w_we <= 1'b0; w_load <= 1'b0; m_st <= 2'd0; dv_n <= 6'd0;
    end else begin
        pc     <= pc_next;
        w_we   <= run && (rd != 5'd0) && !is_store && (opcode != OP_BR) && (opcode != OP_FENCE);
        w_load <= run && is_load;
        w_rd   <= rd;
        w_val  <= res;
        w_f3   <= f3;
        w_alo  <= ls_addr[1:0];
        if (run) instret <= instret + 1'b1;
        if (!halt && !legal) begin halt <= 1'b1; halt_pc <= pc; end

        // MUL: operands in, product, then the instruction completes
        if (muldiv && !f3[2] && !load_use && !halt)
            m_st <= (m_st == 2'd2) ? 2'd0 : m_st + 1'b1;

        // DIV/REM: start, 32 steps, then the instruction completes
        if (muldiv && f3[2] && !load_use && !halt) begin
            if (dv_n == 6'd0) begin
                dv_q  <= (dv_s && a[31]) ? -a : a;
                dv_d  <= (dv_s && b[31]) ? -b : b;
                dv_r  <= 32'd0;
                dv_z  <= (b == 32'd0);
                dv_nq <= dv_s && (a[31] ^ b[31]);
                dv_nr <= dv_s && a[31];
                dv_n  <= 6'd33;
            end else if (dv_n == 6'd1)
                dv_n <= 6'd0;
            else begin
                dv_r  <= dv_ge ? dv_t[31:0] - dv_d : dv_t[31:0];
                dv_q  <= {dv_q[30:0], dv_ge};
                dv_n  <= dv_n - 1'b1;
            end
        end
    end
end

endmodule
`default_nettype wire

`timescale 1ns/1ps
`default_nettype none

// ============================================================================
// gfx_vtx -- vertex engine of the scene CPU: a meshlet's vertices in the CPU's
// RAM are turned, projected and lit and written back as screen vertices,
// exactly as g3d_vertex() in cpu/g3d_mesh.c does it. About 28 clocks a vertex.
//   in, 3 words:  {Y, X}, {material, 0, Z}, {0, NZ, NY, NX}  (X, Y, Z 16-bit;
//                 normal 10 bits a component)
//   out, 4 words: sx, sy (1/16 pixel), z (24 bits; bit 31: before the near
//                 plane, not drawable), rgb
// Registers: 0-8 matrix rows (Q14), 9-11 translation, 12-14 light and 15-17
// half-way vector (Q14, in the mesh's frame), 18 source and 19 destination
// (RAM word addresses), 20 count (writing it starts). Materials: 256 words,
// {gloss, R, G, B}. RAM: one access a clock when granted; read data a clock
// after the grant.
// Stage 5a: 21 where reflection places go (a word a vertex); 22 the mode:
// bit 0 reflection places wanted, bits 3:2 the near plane (1024 >> n); 23 the
// unlit share of the light (Q14); 24, 25 the place's scale k and centre: with
// (u, v) the normal's x and y as the view sees it (the matrix's first rows;
// Q14), the place is {centre - (v k >> 14), centre + (u k >> 14)}, each kept
// in 0..65535: a texel of a picture of a mirror ball. After rst: no places,
// near plane 1024, unlit share 4096.
// ============================================================================
module gfx_vtx (
    input  wire        clk,
    input  wire        rst,
    input  wire        reg_we,
    input  wire [4:0]  reg_a,
    input  wire [31:0] reg_d,
    input  wire        mat_we,
    input  wire [7:0]  mat_a,
    output wire        busy,
    output reg         ram_req = 1'b0,
    output reg         ram_we  = 1'b0,
    output reg  [13:0] ram_a   = 14'd0,
    output wire [31:0] ram_d,
    input  wire        ram_gnt,
    input  wire [31:0] ram_q
);

localparam [3:0] V_IDLE = 4'd0, V_RD = 4'd1, V_X1 = 4'd2, V_X2 = 4'd3, V_X3 = 4'd4, V_SQ = 4'd5,
                 V_COL = 4'd6, V_DIV = 4'd7, V_P1 = 4'd8, V_P2 = 4'd9, V_P3 = 4'd10, V_WR = 4'd11;

// ---------------------------------------------------------------- registers and materials
// Q14 values (at most 2^14 in size) in 18 bits: each product below is one DSP48A1
reg  signed [17:0] m0 = 0, m1 = 0, m2 = 0, m3 = 0, m4 = 0, m5 = 0, m6 = 0, m7 = 0, m8 = 0;
reg  signed [17:0] lx = 0, ly = 0, lz = 0, hx = 0, hy = 0, hz = 0;
reg  signed [31:0] atx = 0, aty = 0, atz = 0;
reg  [13:0] src = 14'd0, dst = 14'd0, uvd = 14'd0;
reg  [12:0] n = 13'd0;
reg         cap_on = 1'b0;                   // reflection places wanted
reg  [1:0]  zn = 2'd0;                       // near plane: 1024 >> zn
reg  signed [17:0] amb = 18'sd4096;          // unlit share of the light
reg  signed [15:0] mk = 16'sd0;              // a place's scale
reg  [15:0] mcen = 16'd0;                    // and centre
reg  [3:0]  vs = V_IDLE;
reg  [31:0] mtab [0:255];
reg  [31:0] mq = 32'd0;
reg  [7:0]  mat = 8'd0;

assign busy = (vs != V_IDLE);

always @(posedge clk) begin
    if (mat_we) mtab[mat_a] <= reg_d;
    mq <= mtab[mat];
end

// ---------------------------------------------------------------- the vertex, its products and results
reg  [31:0] w0 = 0, w1 = 0, w2 = 0;
reg  [1:0]  rn = 2'd0, cn = 2'd0;            // words asked for, captured
reg         cap = 1'b0;                      // a read was granted: data next clock
wire signed [15:0] px = w0[15:0], py = w0[31:16], pz = w1[15:0];
wire signed [9:0]  nx = w2[9:0], ny = w2[19:10], nz = w2[29:20];

reg  signed [33:0] t0 = 0, t1 = 0, t2 = 0, t3 = 0, t4 = 0, t5 = 0, t6 = 0, t7 = 0, t8 = 0;
reg  signed [27:0] q0 = 0, q1 = 0, q2 = 0, q3 = 0, q4 = 0, q5 = 0;
reg  signed [27:0] u0 = 0, u1 = 0, u2 = 0, u3 = 0, u4 = 0, u5 = 0;
reg  signed [17:0] mu = 0, mv = 0;           // the normal's x and y in view
reg  signed [33:0] pu = 0, pv = 0;
reg  signed [31:0] vx = 0, vy = 0, vz = 0;
reg  signed [17:0] dl = 0, ds = 0, lit = 0;   // 0..16384
reg  [2:0]  sq = 3'd0;
reg  [8:0]  cr = 0, cg = 0, cb = 0;
reg  signed [63:0] ppx = 0, ppy = 0;
reg  [31:0] o_sx = 0, o_sy = 0, o_z = 0, o_rgb = 0, o_uv = 0;
reg  [2:0]  wn = 3'd0;
reg         front = 1'b0;

// sums of the products, arithmetic shifts as in C (>> of a signed int)
wire signed [35:0] sx_sum = t0 + t1 + t2, sy_sum = t3 + t4 + t5, sz_sum = t6 + t7 + t8;
wire signed [29:0] sl_sum = q0 + q1 + q2, sh_sum = q3 + q4 + q5;
wire signed [20:0] dl_v = sl_sum >>> 9, ds_v = sh_sum >>> 9;
wire signed [35:0] dsq  = ds * ds;
wire signed [35:0] dlk  = dl * (18'sd16384 - amb);
wire signed [29:0] su_sum = u0 + u1 + u2, sv_sum = u3 + u4 + u5;
wire signed [20:0] mu_v = su_sum >>> 9, mv_v = sv_sum >>> 9;
wire signed [20:0] pu_v = $signed({5'd0, mcen}) + (pu >>> 14), pv_v = $signed({5'd0, mcen}) - (pv >>> 14);
wire [15:0] uu = pu_v < 0 ? 16'd0 : (pu_v > 21'sd65535) ? 16'hFFFF : pu_v[15:0];
wire [15:0] vv = pv_v < 0 ? 16'd0 : (pv_v > 21'sd65535) ? 16'hFFFF : pv_v[15:0];
wire signed [31:0] znear = (zn == 2'd0) ? 32'sd1024 : (zn == 2'd1) ? 32'sd512 : (zn == 2'd2) ? 32'sd256 : 32'sd128;
wire signed [26:0] spp  = ds * $signed({1'b0, mq[31:24]});
wire signed [26:0] rp   = lit * $signed({1'b0, mq[23:16]}), gp = lit * $signed({1'b0, mq[15:8]}),
                   bp   = lit * $signed({1'b0, mq[7:0]});
wire signed [12:0] sp   = spp >>> 14;
wire signed [12:0] rr   = (rp >>> 14) + sp, gg = (gp >>> 14) + sp, bb = (bp >>> 14) + sp;

// ---------------------------------------------------------------- 2^30 / z, unsigned, two bits a clock
// z >= 128 (the near plane): the quotient's top 8 bits are 0 and 64 is left after them, so the
// division starts there with 24 bits to go (2^30 has no more ones to bring down)
reg  [5:0]  dc = 6'd0;
reg  [31:0] dr = 32'd0, dq = 32'd0, dd = 32'd0;
wire [32:0] dta = {dr, 1'b0};
wire        dga = (dta >= {1'b0, dd});
wire [31:0] dra = dga ? dta[31:0] - dd : dta[31:0];
wire [32:0] dtb = {dra, 1'b0};
wire        dgb = (dtb >= {1'b0, dd});
wire signed [31:0] inv = dq;                 // at most 2^23

wire signed [63:0] dqx = $signed({32'd0, dq});
wire signed [63:0] dqn = (zn == 2'd0) ? dqx <<< 4 : (zn == 2'd1) ? dqx <<< 3 : (zn == 2'd2) ? dqx <<< 2 : dqx <<< 1;
wire signed [63:0] zd = 64'sd16777216 - dqn;   // dq << (4 - zn), the shift as a mux (XST)

assign ram_d = (wn == 3'd0) ? o_sx : (wn == 3'd1) ? o_sy : (wn == 3'd2) ? o_z : (wn == 3'd3) ? o_rgb : o_uv;

always @(posedge clk) begin
    cap <= ram_req && ram_gnt && !ram_we;
    if (cap) begin                                            // read data in
        case (cn)
        2'd0:    w0 <= ram_q;
        2'd1:    w1 <= ram_q;
        default: w2 <= ram_q;
        endcase
        cn <= cn + 1'b1;
    end
    if (dc != 6'd0) begin                                     // divider
        dr <= dgb ? dtb[31:0] - dd : dtb[31:0];
        dq <= {dq[29:0], dga, dgb};
        dc <= dc - 1'b1;
    end
    if (reg_we) case (reg_a)
        5'd0:  m0 <= reg_d[17:0];  5'd1:  m1 <= reg_d[17:0];  5'd2:  m2 <= reg_d[17:0];
        5'd3:  m3 <= reg_d[17:0];  5'd4:  m4 <= reg_d[17:0];  5'd5:  m5 <= reg_d[17:0];
        5'd6:  m6 <= reg_d[17:0];  5'd7:  m7 <= reg_d[17:0];  5'd8:  m8 <= reg_d[17:0];
        5'd9:  atx <= reg_d;       5'd10: aty <= reg_d;       5'd11: atz <= reg_d;
        5'd12: lx <= reg_d[17:0];  5'd13: ly <= reg_d[17:0];  5'd14: lz <= reg_d[17:0];
        5'd15: hx <= reg_d[17:0];  5'd16: hy <= reg_d[17:0];  5'd17: hz <= reg_d[17:0];
        5'd18: src <= reg_d[13:0];
        5'd19: dst <= reg_d[13:0];
        5'd21: uvd <= reg_d[13:0];
        5'd22: begin cap_on <= reg_d[0]; zn <= reg_d[3:2]; end
        5'd23: amb <= reg_d[17:0];
        5'd24: mk <= reg_d[15:0];
        5'd25: mcen <= reg_d[15:0];
        default: ;
    endcase
    if (rst) begin cap_on <= 1'b0; zn <= 2'd0; amb <= 18'sd4096; end
    if (rst) begin
        vs <= V_IDLE; ram_req <= 1'b0; dc <= 6'd0;
    end else case (vs)
    V_IDLE: if (reg_we && reg_a == 5'd20 && reg_d[12:0] != 13'd0) begin
        n <= reg_d[12:0]; rn <= 2'd0; cn <= 2'd0;
        ram_req <= 1'b1; ram_we <= 1'b0; ram_a <= src;
        vs <= V_RD;
    end
    V_RD: begin                                               // three words
        if (ram_req && ram_gnt) begin
            ram_a <= ram_a + 1'b1;
            rn <= rn + 1'b1;
            if (rn == 2'd2) ram_req <= 1'b0;
        end
        if (cap && cn == 2'd2) vs <= V_X1;                    // w2 arrives this clock
    end
    V_X1: begin                                               // products; the material
        t0 <= m0 * px;  t1 <= m1 * py;  t2 <= m2 * pz;
        t3 <= m3 * px;  t4 <= m4 * py;  t5 <= m5 * pz;
        t6 <= m6 * px;  t7 <= m7 * py;  t8 <= m8 * pz;
        q0 <= nx * lx;  q1 <= ny * ly;  q2 <= nz * lz;
        q3 <= nx * hx;  q4 <= ny * hy;  q5 <= nz * hz;
        u0 <= nx * m0;  u1 <= ny * m1;  u2 <= nz * m2;
        u3 <= nx * m3;  u4 <= ny * m4;  u5 <= nz * m5;
        mat <= w1[31:24];
        src <= src + 14'd3;
        vs <= V_X2;
    end
    V_X2: begin                                               // view position; light terms
        vx <= (sx_sum >>> 14) + atx;
        vy <= (sy_sum >>> 14) + aty;
        vz <= (sz_sum >>> 14) + atz;
        dl <= (dl_v < 0) ? 18'sd0 : dl_v[17:0];
        ds <= (ds_v < 0) ? 18'sd0 : ds_v[17:0];
        mu <= (mu_v > 21'sd131071) ? 18'sd131071 : (mu_v < -21'sd131072) ? -18'sd131072 : mu_v[17:0];
        mv <= (mv_v > 21'sd131071) ? 18'sd131071 : (mv_v < -21'sd131072) ? -18'sd131072 : mv_v[17:0];
        vs <= V_X3;
    end
    V_X3: begin                                               // before the near plane: divide
        front <= (vz >= znear);
        if (vz >= znear) begin
            dr <= 32'd64; dq <= 32'd0; dd <= vz; dc <= 6'd12;
        end
        lit <= amb + dlk[31:14];
        pu <= mu * mk; pv <= mv * mk;
        sq <= 3'd0;
        vs <= V_SQ;
    end
    V_SQ: begin                                               // highlight ^16
        ds <= dsq[31:14];
        sq <= sq + 1'b1;
        if (sq == 3'd3) vs <= V_COL;
    end
    V_COL: begin
        cr <= (rr > 13'sd255) ? 9'd255 : rr[8:0];
        cg <= (gg > 13'sd255) ? 9'd255 : gg[8:0];
        cb <= (bb > 13'sd255) ? 9'd255 : bb[8:0];
        vs <= front ? V_DIV : V_P3;
    end
    V_DIV: if (dc == 6'd0) vs <= V_P1;
    V_P1: begin ppx <= vx * inv; ppy <= vy * inv; vs <= V_P2; end      // then times 16 FOCAL = 6400, by adding:
    V_P2: begin                                                       // nothing is lost however far to the side
        ppx <= (ppx <<< 12) + (ppx <<< 11) + (ppx <<< 8);
        ppy <= (ppy <<< 12) + (ppy <<< 11) + (ppy <<< 8);
        vs <= V_P3;
    end
    V_P3: begin
        o_sx  <= 32'sd5120 + (ppx >>> 30);
        o_sy  <= 32'sd3840 - (ppy >>> 30);
        o_z   <= !front ? 32'h8000_0000 : (zd < 0) ? 32'd0 : (zd > 64'sd16777215) ? 32'h00FF_FFFF : zd[31:0];
        o_rgb <= {8'd0, cr[7:0], cg[7:0], cb[7:0]};
        o_uv  <= {vv, uu};
        wn <= 3'd0;
        ram_req <= 1'b1; ram_we <= 1'b1; ram_a <= dst;
        vs <= V_WR;
    end
    default: begin                                            // V_WR: four words out, then the place
        if (ram_gnt) begin
            ram_a <= (wn == 3'd3) ? uvd : ram_a + 1'b1;
            wn <= wn + 1'b1;
            if (wn == (cap_on ? 3'd4 : 3'd3)) begin
                if (cap_on) uvd <= uvd + 1'b1;
                ram_req <= 1'b0; dst <= dst + 14'd4; n <= n - 1'b1;
                if (n == 13'd1) vs <= V_IDLE;
                else begin
                    rn <= 2'd0; cn <= 2'd0; ram_req <= 1'b1; ram_we <= 1'b0; ram_a <= src;
                    vs <= V_RD;
                end
            end
        end
    end
    endcase
end

endmodule
`default_nettype wire

`timescale 1ns/1ps
`default_nettype none

// ============================================================================
// gfx_gpu -- the 3D renderer (stage 3a). GO from the scene CPU starts a frame:
// wait for WrIdle (the CPU's lists have reached SDRAM), read the band table
// (30 x {list address, triangles}), then for each band stream its list (128-word
// bursts, while the 256-word FIFO has room for them) through gfx_raster into band
// buffer b & 1, once the frame writer has freed it. The first frame clears
// both buffers first.
// Band buffers: 640 x 16 x {drawn, colour} and z. The frame writer reads a
// colour and clears the pixel (not drawn, bg, far) in the same clock, then
// frees the buffer (free).
// Stage 4a: rec12 (lists of 12-word triangles) and textures. The texture
// unit's lines come over the same SDRAM port, several out at a time, ahead of
// list bursts unless the list FIFO is under 64 words (the words come back in
// order: a queue of the bursts out says whose they are); depth and colour are
// written apart (the colour after the texture unit).
// Stage 5a: fog_on (haze) and blending: the pixel unit reads the colour that
// is there, on its own address; "drawn" is a colour written, so blends over
// the background count.
// ============================================================================
module gfx_gpu (
    input  wire        clk,
    input  wire        rst,
    input  wire        rec12,                 // 12 words a triangle
    input  wire        fog_on,                // haze
    input  wire        d_we,                  // the pixel unit's registers (gfx_tex)
    input  wire [8:0]  d_a,
    input  wire [31:0] d_d,
    input  wire        go,                    // start a frame
    input  wire [22:0] table_addr,
    input  wire [23:0] bg,
    output wire        busy,                  // reading lists and drawing
    input  wire        wr_idle,
    output wire        dma_want,
    output wire        tex_wait,              // a texture line is wanted or on its way
    output reg         dma_req  = 1'b0,
    output reg  [22:0] dma_addr = 23'd0,
    output reg  [8:0]  dma_len  = 9'd0,
    input  wire        dma_rdy,
    input  wire [31:0] dma_data,
    input  wire        dma_valid,
    output wire [1:0]  ready,                 // buffer holds a finished band
    input  wire        rd,                    // frame writer: read a colour and clear it
    input  wire        rd_buf,
    input  wire [13:0] rd_a,
    output wire [23:0] rd_q,                  // a clock later
    output wire        rd_cov,                // with rd_q: the pixel was drawn
    input  wire [1:0]  free,                  // the frame writer is done with these buffers
    input  wire [2:0]  pr_sel,                // gfx_raster's setup probe
    output wire [31:0] pr_q
);

localparam [2:0] G_IDLE = 3'd0, G_CLEAR = 3'd1, G_WAIT = 3'd2, G_TAB = 3'd3, G_BAND = 3'd4, G_LIST = 3'd5;

reg  [2:0]  gs = G_IDLE;
reg         fresh = 1'b1;                    // buffers not yet cleared
reg  [13:0] ca = 14'd0;
reg  [22:0] tab_a = 23'd0;
reg  [31:0] tab [0:63];                      // the band table
reg  [5:0]  tn = 6'd0;
reg  [4:0]  b = 5'd0;                        // band being drawn
reg  [1:0]  full = 2'b00;
reg  [22:0] la = 23'd0;                      // list: next address
reg  [17:0] lw = 18'd0;                      // words still to request
reg  [8:0]  fly = 9'd0;                      // table or list words requested, not yet arrived
reg         want = 1'b0;
reg  [2:0]  t_out = 3'd0;                    // texture lines asked for, not all in
wire        t_mreq;
wire [22:0] t_maddr;
wire [4:0]  t_mlen;
wire        l_low;                                               // few list words left: their burst first
wire        t_go = t_mreq && !dma_req && !l_low;                 // else a line goes before list bursts

// the bursts out, oldest first: a texture line's or not, its words
reg         bq_t [0:3];
reg  [8:0]  bq_n [0:3];
reg  [1:0]  bq_w = 2'd0, bq_r = 2'd0;
reg  [8:0]  bq_got = 9'd0;
wire        w_tex  = dma_valid && bq_t[bq_r];                    // this word is a texture line's
wire        w_list = dma_valid && !bq_t[bq_r];                   // the band table's or a list's

assign busy     = (gs != G_IDLE);
assign ready    = full;
assign dma_want = want || t_go;
assign tex_wait = t_mreq || t_out != 3'd0;

// ---------------------------------------------------------------- list FIFO: 256 words, read in the same clock
reg  [31:0] fm [0:255];
reg  [7:0]  fwa = 8'd0, fra = 8'd0;
reg  [8:0]  fcnt = 9'd0;
wire        t_valid = (fcnt != 9'd0);
wire        t_ready, r_idle;
wire        pop  = t_valid && t_ready;
wire        push = w_list && (gs == G_LIST);

always @(posedge clk) begin
    if (push) fm[fwa] <= dma_data;
    if (rst) begin
        fwa <= 8'd0; fra <= 8'd0; fcnt <= 9'd0;
    end else begin
        if (push) fwa <= fwa + 1'b1;
        if (pop)  fra <= fra + 1'b1;
        fcnt <= fcnt + push - pop;
    end
end

// ---------------------------------------------------------------- renderer and band buffers
wire [13:0] bz_a, bw_a, cr_a, cw_a;
wire        bw, cw;
wire [23:0] bw_z, cw_c, z0, z1;
wire [24:0] c0, c1;
reg         rbd = 1'b0;                       // rd_buf, a clock later
wire        rsel = b[0];

gfx_raster ras_u (
    .clk(clk), .rst(rst), .rec12(rec12), .fog_on(fog_on), .band_y0({b, 4'd0}), .t_valid(t_valid), .t_data(fm[fra]),
    .t_ready(t_ready), .idle(r_idle), .bz_a(bz_a), .bz_q(rsel ? z1 : z0),
    .bw(bw), .bw_a(bw_a), .bw_z(bw_z), .cr_a(cr_a), .cr_q(rsel ? c1[23:0] : c0[23:0]),
    .cw(cw), .cw_a(cw_a), .cw_c(cw_c), .d_we(d_we), .d_a(d_a), .d_d(d_d),
    .m_req(t_mreq), .m_addr(t_maddr), .m_len(t_mlen), .m_rdy(t_go && dma_rdy), .m_valid(w_tex),
    .m_data(dma_data), .pr_sel(pr_sel), .pr_q(pr_q));

wire        clr = (gs == G_CLEAR);
wire        rd0 = rd && !rd_buf, rd1 = rd && rd_buf;

gfx_bandbuf buf0_u (
    .clk(clk), .az_a(rd0 ? rd_a : bz_a), .ac_a(rd0 ? rd_a : cr_a), .a_we(rd0), .a_c({1'b0, bg}), .qz(z0), .qc(c0),
    .z_we(clr || (bw && !rsel)), .z_a(clr ? ca : bw_a), .z_d(clr ? 24'hFFFFFF : bw_z),
    .c_we(clr || (cw && !rsel)), .c_a(clr ? ca : cw_a), .c_d(clr ? {1'b0, bg} : {1'b1, cw_c}));
gfx_bandbuf buf1_u (
    .clk(clk), .az_a(rd1 ? rd_a : bz_a), .ac_a(rd1 ? rd_a : cr_a), .a_we(rd1), .a_c({1'b0, bg}), .qz(z1), .qc(c1),
    .z_we(clr || (bw && rsel)), .z_a(clr ? ca : bw_a), .z_d(clr ? 24'hFFFFFF : bw_z),
    .c_we(clr || (cw && rsel)), .c_a(clr ? ca : cw_a), .c_d(clr ? {1'b0, bg} : {1'b1, cw_c}));

always @(posedge clk) rbd <= rd_buf;
assign rd_q   = rbd ? c1[23:0] : c0[23:0];
assign rd_cov = rbd ? c1[24] : c0[24];

// ---------------------------------------------------------------- frame sequence
wire [31:0] t_addr = tab[{b, 1'b0}];
wire [31:0] t_cnt  = tab[{b, 1'b1}];
wire [8:0]  blen   = (lw > 18'd128) ? 9'd128 : lw[8:0];
assign      l_low  = (gs == G_LIST) && want && fcnt < 9'd64;
wire        l_go   = (gs == G_LIST) && want && dma_rdy && !t_go && !dma_req;   // a list burst goes
wire        b_go   = (gs == G_TAB) && want && dma_rdy && !t_go;                // the band table's

always @(posedge clk) begin
    dma_req <= 1'b0;
    if (free[0]) full[0] <= 1'b0;
    if (free[1]) full[1] <= 1'b0;
    if (w_list && gs == G_TAB) begin tab[tn] <= dma_data; tn <= tn + 1'b1; end
    fly <= fly + (l_go ? blen : b_go ? 9'd60 : 9'd0) - ((w_list && fly != 9'd0) ? 9'd1 : 9'd0);
    if (dma_valid) begin                                      // the oldest burst out: done with it?
        if (bq_got == bq_n[bq_r] - 9'd1) begin bq_got <= 9'd0; bq_r <= bq_r + 1'b1; end
        else bq_got <= bq_got + 1'b1;
    end
    t_out <= t_out + ((t_go && dma_rdy) ? 3'd1 : 3'd0) - ((w_tex && bq_got == bq_n[bq_r] - 9'd1) ? 3'd1 : 3'd0);
    if (t_go && dma_rdy) begin                                // a texture line: 12 or 16 words
        dma_req <= 1'b1; dma_addr <= t_maddr; dma_len <= {4'd0, t_mlen};
        bq_t[bq_w] <= 1'b1; bq_n[bq_w] <= {4'd0, t_mlen}; bq_w <= bq_w + 1'b1;
    end
    if (rst) begin
        gs <= G_IDLE; full <= 2'b00; want <= 1'b0; fly <= 9'd0; fresh <= 1'b1; t_out <= 3'd0;
        bq_w <= 2'd0; bq_r <= 2'd0; bq_got <= 9'd0;
    end else case (gs)
    G_IDLE: if (go) begin tab_a <= table_addr; ca <= 14'd0; gs <= fresh ? G_CLEAR : G_WAIT; end
    G_CLEAR: if (ca == 14'd10239) begin fresh <= 1'b0; gs <= G_WAIT; end else ca <= ca + 1'b1;
    G_WAIT: if (wr_idle) begin want <= 1'b1; tn <= 6'd0; gs <= G_TAB; end
    G_TAB: begin                                              // the band table, one burst
        if (b_go) begin
            dma_req <= 1'b1; dma_addr <= tab_a; dma_len <= 9'd60; want <= 1'b0;
            bq_t[bq_w] <= 1'b0; bq_n[bq_w] <= 9'd60; bq_w <= bq_w + 1'b1;
        end
        if (!want && fly == 9'd0) begin b <= 5'd0; gs <= G_BAND; end
    end
    G_BAND: if (!full[b[0]]) begin                           // this band's buffer is free
        la <= t_addr[22:0];
        lw <= rec12 ? {t_cnt[13:0], 3'd0} + {1'b0, t_cnt[13:0], 2'd0}   // 12 or 9 words a triangle
                    : {t_cnt[13:0], 3'd0} + {4'd0, t_cnt[13:0]};
        gs <= G_LIST;
    end
    default: begin                                            // G_LIST: list bursts while the FIFO has room for them
        if (!want && lw != 18'd0 && fcnt + fly <= 9'd128) want <= 1'b1;
        if (l_go) begin
            dma_req <= 1'b1; dma_addr <= la; dma_len <= blen;
            la <= la + blen; lw <= lw - blen; want <= 1'b0;
            bq_t[bq_w] <= 1'b0; bq_n[bq_w] <= blen; bq_w <= bq_w + 1'b1;
        end
        if (!want && fly == 9'd0 && t_out == 3'd0 && lw == 18'd0 && fcnt == 9'd0 && r_idle) begin
            full[b[0]] <= 1'b1;
            if (b == 5'd29) gs <= G_IDLE;
            else begin b <= b + 1'b1; gs <= G_BAND; end
        end
    end
    endcase
end

endmodule

// ============================================================================
// gfx_bandbuf -- one band buffer, 640 x 16, as two RAMs: z, and the colour
// with a bit "drawn". Port A reads each at its own address and may clear both
// in the same clock (a_we: far, a_c; the reads get the old values; both
// addresses are then the same); port B writes depth and colour apart.
// ============================================================================
module gfx_bandbuf (
    input  wire        clk,
    input  wire [13:0] az_a,
    input  wire [13:0] ac_a,
    input  wire        a_we,
    input  wire [24:0] a_c,
    output reg  [23:0] qz = 24'd0,
    output reg  [24:0] qc = 25'd0,
    input  wire        z_we,
    input  wire [13:0] z_a,
    input  wire [23:0] z_d,
    input  wire        c_we,
    input  wire [13:0] c_a,
    input  wire [24:0] c_d
);

reg  [23:0] mz [0:10239];
reg  [24:0] mc [0:10239];

always @(posedge clk) begin
    if (a_we) mz[az_a] <= 24'hFFFFFF;
    qz <= mz[az_a];
end
always @(posedge clk)
    if (z_we) mz[z_a] <= z_d;

always @(posedge clk) begin
    if (a_we) mc[ac_a] <= a_c;
    qc <= mc[ac_a];
end
always @(posedge clk)
    if (c_we) mc[c_a] <= c_d;

endmodule
`default_nettype wire

`timescale 1ns/1ps
`default_nettype none

// ============================================================================
// gfx_raster -- GFX4 band renderer core (stages 3a, 4a, 5a): triangle setup,
// then one pixel a clock into a band buffer (16 lines of 640) with a depth
// test. Bit-exact with tools/g4_raster.py, which documents the numbers.
// A triangle is 9 words, 3 a vertex: {Y, X} (12.4), {flags, Z}, {0, R, G, B};
// with rec12, 12: a vertex's fourth {V, U}. Flags: vertex 0 bit 24 two-sided,
// bit 25 textured, bits 31:26 the slot; vertex 1 bits 25:24 the mode (0
// opaque, 1 cut-out, 2 blend, 3 add), bit 26 no haze; vertex 2 bits 31:24
// alpha. A textured triangle's setup adds q and k, the planes U q and V q (8
// fraction bits) and the level constant.
// Pixels are walked down each column of the triangle's box (at most 16 lines
// of the band), so a small texture cache follows them; a column the triangle
// misses (an edge has both its ends outside) takes one clock, and a column is
// left once the triangle has been passed. A pixel that passes the
// depth test goes through gfx_tex for its colour; an opaque one writes its
// depth at once, a cut-out's comes back from gfx_tex with the colour (if its
// alpha is solid), blends and adds write none. While gfx_tex takes no pixel
// (its queue or its line fills full), everything here holds.
// Setup (about 40 to 60 clocks: products, 12 dividers, the start values a bit
// a clock) runs while the triangle before is drawn, and the next one's
// words come in during it. Between triangles the drawing pauses,
// so pixels of two triangles are at least 5 apart in the pipeline: the last
// depth and colour are written before the next ones are read. After a
// cut-out the next triangle waits until its pixels have all left gfx_tex.
// Buffer: z read at bz_a, colour at cr_a (values a clock later).
// ============================================================================
module gfx_raster (
    input  wire        clk,
    input  wire        rst,
    input  wire        rec12,                 // 12-word triangles
    input  wire        fog_on,                // haze (gfx_tex)
    input  wire [8:0]  band_y0,               // first line of the band, fixed while it is drawn
    input  wire        t_valid,               // triangle words
    input  wire [31:0] t_data,
    output wire        t_ready,
    output wire        idle,                  // nothing in setup, drawing or the pixel pipeline
    output wire [13:0] bz_a,
    input  wire [23:0] bz_q,
    output wire        bw,                    // depth write
    output wire [13:0] bw_a,
    output wire [23:0] bw_z,
    output wire [13:0] cr_a,                  // colour read
    input  wire [23:0] cr_q,
    output wire        cw,                    // colour write
    output wire [13:0] cw_a,
    output wire [23:0] cw_c,
    input  wire        d_we,                  // gfx_tex's registers
    input  wire [8:0]  d_a,
    input  wire [31:0] d_d,
    output wire        m_req,                 // texture lines
    output wire [22:0] m_addr,
    output wire [4:0]  m_len,
    input  wire        m_rdy,
    input  wire        m_valid,
    input  wire [31:0] m_data,
    input  wire [2:0]  pr_sel,                // setup probe (the scene CPU's check): which word
    output wire [31:0] pr_q
);

localparam [3:0] U_IN = 4'd0, U_MUL = 4'd2, U_SUB = 4'd3, U_BOX = 4'd4, U_EDG = 4'd5,
                 U_EMU = 4'd6, U_ESUM = 4'd7, U_DIV = 4'd8, U_START = 4'd9, U_DONE = 4'd10,
                 U_TQ = 4'd11, U_TS = 4'd12, U_TP = 4'd13, U_TD = 4'd14;

// ---------------------------------------------------------------- words in: a 12-word shift register, filled
// while the triangle before is set up (its words are taken at the setup's first step)
reg  [3:0]  us = U_IN;
reg  [3:0]  wn = 4'd0;
reg         wf = 1'b0;                        // a whole triangle's words are in
reg  [31:0] w0 = 0, w1 = 0, w2 = 0, w3 = 0, w4 = 0, w5 = 0, w6 = 0, w7 = 0, w8 = 0, w9 = 0, w10 = 0, w11 = 0;
assign t_ready = !wf;
// a vertex's words: 9-word triangles end up in w3..w11, 12-word ones fill w0..w11
wire [31:0] v0xy = rec12 ? w0 : w3, v0z = rec12 ? w1 : w4,  v0c = rec12 ? w2 : w5;
wire [31:0] v1xy = rec12 ? w4 : w6, v1z = rec12 ? w5 : w7,  v1c = rec12 ? w6 : w8;
wire [31:0] v2xy = rec12 ? w8 : w9, v2z = rec12 ? w9 : w10, v2c = rec12 ? w10 : w11;

// textures: q, its scale k, U q and V q, their differences and products; the level constant
reg         tx = 1'b0;
reg  [5:0]  slot = 6'd0;
reg  [24:0] q0 = 0, q1 = 0, q2 = 0;
reg  [15:0] U0 = 0, U1 = 0, U2 = 0, V0 = 0, V1 = 0, V2 = 0;
reg  [3:0]  tk = 4'd0;
reg  [15:0] qn0 = 16'd1, qn1 = 16'd1, qn2 = 16'd1;
reg  [31:0] S0 = 0, S1 = 0, S2 = 0, T0 = 0, T1 = 0, T2 = 0;
reg  [9:0]  lgq = 10'd0;
reg  signed [32:0] sd1 = 0, sd2 = 0, td1 = 0, td2 = 0;
reg  signed [16:0] ud1 = 0, ud2 = 0, vd1 = 0, vd2 = 0;
reg  signed [50:0] sn1 = 0, sn2 = 0, sn3 = 0, sn4 = 0, tn1 = 0, tn2 = 0, tn3 = 0, tn4 = 0;
reg  signed [33:0] uvp1 = 0, uvp2 = 0;
reg  signed [51:0] sNx = 0, sNy = 0, tNx = 0, tNy = 0;
reg  [34:0] auv = 35'd0;
reg  signed [15:0] tC = 16'sd0;
wire [24:0] qm = (q0 > q1) ? ((q0 > q2) ? q0 : q2) : ((q1 > q2) ? q1 : q2);
wire [3:0]  kq = qm[24] ? 4'd9 : qm[23] ? 4'd8 : qm[22] ? 4'd7 : qm[21] ? 4'd6 : qm[20] ? 4'd5 :
                 qm[19] ? 4'd4 : qm[18] ? 4'd3 : qm[17] ? 4'd2 : qm[16] ? 4'd1 : 4'd0;
wire [24:0] qs0 = q0 >> tk, qs1 = q1 >> tk, qs2 = q2 >> tk;

// lg(x) = 16 (bits(x) - 1) + LG[the 4 bits after the leading one], for x up to 48 bits
function [9:0] lgw;
    input [47:0] x;
    reg   [5:0]  n;
    reg   [47:0] y;
    reg   [3:0]  m;
    integer      i;
    begin
        n = 6'd0;
        for (i = 0; i < 48; i = i + 1) if (x[i]) n = i[5:0];
        y = x;                                 // the leading one to bit 47 (constant shifts: XST)
        if (y[47:16] == 32'd0) y = y << 32;
        if (y[47:32] == 16'd0) y = y << 16;
        if (y[47:40] == 8'd0)  y = y << 8;
        if (y[47:44] == 4'd0)  y = y << 4;
        if (y[47:46] == 2'd0)  y = y << 2;
        if (!y[47])            y = y << 1;
        m = y[46:43];
        case (m)
            4'd0: lgw = {n, 4'd0};  4'd1: lgw = {n, 4'd1};  4'd2: lgw = {n, 4'd3};  4'd3: lgw = {n, 4'd4};
            4'd4: lgw = {n, 4'd5};  4'd5: lgw = {n, 4'd6};  4'd6: lgw = {n, 4'd7};  4'd7: lgw = {n, 4'd8};
            4'd8: lgw = {n, 4'd9};  4'd9: lgw = {n, 4'd10}; 4'd10: lgw = {n, 4'd11}; 4'd11: lgw = {n, 4'd12};
            4'd12: lgw = {n, 4'd13}; 4'd13: lgw = {n, 4'd14}; default: lgw = {n, 4'd15};
        endcase
    end
endfunction

// ---------------------------------------------------------------- setup registers
reg  signed [17:0] X0 = 0, Y0 = 0, X1 = 0, Y1 = 0, X2 = 0, Y2 = 0;
reg  signed [17:0] dX1 = 0, dY1 = 0, dX2 = 0, dY2 = 0;
reg  signed [24:0] zd1 = 0, zd2 = 0;          // attribute differences: z, then r g b
reg  signed [8:0]  rd1 = 0, rd2 = 0, gd1 = 0, gd2 = 0, bd1 = 0, bd2 = 0;
reg  [23:0] z0 = 0;
reg  [7:0]  r0 = 0, g0 = 0, b0 = 0;
reg         two = 1'b0;
reg  [1:0]  mode = 2'd0;                      // 0 opaque, 1 cut-out, 2 blend, 3 add
reg         nofog = 1'b0;
reg  [7:0]  alpha = 8'd0;
reg  signed [35:0] pa = 0, pb = 0;
reg  signed [42:0] zn1 = 0, zn2 = 0, zn3 = 0, zn4 = 0;
reg  signed [26:0] rn1 = 0, rn2 = 0, rn3 = 0, rn4 = 0, gn1 = 0, gn2 = 0, gn3 = 0, gn4 = 0,
                   bn1 = 0, bn2 = 0, bn3 = 0, bn4 = 0;
reg  signed [36:0] A2 = 0;
reg  signed [43:0] zNx = 0, zNy = 0;
reg  signed [27:0] rNx = 0, rNy = 0, gNx = 0, gNy = 0, bNx = 0, bNy = 0;
reg  signed [17:0] xlo = 0, xhi = 0, ylo = 0, yhi = 0;   // vertex extent

// the pixel box, clipped to the screen and the band
wire signed [17:0] bx0 = (xlo + 18'sd7) >>> 4, bx1 = (xhi - 18'sd8) >>> 4;
wire signed [17:0] by0 = (ylo + 18'sd7) >>> 4, by1 = (yhi - 18'sd8) >>> 4;
wire signed [17:0] yb0 = {9'd0, band_y0}, yb1 = {9'd0, band_y0} + 18'sd15;
wire signed [17:0] cx0 = (bx0 < 18'sd0) ? 18'sd0 : bx0, cx1 = (bx1 > 18'sd639) ? 18'sd639 : bx1;
wire signed [17:0] cy0 = (by0 < yb0) ? yb0 : by0,        cy1 = (by1 > yb1) ? yb1 : by1;

reg  [9:0]  sxmin = 0, sxmax = 0;
reg  [8:0]  symin = 0, symax = 0;
reg  signed [17:0] PX = 0, PY = 0;
// edges 0: 1->2, 1: 2->0, 2: 0->1 (1 and 2 swapped for a two-sided back face)
reg  signed [17:0] va0 = 0, va1 = 0, va2 = 0, wa0 = 0, wa1 = 0, wa2 = 0;   // edge start vertex x, y
reg  signed [17:0] ea0 = 0, ea1 = 0, ea2 = 0, eb0 = 0, eb1 = 0, eb2 = 0;   // Ya - Yb, Xb - Xa
reg                tl0 = 0, tl1 = 0, tl2 = 0;                             // top or left edge
reg  signed [35:0] p10 = 0, p11 = 0, p12 = 0, p20 = 0, p21 = 0, p22 = 0;
reg  signed [39:0] se0 = 0, se1 = 0, se2 = 0;                             // at the first pixel
reg  signed [39:0] sbot0 = 0, sbot1 = 0, sbot2 = 0;                             // at the first column's last pixel
wire [3:0]  colh = symax[3:0] - symin[3:0];                               // the box's lines in the band, less one
reg  signed [17:0] sdx = 0, sdy = 0;          // first pixel from vertex 0

// ---------------------------------------------------------------- dividers: G = trunc(N * 2^20 / A2)
wire        dv_go = (us == U_BOX);
wire [11:0] hr1, hr2, hr3, hr4;
wire [2:0]  dv_skip = &hr4 ? 3'd4 : &hr3 ? 3'd3 : &hr2 ? 3'd2 : &hr1 ? 3'd1 : 3'd0;   // what all can skip
wire [36:0] a2m = A2[36] ? -A2 : A2;
wire [11:0] dv_done;
wire [63:0] dq [0:11];
wire signed [63:0] nsh [0:11];                 // N * 2^20; the texture planes N * 2^8
assign nsh[0] = {{20{zNx[43]}}, zNx} <<< 20;  assign nsh[1] = {{20{zNy[43]}}, zNy} <<< 20;
assign nsh[2] = {{36{rNx[27]}}, rNx} <<< 20;  assign nsh[3] = {{36{rNy[27]}}, rNy} <<< 20;
assign nsh[4] = {{36{gNx[27]}}, gNx} <<< 20;  assign nsh[5] = {{36{gNy[27]}}, gNy} <<< 20;
assign nsh[6] = {{36{bNx[27]}}, bNx} <<< 20;  assign nsh[7] = {{36{bNy[27]}}, bNy} <<< 20;
assign nsh[8] = {{12{sNx[51]}}, sNx} <<< 8;    assign nsh[9] = {{12{sNy[51]}}, sNy} <<< 8;
assign nsh[10] = {{12{tNx[51]}}, tNx} <<< 8;   assign nsh[11] = {{12{tNy[51]}}, tNy} <<< 8;

genvar gi;
generate for (gi = 0; gi < 12; gi = gi + 1) begin : div
    gfx_div64 d_u (.clk(clk), .go(dv_go), .skip(dv_skip), .n(nsh[gi]), .d(a2m), .dneg(A2[36]), .q(dq[gi]), .done(dv_done[gi]));
    // bytes at this numerator's top that hold only its sign (a texture plane's count only if textured)
    assign hr1[gi] = (gi >= 8 && !tx) || nsh[gi][63:55] == {9{nsh[gi][63]}};
    assign hr2[gi] = (gi >= 8 && !tx) || nsh[gi][63:47] == {17{nsh[gi][63]}};
    assign hr3[gi] = (gi >= 8 && !tx) || nsh[gi][63:39] == {25{nsh[gi][63]}};
    assign hr4[gi] = (gi >= 8 && !tx) || nsh[gi][63:31] == {33{nsh[gi][63]}};
end endgenerate

// ---------------------------------------------------------------- start values: shift-add, 18 steps (as stage 4a)
// Bit k of dx and dy adds the gradients times 2^k (bit 17, the sign, subtracts); each value wraps as its register
// does. mz and mr gather the z and red sums alone, for the probe.
reg  [4:0]  sm_n = 5'd0;
reg  [47:0] sz = 0, ss = 0, st = 0;
reg  [31:0] sr = 0, sg = 0, sb = 0;
reg  [31:0] mz = 0, mr = 0;
wire        bx = sdx[sm_n], by = sdy[sm_n];
wire        last = (sm_n == 5'd17);            // the sign bit: its weight is negative
wire [47:0] tz = (bx ? dq[0][47:0] << sm_n : 48'd0) + (by ? dq[1][47:0] << sm_n : 48'd0);
wire [31:0] tr = (bx ? dq[2][31:0] << sm_n : 32'd0) + (by ? dq[3][31:0] << sm_n : 32'd0);
wire [31:0] tg = (bx ? dq[4][31:0] << sm_n : 32'd0) + (by ? dq[5][31:0] << sm_n : 32'd0);
wire [31:0] tb = (bx ? dq[6][31:0] << sm_n : 32'd0) + (by ? dq[7][31:0] << sm_n : 32'd0);
wire [47:0] tsv = (bx ? dq[8][47:0] << sm_n : 48'd0) + (by ? dq[9][47:0] << sm_n : 48'd0);
wire [47:0] ttv = (bx ? dq[10][47:0] << sm_n : 48'd0) + (by ? dq[11][47:0] << sm_n : 48'd0);

// ---------------------------------------------------------------- the record handed to the rasterizer
reg         rec_v = 1'b0;
reg  [9:0]  rxmin = 0, rxmax = 0;
reg  [8:0]  rymin = 0, rymax = 0;
reg  signed [39:0] re0 = 0, re1 = 0, re2 = 0, rb0 = 0, rb1 = 0, rb2 = 0;
reg  signed [21:0] rex0 = 0, rex1 = 0, rex2 = 0, rey0 = 0, rey1 = 0, rey2 = 0;
reg  [47:0] rz = 0, rzx = 0, rzy = 0;
reg  [31:0] rr = 0, rrx = 0, rry = 0, rg = 0, rgx = 0, rgy = 0, rb = 0, rbx = 0, rby = 0;
reg  [47:0] rs = 0, rsx = 0, rsy = 0, rt = 0, rtx = 0, rty = 0;
reg         rtex = 1'b0;
reg  [5:0]  rslot = 6'd0;
reg  [3:0]  rk = 4'd0;
reg  signed [15:0] rC = 16'sd0;
reg  [1:0]  rmode = 2'd0;
reg         rnofog = 1'b0;
reg  [7:0]  ralpha = 8'd0;
reg         take = 1'b0;                      // the rasterizer copies the record

// ---------------------------------------------------------------- setup probe: the record last handed over, low 32 bits.
// pr_sel 0 dx, 1 dy (the first pixel from vertex 0, 1/16 pixel), 2 and 3 the z gradients (a 1/16 pixel step),
// 4 the z sum dx dz/dx + dy dz/dy, 5 the z start, 6 the first pixel {y, x}, 7 the red sum
reg  signed [17:0] pr_dx = 0, pr_dy = 0;
reg  [31:0] pr_mz = 0, pr_mr = 0;
assign pr_q = (pr_sel == 3'd0) ? {{14{pr_dx[17]}}, pr_dx} : (pr_sel == 3'd1) ? {{14{pr_dy[17]}}, pr_dy} :
              (pr_sel == 3'd2) ? rzx[35:4] : (pr_sel == 3'd3) ? rzy[35:4] :
              (pr_sel == 3'd4) ? pr_mz : (pr_sel == 3'd5) ? rz[31:0] :
              (pr_sel == 3'd6) ? {7'd0, rymin, 6'd0, rxmin} : pr_mr;

always @(posedge clk) begin
    if (take) rec_v <= 1'b0;
    if (!wf && t_valid) begin                                    // words in
        {w0, w1, w2, w3, w4, w5, w6, w7, w8, w9, w10} <= {w1, w2, w3, w4, w5, w6, w7, w8, w9, w10, w11};
        w11 <= t_data;
        if (wn == (rec12 ? 4'd11 : 4'd8)) begin wn <= 4'd0; wf <= 1'b1; end else wn <= wn + 1'b1;
    end
    if (rst) begin
        us <= U_IN; wn <= 4'd0; wf <= 1'b0; rec_v <= 1'b0;
    end else case (us)
    U_IN: if (wf) begin                                          // vertices, differences; the words are free again
        wf <= 1'b0;
        X0 <= {{2{v0xy[15]}}, v0xy[15:0]}; Y0 <= {{2{v0xy[31]}}, v0xy[31:16]};
        X1 <= {{2{v1xy[15]}}, v1xy[15:0]}; Y1 <= {{2{v1xy[31]}}, v1xy[31:16]};
        X2 <= {{2{v2xy[15]}}, v2xy[15:0]}; Y2 <= {{2{v2xy[31]}}, v2xy[31:16]};
        dX1 <= $signed({{2{v1xy[15]}}, v1xy[15:0]}) - $signed({{2{v0xy[15]}}, v0xy[15:0]});
        dY1 <= $signed({{2{v1xy[31]}}, v1xy[31:16]}) - $signed({{2{v0xy[31]}}, v0xy[31:16]});
        dX2 <= $signed({{2{v2xy[15]}}, v2xy[15:0]}) - $signed({{2{v0xy[15]}}, v0xy[15:0]});
        dY2 <= $signed({{2{v2xy[31]}}, v2xy[31:16]}) - $signed({{2{v0xy[31]}}, v0xy[31:16]});
        zd1 <= $signed({1'b0, v1z[23:0]}) - $signed({1'b0, v0z[23:0]});
        zd2 <= $signed({1'b0, v2z[23:0]}) - $signed({1'b0, v0z[23:0]});
        rd1 <= $signed({1'b0, v1c[23:16]}) - $signed({1'b0, v0c[23:16]});
        rd2 <= $signed({1'b0, v2c[23:16]}) - $signed({1'b0, v0c[23:16]});
        gd1 <= $signed({1'b0, v1c[15:8]}) - $signed({1'b0, v0c[15:8]});
        gd2 <= $signed({1'b0, v2c[15:8]}) - $signed({1'b0, v0c[15:8]});
        bd1 <= $signed({1'b0, v1c[7:0]}) - $signed({1'b0, v0c[7:0]});
        bd2 <= $signed({1'b0, v2c[7:0]}) - $signed({1'b0, v0c[7:0]});
        z0 <= v0z[23:0]; r0 <= v0c[23:16]; g0 <= v0c[15:8]; b0 <= v0c[7:0];
        two <= v0z[24]; mode <= v1z[25:24]; nofog <= v1z[26]; alpha <= v2z[31:24];
        tx <= rec12 && v0z[25]; slot <= v0z[31:26];
        q0 <= 25'h1000000 - {1'b0, v0z[23:0]}; q1 <= 25'h1000000 - {1'b0, v1z[23:0]}; q2 <= 25'h1000000 - {1'b0, v2z[23:0]};
        U0 <= w3[15:0]; V0 <= w3[31:16]; U1 <= w7[15:0]; V1 <= w7[31:16]; U2 <= w11[15:0]; V2 <= w11[31:16];
        us <= (rec12 && v0z[25]) ? U_TQ : U_MUL;
    end
    U_TQ: begin tk <= kq; us <= U_TS; end                        // q to 16 bits
    U_TS: begin
        qn0 <= (qs0[15:0] == 16'd0) ? 16'd1 : qs0[15:0];
        qn1 <= (qs1[15:0] == 16'd0) ? 16'd1 : qs1[15:0];
        qn2 <= (qs2[15:0] == 16'd0) ? 16'd1 : qs2[15:0];
        us <= U_TP;
    end
    U_TP: begin                                                  // U q, V q (DSP)
        S0 <= U0 * qn0; S1 <= U1 * qn1; S2 <= U2 * qn2;
        T0 <= V0 * qn0; T1 <= V1 * qn1; T2 <= V2 * qn2;
        lgq <= lgw({32'd0, qn0}) + lgw({32'd0, qn1}) + lgw({32'd0, qn2});
        us <= U_TD;
    end
    U_TD: begin
        sd1 <= $signed({1'b0, S1}) - $signed({1'b0, S0}); sd2 <= $signed({1'b0, S2}) - $signed({1'b0, S0});
        td1 <= $signed({1'b0, T1}) - $signed({1'b0, T0}); td2 <= $signed({1'b0, T2}) - $signed({1'b0, T0});
        ud1 <= $signed({1'b0, U1}) - $signed({1'b0, U0}); ud2 <= $signed({1'b0, U2}) - $signed({1'b0, U0});
        vd1 <= $signed({1'b0, V1}) - $signed({1'b0, V0}); vd2 <= $signed({1'b0, V2}) - $signed({1'b0, V0});
        us <= U_MUL;
    end
    U_MUL: begin                                                 // products (DSP)
        pa  <= dX1 * dY2;  pb  <= dX2 * dY1;
        zn1 <= zd1 * dY2;  zn2 <= zd2 * dY1;  zn3 <= zd2 * dX1;  zn4 <= zd1 * dX2;
        rn1 <= rd1 * dY2;  rn2 <= rd2 * dY1;  rn3 <= rd2 * dX1;  rn4 <= rd1 * dX2;
        gn1 <= gd1 * dY2;  gn2 <= gd2 * dY1;  gn3 <= gd2 * dX1;  gn4 <= gd1 * dX2;
        bn1 <= bd1 * dY2;  bn2 <= bd2 * dY1;  bn3 <= bd2 * dX1;  bn4 <= bd1 * dX2;
        sn1 <= sd1 * dY2;  sn2 <= sd2 * dY1;  sn3 <= sd2 * dX1;  sn4 <= sd1 * dX2;
        tn1 <= td1 * dY2;  tn2 <= td2 * dY1;  tn3 <= td2 * dX1;  tn4 <= td1 * dX2;
        uvp1 <= ud1 * vd2; uvp2 <= ud2 * vd1;
        xlo <= (X0 < X1) ? ((X0 < X2) ? X0 : X2) : ((X1 < X2) ? X1 : X2);
        xhi <= (X0 > X1) ? ((X0 > X2) ? X0 : X2) : ((X1 > X2) ? X1 : X2);
        ylo <= (Y0 < Y1) ? ((Y0 < Y2) ? Y0 : Y2) : ((Y1 < Y2) ? Y1 : Y2);
        yhi <= (Y0 > Y1) ? ((Y0 > Y2) ? Y0 : Y2) : ((Y1 > Y2) ? Y1 : Y2);
        us <= U_SUB;
    end
    U_SUB: begin                                                 // area, plane numerators
        A2  <= pa - pb;
        zNx <= zn1 - zn2;  zNy <= zn3 - zn4;
        rNx <= rn1 - rn2;  rNy <= rn3 - rn4;
        gNx <= gn1 - gn2;  gNy <= gn3 - gn4;
        bNx <= bn1 - bn2;  bNy <= bn3 - bn4;
        sNx <= sn1 - sn2;  sNy <= sn3 - sn4;
        tNx <= tn1 - tn2;  tNy <= tn3 - tn4;
        auv <= (uvp1 > uvp2) ? uvp1 - uvp2 : uvp2 - uvp1;
        us <= U_BOX;
    end
    U_BOX: begin                                                 // drop, clip; dividers start here
        if (A2 == 37'sd0 || (A2 < 37'sd0 && !two) || cx0 > cx1 || cy0 > cy1)
            us <= U_IN;
        else begin
            sxmin <= cx0[9:0]; sxmax <= cx1[9:0]; symin <= cy0[8:0]; symax <= cy1[8:0];
            PX <= {cx0[13:0], 4'd8}; PY <= {cy0[13:0], 4'd8};
            va0 <= A2[36] ? X2 : X1;  wa0 <= A2[36] ? Y2 : Y1;
            va1 <= A2[36] ? X1 : X2;  wa1 <= A2[36] ? Y1 : Y2;
            va2 <= X0;                wa2 <= Y0;
            ea0 <= A2[36] ? Y2 - Y1 : Y1 - Y2;   eb0 <= A2[36] ? X1 - X2 : X2 - X1;
            ea1 <= A2[36] ? Y1 - Y0 : Y2 - Y0;   eb1 <= A2[36] ? X0 - X1 : X0 - X2;
            ea2 <= A2[36] ? Y0 - Y2 : Y0 - Y1;   eb2 <= A2[36] ? X2 - X0 : X1 - X0;
            us <= U_EDG;
        end
    end
    U_EDG: begin                                                 // top-left rule; distances
        tC <= (auv == 35'd0) ? -16'sd32768 : $signed({6'd0, lgw({13'd0, auv})}) + $signed({6'd0, lgq}) - $signed({6'd0, lgw({11'd0, a2m})});
        tl0 <= (ea0 > 18'sd0) || (ea0 == 18'sd0 && eb0 > 18'sd0);
        tl1 <= (ea1 > 18'sd0) || (ea1 == 18'sd0 && eb1 > 18'sd0);
        tl2 <= (ea2 > 18'sd0) || (ea2 == 18'sd0 && eb2 > 18'sd0);
        va0 <= PX - va0;  va1 <= PX - va1;  va2 <= PX - va2;
        wa0 <= PY - wa0;  wa1 <= PY - wa1;  wa2 <= PY - wa2;
        sdx <= PX - X0; sdy <= PY - Y0;
        us <= U_EMU;
    end
    U_EMU: begin                                                 // edge terms (DSP)
        p10 <= ea0 * va0;  p11 <= ea1 * va1;  p12 <= ea2 * va2;
        p20 <= eb0 * wa0;  p21 <= eb1 * wa1;  p22 <= eb2 * wa2;
        us <= U_ESUM;
    end
    U_ESUM: begin
        se0 <= p10 + p20 - (tl0 ? 40'sd0 : 40'sd1);
        se1 <= p11 + p21 - (tl1 ? 40'sd0 : 40'sd1);
        se2 <= p12 + p22 - (tl2 ? 40'sd0 : 40'sd1);
        us <= U_DIV;
    end
    U_DIV: if (&dv_done) begin                                   // start values from vertex 0
        sz <= {z0, 20'd0} + 48'h80000;
        sr <= {4'd0, r0, 20'd0} + 32'h80000;
        sg <= {4'd0, g0, 20'd0} + 32'h80000;
        sb <= {4'd0, b0, 20'd0} + 32'h80000;
        ss <= {8'd0, S0, 8'd0} + 48'h80; st <= {8'd0, T0, 8'd0} + 48'h80;
        mz <= 32'd0; mr <= 32'd0;
        sm_n <= 5'd0;
        us <= U_START;
    end
    U_START: begin
        sbot0 <= se0 + $signed({eb0, 4'd0}) * $signed({1'b0, colh});
        sbot1 <= se1 + $signed({eb1, 4'd0}) * $signed({1'b0, colh});
        sbot2 <= se2 + $signed({eb2, 4'd0}) * $signed({1'b0, colh});
        sz <= last ? sz - tz : sz + tz;
        sr <= last ? sr - tr : sr + tr;
        sg <= last ? sg - tg : sg + tg;
        sb <= last ? sb - tb : sb + tb;
        ss <= last ? ss - tsv : ss + tsv;
        st <= last ? st - ttv : st + ttv;
        mz <= last ? mz - tz[31:0] : mz + tz[31:0];
        mr <= last ? mr - tr : mr + tr;
        if (last) us <= U_DONE; else sm_n <= sm_n + 1'b1;
    end
    default: if (!rec_v || take) begin                           // U_DONE: hand the record over
        rec_v <= 1'b1;
        rxmin <= sxmin; rxmax <= sxmax; rymin <= symin; rymax <= symax;
        re0 <= se0; re1 <= se1; re2 <= se2; rb0 <= sbot0; rb1 <= sbot1; rb2 <= sbot2;
        rex0 <= {ea0, 4'd0}; rex1 <= {ea1, 4'd0}; rex2 <= {ea2, 4'd0};
        rey0 <= {eb0, 4'd0}; rey1 <= {eb1, 4'd0}; rey2 <= {eb2, 4'd0};
        rz <= sz; rzx <= {dq[0][43:0], 4'd0}; rzy <= {dq[1][43:0], 4'd0};
        rr <= sr; rrx <= {dq[2][27:0], 4'd0}; rry <= {dq[3][27:0], 4'd0};
        rg <= sg; rgx <= {dq[4][27:0], 4'd0}; rgy <= {dq[5][27:0], 4'd0};
        rb <= sb; rbx <= {dq[6][27:0], 4'd0}; rby <= {dq[7][27:0], 4'd0};
        rs <= ss; rsx <= {dq[8][43:0], 4'd0}; rsy <= {dq[9][43:0], 4'd0};
        rt <= st; rtx <= {dq[10][43:0], 4'd0}; rty <= {dq[11][43:0], 4'd0};
        rtex <= tx; rslot <= slot; rk <= tk; rC <= tC;
        rmode <= mode; rnofog <= nofog; ralpha <= alpha;
        pr_dx <= sdx; pr_dy <= sdy; pr_mz <= mz; pr_mr <= mr;
        us <= U_IN;
    end
    endcase
end

// ---------------------------------------------------------------- rasterizer: one pixel a clock
localparam [1:0] OPAQUE = 2'd0, CUTOUT = 2'd1;
reg         r_on = 1'b0;
reg  [1:0]  gap = 2'd0;
reg         drain = 1'b0;                     // the last triangle was a cut-out: its depths come late
reg  [9:0]  x = 0, xmax = 0;
reg  [8:0]  y = 0, ymin = 0, ymax = 0;
reg  [13:0] la = 0, pa_ = 0;                  // address of the column's first pixel, of this pixel
reg  signed [39:0] e0 = 0, e1 = 0, e2 = 0, l0 = 0, l1 = 0, l2 = 0;
reg  signed [39:0] lb0 = 0, lb1 = 0, lb2 = 0;   // the edges at the column's last pixel
reg         been = 1'b0;                      // inside the triangle earlier in this column
reg  signed [21:0] x0s = 0, x1s = 0, x2s = 0, y0s = 0, y1s = 0, y2s = 0;
reg  [47:0] az = 0, lz = 0, azx = 0, azy = 0;
reg  [31:0] ar = 0, lr = 0, arx = 0, ary = 0, ag = 0, lg = 0, agx = 0, agy = 0,
            ab = 0, lb = 0, abx = 0, aby = 0;
reg  [47:0] as_ = 0, ls_ = 0, asx = 0, asy = 0, at_ = 0, lt_ = 0, atx = 0, aty = 0;
reg         ptex = 1'b0;                      // the triangle being drawn: texture, mode
reg  [5:0]  pslot = 6'd0;
reg  [3:0]  pk = 4'd0;
reg  signed [15:0] pC = 16'sd0;
reg  [10:0] pfx = 11'd0;                      // {mode, no haze, alpha}

reg         p1 = 1'b0, p2 = 1'b0;             // pixel pipeline: inside, then depth test
reg  [13:0] p1a = 0, p2a = 0;
reg  [23:0] p1z = 0, p2z = 0, p1c = 0, p2c = 0;
reg  [33:0] p1s = 0, p2s = 0, p1t = 0, p2t = 0;
reg         p1tx = 0, p2tx = 0;
reg  [5:0]  p1sl = 0, p2sl = 0;
reg  [3:0]  p1k = 0, p2k = 0;
reg  signed [15:0] p1C = 0, p2C = 0;
reg  [10:0] p1fx = 0, p2fx = 0;

wire        inside = (e0 >= 40'sd0) && (e1 >= 40'sd0) && (e2 >= 40'sd0);
wire        miss = (l0[39] && lb0[39]) || (l1[39] && lb1[39]) || (l2[39] && lb2[39]);   // the column misses it
wire [23:0] zv = az[47] ? 24'd0 : (az[46:44] != 3'd0) ? 24'hFFFFFF : az[43:20];
wire [7:0]  rv = ar[31] ? 8'd0 : (ar[30:28] != 3'd0) ? 8'd255 : ar[27:20];
wire [7:0]  gv = ag[31] ? 8'd0 : (ag[30:28] != 3'd0) ? 8'd255 : ag[27:20];
wire [7:0]  bv = ab[31] ? 8'd0 : (ab[30:28] != 3'd0) ? 8'd255 : ab[27:20];
wire [33:0] sv = as_[47] ? 34'd0 : (as_[46:42] != 5'd0) ? 34'h3FFFFFFFF : as_[41:8];   // max(0, S >> 8)
wire [33:0] tv = at_[47] ? 34'd0 : (at_[46:42] != 5'd0) ? 34'h3FFFFFFFF : at_[41:8];

// the depth test; everything here moves with gfx_tex. While it waits the depth RAM reads the
// waiting pixel's place again, so the test stays its own
wire        pass = p2 && (p2z < bz_q);
wire        adv, tex_busy;
assign bz_a = adv ? p1a : p2a;

// an opaque pixel's depth is written as it goes on; a cut-out's comes back from gfx_tex
reg         fw = 1'b0;
reg  [13:0] fw_a = 14'd0;
reg  [23:0] fw_z = 24'd0;
wire        t_zw;
wire [23:0] t_z;
assign bw   = fw || (cw && t_zw);
assign bw_a = fw ? fw_a : cw_a;
assign bw_z = fw ? fw_z : t_z;

// the band row times 640, for the record's first line
wire [3:0]  row0 = rymin[3:0] - band_y0[3:0];
wire [13:0] ra0 = {row0, 9'd0} + {row0, 7'd0} + {4'd0, rxmin};

assign idle = (us == U_IN) && !wf && wn == 4'd0 && !rec_v && !r_on && !p1 && !p2 && !fw && !tex_busy;

always @(posedge clk) begin
    take <= 1'b0;
    fw <= pass && adv && p2fx[10:9] == OPAQUE; fw_a <= p2a; fw_z <= p2z;
    if (rst) begin
        r_on <= 1'b0; gap <= 2'd0; p1 <= 1'b0; p2 <= 1'b0; drain <= 1'b0;
    end else if (adv) begin
        p1 <= 1'b0;
        if (!r_on) begin
            if (gap != 2'd0) gap <= gap - 1'b1;
            else if (rec_v && !take && !(drain && (p1 || p2 || tex_busy))) begin   // next triangle
                take <= 1'b1; r_on <= 1'b1; drain <= (rmode == CUTOUT);
                x <= rxmin; xmax <= rxmax; y <= rymin; ymin <= rymin; ymax <= rymax;
                la <= ra0; pa_ <= ra0;
                e0 <= re0; e1 <= re1; e2 <= re2; l0 <= re0; l1 <= re1; l2 <= re2;
                lb0 <= rb0; lb1 <= rb1; lb2 <= rb2; been <= 1'b0;
                x0s <= rex0; x1s <= rex1; x2s <= rex2; y0s <= rey0; y1s <= rey1; y2s <= rey2;
                az <= rz; lz <= rz; azx <= rzx; azy <= rzy;
                ar <= rr; lr <= rr; arx <= rrx; ary <= rry;
                ag <= rg; lg <= rg; agx <= rgx; agy <= rgy;
                ab <= rb; lb <= rb; abx <= rbx; aby <= rby;
                as_ <= rs; ls_ <= rs; asx <= rsx; asy <= rsy;
                at_ <= rt; lt_ <= rt; atx <= rtx; aty <= rty;
                ptex <= rtex; pslot <= rslot; pk <= rk; pC <= rC; pfx <= {rmode, rnofog, ralpha};
            end
        end else begin
            p1 <= inside; p1a <= pa_; p1z <= zv; p1c <= {rv, gv, bv};
            p1s <= sv; p1t <= tv; p1tx <= ptex; p1sl <= pslot; p1k <= pk; p1C <= pC; p1fx <= pfx;
            if (y == ymax || (y == ymin && miss) || (been && !inside)) begin   // next column
                if (x == xmax) begin r_on <= 1'b0; gap <= 2'd3; end
                y <= ymin; x <= x + 1'b1; been <= 1'b0;
                la <= la + 1'b1; pa_ <= la + 1'b1;
                e0 <= l0 + x0s; e1 <= l1 + x1s; e2 <= l2 + x2s;
                l0 <= l0 + x0s; l1 <= l1 + x1s; l2 <= l2 + x2s;
                lb0 <= lb0 + x0s; lb1 <= lb1 + x1s; lb2 <= lb2 + x2s;
                az <= lz + azx; lz <= lz + azx;
                ar <= lr + arx; lr <= lr + arx;
                ag <= lg + agx; lg <= lg + agx;
                ab <= lb + abx; lb <= lb + abx;
                as_ <= ls_ + asx; ls_ <= ls_ + asx;
                at_ <= lt_ + atx; lt_ <= lt_ + atx;
            end else begin
                y <= y + 1'b1; pa_ <= pa_ + 14'd640; been <= been | inside;
                e0 <= e0 + y0s; e1 <= e1 + y1s; e2 <= e2 + y2s;
                az <= az + azy; ar <= ar + ary; ag <= ag + agy; ab <= ab + aby;
                as_ <= as_ + asy; at_ <= at_ + aty;
            end
        end
        p2 <= p1; p2a <= p1a; p2z <= p1z; p2c <= p1c;
        p2s <= p1s; p2t <= p1t; p2tx <= p1tx; p2sl <= p1sl; p2k <= p1k; p2C <= p1C; p2fx <= p1fx;
    end
end

// ---------------------------------------------------------------- colours: through the pixel unit
gfx_tex tex_u (
    .clk(clk), .rst(rst), .fog_on(fog_on), .i_v(pass), .i_ready(adv), .i_a(p2a), .i_lit(p2c), .i_tex(p2tx),
    .i_slot(p2sl), .i_k(p2k), .i_c(p2C), .i_s(p2s), .i_t(p2t), .i_z(p2z),
    .i_mode(p2fx[10:9]), .i_nofog(p2fx[8]), .i_alpha(p2fx[7:0]), .cr_a(cr_a), .cr_q(cr_q),
    .o_w(cw), .o_a(cw_a), .o_c(cw_c), .o_zw(t_zw), .o_z(t_z), .d_we(d_we), .d_a(d_a), .d_d(d_d),
    .m_req(m_req), .m_addr(m_addr), .m_len(m_len), .m_rdy(m_rdy), .m_valid(m_valid), .m_data(m_data),
    .busy(tex_busy));

endmodule

// ============================================================================
// gfx_div64 -- q = trunc(n / (dneg ? -d : d)); n signed, d the divisor's
// magnitude. Restoring division on magnitudes, two bits a clock, then the sign.
// skip (0..4): so many bytes at the top of n hold no more than its sign (the
// top 8 skip + 1 bits are alike); their quotient bits are zero and are not
// worked. done 32 - 4 skip clocks after go.
// ============================================================================
module gfx_div64 (
    input  wire        clk,
    input  wire        go,
    input  wire [2:0]  skip,
    input  wire [63:0] n,
    input  wire [36:0] d,
    input  wire        dneg,
    output wire [63:0] q,
    output wire        done
);

reg  [5:0]  cnt = 6'd0;
reg  [37:0] rem = 38'd0;
reg  [63:0] num = 64'd0, quo = 64'd0;
reg  [36:0] dv = 37'd0;
reg         neg = 1'b0;
wire [63:0] mag = n[63] ? -n : n;
wire [37:0] t1 = {rem[36:0], num[63]};                 // the first bit's step
wire        g1 = (t1 >= {1'b0, dv});
wire [37:0] r1 = g1 ? t1 - {1'b0, dv} : t1;
wire [37:0] t2 = {r1[36:0], num[62]};                  // the second's
wire        g2 = (t2 >= {1'b0, dv});

assign done = (cnt == 6'd0);
assign q = neg ? -quo : quo;

always @(posedge clk)
    if (go) begin
        case (skip)
        3'd0:    num <= mag;
        3'd1:    num <= {mag[55:0], 8'd0};
        3'd2:    num <= {mag[47:0], 16'd0};
        3'd3:    num <= {mag[39:0], 24'd0};
        default: num <= {mag[31:0], 32'd0};
        endcase
        dv <= d; neg <= n[63] ^ dneg;
        rem <= 38'd0; quo <= 64'd0; cnt <= 6'd32 - {1'b0, skip, 2'd0};
    end else if (cnt != 6'd0) begin
        rem <= g2 ? t2 - {1'b0, dv} : t2;
        num <= {num[61:0], 2'd0};
        quo <= {quo[61:0], g1, g2};
        cnt <= cnt - 1'b1;
    end

endmodule
`default_nettype wire

`timescale 1ns/1ps
`default_nettype none

// ============================================================================
// gfx_tex -- GFX4 pixel unit (stages 4a, 5a): textures, haze and blending.
// Pixels that passed the depth test come in order; each leaves, in order, as
// a colour for the band buffer. Bit-exact with tools/g4_raster.py and
// g4_tex.py (C24, C24A, lines):
//   qp = max(1, (2^24 - z) >> k); u = (s << 4) / qp, v = (t << 4) / qp
//   (1/256 texel of level 0, at most 2^20 - 1); level = clamp((C - 3 lg qp)
//   >> 5, 0, levels - 1); bilinear on texels that wrap, alpha as the colours;
//   src = min(255, (c lit + 64) >> 7), untextured: lit, alpha 255
//   haze (fog_on, unless the pixel says no): F = table[{e, m}], e the place
//   of q's leading one (q = 2^24 - z), m the 5 bits after it;
//   src += ((fog colour - src) F' + 128) >> 8, F' = F + (F >> 7)
//   A = (texture alpha (alpha + (alpha >> 7)) + 128) >> 8, A' = A + (A >> 7)
//   opaque: src. Cut-out, blend: dst + (((src - dst) A' + 128) >> 8), the
//   cut-out's depth stored too if A >= 128. Add: min(255, dst + ((src A' +
//   128) >> 8)), its haze towards black.
// Cache: 256 lines of 8x8 texels (RGBA), decoded, direct-mapped, in four banks
// by texel parity, so a footprint's four texels come in one clock wherever it
// falls. Lines are fetched ahead: the lookup (S24) checks the tags and takes a
// missing line's place at once (its tag written, its fill queued; a clock a
// line), then queues the pixel (512 places) with the number of fills queued so
// far. Up to 16 fills are out: asked for in turn (12 words, C24A 16), their
// words kept as they come, each decoded into the banks (17 clocks, four texels
// a clock) once the pixels queued before the one that wanted it have read the
// banks. A pixel leaves the queue once its fills are in. The lookup holds the
// pixels before it (i_ready low) only while the queue or the fills are full.
// From the queue on nothing waits: a pixel's colour is read 5 clocks after it
// leaves and written 3 clocks later, so it leaves no sooner than 4 clocks after
// one at the same place.
// Registers (d_a the CPU's device address / 4): 0x080-0x0FF the texture
// table, two words a slot: the base, then {alpha (bit 12), levels, lh, lw};
// 0x014 haze colour, 0x015 haze table pointer (words), 0x016 four table
// bytes at the pointer, which then steps on.
// ============================================================================
module gfx_tex (
    input  wire        clk,
    input  wire        rst,
    input  wire        fog_on,                // haze (GPU_MODE bit 1)
    input  wire        i_v,                   // a pixel
    output wire        i_ready,               // taken this clock when i_v
    input  wire [13:0] i_a,                   // its place in the band buffer
    input  wire [23:0] i_lit,                 // Gouraud colour
    input  wire        i_tex,                 // textured: slot, k, c, s, t matter
    input  wire [5:0]  i_slot,
    input  wire [3:0]  i_k,
    input  wire signed [15:0] i_c,            // the triangle's level constant
    input  wire [33:0] i_s,                   // max(0, S >> 8)
    input  wire [33:0] i_t,
    input  wire [23:0] i_z,
    input  wire [1:0]  i_mode,                // 0 opaque, 1 cut-out, 2 blend, 3 add
    input  wire        i_nofog,
    input  wire [7:0]  i_alpha,
    output wire [13:0] cr_a,                  // the band buffer's colour there: a clock later
    input  wire [23:0] cr_q,
    output reg         o_w = 1'b0,            // a colour for the band buffer
    output reg  [13:0] o_a = 14'd0,
    output reg  [23:0] o_c = 24'd0,
    output reg         o_zw = 1'b0,           // with o_w: the depth too (a cut-out's solid pixel)
    output reg  [23:0] o_z = 24'd0,
    input  wire        d_we,                  // registers
    input  wire [8:0]  d_a,
    input  wire [31:0] d_d,
    output reg         m_req = 1'b0,          // a line: m_len words from m_addr
    output reg  [22:0] m_addr = 23'd0,
    output wire [4:0]  m_len,
    input  wire        m_rdy,                 // the request taken
    input  wire        m_valid,               // a word of it
    input  wire [31:0] m_data,
    output wire        busy                   // pixels inside or a line coming
);

localparam [1:0] OPAQUE = 2'd0, CUTOUT = 2'd1, ADD = 2'd3;
localparam DIV = 20;                          // quotient bits

// ---------------------------------------------------------------- texture table, haze registers
reg  [22:0] t_base [0:63];
reg  [12:0] t_desc [0:63];
reg  [31:0] fogm [0:255];                     // the haze table, four bytes a word
reg  [7:0]  fog_p = 8'd0;
reg  [23:0] fog_c = 24'd0;
wire        tab_we = d_we && d_a[8:7] == 2'b01;
always @(posedge clk) begin
    if (tab_we) begin
        if (d_a[0]) t_desc[d_a[6:1]] <= d_d[12:0];
        else        t_base[d_a[6:1]] <= d_d[22:0];
    end
    if (d_we && d_a == 9'h014) fog_c <= d_d[23:0];
    if (d_we && d_a == 9'h015) fog_p <= d_d[7:0];
    if (d_we && d_a == 9'h016) begin fogm[fog_p] <= d_d; fog_p <= fog_p + 1'b1; end
end

// ---------------------------------------------------------------- the front's pace (S1 .. S24)
wire fadv;
assign i_ready = fadv;

// ---------------------------------------------------------------- S1: qp, the dividers' start
reg         s1 = 1'b0, s1_tex = 1'b0;
reg  [13:0] s1_a = 0;
reg  [23:0] s1_lit = 0;
reg  [5:0]  s1_slot = 0;
reg  signed [15:0] s1_c = 0;
reg  [15:0] s1_q = 16'd1;
reg  [37:0] s1_du = 0, s1_dv = 0;
reg  [34:0] s1_e = 0;                         // {z, mode, no haze, alpha}: needed at the end
wire [24:0] q25 = 25'h1000000 - {1'b0, i_z};
wire [24:0] qsh = q25 >> i_k;
always @(posedge clk)
    if (rst) s1 <= 1'b0;
    else if (fadv) begin
        s1 <= i_v; s1_tex <= i_tex; s1_a <= i_a; s1_lit <= i_lit; s1_slot <= i_slot; s1_c <= i_c;
        s1_q <= (qsh[24:16] != 0) ? 16'hFFFF : (qsh[15:0] == 16'd0 ? 16'd1 : qsh[15:0]);
        s1_du <= {i_s, 4'd0}; s1_dv <= {i_t, 4'd0};
        s1_e <= {i_z, i_mode, i_nofog, i_alpha};
    end

// those travel beside the stages: 23 steps on they are with x2
localparam EN = 23;
reg  [34:0] ed [0:EN-1];
integer e;
always @(posedge clk)
    if (fadv) begin
        ed[0] <= s1_e;
        for (e = 1; e < EN; e = e + 1) ed[e] <= ed[e-1];
    end
wire [34:0] x2_e = ed[EN-1];

// lg(x) = 16 (bits(x) - 1) + LG[the 4 bits after the leading one]
function [7:0] lg16;
    input [15:0] x;
    reg   [3:0]  n, m;
    reg   [15:0] y;
    integer      i;
    begin
        n = 4'd0;
        for (i = 0; i < 16; i = i + 1) if (x[i]) n = i[3:0];
        y = x;                                 // the leading one to bit 15 (constant shifts: XST)
        if (y[15:8] == 8'd0)  y = y << 8;
        if (y[15:12] == 4'd0) y = y << 4;
        if (y[15:14] == 2'd0) y = y << 2;
        if (!y[15])           y = y << 1;
        m = y[14:11];
        case (m)
            4'd0: lg16 = {n, 4'd0};  4'd1: lg16 = {n, 4'd1};  4'd2: lg16 = {n, 4'd3};  4'd3: lg16 = {n, 4'd4};
            4'd4: lg16 = {n, 4'd5};  4'd5: lg16 = {n, 4'd6};  4'd6: lg16 = {n, 4'd7};  4'd7: lg16 = {n, 4'd8};
            4'd8: lg16 = {n, 4'd9};  4'd9: lg16 = {n, 4'd10}; 4'd10: lg16 = {n, 4'd11}; 4'd11: lg16 = {n, 4'd12};
            4'd12: lg16 = {n, 4'd13}; 4'd13: lg16 = {n, 4'd14}; default: lg16 = {n, 4'd15};
        endcase
    end
endfunction

// ---------------------------------------------------------------- S2..S21: divide; the level beside it
reg         dv_v  [0:DIV];
reg         dv_tx [0:DIV];
reg  [13:0] dv_a  [0:DIV];
reg  [23:0] dv_l  [0:DIV];
reg  [5:0]  dv_sl [0:DIV];
reg  [3:0]  dv_lv [0:DIV];
reg  [15:0] dv_q  [0:DIV];
reg  [16:0] ru [0:DIV], rv [0:DIV];           // partial remainders
reg  [19:0] nu [0:DIV], nv [0:DIV];           // dividend bits still to come
reg  [19:0] qu [0:DIV], qv [0:DIV];           // quotient so far
reg         ou [0:DIV], ov [0:DIV];           // saturated
wire [7:0]  lgq = lg16(s1_q);
wire signed [17:0] l2 = {{2{s1_c[15]}}, s1_c} - {8'd0, lgq, 1'b0} - {9'd0, lgq};   // C - 3 lg
wire signed [12:0] lsh = l2 >>> 5;
wire [12:0] s1_desc = t_desc[s1_slot];
wire [3:0]  lmax = s1_desc[11:8] - 1'b1;
wire [3:0]  lvl = lsh < 0 ? 4'd0 : (lsh > $signed({9'd0, lmax})) ? lmax : lsh[3:0];
integer j;
always @(posedge clk)
    if (rst) begin
        for (j = 0; j <= DIV; j = j + 1) dv_v[j] <= 1'b0;
    end else if (fadv) begin
        dv_v[0] <= s1; dv_tx[0] <= s1_tex; dv_a[0] <= s1_a; dv_l[0] <= s1_lit; dv_sl[0] <= s1_slot; dv_lv[0] <= lvl;
        dv_q[0] <= s1_q;
        ou[0] <= (s1_du[37:20] >= {2'b00, s1_q}); ov[0] <= (s1_dv[37:20] >= {2'b00, s1_q});
        ru[0] <= {1'b0, s1_du[35:20]}; rv[0] <= {1'b0, s1_dv[35:20]};
        nu[0] <= s1_du[19:0]; nv[0] <= s1_dv[19:0]; qu[0] <= 20'd0; qv[0] <= 20'd0;
        for (j = 1; j <= DIV; j = j + 1) begin
            dv_v[j] <= dv_v[j-1]; dv_tx[j] <= dv_tx[j-1]; dv_a[j] <= dv_a[j-1]; dv_l[j] <= dv_l[j-1];
            dv_sl[j] <= dv_sl[j-1]; dv_lv[j] <= dv_lv[j-1]; dv_q[j] <= dv_q[j-1];
            ou[j] <= ou[j-1]; ov[j] <= ov[j-1];
            nu[j] <= {nu[j-1][18:0], 1'b0}; nv[j] <= {nv[j-1][18:0], 1'b0};
            if ({ru[j-1][15:0], nu[j-1][19]} >= {1'b0, dv_q[j-1]}) begin
                ru[j] <= {ru[j-1][15:0], nu[j-1][19]} - {1'b0, dv_q[j-1]}; qu[j] <= {qu[j-1][18:0], 1'b1};
            end else begin
                ru[j] <= {ru[j-1][15:0], nu[j-1][19]}; qu[j] <= {qu[j-1][18:0], 1'b0};
            end
            if ({rv[j-1][15:0], nv[j-1][19]} >= {1'b0, dv_q[j-1]}) begin
                rv[j] <= {rv[j-1][15:0], nv[j-1][19]} - {1'b0, dv_q[j-1]}; qv[j] <= {qv[j-1][18:0], 1'b1};
            end else begin
                rv[j] <= {rv[j-1][15:0], nv[j-1][19]}; qv[j] <= {qv[j-1][18:0], 1'b0};
            end
        end
    end
wire [19:0] u_q = ou[DIV] ? 20'hFFFFF : qu[DIV];
wire [19:0] v_q = ov[DIV] ? 20'hFFFFF : qv[DIV];

// ---------------------------------------------------------------- S22: the level's texel and weights
reg         x1 = 1'b0, x1_tx = 1'b0;
reg  [13:0] x1_a = 0;
reg  [23:0] x1_l = 0;
reg  [5:0]  x1_sl = 0;
reg  [3:0]  x1_lv = 0, x1_fx = 0, x1_fy = 0, x1_lw = 0, x1_lh = 0;
reg  signed [12:0] x1_x0 = 0, x1_y0 = 0;
wire [12:0] dsc = t_desc[dv_sl[DIV]];
wire [19:0] ul = u_q >> dv_lv[DIV], vl = v_q >> dv_lv[DIV];
wire signed [20:0] xs = $signed({1'b0, ul}) - 21'sd128, ys = $signed({1'b0, vl}) - 21'sd128;
always @(posedge clk)
    if (rst) x1 <= 1'b0;
    else if (fadv) begin
        x1 <= dv_v[DIV]; x1_tx <= dv_tx[DIV]; x1_a <= dv_a[DIV]; x1_l <= dv_l[DIV]; x1_sl <= dv_sl[DIV];
        x1_lv <= dv_lv[DIV];
        x1_x0 <= xs >>> 8; x1_y0 <= ys >>> 8; x1_fx <= xs[7:4]; x1_fy <= ys[7:4];
        x1_lw <= dsc[3:0] - dv_lv[DIV]; x1_lh <= dsc[7:4] - dv_lv[DIV];
    end

// ---------------------------------------------------------------- S23: the four texels, by bank
reg         x2 = 1'b0, x2_tx = 1'b0;
reg  [13:0] x2_a = 0;
reg  [23:0] x2_l = 0;
reg  [5:0]  x2_sl = 0;
reg  [3:0]  x2_lv = 0;
reg  [10:0] bx [0:3], by [0:3];               // bank b's texel (b = 2 py + px)
reg  [8:0]  bw [0:3];                         // its bilinear weight
wire [10:0] wm = (11'd1 << x1_lw) - 1'b1, hm = (11'd1 << x1_lh) - 1'b1;
wire [10:0] X0 = x1_x0[10:0] & wm, X1 = (x1_x0[10:0] + 1'b1) & wm;
wire [10:0] Y0 = x1_y0[10:0] & hm, Y1 = (x1_y0[10:0] + 1'b1) & hm;
wire [4:0]  wx0 = 5'd16 - {1'b0, x1_fx}, wy0 = 5'd16 - {1'b0, x1_fy};
integer b;
always @(posedge clk)
    if (rst) x2 <= 1'b0;
    else if (fadv) begin
        x2 <= x1; x2_tx <= x1_tx; x2_a <= x1_a; x2_l <= x1_l; x2_sl <= x1_sl; x2_lv <= x1_lv;
        for (b = 0; b < 4; b = b + 1) begin
            bx[b] <= (X0[0] == b[0]) ? X0 : X1;
            by[b] <= (Y0[0] == b[1]) ? Y0 : Y1;
            bw[b] <= ((X0[0] == b[0]) ? wx0 : {1'b0, x1_fx}) * ((Y0[0] == b[1]) ? wy0 : {1'b0, x1_fy});
        end
    end

// ---------------------------------------------------------------- S24: the tags; a missing line's place taken
localparam QA = 9;                            // the queue: 2^QA pixels
localparam FA = 4;                            // the fills: 2^FA
localparam QW = 163;                          // a pixel's bits there
wire [7:0]  lx [0:3], ly [0:3];
wire [7:0]  li [0:3];
wire [25:0] want [0:3];                       // {slot, level, lx, ly}
wire [11:0] ra [0:3];                         // bank g's texel: {line, y, x}
wire [3:0]  hit;
wire [31:0] tx [0:3];                         // the head's texels {A, R, G, B}, a clock after it leaves
wire [11:0] h_ra [0:3];
wire        go;                               // the queue's head leaves: the banks are read for it
wire        fill_we;
wire [11:0] fill_a;
wire [31:0] fill_d [0:3];
wire [1:0]  first = !hit[0] ? 2'd0 : !hit[1] ? 2'd1 : !hit[2] ? 2'd2 : 2'd3;
wire        alloc;                            // the first missing line: its tag now, its fill queued
genvar g;
generate for (g = 0; g < 4; g = g + 1) begin : look
    assign lx[g] = bx[g][10:3];
    assign ly[g] = by[g][10:3];
    assign li[g] = {ly[g][3:0] ^ x2_lv, lx[g][3:0] ^ x2_sl[3:0]};
    assign want[g] = {x2_sl, x2_lv, lx[g], ly[g]};
    assign ra[g] = {li[g], by[g][2:1], bx[g][2:1]};
    gfx_tex_bank bank_u (
        .clk(clk), .inval(tab_we || rst), .li(li[g]), .want(want[g]), .hit(hit[g]),
        .rd(go), .ra(h_ra[g]), .rq(tx[g]),
        .we(fill_we), .wa(fill_a), .wd(fill_d[g]), .tag_we(alloc), .tag_i(li[first]), .tag_d(want[first]));
end endgenerate

// the fills: taken (fq_a), asked for (fq_q), in (fq_r), decoded (fq_c); at most 2^FA taken, not decoded
reg  [FA:0] fq_a = 0, fq_q = 0, fq_r = 0, fq_c = 0;
reg  [25:0] fq_t [0:(1 << FA) - 1];           // the line {slot, level, lx, ly}
reg  [7:0]  fq_i [0:(1 << FA) - 1];           // its place
reg         fq_4 [0:(1 << FA) - 1];           // C24A: 16 words
reg  [QA:0] fq_p [0:(1 << FA) - 1];           // the queue place of the pixel that wanted it
wire [12:0] fdesc = t_desc[x2_sl];

// the queue: pixels in (q_w) and out (q_r); q_wd, q_w a clock late, is what the head's read sees
reg  [QW-1:0] qm [0:(1 << QA) - 1];
reg  [QW-1:0] qh = {QW{1'b0}};               // the head, read
reg  [QA:0]   q_w = 0, q_r = 0, q_wd = 0;
wire [QA:0]   q_n = q_w - q_r;               // pixels in it
wire          x2_miss = x2 && x2_tx && (hit != 4'b1111);
wire          enq = x2 && !x2_miss && !q_n[QA];
wire [FA:0]   fq_n = fq_a - fq_c;            // fills taken, not decoded
assign alloc = x2_miss && !fq_n[FA] && !rst;
assign fadv  = !x2 || enq;
wire [QW-1:0] q_in = {fq_a, x2_tx, x2_a, x2_l, x2_e, ra[3], ra[2], ra[1], ra[0], bw[3], bw[2], bw[1], bw[0]};

// the head leaves once the fills queued before it are in, 4 clocks or more after one at its place
wire [FA:0]   h_need = qh[162:158];
wire          h_tx = qh[157];
wire [13:0]   h_a = qh[156:143];
wire [23:0]   h_l = qh[142:119];
wire [34:0]   h_e = qh[118:84];
wire [8:0]    h_w [0:3];
generate for (g = 0; g < 4; g = g + 1) begin : head
    assign h_ra[g] = qh[36 + 12 * g +: 12];
    assign h_w[g]  = qh[9 * g +: 9];
end endgenerate
reg         r1 = 1'b0, r2 = 1'b0, r3 = 1'b0;
reg  [13:0] r1_a = 0, r2_a = 0, r3_a = 0;
assign go = (q_r != q_wd) && (h_need == fq_c) &&
            !(r1 && r1_a == h_a) && !(r2 && r2_a == h_a) && !(r3 && r3_a == h_a);
wire [QA:0] q_rn = q_r + {{QA{1'b0}}, go};

always @(posedge clk) begin
    if (enq) qm[q_w[QA-1:0]] <= q_in;
    qh <= qm[q_rn[QA-1:0]];
    if (alloc) begin
        fq_t[fq_a[FA-1:0]] <= want[first]; fq_i[fq_a[FA-1:0]] <= li[first]; fq_4[fq_a[FA-1:0]] <= fdesc[12];
        fq_p[fq_a[FA-1:0]] <= q_w;
    end
    if (rst) begin
        q_w <= 0; q_r <= 0; q_wd <= 0; fq_a <= 0;
    end else begin
        q_w <= q_w + {{QA{1'b0}}, enq}; q_r <= q_rn; q_wd <= q_w;
        if (alloc) fq_a <= fq_a + 1'b1;
    end
end

// ---------------------------------------------------------------- r1: the texels; r2, r3: bilinear
reg         r1_tx = 1'b0, r2_tx = 1'b0, r3_tx = 1'b0;
reg  [23:0] r1_l = 0, r2_l = 0, r3_l = 0;
reg  [34:0] r1_e = 0, r2_e = 0, r3_e = 0;
reg  [8:0]  r1_w [0:3];
reg  [16:0] pr [0:3][0:3];                    // bank, channel (0 B, 1 G, 2 R, 3 A)
reg  [7:0]  r3_c [0:3];
reg  [9:0]  r3_fi = 0;                        // the haze table's entry
integer c;
wire [18:0] sum0 = pr[0][0] + pr[1][0] + pr[2][0] + pr[3][0] + 19'd128;
wire [18:0] sum1 = pr[0][1] + pr[1][1] + pr[2][1] + pr[3][1] + 19'd128;
wire [18:0] sum2 = pr[0][2] + pr[1][2] + pr[2][2] + pr[3][2] + 19'd128;
wire [18:0] sum3 = pr[0][3] + pr[1][3] + pr[2][3] + pr[3][3] + 19'd128;

function [9:0] fogi;                          // {e, m}: q's leading one and the 5 bits after it
    input [23:0] z;
    reg   [24:0] q, y;
    reg   [4:0]  n;
    integer      i;
    begin
        q = 25'h1000000 - {1'b0, z};
        n = 5'd0;
        for (i = 0; i < 25; i = i + 1) if (q[i]) n = i[4:0];
        y = q;                                 // the leading one to bit 24 (constant shifts: XST)
        if (y[24:9] == 16'd0)  y = y << 16;
        if (y[24:17] == 8'd0)  y = y << 8;
        if (y[24:21] == 4'd0)  y = y << 4;
        if (y[24:23] == 2'd0)  y = y << 2;
        if (!y[24])            y = y << 1;
        fogi = {n, y[23:19]};
    end
endfunction

always @(posedge clk) begin
    r1 <= go && !rst; r1_tx <= h_tx; r1_a <= h_a; r1_l <= h_l; r1_e <= h_e;
    for (b = 0; b < 4; b = b + 1) r1_w[b] <= h_w[b];
    r2 <= r1 && !rst; r2_tx <= r1_tx; r2_a <= r1_a; r2_l <= r1_l; r2_e <= r1_e;
    for (b = 0; b < 4; b = b + 1)
        for (c = 0; c < 4; c = c + 1) pr[b][c] <= tx[b][8 * c +: 8] * r1_w[b];
    r3 <= r2 && !rst; r3_tx <= r2_tx; r3_a <= r2_a; r3_l <= r2_l; r3_e <= r2_e;
    r3_c[0] <= sum0[15:8]; r3_c[1] <= sum1[15:8]; r3_c[2] <= sum2[15:8]; r3_c[3] <= sum3[15:8];
    r3_fi <= fogi(r2_e[34:11]);
end

// ---------------------------------------------------------------- o1: light, alpha; o2: haze
function [7:0] light;                         // min(255, (c lit + 64) >> 7)
    input [7:0] cc, l;
    reg   [16:0] p;
    begin
        p = cc * l + 17'd64;
        light = p[16:15] != 2'b00 ? 8'd255 : p[14:7];
    end
endfunction
function [7:0] mix;                           // d + (((s - d) w + 128) >> 8), w 0 .. 256
    input [7:0] d, s_;
    input [8:0] w;
    reg   signed [8:0]  df;
    reg   signed [19:0] p;
    reg   signed [11:0] q;
    begin
        df = $signed({1'b0, s_}) - $signed({1'b0, d});
        p  = df * $signed({1'b0, w}) + 20'sd128;
        q  = $signed({4'd0, d}) + (p >>> 8);
        mix = q[7:0];
    end
endfunction
function [7:0] addc;                          // min(255, d + ((s w + 128) >> 8))
    input [7:0] d, s_;
    input [8:0] w;
    reg   [16:0] p;
    reg   [9:0]  q;
    begin
        p = s_ * w + 17'd128;
        q = {2'd0, d} + {1'b0, p[16:8]};
        addc = q[9:8] != 2'b00 ? 8'd255 : q[7:0];
    end
endfunction

reg         o1 = 1'b0, o2 = 1'b0;
reg  [13:0] o1_a = 0, o2_a = 0;
reg  [23:0] o1_c = 0, o2_c = 0, o1_z = 0, o2_z = 0;
reg  [1:0]  o1_m = 0, o2_m = 0, o1_fb = 0;
reg         o1_nf = 1'b0, o2_st = 1'b0;
reg  [7:0]  o1_al = 0;                        // A
reg  [8:0]  o2_al = 0;                        // A'
reg  [31:0] fog_q = 32'd0;
wire [7:0]  r3_ta = r3_tx ? r3_c[3] : 8'd255;
wire [8:0]  r3_aw = {1'b0, r3_e[7:0]} + {8'd0, r3_e[7]};
wire [16:0] r3_ap = r3_ta * r3_aw + 17'd128;
wire [7:0]  fog_f = (o1_fb == 2'd0) ? fog_q[7:0] : (o1_fb == 2'd1) ? fog_q[15:8] :
                    (o1_fb == 2'd2) ? fog_q[23:16] : fog_q[31:24];
wire [8:0]  fog_w = {1'b0, fog_f} + {8'd0, fog_f[7]};
wire [23:0] fog_t = (o1_m == ADD) ? 24'd0 : fog_c;
always @(posedge clk) begin
    fog_q <= fogm[r3_fi[9:2]];
    if (rst) begin o1 <= 1'b0; o2 <= 1'b0; end
    else begin
        o1 <= r3; o1_a <= r3_a; o1_z <= r3_e[34:11]; o1_m <= r3_e[10:9]; o1_nf <= r3_e[8]; o1_fb <= r3_fi[1:0];
        o1_c <= r3_tx ? {light(r3_c[2], r3_l[23:16]), light(r3_c[1], r3_l[15:8]), light(r3_c[0], r3_l[7:0])} : r3_l;
        o1_al <= r3_ap[15:8];
        o2 <= o1; o2_a <= o1_a; o2_z <= o1_z; o2_m <= o1_m;
        o2_c <= (fog_on && !o1_nf) ? {mix(o1_c[23:16], fog_t[23:16], fog_w), mix(o1_c[15:8], fog_t[15:8], fog_w),
                                      mix(o1_c[7:0], fog_t[7:0], fog_w)} : o1_c;
        o2_al <= {1'b0, o1_al} + {8'd0, o1_al[7]};
        o2_st <= (o1_m == CUTOUT) && o1_al[7];
    end
end

// ---------------------------------------------------------------- the band buffer's colour: read, blend, write
reg         b1 = 1'b0, b2 = 1'b0, b1_st = 1'b0, b2_st = 1'b0;
reg  [13:0] b1_a = 0, b2_a = 0;
reg  [23:0] b1_c = 0, b2_c = 0, b1_z = 0, b2_z = 0, b2_d = 0;
reg  [1:0]  b1_m = 0, b2_m = 0;
reg  [8:0]  b1_al = 0, b2_al = 0;
assign cr_a = o2_a;
always @(posedge clk) begin
    b1 <= o2 && !rst; b1_a <= o2_a; b1_c <= o2_c; b1_z <= o2_z; b1_m <= o2_m; b1_al <= o2_al; b1_st <= o2_st;
    b2 <= b1 && !rst; b2_a <= b1_a; b2_c <= b1_c; b2_z <= b1_z; b2_m <= b1_m; b2_al <= b1_al; b2_st <= b1_st;
    b2_d <= cr_q;                                                // what is there now
    o_w <= b2 && !rst; o_a <= b2_a; o_z <= b2_z; o_zw <= b2 && b2_st && !rst;
    o_c <= (b2_m == OPAQUE) ? b2_c :
           (b2_m == ADD)    ? {addc(b2_d[23:16], b2_c[23:16], b2_al), addc(b2_d[15:8], b2_c[15:8], b2_al),
                               addc(b2_d[7:0], b2_c[7:0], b2_al)}
                            : {mix(b2_d[23:16], b2_c[23:16], b2_al), mix(b2_d[15:8], b2_c[15:8], b2_al),
                               mix(b2_d[7:0], b2_c[7:0], b2_al)};
end

// ---------------------------------------------------------------- line fills: asked for, in, decoded
reg  [DIV:0] dvv;
always @(*) for (j = 0; j <= DIV; j = j + 1) dvv[j] = dv_v[j];
assign busy = s1 || (dvv != 0) || x1 || x2 || (q_w != q_r) || r1 || r2 || r3 || o1 || o2 || b1 || b2 || o_w ||
              (fq_c != fq_a);

// asking, one at a time: walk the levels before the line's own (their sizes), then the request
reg  [1:0]  rs = 2'd0;                        // 0 idle, 1 the levels, 2 asking
reg  [3:0]  fj = 4'd0;                        // levels walked so far
reg  [22:0] fo = 23'd0;                       // that level's offset
reg         m_4 = 1'b0;                       // the line asked for is C24A
wire [25:0] ask_t = fq_t[fq_q[FA-1:0]];
wire [5:0]  ask_sl = ask_t[25:20];
wire [3:0]  ask_lv = ask_t[19:16];
wire        ask_4 = fq_4[fq_q[FA-1:0]];
wire [12:0] ask_d = t_desc[ask_sl];
wire [3:0]  ask_lw = ask_d[3:0] - ask_lv;       // its level's width, log 2
wire [3:0]  ask_ls = ask_lw - 4'd3;             // its lines a row, log 2
wire signed [6:0] sh = $signed({3'd0, ask_d[3:0]}) + $signed({3'd0, ask_d[7:4]}) - $signed({2'd0, fj, 1'b0}) - 7'sd6;
wire [22:0] lines = (sh < 0) ? 23'd0 : (23'd1 << sh[4:0]);   // the level's lines: W H / 64
assign m_len = m_4 ? 5'd16 : 5'd12;
always @(posedge clk)
    if (rst) begin
        rs <= 2'd0; m_req <= 1'b0; fq_q <= 0;
    end else case (rs)
    2'd0: if (fq_q != fq_a) begin fj <= 4'd0; fo <= 23'd0; rs <= 2'd1; end
    2'd1:
        if (fj == ask_lv) begin
            m_addr <= t_base[ask_sl] + fo + ((({15'd0, ask_t[7:0]} << ask_ls) + {15'd0, ask_t[15:8]}) * (ask_4 ? 23'd16 : 23'd12));
            m_4 <= ask_4; m_req <= 1'b1; rs <= 2'd2;
        end else begin
            fo <= fo + (lines << 3) + (ask_4 ? lines << 3 : lines << 2);   // 12 or 16 words a line
            fj <= fj + 1'b1;
        end
    default: if (m_rdy) begin m_req <= 1'b0; fq_q <= fq_q + 1'b1; rs <= 2'd0; end
    endcase

// the words, in the order asked for: kept by fill, in four copies (a block's words are read together,
// a clock after their addresses)
reg  [3:0]  rx_n = 4'd0;                      // words of fill fq_r in
reg  [31:0] ws0 [0:(16 << FA) - 1], ws1 [0:(16 << FA) - 1], ws2 [0:(16 << FA) - 1], ws3 [0:(16 << FA) - 1];
reg  [31:0] dw0 = 0, dw1 = 0, dw2 = 0, dw3 = 0;
wire [FA+3:0] ww = {fq_r[FA-1:0], rx_n};
wire [FA+3:0] dra0, dra1, dra2, dra3;         // the decoder's addresses
always @(posedge clk) begin
    if (m_valid) begin ws0[ww] <= m_data; ws1[ww] <= m_data; ws2[ww] <= m_data; ws3[ww] <= m_data; end
    dw0 <= ws0[dra0]; dw1 <= ws1[dra1]; dw2 <= ws2[dra2]; dw3 <= ws3[dra3];
    if (rst) begin
        fq_r <= 0; rx_n <= 4'd0;
    end else if (m_valid) begin
        if (rx_n == (fq_4[fq_r[FA-1:0]] ? 4'd15 : 4'd11)) begin rx_n <= 4'd0; fq_r <= fq_r + 1'b1; end
        else rx_n <= rx_n + 1'b1;
    end
end

// decoding: texel i (row-major) of a block's words; an end's share is w / 64
function [7:0] ends;                          // (e0 (64 - w) + e1 w + 32) >> 6
    input [7:0] e0, e1;
    input [6:0] w;
    reg   signed [8:0]  df;
    reg   signed [16:0] p;
    begin
        df = $signed({1'b0, e1}) - $signed({1'b0, e0});
        p  = df * $signed({1'b0, w}) + $signed({3'd0, e0, 6'd32});
        ends = p[13:6];
    end
endfunction
function [6:0] w8;                            // C24: 8 levels
    input [2:0] k;
    case (k)
        3'd0: w8 = 7'd0;  3'd1: w8 = 7'd9;  3'd2: w8 = 7'd18; 3'd3: w8 = 7'd27;
        3'd4: w8 = 7'd37; 3'd5: w8 = 7'd46; 3'd6: w8 = 7'd55; default: w8 = 7'd64;
    endcase
endfunction
function [6:0] w4;                            // C24A: 4 levels
    input [1:0] k;
    case (k)
        2'd0: w4 = 7'd0; 2'd1: w4 = 7'd21; 2'd2: w4 = 7'd43; default: w4 = 7'd64;
    endcase
endfunction
function [31:0] texel;                        // {A, R, G, B}
    input        alpha;
    input [31:0] w0, w1, w2, w3;
    input [3:0]  i;
    reg   [47:0] bits;
    reg   [2:0]  k3;
    reg   [1:0]  k2, ka;
    reg   [6:0]  wc, wa;
    integer      t;
    begin
        bits = {w2, w1[31:24], w0[31:24]};
        k3 = 3'd0; k2 = 2'd0; ka = 2'd0;
        for (t = 0; t < 16; t = t + 1)         // texel i's codes (constant selects: XST)
            if (i == t) begin k3 = bits[3 * t +: 3]; k2 = w2[2 * t +: 2]; ka = w3[2 * t +: 2]; end
        wc = alpha ? w4(k2) : w8(k3);
        wa = w4(ka);
        texel = {alpha ? ends(w0[31:24], w1[31:24], wa) : 8'd255,
                 ends(w0[23:16], w1[23:16], wc), ends(w0[15:8], w1[15:8], wc), ends(w0[7:0], w1[7:0], wc)};
    end
endfunction

// fill fq_c into the banks once its words are in and the pixels queued before the one that wanted
// it have left (read the banks): texels {ey, ex} = fe, four a clock, written a clock after their
// words are read; bank {py, px} gets line texel (2 ex + px, 2 ey + py): block {y >> 2, x >> 2},
// texel {y & 3, x & 3}
reg         cd = 1'b0, cd1 = 1'b0;            // decoding: words read, texels written
reg  [3:0]  fe = 4'd0, fe1 = 4'd0;
reg         c41 = 1'b0;
reg  [7:0]  ci1 = 8'd0;
wire [FA-1:0] cs = fq_c[FA-1:0];
wire        c_4 = fq_4[cs];
wire [3:0]  kb = c_4 ? {fe[3], fe[1], 2'b00} : 4'd3 * {2'd0, fe[3], fe[1]};   // the block's first word
assign dra0 = {cs, kb}; assign dra1 = {cs, kb + 4'd1}; assign dra2 = {cs, kb + 4'd2}; assign dra3 = {cs, kb + 4'd3};
generate for (g = 0; g < 4; g = g + 1) begin : dec
    assign fill_d[g] = texel(c41, dw0, dw1, dw2, dw3, {fe1[2], g[1], fe1[0], g[0]});
end endgenerate
assign fill_we = cd1;
assign fill_a  = {ci1, fe1};
always @(posedge clk) begin
    cd1 <= cd && !rst; fe1 <= fe; c41 <= c_4; ci1 <= fq_i[cs];
    if (rst) begin
        cd <= 1'b0; fq_c <= 0;
    end else begin
        if (!cd) begin
            if (!cd1 && fq_c != fq_r && q_r == fq_p[cs]) begin cd <= 1'b1; fe <= 4'd0; end
        end else begin
            fe <= fe + 1'b1;
            if (fe == 4'd15) cd <= 1'b0;
        end
        if (cd1 && fe1 == 4'd15) fq_c <= fq_c + 1'b1;      // with the last texels written
    end
end

endmodule

// ============================================================================
// gfx_tex_bank -- one of the texture cache's four banks: 256 lines of 16
// texels (those of its parity), a copy of the tags, valid bits (cleared by
// inval).
// ============================================================================
module gfx_tex_bank (
    input  wire        clk,
    input  wire        inval,
    input  wire [7:0]  li,                    // lookup: the line's index and tag
    input  wire [25:0] want,
    output wire        hit,
    input  wire        rd,                    // texel read, registered
    input  wire [11:0] ra,
    output reg  [31:0] rq = 32'd0,
    input  wire        we,                    // texel write (fills)
    input  wire [11:0] wa,
    input  wire [31:0] wd,
    input  wire        tag_we,
    input  wire [7:0]  tag_i,
    input  wire [25:0] tag_d
);

reg  [31:0] mem [0:4095];
reg  [25:0] tags [0:255];
reg  [255:0] valid = 256'd0;
assign hit = valid[li] && (tags[li] == want);
always @(posedge clk) begin
    if (rd) rq <= mem[ra];
    if (we) mem[wa] <= wd;
    if (tag_we) tags[tag_i] <= tag_d;
    if (inval) valid <= 256'd0;
    else if (tag_we) valid[tag_i] <= 1'b1;
end

endmodule

`default_nettype wire


// ============================================================================
// g3d_loader -- .g3d scene stream into SDRAM
// Chunk: A5 3D, type[2], address[4], length[4] (words), sum[4], payload
// (length x 4 bytes), all big-endian. Bytes outside a chunk are skipped, so
// the parser resyncs on the next marker. Payload words go to the SDRAM write
// port at address, address + 1, ... through wreq/wack. A chunk whose payload
// sum differs counts as bad; every chunk reports ev_chunk. Type GO starts the
// renderer once every word has reached the SDRAM.
// ============================================================================
module g3d_loader #(
    parameter FIFO_LATENCY = 2,                // fifo dout valid this many clocks after rd
    parameter AW           = 23
)(
    input  wire          clk,
    input  wire          rst,

    input  wire [7:0]    fifo_dout,
    input  wire          fifo_empty,
    output wire          fifo_rd,

    output wire          wreq,                  // payload word for the SDRAM, held until wack
    output reg  [AW-1:0] wr_addr = {AW{1'b0}},
    output reg  [31:0]   wr_data = 32'd0,
    input  wire          wack,
    input  wire          wr_idle,

    output reg           go      = 1'b0,        // start the renderer at go_addr
    output reg  [AW-1:0] go_addr = {AW{1'b0}},
    output reg  [15:0]   n_ok    = 16'd0,       // chunks with a good sum
    output reg  [15:0]   n_bad   = 16'd0,
    output reg  [31:0]   n_rx    = 32'd0,       // bytes taken from the FIFO

    output reg           ev_head  = 1'b0,       // a chunk header was read
    output reg           ev_chunk = 1'b0,       // a chunk ended: ev_ok, type, address, length, sums
    output reg           ev_ok    = 1'b0,
    output reg  [15:0]   ev_type  = 16'd0,
    output reg  [AW-1:0] ev_addr  = {AW{1'b0}},
    output reg  [31:0]   ev_len   = 32'd0,
    output reg  [31:0]   ev_sum   = 32'd0,
    output reg  [31:0]   ev_exp   = 32'd0
);

localparam [2:0] P_HUNT = 3'd0, P_MARK = 3'd1, P_TYPE = 3'd2, P_ADDR = 3'd3, P_LEN = 3'd4,
                 P_SUM  = 3'd5, P_DATA = 3'd6;
localparam [15:0] T_GO = {8'h47, 8'h4F};

reg  [2:0]    ps = P_HUNT;
reg  [1:0]    nb = 2'd0;                     // byte within the field or word
reg  [31:0]   sh = 32'd0;                    // bytes collected, big-endian
reg  [15:0]   ctype = 16'd0;
reg  [AW-1:0] caddr = {AW{1'b0}};
reg  [31:0]   clen = 32'd0, csum = 32'd0, sum = 32'd0, cnt = 32'd0;
reg           wpend = 1'b0;                  // payload word waiting for the write port
reg           go_wait = 1'b0;                // GO chunk seen, waiting for the writes to land
reg  [FIFO_LATENCY-1:0] rdp = {FIFO_LATENCY{1'b0}};

wire          hold  = wpend;                 // at most FIFO_LATENCY (< 4) bytes still arrive
wire          bv    = rdp[FIFO_LATENCY-1];  // fifo_dout holds a byte
wire [31:0]   word  = {sh[23:0], fifo_dout};
wire [31:0]   total = sum + word;           // payload sum with this word

assign fifo_rd = ~fifo_empty & ~hold & ~rst;
assign wreq    = wpend;

// chunk end: count it and report it
task chunk_end(input [31:0] s, input [31:0] e);   // payload sum, expected sum
    begin
        if (s == e) n_ok <= n_ok + 1'b1; else n_bad <= n_bad + 1'b1;
        ev_chunk <= 1'b1; ev_ok <= (s == e); ev_type <= ctype; ev_addr <= caddr;
        ev_len <= clen; ev_sum <= s; ev_exp <= e;
        if (ctype == T_GO && s == e) begin go_wait <= 1'b1; go_addr <= caddr; end
        ps <= P_HUNT;
    end
endtask

always @(posedge clk) begin
    rdp  <= {rdp[FIFO_LATENCY-2:0], fifo_rd};
    go   <= 1'b0;
    ev_head  <= 1'b0;
    ev_chunk <= 1'b0;
    if (fifo_rd) n_rx <= n_rx + 1'b1;

    if (rst) begin
        ps <= P_HUNT; wpend <= 1'b0; go_wait <= 1'b0;
    end else begin
        if (wack) wpend <= 1'b0;
        if (bv) begin
            sh <= word;
            case (ps)
            P_HUNT: if (fifo_dout == 8'hA5) ps <= P_MARK;
            P_MARK: ps <= (fifo_dout == 8'h3D) ? P_TYPE : (fifo_dout == 8'hA5) ? P_MARK : P_HUNT;
            P_TYPE: begin nb <= nb + 1'b1; if (nb == 2'd1) begin ctype <= word[15:0]; nb <= 2'd0; ps <= P_ADDR; end end
            P_ADDR: begin nb <= nb + 1'b1; if (nb == 2'd3) begin caddr <= word[AW-1:0]; ps <= P_LEN; end end
            P_LEN:  begin nb <= nb + 1'b1; if (nb == 2'd3) begin clen  <= word; ps <= P_SUM; end end
            P_SUM:  begin
                nb <= nb + 1'b1;
                if (nb == 2'd3) begin
                    csum <= word; sum <= 32'd0; cnt <= 32'd0; ev_head <= 1'b1;
                    wr_addr <= caddr - 1'b1;
                    if (clen != 32'd0)
                        ps <= P_DATA;
                    else
                        chunk_end(32'd0, word);                 // empty chunk: sum must be 0
                end
            end
            P_DATA: begin
                nb <= nb + 1'b1;
                if (nb == 2'd3) begin
                    wr_data <= word; wr_addr <= wr_addr + 1'b1; wpend <= 1'b1;
                    sum <= total; cnt <= cnt + 1'b1;
                    if (cnt + 1'b1 == clen) chunk_end(total, csum); // last word: check the sum
                end
            end
            default: ;
            endcase
        end
        if (go_wait && wr_idle && !wpend) begin               // start once the scene is in SDRAM
            go <= 1'b1; go_wait <= 1'b0;
        end
    end
end

endmodule


// ============================================================================
// gfx_scene_dma -- after GO, reads every loaded chunk back from SDRAM through
// the DMA port (bursts of up to 256 words), streams the words out
// (o_type/o_idx/o_data) and checks each chunk's sum. One console event per
// chunk and a summary; each event is held in ev until ev_take.
// ============================================================================
module gfx_scene_dma #(
    parameter AW      = 23,
    parameter CLK_MHZ = 50
)(
    input  wire          clk,
    input  wire          rst,

    input  wire          ch_v,                  // a chunk loaded with a good sum
    input  wire [15:0]   ch_type,
    input  wire [AW-1:0] ch_addr,
    input  wire [31:0]   ch_len,
    input  wire [31:0]   ch_sum,
    input  wire          go,

    output wire          dma_want,
    output reg           dma_req  = 1'b0,
    output reg  [AW-1:0] dma_addr = {AW{1'b0}},
    output reg  [8:0]    dma_len  = 9'd0,
    input  wire          dma_rdy,
    input  wire [31:0]   dma_data,
    input  wire          dma_valid,

    output reg           o_v    = 1'b0,         // word of chunk o_type at index o_idx
    output reg  [15:0]   o_type = 16'd0,
    output reg  [23:0]   o_idx  = 24'd0,
    output reg  [31:0]   o_data = 32'd0,

    output reg           done = 1'b0,           // pulse: read-back finished
    output reg           ok   = 1'b0,           // ... descriptor present, no bad chunk

    output reg           ev_v = 1'b0,
    output reg  [131:0]  ev   = 132'd0,
    input  wire          ev_take,
    output wire          busy
);

// message ids and template start addresses (con_rom.py)
localparam [3:0] M_BANNER = 4'd0;
localparam [3:0] M_SDRAM_OK = 4'd1;
localparam [3:0] M_SDRAM_BAD = 4'd2;
localparam [3:0] M_WAIT = 4'd3;
localparam [3:0] M_CHUNK_OK = 4'd4;
localparam [3:0] M_CHUNK_BAD = 4'd5;
localparam [3:0] M_GO = 4'd6;
localparam [3:0] M_CPU_HALT = 4'd7;
localparam [3:0] M_STATUS = 4'd8;
localparam [3:0] M_WRLOST = 4'd9;
localparam [3:0] M_RB_OK = 4'd10;
localparam [3:0] M_RB_BAD = 4'd11;
localparam [3:0] M_RB_DONE = 4'd12;
localparam [3:0] M_RUN = 4'd13;
localparam [3:0] M_NORUN = 4'd14;
localparam [3:0] M_CPU = 4'd15;

function [9:0] msg_start;
    input [3:0] id;
    case (id)
    4'd0:    msg_start = 10'd0;
    4'd1:    msg_start = 10'd83;
    4'd2:    msg_start = 10'd167;
    4'd3:    msg_start = 10'd235;
    4'd4:    msg_start = 10'd297;
    4'd5:    msg_start = 10'd336;
    4'd6:    msg_start = 10'd379;
    4'd7:    msg_start = 10'd427;
    4'd8:    msg_start = 10'd486;
    4'd9:    msg_start = 10'd557;
    4'd10:    msg_start = 10'd634;
    4'd11:    msg_start = 10'd672;
    4'd12:    msg_start = 10'd710;
    4'd13:    msg_start = 10'd773;
    4'd14:    msg_start = 10'd803;
    4'd15:    msg_start = 10'd848;
    default: msg_start = 10'd0;
    endcase
endfunction

localparam [2:0] D_IDLE = 3'd0, D_NEXT = 3'd1, D_REQ = 3'd2, D_DATA = 3'd3, D_CHECK = 3'd4,
                 D_DONE = 3'd5, D_NO3D = 3'd6;

// chunk table
reg  [15:0]   t_type [0:15];
reg  [AW-1:0] t_addr [0:15];
reg  [23:0]   t_len  [0:15];
reg  [31:0]   t_sum  [0:15];
reg  [4:0]    n = 5'd0, k = 5'd0;

reg  [2:0]    ds = D_IDLE;
reg  [AW-1:0] cur = {AW{1'b0}};
reg  [23:0]   left = 24'd0;
reg  [8:0]    blen = 9'd0, bcnt = 9'd0;
reg  [31:0]   sum = 32'd0, words = 32'd0;
reg  [15:0]   n_ok = 16'd0, n_bad = 16'd0;
reg  [5:0]    pre = 6'd0;
reg  [31:0]   us = 32'd0, us0 = 32'd0;
reg  [31:0]   s_nvtx = 32'd0, s_ntri = 32'd0, s_keys = 32'd0;
reg           s_desc = 1'b0;
localparam [15:0] T_SC = {8'h53, 8'h43}, T_PO = {8'h50, 8'h4F};

assign busy     = (ds != D_IDLE);
assign dma_want = (ds == D_REQ) && (left != 24'd0);

always @(posedge clk) begin                 // microseconds
    if (pre == CLK_MHZ - 1) begin pre <= 6'd0; us <= us + 1'b1; end
    else pre <= pre + 1'b1;
end

always @(posedge clk) begin
    dma_req <= 1'b0;
    o_v     <= 1'b0;
    done    <= 1'b0;
    if (o_v && o_type == T_SC && o_idx == 24'd2) s_nvtx <= o_data;     // counts for the console
    if (o_v && o_type == T_SC && o_idx == 24'd4) begin s_ntri <= o_data; s_desc <= 1'b1; end
    if (o_v && o_type == T_PO && o_idx[4:0] == 5'd0) s_keys <= s_keys + 1'b1;
    if (ev_take) ev_v <= 1'b0;

    if (rst) begin
        ds <= D_IDLE; n <= 5'd0; ev_v <= 1'b0;
    end else case (ds)
    D_IDLE: begin
        if (ch_v && ch_len != 32'd0 && !n[4]) begin
            t_type[n[3:0]] <= ch_type; t_addr[n[3:0]] <= ch_addr;
            t_len[n[3:0]]  <= ch_len[23:0]; t_sum[n[3:0]] <= ch_sum;
            n <= n + 1'b1;
        end
        if (go) begin
            k <= 5'd0; words <= 32'd0; n_ok <= 16'd0; n_bad <= 16'd0; us0 <= us;
            s_keys <= 32'd0; s_desc <= 1'b0;
            ds <= D_NEXT;
        end
    end
    D_NEXT:
        if (k == n) ds <= D_DONE;
        else begin
            cur <= t_addr[k[3:0]]; left <= t_len[k[3:0]]; sum <= 32'd0;
            o_type <= t_type[k[3:0]]; o_idx <= 24'd0 - 1'b1;
            ds <= D_REQ;
        end
    D_REQ:
        if (left == 24'd0) ds <= D_CHECK;
        else if (dma_rdy && !dma_req) begin
            blen     <= (left > 24'd256) ? 9'd256 : left[8:0];
            dma_len  <= (left > 24'd256) ? 9'd256 : left[8:0];
            dma_addr <= cur; dma_req <= 1'b1; bcnt <= 9'd0;
            ds <= D_DATA;
        end
    D_DATA: if (dma_valid) begin
        sum <= sum + dma_data;
        o_v <= 1'b1; o_data <= dma_data; o_idx <= o_idx + 1'b1;
        bcnt <= bcnt + 1'b1;
        if (bcnt + 1'b1 == blen) begin
            cur <= cur + blen; left <= left - blen; ds <= D_REQ;
        end
    end
    D_CHECK: if (!ev_v) begin
        ev_v <= 1'b1;
        ev   <= (sum == t_sum[k[3:0]])
              ? {M_RB_OK,  16'd0, o_type, {(32 - AW){1'b0}}, t_addr[k[3:0]], 8'd0, t_len[k[3:0]], sum}
              : {M_RB_BAD, 16'd0, o_type, {(32 - AW){1'b0}}, t_addr[k[3:0]], sum, t_sum[k[3:0]]};
        if (sum == t_sum[k[3:0]]) n_ok <= n_ok + 1'b1; else n_bad <= n_bad + 1'b1;
        words <= words + t_len[k[3:0]];
        k <= k + 1'b1;
        ds <= D_NEXT;
    end
    D_DONE: if (!ev_v) begin
        ev_v <= 1'b1;
        ev   <= {M_RB_DONE, 16'd0, n_ok, 16'd0, n_bad, words, us - us0};
        ds   <= D_NO3D;
    end
    D_NO3D: if (!ev_v) begin
        ev_v <= 1'b1;
        ev   <= (s_desc && n_bad == 16'd0) ? {M_RUN, s_nvtx, s_ntri, s_keys, 32'd0} : {M_NORUN, 128'd0};
        done <= 1'b1;
        ok   <= s_desc && (n_bad == 16'd0);
        n    <= 5'd0;                                // next file starts a new table
        ds   <= D_IDLE;
    end
    default: ds <= D_IDLE;
    endcase
end

endmodule


// ============================================================================
// gfx_console -- debug text screen, 80 x 30 characters of 8 x 16 pixels
// Events (id + four arguments) are queued and each prints one line from its
// template (gfx_msg_rom): rows 0..28 scroll, row 29 is a status bar redrawn
// every STATUS_PERIOD clocks from s0..s7. Template bytes: 0 end, 1..5 colour,
// 8'b1aaa_ffff field of argument a: f = 1..8 hex digits, 9 decimal,
// 10 two characters from [15:0], 12 decimal of [31:16]; others are printed.
// Read port: screen row/column in, {colour[2:0], char[6:0]} out a clock later.
// ============================================================================
module gfx_console #(
    parameter STATUS_PERIOD = 5000000
)(
    input  wire        clk,
    input  wire        rst,

    input  wire        ev_v,
    input  wire [3:0]  ev_id,
    input  wire [31:0] ev_a0, ev_a1, ev_a2, ev_a3,
    output wire        ev_full,

    input  wire [31:0] s0, s1, s2, s3, s4, s5, s6, s7,

    input  wire [4:0]  rd_row,
    input  wire [6:0]  rd_col,
    output reg  [9:0]  rd_q = 10'd0,

    output reg  [7:0]  tx_a = 8'd0,           // CPU text lines: {buffer, character}
    input  wire [7:0]  tx_q,                  // the character, a clock after tx_a
    output reg         tx_done = 1'b0         // the line is on the console
);

// message ids and template start addresses (con_rom.py)
localparam [3:0] M_BANNER = 4'd0;
localparam [3:0] M_SDRAM_OK = 4'd1;
localparam [3:0] M_SDRAM_BAD = 4'd2;
localparam [3:0] M_WAIT = 4'd3;
localparam [3:0] M_CHUNK_OK = 4'd4;
localparam [3:0] M_CHUNK_BAD = 4'd5;
localparam [3:0] M_GO = 4'd6;
localparam [3:0] M_CPU_HALT = 4'd7;
localparam [3:0] M_STATUS = 4'd8;
localparam [3:0] M_WRLOST = 4'd9;
localparam [3:0] M_RB_OK = 4'd10;
localparam [3:0] M_RB_BAD = 4'd11;
localparam [3:0] M_RB_DONE = 4'd12;
localparam [3:0] M_RUN = 4'd13;
localparam [3:0] M_NORUN = 4'd14;
localparam [3:0] M_CPU = 4'd15;

function [9:0] msg_start;
    input [3:0] id;
    case (id)
    4'd0:    msg_start = 10'd0;
    4'd1:    msg_start = 10'd83;
    4'd2:    msg_start = 10'd167;
    4'd3:    msg_start = 10'd235;
    4'd4:    msg_start = 10'd297;
    4'd5:    msg_start = 10'd336;
    4'd6:    msg_start = 10'd379;
    4'd7:    msg_start = 10'd427;
    4'd8:    msg_start = 10'd486;
    4'd9:    msg_start = 10'd557;
    4'd10:    msg_start = 10'd634;
    4'd11:    msg_start = 10'd672;
    4'd12:    msg_start = 10'd710;
    4'd13:    msg_start = 10'd773;
    4'd14:    msg_start = 10'd803;
    4'd15:    msg_start = 10'd848;
    default: msg_start = 10'd0;
    endcase
endfunction

localparam ROWS = 29, COLS = 80;
localparam [3:0] F_IDLE = 4'd0, F_NL = 4'd1, F_CLR = 4'd2, F_RD = 4'd3, F_EX = 4'd4, F_HEX = 4'd5,
                 F_DD = 4'd6, F_DOUT = 4'd7, F_CH2 = 4'd8, F_PAD = 4'd9, F_WT = 4'd10,
                 F_TXA = 4'd11, F_TXD = 4'd12, F_TXC = 4'd13;

// ---------------------------------------------------------------- event FIFO
reg  [131:0] evq [0:15];
reg  [4:0]   ew = 5'd0, er = 5'd0;
wire         ev_empty = (ew == er);
assign       ev_full  = (ew - er) >= 5'd15;

always @(posedge clk)
    if (ev_v && !ev_full) begin evq[ew[3:0]] <= {ev_id, ev_a0, ev_a1, ev_a2, ev_a3}; ew <= ew + 1'b1; end

// ---------------------------------------------------------------- text: 32 line ring + status line
reg  [9:0]  ring [0:4095];
reg  [9:0]  stat [0:127];
reg  [4:0]  top = 5'd0, cur = 5'd31, used = 5'd0;
reg         tw = 1'b0, tw_st = 1'b0;
reg  [11:0] tw_a = 12'd0;
reg  [9:0]  tw_d = 10'd0;
// synthesis translate_off
integer     n;
initial for (n = 0; n < 4096; n = n + 1) ring[n] = 10'h020;        // hardware: 0, also blank
initial for (n = 0; n < 128;  n = n + 1) stat[n] = 10'h020;
// synthesis translate_on

reg         rd_st = 1'b0;
reg  [9:0]  ring_q = 10'd0, stat_q = 10'd0;
always @(posedge clk) begin
    if (tw && !tw_st) ring[tw_a] <= tw_d;
    if (tw &&  tw_st) stat[tw_a[6:0]] <= tw_d;
    ring_q <= ring[{top + rd_row, rd_col}];
    stat_q <= stat[rd_col];
    rd_st  <= (rd_row == 5'd29);
end
always @(*) rd_q = rd_st ? stat_q : ring_q;

// ---------------------------------------------------------------- formatter
reg  [3:0]   fs = F_IDLE;
reg  [9:0]   mp = 10'd0;
wire [7:0]   mb;
gfx_msg_rom  msg_u (.clk(clk), .ra(mp), .rd(mb));

reg  [255:0] args = 256'd0;
reg  [6:0]   col = 7'd0;
reg  [2:0]   color = 3'd1;
reg          st_t = 1'b0;                    // formatting the status line
reg  [3:0]   nd = 4'd0;                      // digits left
reg  [31:0]  fv = 32'd0;                     // field value
reg  [39:0]  bcd = 40'd0;
reg  [5:0]   ddc = 6'd0;
reg          lz = 1'b1;                      // still skipping leading zeros
reg  [31:0]  stc = 32'd0;
reg  [3:0]   ev_id_q = 4'd0;

wire [31:0]  fa  = args[255 - 32 * mb[6:4] -: 32];
wire [3:0]   nib = fv[4 * nd - 1 -: 4];
wire [7:0]   hexc = (nib < 4'd10) ? 8'h30 + nib : 8'h37 + nib;
wire [3:0]   dig = bcd[39:36];

function [39:0] add3;                        // double-dabble correction
    input [39:0] b;
    integer k;
    begin
        for (k = 0; k < 10; k = k + 1)
            add3[4 * k +: 4] = (b[4 * k +: 4] >= 4'd5) ? b[4 * k +: 4] + 4'd3 : b[4 * k +: 4];
    end
endfunction

task put(input [7:0] ch);                    // character at the cursor
    begin
        tw <= (col < COLS); tw_st <= st_t; tw_a <= {cur, col}; tw_d <= {color, ch[6:0]};
        col <= col + 1'b1;
    end
endtask

always @(posedge clk) begin
    tw <= 1'b0;
    tx_done <= 1'b0;
    stc <= (stc == STATUS_PERIOD - 1) ? 32'd0 : stc + 1'b1;
    if (rst) fs <= F_IDLE;
    else case (fs)
    F_IDLE: if (!ev_empty) begin
        {ev_id_q, args[255:128]} <= evq[er[3:0]];
        args[127:0] <= 128'd0;
        er <= er + 1'b1;
        st_t <= 1'b0; color <= 3'd1;
        fs <= F_NL;
    end else if (stc == 32'd0) begin
        args <= {s0, s1, s2, s3, s4, s5, s6, s7};
        st_t <= 1'b1; color <= 3'd1; col <= 7'd0;
        mp <= msg_start(M_STATUS); fs <= F_WT;
    end
    F_NL: begin                                              // new line, scroll when full
        cur <= cur + 1'b1;
        if (used == ROWS) top <= top + 1'b1; else used <= used + 1'b1;
        col <= 7'd0; fs <= F_CLR;
    end
    F_CLR: begin
        tw <= 1'b1; tw_st <= 1'b0; tw_a <= {cur, col}; tw_d <= 10'h020;
        col <= col + 1'b1;
        if (col == COLS - 1) begin col <= 7'd0; mp <= msg_start(ev_id_q); fs <= F_WT; end
    end
    F_WT: fs <= F_EX;                                        // template byte read
    F_EX: begin
        if (mb == 8'd0)
            fs <= st_t ? F_PAD : F_IDLE;
        else if (mb < 8'd6) begin
            color <= mb[2:0]; mp <= mp + 1'b1; fs <= F_WT;
        end else if (mb[7]) begin
            fv <= fa;
            case (mb[3:0])
            4'd9:    begin bcd <= 40'd0; ddc <= 6'd32; fs <= F_DD; end
            4'd12:   begin fv <= {16'd0, fa[31:16]}; bcd <= 40'd0; ddc <= 6'd32; fs <= F_DD; end
            4'd10:   begin nd <= 4'd2; fs <= F_CH2; end
            4'd11:   begin tx_a <= {fa[0], 7'd0}; fs <= F_TXD; end        // %L: a CPU line
            default: begin nd <= mb[3:0]; fs <= F_HEX; end
            endcase
        end else begin
            put(mb); mp <= mp + 1'b1; fs <= F_WT;
        end
    end
    F_HEX: begin
        put(hexc); nd <= nd - 1'b1;
        if (nd == 4'd1) begin mp <= mp + 1'b1; fs <= F_WT; end
    end
    F_CH2: begin
        put(nd[1] ? fv[15:8] : fv[7:0]); nd <= nd - 1'b1;
        if (nd == 4'd1) begin mp <= mp + 1'b1; fs <= F_WT; end
    end
    F_DD: begin                                              // binary to BCD, one bit a clock
        if (ddc == 6'd0) begin nd <= 4'd10; lz <= 1'b1; fs <= F_DOUT; end
        else begin
            {bcd, fv} <= {add3(bcd), fv} << 1;
            ddc <= ddc - 1'b1;
        end
    end
    F_DOUT: begin
        if (dig != 4'd0 || !lz || nd == 4'd1) begin put(8'h30 + dig); lz <= 1'b0; end
        bcd <= bcd << 4; nd <= nd - 1'b1;
        if (nd == 4'd1) begin mp <= mp + 1'b1; fs <= F_WT; end
    end
    F_TXA: begin tx_a[6:0] <= tx_a[6:0] + 1'b1; fs <= F_TXD; end
    F_TXD: fs <= F_TXC;                                      // character read
    F_TXC: begin                                             // colours 1..5, text, ends at 0 or 80
        if (tx_q != 8'd0 && tx_q < 8'd6) color <= tx_q[2:0];
        else if (tx_q != 8'd0) put(tx_q);
        if (tx_q == 8'd0 || tx_a[6:0] == 7'd79) begin tx_done <= 1'b1; mp <= mp + 1'b1; fs <= F_WT; end
        else fs <= F_TXA;
    end
    F_PAD: begin                                             // rest of the status line
        put(8'h20);
        if (col == COLS - 1) fs <= F_IDLE;
    end
    default: fs <= F_IDLE;
    endcase
end

endmodule

// ============================================================================
// gfx_wr_arb -- SDRAM write port shared by two clients (a first). One word per
// 3 clocks at most, WrFull sampled at the clock the word is offered.
// A client holds req, addr and data until ack.
// ============================================================================
module gfx_wr_arb #(
    parameter AW = 23
)(
    input  wire          clk,
    input  wire          a_req,
    input  wire [AW-1:0] a_addr,
    input  wire [31:0]   a_data,
    output reg           a_ack = 1'b0,
    input  wire          b_req,
    input  wire [AW-1:0] b_addr,
    input  wire [31:0]   b_data,
    output reg           b_ack = 1'b0,

    output reg           WR     = 1'b0,
    output reg  [AW-1:0] WrAddr = {AW{1'b0}},
    output reg  [31:0]   WrData = 32'd0,
    input  wire          WrFull
);

reg [1:0] gap = 2'd0;

always @(posedge clk) begin
    WR <= 1'b0; a_ack <= 1'b0; b_ack <= 1'b0;
    if (gap != 2'd0) gap <= gap - 1'b1;
    if (gap == 2'd0 && !WrFull && !a_ack && !b_ack) begin
        if (a_req) begin
            WR <= 1'b1; WrAddr <= a_addr; WrData <= a_data; a_ack <= 1'b1; gap <= 2'd2;
        end else if (b_req) begin
            WR <= 1'b1; WrAddr <= b_addr; WrData <= b_data; b_ack <= 1'b1; gap <= 2'd2;
        end
    end
end

endmodule

// ============================================================================
// gfx_dma_arb -- one SDRAM DMA port for four clients, in the order 0, 1, 3, 2,
// 1 and 3 taking turns when both wait.
// A client that wants the port is granted it and, while rdy, issues req with
// addr and len; the grant holds until then. Up to four bursts may be in flight
// (the controller takes the next once the last is issued): their words come
// back in order, each on v of its burst's owner.
// ============================================================================
module gfx_dma_arb (
    input  wire        clk,
    input  wire        rst,
    input  wire        w0, r0, input wire [22:0] a0, input wire [8:0] n0, output wire y0, output wire v0,
    input  wire        w1, r1, input wire [22:0] a1, input wire [8:0] n1, output wire y1, output wire v1,
    input  wire        w2, r2, input wire [22:0] a2, input wire [8:0] n2, output wire y2, output wire v2,
    input  wire        w3, r3, input wire [22:0] a3, input wire [8:0] n3, output wire y3, output wire v3,

    output reg         DmaReq  = 1'b0,
    output reg  [22:0] DmaAddr = 23'd0,
    output reg  [8:0]  DmaLen  = 9'd0,
    input  wire        DmaRdy,
    input  wire        DmaValid
);

reg  [2:0] g    = 3'd4;                      // granted client, 4: none
reg  [1:0] qo [0:3];                         // the bursts in flight: owner, length; oldest at qr
reg  [8:0] ql [0:3];
reg  [1:0] qw = 2'd0, qr = 2'd0;
reg  [2:0] qn = 3'd0;
reg  [8:0] got = 9'd0;                       // words of the oldest arrived
reg        turn = 1'b0;                      // 1 and 3 both waiting: 1 unless 1 went last
wire [1:0] own  = qo[qr];
wire       open_ = !DmaReq && qn != 3'd4 && DmaRdy;
wire       gr   = (g == 3'd0) ? r0 : (g == 3'd1) ? r1 : (g == 3'd2) ? r2 : r3;
wire       gw   = (g == 3'd0) ? w0 : (g == 3'd1) ? w1 : (g == 3'd2) ? w2 : w3;
wire       push = (g != 3'd4) && gr && open_;
wire       pop  = DmaValid && qn != 3'd0 && got == ql[qr] - 9'd1;

assign y0 = (g == 3'd0) & open_ & !r0;     // not while its request is being taken: one at a time
assign y1 = (g == 3'd1) & open_ & !r1;
assign y2 = (g == 3'd2) & open_ & !r2;
assign y3 = (g == 3'd3) & open_ & !r3;
assign v0 = DmaValid & (own == 2'd0);
assign v1 = DmaValid & (own == 2'd1);
assign v2 = DmaValid & (own == 2'd2);
assign v3 = DmaValid & (own == 2'd3);

always @(posedge clk) begin
    DmaReq <= 1'b0;
    if (DmaValid) got <= pop ? 9'd0 : got + 1'b1;
    if (pop) qr <= qr + 1'b1;
    if (push) begin
        DmaReq  <= 1'b1;
        DmaAddr <= (g == 3'd0) ? a0 : (g == 3'd1) ? a1 : (g == 3'd2) ? a2 : a3;
        DmaLen  <= (g == 3'd0) ? n0 : (g == 3'd1) ? n1 : (g == 3'd2) ? n2 : n3;
        qo[qw] <= g[1:0];
        ql[qw] <= (g == 3'd0) ? n0 : (g == 3'd1) ? n1 : (g == 3'd2) ? n2 : n3;
        qw <= qw + 1'b1;
    end
    qn <= qn + push - pop;
    if (push && g[1:0] == 2'd1) turn <= 1'b1;
    if (push && g[1:0] == 2'd3) turn <= 1'b0;
    if (rst) begin
        g <= 3'd4; qw <= 2'd0; qr <= 2'd0; qn <= 3'd0; got <= 9'd0;
    end else if (g == 3'd4)
        g <= w0 ? 3'd0 : (w1 && w3) ? (turn ? 3'd3 : 3'd1) : w1 ? 3'd1 : w3 ? 3'd3 : w2 ? 3'd2 : 3'd4;
    else if (push || !gw)
        g <= 3'd4;
end

endmodule

`default_nettype wire