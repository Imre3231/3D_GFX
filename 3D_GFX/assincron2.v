`timescale 1ns/1ps
// ============================================================================
// async_fifo.v  —  Async dual-clock FIFO, Gray pointers, 2-FF sync, BRAM.
//
// FIXES vs original:
//  1. gray2bin uses an internal reg variable (not self-referential function-
//     return indexing).  ISE 14.7 mishandles gray2bin[i+1] inside a loop body
//     when gray2bin is the function's own return value — causes wrong XOR chain
//     in synthesis at large ADDR_BITS while simulation still passes.
//
//  2. rd_afull has HYSTERESIS (new parameter AFULL_HYSTERESIS).
//     Without it a single threshold makes dreq toggle every cycle when the
//     FIFO sits near the boundary — thousands of glitches per second.
//
//     rd_afull  ON  when occupancy >= DEPTH - AFULL_MARGIN
//     rd_afull  OFF when occupancy <  DEPTH - AFULL_MARGIN - AFULL_HYSTERESIS
//     rd_afull  HOLD (unchanged) between those two levels
//
//     dreq = ~rd_afull in the top module.
//
//  3. level / rd_level width is ADDR_BITS+1 (exact, no truncation).
//     The old generate/zero-pad blocks are gone.
//
//  4. Default ADDR_BITS = 14  (16 384 entries, maps to two 18k BRAMs).
//     Change to any value >= 2; ISE infers the correct number of BRAMs.
//
// Semantics (unchanged from original):
//  - Write: wr_clk rising edge, wr_en && !full
//  - Read:  rd_clk rising edge, rd_en && !empty
//  - DOUT_DELAY=0: dout valid 1 rd_clk after accepted rd_en
//  - DOUT_DELAY=1: dout valid 2 rd_clk after accepted rd_en
//  - wr_rst / rd_rst: async active-high
// ============================================================================

module async_fifo #(
    parameter integer WIDTH            = 8,
    parameter integer ADDR_BITS        = 14,    // depth = 2**ADDR_BITS; must be >= 2
    // -------------------------------------------------------------------------
    // AFULL_MARGIN    : rd_afull fires when free slots <= AFULL_MARGIN
    //                   (i.e. occupancy >= DEPTH - AFULL_MARGIN)
    //                   Keep small (256-512) so the FIFO fills up before BP fires.
    //
    // AFULL_HYSTERESIS: rd_afull releases when free slots >
    //                   AFULL_MARGIN + AFULL_HYSTERESIS
    //                   Set to ~512 to give dreq a wide dead-band.
    //                   Must be >= 1.  Clamped automatically if too large.
    // -------------------------------------------------------------------------
    parameter integer AFULL_MARGIN     = 1000,
    parameter integer AFULL_HYSTERESIS = 8,
    parameter integer DOUT_DELAY       = 0
)(
    // -- Write port --
    input  wire                  wr_clk,
    input  wire                  wr_rst,
    input  wire                  wr_en,
    input  wire [WIDTH-1:0]      din,
    output wire                  full,

    // -- Read port --
    input  wire                  rd_clk,
    input  wire                  rd_rst,
    input  wire                  rd_en,
    output reg  [WIDTH-1:0]      dout,
    output wire                  empty,

    output reg  [ADDR_BITS:0]    level,     // write-side occupancy (ADDR_BITS+1 bits)
    output reg  [ADDR_BITS:0]    rd_level,  // read-side  occupancy (ADDR_BITS+1 bits)
    output reg                   rd_afull   // almost-full with hysteresis  (› use for dreq)
);

    // -----------------------------------------------------------------------
    // Derived constants
    // -----------------------------------------------------------------------
    localparam integer DEPTH = (1 << ADDR_BITS);
    localparam integer PTR_W = ADDR_BITS + 1;   // one extra bit for full/empty

    // Clamp AFULL_MARGIN to [1 .. DEPTH-1]
    localparam integer M_ON_RAW = (AFULL_MARGIN    < 1)       ? 1       :
                                  (AFULL_MARGIN    >= DEPTH)  ? DEPTH-1 : AFULL_MARGIN;

    // Clamp combined (on_margin + hysteresis) to [M_ON_RAW+1 .. DEPTH-1]
    // so AF_OFF_THRESH is always strictly < AF_ON_THRESH (never negative free)
    localparam integer M_OFF_RAW = M_ON_RAW + AFULL_HYSTERESIS;
    localparam integer M_OFF = (M_OFF_RAW >= DEPTH) ? DEPTH-1 : M_OFF_RAW;

    // Occupancy thresholds (PTR_W bits)
    //   rd_afull asserts   when rocc_next >= AF_ON_THRESH
    //   rd_afull deasserts when rocc_next <  AF_OFF_THRESH
    localparam [PTR_W-1:0] AF_ON_THRESH  = DEPTH - M_ON_RAW;
    localparam [PTR_W-1:0] AF_OFF_THRESH = DEPTH - M_OFF;

    // -----------------------------------------------------------------------
    // BRAM storage
    // -----------------------------------------------------------------------
    (* ram_style = "block" *) reg [WIDTH-1:0] mem [0:DEPTH-1];
    /* synthesis ram_style = "block" */

    // -----------------------------------------------------------------------
    // Pointer registers
    // -----------------------------------------------------------------------
    reg [PTR_W-1:0] wbin,  wgray;
    reg [PTR_W-1:0] rbin,  rgray;

    (* ASYNC_REG = "TRUE" *) reg [PTR_W-1:0] rq1_rgray, rq2_rgray; // rgray in wr_clk domain
    (* ASYNC_REG = "TRUE" *) reg [PTR_W-1:0] wq1_wgray, wq2_wgray; // wgray in rd_clk domain

    reg wfull_r;
    reg rempty_r;

    // -----------------------------------------------------------------------
    // Gray <-> binary conversion
    //
    // IMPORTANT — gray2bin uses an internal 'reg b' instead of self-indexing
    // the implicit function return register (gray2bin[i+1]).
    // ISE 14.7 may silently missynthesize the self-referential form at
    // PTR_W > 10, producing an XOR tree that is wrong in hardware while
    // simulation continues to pass.
    // -----------------------------------------------------------------------
    function [PTR_W-1:0] bin2gray;
        input [PTR_W-1:0] b;
        begin
            bin2gray = (b >> 1) ^ b;
        end
    endfunction

    function [PTR_W-1:0] gray2bin;
        input   [PTR_W-1:0] g;
        reg     [PTR_W-1:0] b;     // <- local intermediate, not function return
        integer k;
        begin
            b = {PTR_W{1'b0}};
            b[PTR_W-1] = g[PTR_W-1];
            for (k = PTR_W-2; k >= 0; k = k-1)
                b[k] = b[k+1] ^ g[k];  // reads b[k+1], not gray2bin[k+1]
            gray2bin = b;
        end
    endfunction

    // -----------------------------------------------------------------------
    // Write-side combinational
    // -----------------------------------------------------------------------
    wire                 winc       = wr_en && !wfull_r;
    wire [PTR_W-1:0]     wbin_next  = wbin + {{(PTR_W-1){1'b0}}, winc};
    wire [PTR_W-1:0]     wgray_next = bin2gray(wbin_next);
    wire [ADDR_BITS-1:0] waddr      = wbin[ADDR_BITS-1:0];

    // Full: Cummings method — invert top 2 bits of synced read-gray, compare
    // with write-gray.  Requires ADDR_BITS >= 2 (PTR_W >= 3).
    wire [PTR_W-1:0] wfull_cmp  = {~rq2_rgray[PTR_W-1:PTR_W-2],
                                     rq2_rgray[PTR_W-3:0]};
    wire             wfull_next = (wgray_next == wfull_cmp);

    // Write-side occupancy: convert synced read-gray to binary, subtract
    wire [PTR_W-1:0] rq2_rbin  = gray2bin(rq2_rgray);
    wire [PTR_W-1:0] wocc_next = wbin_next - rq2_rbin;

    // -----------------------------------------------------------------------
    // Read-side combinational
    // -----------------------------------------------------------------------
    wire                 rinc       = rd_en && !rempty_r;
    wire [PTR_W-1:0]     rbin_next  = rbin + {{(PTR_W-1){1'b0}}, rinc};
    wire [PTR_W-1:0]     rgray_next = bin2gray(rbin_next);
    wire [ADDR_BITS-1:0] raddr      = rbin[ADDR_BITS-1:0];

    wire rempty_next = (rgray_next == wq2_wgray);

    // Read-side occupancy: convert synced write-gray to binary, subtract
    wire [PTR_W-1:0] wq2_wbin  = gray2bin(wq2_wgray);
    wire [PTR_W-1:0] rocc_next = wq2_wbin - rbin_next;

    // -----------------------------------------------------------------------
    // Hysteresis on rd_afull
    //
    //  ON  band : rocc_next >= AF_ON_THRESH  ›  force 1
    //  OFF band : rocc_next <  AF_OFF_THRESH ›  force 0
    //  HOLD band: between thresholds          ›  keep current rd_afull
    //
    // dreq = ~rd_afull in MPEG2_top.
    // The dead-band of AFULL_HYSTERESIS entries prevents high-frequency
    // toggling when the producer refills after being stalled.
    // -----------------------------------------------------------------------
    wire rd_afull_on   = (rocc_next >= AF_ON_THRESH);   // nearly full › stop
    wire rd_afull_off  = (rocc_next <  AF_OFF_THRESH);  // drained enough › go
    wire rd_afull_next = rd_afull_on  ? 1'b1 :
                         rd_afull_off ? 1'b0 :
                                        rd_afull;        // hold state

    // -----------------------------------------------------------------------
    // Write clock domain
    // -----------------------------------------------------------------------
    always @(posedge wr_clk or posedge wr_rst) begin
        if (wr_rst) begin
            wbin      <= {PTR_W{1'b0}};
            wgray     <= {PTR_W{1'b0}};
            rq1_rgray <= {PTR_W{1'b0}};
            rq2_rgray <= {PTR_W{1'b0}};
            wfull_r   <= 1'b0;
            level     <= {(ADDR_BITS+1){1'b0}};
        end else begin
            // 2-FF synchroniser for read pointer
            rq1_rgray <= rgray;
            rq2_rgray <= rq1_rgray;

            if (winc) begin
                mem[waddr] <= din;
                wbin       <= wbin_next;
                wgray      <= wgray_next;
            end

            wfull_r <= wfull_next;
            level   <= wocc_next[ADDR_BITS:0];
        end
    end

    assign full  = wfull_r;

    // -----------------------------------------------------------------------
    // Read clock domain  (DOUT_DELAY=0 or =1)
    // -----------------------------------------------------------------------
    generate
        if (DOUT_DELAY == 0) begin : GRD0
            always @(posedge rd_clk or posedge rd_rst) begin
                if (rd_rst) begin
                    rbin      <= {PTR_W{1'b0}};
                    rgray     <= {PTR_W{1'b0}};
                    wq1_wgray <= {PTR_W{1'b0}};
                    wq2_wgray <= {PTR_W{1'b0}};
                    rempty_r  <= 1'b1;
                    dout      <= {WIDTH{1'b0}};
                    rd_level  <= {(ADDR_BITS+1){1'b0}};
                    rd_afull  <= 1'b0;
                end else begin
                    // 2-FF synchroniser for write pointer
                    wq1_wgray <= wgray;
                    wq2_wgray <= wq1_wgray;

                    if (rinc) begin
                        dout  <= mem[raddr];
                        rbin  <= rbin_next;
                        rgray <= rgray_next;
                    end

                    rempty_r <= rempty_next;
                    rd_level <= rocc_next[ADDR_BITS:0];
                    rd_afull <= rd_afull_next;
                end
            end
        end else begin : GRD1
            reg [WIDTH-1:0] dout_q;
            reg             rinc_d;

            always @(posedge rd_clk or posedge rd_rst) begin
                if (rd_rst) begin
                    rbin      <= {PTR_W{1'b0}};
                    rgray     <= {PTR_W{1'b0}};
                    wq1_wgray <= {PTR_W{1'b0}};
                    wq2_wgray <= {PTR_W{1'b0}};
                    rempty_r  <= 1'b1;
                    dout      <= {WIDTH{1'b0}};
                    dout_q    <= {WIDTH{1'b0}};
                    rinc_d    <= 1'b0;
                    rd_level  <= {(ADDR_BITS+1){1'b0}};
                    rd_afull  <= 1'b0;
                end else begin
                    wq1_wgray <= wgray;
                    wq2_wgray <= wq1_wgray;

                    rinc_d <= rinc;

                    if (rinc) begin
                        dout_q <= mem[raddr];
                        rbin   <= rbin_next;
                        rgray  <= rgray_next;
                    end

                    if (rinc_d)
                        dout <= dout_q;

                    rempty_r <= rempty_next;
                    rd_level <= rocc_next[ADDR_BITS:0];
                    rd_afull <= rd_afull_next;
                end
            end
        end
    endgenerate

    assign empty = rempty_r;

endmodule

