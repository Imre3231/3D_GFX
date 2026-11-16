// DVI_TOP.v -- DVI (TMDS) output for Spartan-6.  ISE 14.7 / XST, Verilog-2001.
//
// 640x480@60, 25 MHz pixel clock, timing as the old control_generator.
// res_320x240 selects a 320x240 framebuffer shown 2x2, otherwise 640x480;
// fb_addr = y * width + x, cycle for cycle as the old Address1.
// Lanes: 0 blue + HSYNC/VSYNC, 1 green, 2 red.  a = +, b = -.

`timescale 1ns / 1ps

module DVI_TOP #(
    parameter integer CLKIN_MHZ  = 100,          // CLK_IN_100MHZ frequency: 100 or 50
    parameter integer RD_LATENCY = 1,            // pix_clk edges from fb_addr to RGB sample, >= 1 (1: as the old design)
    parameter integer POR_US     = 1000000,      // power-up hold before the PLL starts
    parameter [3:0]   PN_SWAP    = 4'b0000,      // swap a/b per lane: {clk, 2, 1, 0}
    parameter integer TEST_PATTERN = 0           // 1: test pattern instead of the framebuffer
)(
    input         CLK_IN_100MHZ,
    input         rst_in,        // async, active high: restarts the power-up sequence
    input         res_320x240,   // async; 1: 320x240 framebuffer, 0: 640x480
    output        pix_clk,
    output        pix_rst,       // synchronous to pix_clk
    output [22:0] fb_addr,       // pix_clk domain
    output        fb_rd,         // fb_addr is a picture read
    output        fb_vblank,
    input  [7:0]  red,           // sampled RD_LATENCY pix_clk edges after fb_addr
    input  [7:0]  green,
    input  [7:0]  blue,
    output        link_up,       // CLK_IN_100MHZ domain
    output        tmds0b,        // blue + HSYNC/VSYNC
    output        tmds0a,
    output        tmds1b,        // green
    output        tmds1a,
    output        tmds2b,        // red
    output        tmds2a,
    output        tmds_clkb,     // pixel clock
    output        tmds_clka
);
    wire        clk5x;
    wire        rst5x;
    wire        vid_de;          // vid_* aligned with rgb_q
    wire        vid_hs;
    wire        vid_vs;
    wire [2:0]  vid_pat;         // test pattern colour {R, G, B}
    wire [23:0] pix;
    wire [29:0] word;            // {red, green, blue} TMDS characters
    wire [3:0]  lane_a;          // {clk, red, green, blue}
    wire [3:0]  lane_b;
    reg  [23:0] rgb_q = 24'h000000;

    dvi_clkgen #(
        .CLKIN_MHZ (CLKIN_MHZ),
        .POR_US    (POR_US)
    ) u_clk (
        .clk_in  (CLK_IN_100MHZ),
        .rst_in  (rst_in),
        .pix_clk (pix_clk),
        .clk5x   (clk5x),
        .pix_rst (pix_rst),
        .rst5x   (rst5x),
        .link_up (link_up)
    );

    dvi_timing #(
        .RD_LATENCY (RD_LATENCY)
    ) u_tim (
        .clk       (pix_clk),
        .rst       (pix_rst),
        .sel       (res_320x240),
        .fb_addr   (fb_addr),
        .fb_rd     (fb_rd),
        .fb_vblank (fb_vblank),
        .vid_de    (vid_de),
        .vid_hs    (vid_hs),
        .vid_vs    (vid_vs),
        .vid_pat   (vid_pat)
    );

    always @(posedge pix_clk)
        rgb_q <= {red, green, blue};

    assign pix = TEST_PATTERN ? {{8{vid_pat[2]}}, {8{vid_pat[1]}}, {8{vid_pat[0]}}} : rgb_q;

    dvi_tmds_enc u_enc_b (.clk(pix_clk), .rst(pix_rst), .de(vid_de), .d(pix[7:0]),     .c({vid_vs, vid_hs}), .q(word[9:0]));
    dvi_tmds_enc u_enc_g (.clk(pix_clk), .rst(pix_rst), .de(vid_de), .d(pix[15:8]),    .c(2'b00),            .q(word[19:10]));
    dvi_tmds_enc u_enc_r (.clk(pix_clk), .rst(pix_rst), .de(vid_de), .d(pix[23:16]),   .c(2'b00),            .q(word[29:20]));

    dvi_serializer #(
        .PN_SWAP (PN_SWAP)
    ) u_ser (
        .pix_clk (pix_clk),
        .clk5x   (clk5x),
        .rst5x   (rst5x),
        .word    (word),
        .lane_a  (lane_a),
        .lane_b  (lane_b)
    );

    assign {tmds_clka, tmds2a, tmds1a, tmds0a} = lane_a;
    assign {tmds_clkb, tmds2b, tmds1b, tmds0b} = lane_b;
endmodule


// Clocks and reset sequencing.  PLL: PFD 50 MHz, VCO 750 MHz, 125 MHz 5x clock,
// 25 MHz pixel clock.  The PLL is held in reset for POR_US after configuration
// or rst_in, then released; the link runs once LOCKED has been stable for
// 100 us.  Loss of lock stops the link and restarts the PLL.
module dvi_clkgen #(
    parameter integer CLKIN_MHZ = 100,
    parameter integer POR_US    = 1000000
)(
    input  clk_in,
    input  rst_in,
    output pix_clk,
    output clk5x,
    output pix_rst,
    output rst5x,
    output link_up
);
    localparam         CLKIN_NS = 1000.0 / CLKIN_MHZ;   // real: PLL_BASE rejects an integer
    localparam integer POR_CYC  = CLKIN_MHZ * POR_US;
    localparam integer LOCK_CYC = CLKIN_MHZ * 100;     // LOCKED stable for 100 us
    localparam integer RTY_CYC  = CLKIN_MHZ * 100;     // PLL reset on retry: 100 us
    localparam integer TMO_CYC  = CLKIN_MHZ * 2000;    // lock timeout: 2 ms
    localparam [1:0]   S_HOLD   = 2'd0;                // PLL in reset
    localparam [1:0]   S_LOCK   = 2'd1;                // waiting for stable LOCKED
    localparam [1:0]   S_RUN    = 2'd2;

    wire        clkfb;
    wire        clk5x_pll;
    wire        pix_pll;
    wire        pll_locked;
    wire        rst_i;
    reg  [1:0]  st      = S_HOLD;
    reg  [31:0] cnt     = POR_CYC;
    reg  [15:0] stab    = 16'd0;
    reg         pll_rst = 1'b1;
    reg         hold    = 1'b1;                        // link held in reset
    (* ASYNC_REG = "TRUE", SHREG_EXTRACT = "NO" *) reg [1:0] lock_s = 2'b00;

    PLL_BASE #(
        .BANDWIDTH             ("OPTIMIZED"),
        .CLK_FEEDBACK          ("CLKFBOUT"),
        .COMPENSATION          ("INTERNAL"),
        .DIVCLK_DIVIDE         (CLKIN_MHZ / 50),
        .CLKFBOUT_MULT         (15),
        .CLKFBOUT_PHASE        (0.0),
        .CLKIN_PERIOD          (CLKIN_NS),
        .CLKOUT0_DIVIDE        (6),
        .CLKOUT0_PHASE         (0.0),
        .CLKOUT0_DUTY_CYCLE    (0.5),
        .CLKOUT1_DIVIDE        (30),
        .CLKOUT1_PHASE         (0.0),
        .CLKOUT1_DUTY_CYCLE    (0.5),
        .REF_JITTER            (0.1),
        .RESET_ON_LOSS_OF_LOCK ("FALSE")
    ) u_pll (
        .CLKIN    (clk_in),
        .CLKFBIN  (clkfb),
        .RST      (pll_rst),
        .CLKFBOUT (clkfb),
        .CLKOUT0  (clk5x_pll),
        .CLKOUT1  (pix_pll),
        .CLKOUT2  (),
        .CLKOUT3  (),
        .CLKOUT4  (),
        .CLKOUT5  (),
        .LOCKED   (pll_locked)
    );

    BUFG u_bufg_5x  (.I(clk5x_pll), .O(clk5x));
    BUFG u_bufg_pix (.I(pix_pll),   .O(pix_clk));

    dvi_rst_sync u_rs_in  (.clk(clk_in),  .arst(rst_in), .rst(rst_i));
    dvi_rst_sync u_rs_pix (.clk(pix_clk), .arst(hold),   .rst(pix_rst));
    dvi_rst_sync u_rs_5x  (.clk(clk5x),   .arst(hold),   .rst(rst5x));

    always @(posedge clk_in)
        lock_s <= {lock_s[0], pll_locked};

    always @(posedge clk_in)
        if (rst_i) begin
            st      <= S_HOLD;
            cnt     <= POR_CYC;
            pll_rst <= 1'b1;
            hold    <= 1'b1;
        end else
            case (st)
            S_HOLD:
                if (cnt == 0) begin
                    pll_rst <= 1'b0;
                    cnt     <= TMO_CYC;
                    stab    <= 16'd0;
                    st      <= S_LOCK;
                end else
                    cnt <= cnt - 1'b1;
            S_LOCK: begin
                stab <= lock_s[1] ? stab + 1'b1 : 16'd0;
                if (stab == LOCK_CYC) begin
                    hold <= 1'b0;
                    st   <= S_RUN;
                end else if (cnt == 0) begin
                    pll_rst <= 1'b1;
                    cnt     <= RTY_CYC;
                    st      <= S_HOLD;
                end else
                    cnt <= cnt - 1'b1;
            end
            default:
                if (!lock_s[1]) begin
                    pll_rst <= 1'b1;
                    hold    <= 1'b1;
                    cnt     <= RTY_CYC;
                    st      <= S_HOLD;
                end
            endcase

    assign link_up = ~hold;
endmodule


// Reset bridge: asynchronous assert, synchronous release.
module dvi_rst_sync (
    input  clk,
    input  arst,
    output rst
);
    (* ASYNC_REG = "TRUE", SHREG_EXTRACT = "NO" *) reg [2:0] r = 3'b111;

    always @(posedge clk or posedge arst)
        if (arst) r <= 3'b111;
        else      r <= {r[1:0], 1'b0};

    assign rst = r[2];
endmodule


// 640x480@60 raster, mode selection and framebuffer addressing (pix_clk
// domain).  h / v count from the HSYNC / VSYNC leading edges; a new mode
// applies from the next frame.  fb_addr follows control_generator + Address1
// cycle for cycle: the row's first pixel through sync and back porch,
// width * height in the front porch.  The picture fills the active area, so
// fb_rd is also DE.
module dvi_timing #(
    parameter integer RD_LATENCY = 1
)(
    input             clk,
    input             rst,
    input             sel,          // res_320x240, async
    output reg [18:0] fb_addr,
    output reg        fb_rd,
    output reg        fb_vblank,
    output            vid_de,       // vid_*: RD_LATENCY later, aligned with the returned RGB
    output            vid_hs,       // sync levels, negative polarity
    output            vid_vs,
    output     [2:0]  vid_pat       // test pattern colour {R, G, B}
);
    localparam integer H_SYNC = 96;                    // VESA DMT, as control_generator
    localparam integer H_BP   = 48;
    localparam integer H_ACT  = 640;
    localparam integer H_FP   = 16;
    localparam integer V_SYNC = 2;
    localparam integer V_BP   = 33;
    localparam integer V_ACT  = 480;
    localparam integer V_FP   = 10;
    localparam integer H_TOT  = H_SYNC + H_BP + H_ACT + H_FP;
    localparam integer V_TOT  = V_SYNC + V_BP + V_ACT + V_FP;
    localparam integer HA_BEG = H_SYNC + H_BP;
    localparam integer VA_BEG = V_SYNC + V_BP;

    (* ASYNC_REG = "TRUE", SHREG_EXTRACT = "NO" *) reg sel_m = 1'b0;
    (* ASYNC_REG = "TRUE", SHREG_EXTRACT = "NO" *) reg sel_s = 1'b0;
    reg         sel_q = 1'b0;
    reg         req   = 1'b0;       // request, stable for two cycles
    reg         m320  = 1'b0;       // mode of the current frame
    reg  [9:0]  h     = 10'd0;
    reg  [9:0]  v     = 10'd0;
    reg         hs0   = 1'b1;
    reg         vs0   = 1'b1;
    reg  [2:0]  pat0  = 3'b000;
    reg  [6*RD_LATENCY-1:0] dly = {6*RD_LATENCY{1'b0}};

    wire        h_end  = (h == H_TOT - 1);
    wire        f_end  = h_end && (v == V_TOT - 1);
    wire        h_act  = (h >= HA_BEG) && (h < HA_BEG + H_ACT);
    wire        v_act  = (v >= VA_BEG) && (v < VA_BEG + V_ACT);
    // control_generator hcounter / vcounter: position in the active area,
    // 640 / 480 in the front porch, 0 in sync and back porch
    wire [9:0]  hcs    = h_act ? h - HA_BEG : (h < HA_BEG) ? 10'd0 : 10'd640;
    wire [9:0]  vcs    = v_act ? v - VA_BEG : (v < VA_BEG) ? 10'd0 : 10'd480;
    // Address1
    wire [9:0]  ax     = m320 ? {1'b0, hcs[9:1]} : hcs;
    wire [9:0]  ay     = m320 ? {1'b0, vcs[9:1]} : vcs;
    wire        vis    = m320 ? (ax < 10'd320 && ay < 10'd240) : (ax < 10'd640 && ay < 10'd480);
    wire [18:0] row    = m320 ? ({9'd0, ay} << 8) + ({9'd0, ay} << 6)     // y * 320
                              : ({9'd0, ay} << 9) + ({9'd0, ay} << 7);    // y * 640
    wire [18:0] blank  = m320 ? 19'd76800 : 19'd307200;                   // width * height
    wire [2:0]  ruler  = (hcs[1:0] == 2'd0) ? 3'b100 : (hcs[1:0] == 2'd1) ? 3'b010 :
                         (hcs[1:0] == 2'd2) ? 3'b001 : 3'b111;
    wire [5:0]  f0     = {fb_rd, hs0, vs0, pat0};

    // Mode request: synchronised, taken when stable for two cycles
    always @(posedge clk) begin
        sel_m <= sel;
        sel_s <= sel_m;
        sel_q <= sel_s;
        if (sel_s == sel_q)
            req <= sel_s;
    end

    always @(posedge clk)
        if (rst || f_end)
            m320 <= req;

    always @(posedge clk)
        if (rst) begin
            h <= 10'd0;
            v <= 10'd0;
        end else if (h_end) begin
            h <= 10'd0;
            v <= f_end ? 10'd0 : v + 1'b1;
        end else
            h <= h + 1'b1;

    // Stage 0: address, flags and test pattern for (h, v).
    // Pattern: white top and bottom rows, 32-pixel rulers at both sides
    // (columns red, green, blue, white repeating), colour bars between.
    always @(posedge clk)
        if (rst) begin
            hs0       <= 1'b1;
            vs0       <= 1'b1;
            pat0      <= 3'b000;
            fb_rd     <= 1'b0;
            fb_vblank <= 1'b1;
            fb_addr   <= blank;
        end else begin
            hs0       <= (h >= H_SYNC);
            vs0       <= (v >= V_SYNC);
            pat0      <= (vcs == 10'd0 || vcs == V_ACT - 1)   ? 3'b111 :
                         (hcs < 10'd32  || hcs >= H_ACT - 32) ? ruler  : 3'd7 - hcs[9:7];
            fb_rd     <= h_act && v_act;
            fb_vblank <= !v_act;
            fb_addr   <= vis ? row + ax : blank;
        end

    // Delay the flags to meet the RGB returned RD_LATENCY edges after fb_addr
    generate
        if (RD_LATENCY > 1) begin : g_dly
            always @(posedge clk)
                dly <= {dly[6*RD_LATENCY-7:0], f0};
        end else begin : g_dly1
            always @(posedge clk)
                dly <= f0;
        end
    endgenerate

    assign {vid_de, vid_hs, vid_vs, vid_pat} = dly[6*RD_LATENCY-1 -: 6];
endmodule


// DVI 1.0 TMDS encoder, two pipeline stages.  c = {C1, C0}.
module dvi_tmds_enc (
    input            clk,
    input            rst,
    input            de,
    input      [7:0] d,
    input      [1:0] c,
    output reg [9:0] q
);
    // Stage 1: transition-minimised code
    wire [3:0] n1d  = d[0] + d[1] + d[2] + d[3] + d[4] + d[5] + d[6] + d[7];
    wire       xn   = (n1d > 4'd4) || (n1d == 4'd4 && !d[0]);   // XNOR chain
    wire [8:0] qm;
    wire [3:0] n1q  = qm[0] + qm[1] + qm[2] + qm[3] + qm[4] + qm[5] + qm[6] + qm[7];
    reg  [8:0] qm_r = 9'd0;
    reg  [3:0] n1_r = 4'd0;
    reg        de_r = 1'b0;
    reg  [1:0] c_r  = 2'b00;
    // Stage 2: DC balance
    reg  [4:0] cnt  = 5'd0;                        // running disparity, two's complement
    wire [4:0] bal  = {n1_r, 1'b0} - 5'd8;         // ones - zeros of qm_r[7:0]
    wire       zero = (cnt == 5'd0);
    wire       even = (n1_r == 4'd4);
    wire       inv  = (!cnt[4] && !zero && n1_r > 4'd4) || (cnt[4] && n1_r < 4'd4);

    genvar i;
    assign qm[0] = d[0];
    generate
        for (i = 1; i < 8; i = i + 1) begin : g_qm
            assign qm[i] = qm[i-1] ^ d[i] ^ xn;
        end
    endgenerate
    assign qm[8] = !xn;

    always @(posedge clk) begin
        qm_r <= qm;
        n1_r <= n1q;
        de_r <= de && !rst;
        c_r  <= c;
    end

    always @(posedge clk)
        if (rst || !de_r) begin
            cnt <= 5'd0;
            case (c_r)
            2'b00:   q <= 10'b1101010100;
            2'b01:   q <= 10'b0010101011;
            2'b10:   q <= 10'b0101010100;
            default: q <= 10'b1010101011;
            endcase
        end else if (zero || even) begin
            q   <= {!qm_r[8], qm_r[8], qm_r[8] ? qm_r[7:0] : ~qm_r[7:0]};
            cnt <= qm_r[8] ? cnt + bal : cnt - bal;
        end else if (inv) begin
            q   <= {1'b1, qm_r[8], ~qm_r[7:0]};
            cnt <= cnt + {qm_r[8], 1'b0} - bal;
        end else begin
            q   <= {1'b0, qm_r[8], qm_r[7:0]};
            cnt <= cnt - {!qm_r[8], 1'b0} + bal;
        end
endmodule


// 10:1 serializer.  ODDR2 at 5x pixel clock sends two bits per clock, LSB
// first.  pix_clk and clk5x are edge-aligned PLL outputs: a pixel-rate toggle
// marks each new character, which loads 3 clk5x after the pixel edge, inside
// its 5-cycle window.  a and b are complementary single-ended pins, both held
// high in reset.
module dvi_serializer #(
    parameter [3:0] PN_SWAP = 4'b0000
)(
    input         pix_clk,
    input         clk5x,
    input         rst5x,
    input  [29:0] word,           // {red, green, blue}, pix_clk domain
    output [3:0]  lane_a,         // {clk, red, green, blue}
    output [3:0]  lane_b
);
    wire [39:0] chars = {10'b0000011111, word};   // lane 3: pixel clock
    reg         ptog  = 1'b0;
    reg  [1:0]  tog   = 2'b00;
    reg         load  = 1'b0;

    always @(posedge pix_clk)
        ptog <= ~ptog;

    always @(posedge clk5x) begin
        tog  <= {tog[0], ptog};
        load <= tog[1] ^ tog[0];
    end

    genvar k;
    generate
        for (k = 0; k < 4; k = k + 1) begin : g_lane
            wire [9:0] c  = PN_SWAP[k] ? ~chars[10*k +: 10] : chars[10*k +: 10];
            reg  [9:0] sa = 10'h3FF;                   // a pin bits, next pair in [1:0]
            reg  [9:0] sb = 10'h3FF;                   // b pin bits

            always @(posedge clk5x)
                if (rst5x) begin
                    sa <= 10'h3FF;
                    sb <= 10'h3FF;
                end else if (load) begin
                    sa <= c;
                    sb <= ~c;
                end else begin
                    sa <= {2'b11, sa[9:2]};
                    sb <= {2'b11, sb[9:2]};
                end

            ODDR2 #(.DDR_ALIGNMENT("C0"), .INIT(1'b1), .SRTYPE("ASYNC")) u_a (
                .Q(lane_a[k]), .C0(clk5x), .C1(~clk5x), .CE(1'b1),
                .D0(sa[0]), .D1(sa[1]), .R(1'b0), .S(rst5x));

            ODDR2 #(.DDR_ALIGNMENT("C0"), .INIT(1'b1), .SRTYPE("ASYNC")) u_b (
                .Q(lane_b[k]), .C0(clk5x), .C1(~clk5x), .CE(1'b1),
                .D0(sb[0]), .D1(sb[1]), .R(1'b0), .S(rst5x));
        end
    endgenerate
endmodule