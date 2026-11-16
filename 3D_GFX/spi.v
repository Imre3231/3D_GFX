`timescale 1ns / 1ps
// -----------------------------------------------------------------------------
// spi_mpeg_input_por_resync.v
//
// SPI-to-MPEG byte receiver with internal power-on reset, SPI-clocked byte
// capture, async FIFO into MPEG clock domain, one-cycle DATA_WR pulse,
// idle-gap SPI bit-counter realignment, and optional MPEG start-code gate.
//
// ISE 14.7 / Verilog-2001 compatible.
//
// Recommended PIC32 mode:
//   CKP=0, CKE=1, MODE16=0, MODE32=0
// Normal setting for that mode:
//   SAMPLE_FALLING_EDGE=0, MSB_FIRST=1
//
// Important:
//   With no SPI CS/chip-select, no FPGA logic can guarantee byte alignment if
//   the PIC starts while the FPGA is already in the middle of a byte. The safe
//   rule is: FPGA reset/POR finishes, PIC leaves SPI idle for a short gap,
//   then PIC starts sending from a byte boundary.
// -----------------------------------------------------------------------------

module spi_input_mpeg 
#(
    parameter FIFO_ADDR_WIDTH = 7,              // 7 = 128 byte FIFO
    parameter SAMPLE_FALLING_EDGE = 0,          // 0 rising, 1 falling
    parameter MSB_FIRST = 1,
    parameter POWER_ON_RESET_CYCLES = 24'd50000,// 25MHz: 2ms
    parameter GAP_RESET_CYCLES = 16'd16,      // 25MHz: 100us
    parameter ENABLE_START_CODE_ALIGN = 0       // 1 = wait for 00 00 01
)
(
    input  wire       clk_mpeg,
    input  wire       rst,
    input  wire       spi_mosi,
    input  wire       spi_clk,

    output reg  [7:0] DATA_OUT,
    output reg        DATA_WR,

    output wire       fifo_empty,
    output wire       fifo_full,

    output wire       por_active,
    output reg        gap_reset_pulse,
    output reg        start_locked,
    output reg [15:0] dbg_fifo_read_count,
    output reg [15:0] dbg_spi_byte_count,
    output reg        dbg_overflow_sticky
);

    localparam PTR_WIDTH = FIFO_ADDR_WIDTH + 1;
    localparam DEPTH     = (1 << FIFO_ADDR_WIDTH);

    // -------------------------------------------------------------------------
    // Internal power-on reset in clk_mpeg domain
    // -------------------------------------------------------------------------
    reg [23:0] por_cnt;
    reg        por_done;

    assign por_active = ~por_done;

    always @(posedge clk_mpeg or posedge rst) begin
        if (rst) begin
            por_cnt  <= 24'd0;
            por_done <= 1'b0;
        end else begin
            if (!por_done) begin
                if (por_cnt >= POWER_ON_RESET_CYCLES) begin
                    por_done <= 1'b1;
                end else begin
                    por_cnt <= por_cnt + 24'd1;
                end
            end
        end
    end

    wire rst_global;
    assign rst_global = rst | por_active;

    // -------------------------------------------------------------------------
    // SPI idle-gap detector. Resets only the partial SPI bit counter.
    // -------------------------------------------------------------------------
    reg [2:0]  sck_sync_mpeg;
    reg [15:0] gap_cnt;
    reg        gap_reset_done;

    wire sck_changed_mpeg;
    assign sck_changed_mpeg = sck_sync_mpeg[2] ^ sck_sync_mpeg[1];

    always @(posedge clk_mpeg or posedge rst_global) begin
        if (rst_global) begin
            sck_sync_mpeg   <= 3'b000;
            gap_cnt         <= 16'd0;
            gap_reset_done  <= 1'b0;
            gap_reset_pulse <= 1'b0;
        end else begin
            sck_sync_mpeg   <= {sck_sync_mpeg[1:0], spi_clk};
            gap_reset_pulse <= 1'b0;

            if (sck_changed_mpeg) begin
                gap_cnt        <= 16'd0;
                gap_reset_done <= 1'b0;
            end else begin
                if (gap_cnt < GAP_RESET_CYCLES) begin
                    gap_cnt <= gap_cnt + 16'd1;
                end else begin
                    if (!gap_reset_done) begin
                        gap_reset_pulse <= 1'b1;
                        gap_reset_done  <= 1'b1;
                    end
                end
            end
        end
    end

    wire spi_shift_reset_async;
    assign spi_shift_reset_async = rst_global | gap_reset_pulse;

    wire spi_sample_clk;
    assign spi_sample_clk = (SAMPLE_FALLING_EDGE != 0) ? ~spi_clk : spi_clk;

    // -------------------------------------------------------------------------
    // Async FIFO RAM and pointers
    // -------------------------------------------------------------------------
    reg [7:0] mem [0:DEPTH-1];

    reg [PTR_WIDTH-1:0] wr_bin;
    reg [PTR_WIDTH-1:0] wr_gray;
    reg [PTR_WIDTH-1:0] rd_bin;
    reg [PTR_WIDTH-1:0] rd_gray;

    reg [PTR_WIDTH-1:0] rd_gray_sync1_spi;
    reg [PTR_WIDTH-1:0] rd_gray_sync2_spi;
    reg [PTR_WIDTH-1:0] wr_gray_sync1_mpeg;
    reg [PTR_WIDTH-1:0] wr_gray_sync2_mpeg;

    wire [PTR_WIDTH-1:0] wr_bin_next;
    wire [PTR_WIDTH-1:0] wr_gray_next;
    wire [PTR_WIDTH-1:0] rd_bin_next;
    wire [PTR_WIDTH-1:0] rd_gray_next;

    assign wr_bin_next  = wr_bin + {{(PTR_WIDTH-1){1'b0}}, 1'b1};
    assign wr_gray_next = (wr_bin_next >> 1) ^ wr_bin_next;
    assign rd_bin_next  = rd_bin + {{(PTR_WIDTH-1){1'b0}}, 1'b1};
    assign rd_gray_next = (rd_bin_next >> 1) ^ rd_bin_next;

    assign fifo_full = (wr_gray_next ==
                       {~rd_gray_sync2_spi[PTR_WIDTH-1:PTR_WIDTH-2],
                         rd_gray_sync2_spi[PTR_WIDTH-3:0]});

    assign fifo_empty = (rd_gray == wr_gray_sync2_mpeg);

    // -------------------------------------------------------------------------
    // SPI sample-clock domain: shift bits and write completed bytes
    // -------------------------------------------------------------------------
    reg [7:0] shreg_spi;
    reg [2:0] bitcnt_spi;
    reg       byte_toggle_spi;
    reg       overflow_toggle_spi;

    wire [7:0] spi_byte_next;
    wire       spi_byte_complete;

    assign spi_byte_next = (MSB_FIRST != 0) ? {shreg_spi[6:0], spi_mosi}
                                            : {spi_mosi, shreg_spi[7:1]};
    assign spi_byte_complete = (bitcnt_spi == 3'd7);

    always @(posedge spi_sample_clk or posedge spi_shift_reset_async) begin
        if (spi_shift_reset_async) begin
            shreg_spi  <= 8'h00;
            bitcnt_spi <= 3'd0;

            if (rst_global) begin
                wr_bin              <= {PTR_WIDTH{1'b0}};
                wr_gray             <= {PTR_WIDTH{1'b0}};
                rd_gray_sync1_spi   <= {PTR_WIDTH{1'b0}};
                rd_gray_sync2_spi   <= {PTR_WIDTH{1'b0}};
                byte_toggle_spi     <= 1'b0;
                overflow_toggle_spi <= 1'b0;
            end
        end else begin
            rd_gray_sync1_spi <= rd_gray;
            rd_gray_sync2_spi <= rd_gray_sync1_spi;

            shreg_spi <= spi_byte_next;

            if (spi_byte_complete) begin
                bitcnt_spi     <= 3'd0;
                byte_toggle_spi <= ~byte_toggle_spi;

                if (!fifo_full) begin
                    mem[wr_bin[FIFO_ADDR_WIDTH-1:0]] <= spi_byte_next;
                    wr_bin  <= wr_bin_next;
                    wr_gray <= wr_gray_next;
                end else begin
                    overflow_toggle_spi <= ~overflow_toggle_spi;
                end
            end else begin
                bitcnt_spi <= bitcnt_spi + 3'd1;
            end
        end
    end

    // -------------------------------------------------------------------------
    // MPEG clock domain: FIFO read, optional 00 00 01 align, DATA_WR pulse
    // -------------------------------------------------------------------------
    wire [7:0] fifo_dout;
    assign fifo_dout = mem[rd_bin[FIFO_ADDR_WIDTH-1:0]];

    reg [7:0] sync_b0;
    reg [7:0] sync_b1;
    reg [1:0] emit_prefix_count;

    wire start_code_found;
    assign start_code_found = (sync_b0 == 8'h00) &&
                              (sync_b1 == 8'h00) &&
                              (fifo_dout == 8'h01);

    always @(posedge clk_mpeg or posedge rst_global) begin
        if (rst_global) begin
            rd_bin             <= {PTR_WIDTH{1'b0}};
            rd_gray            <= {PTR_WIDTH{1'b0}};
            wr_gray_sync1_mpeg <= {PTR_WIDTH{1'b0}};
            wr_gray_sync2_mpeg <= {PTR_WIDTH{1'b0}};

            DATA_OUT           <= 8'h00;
            DATA_WR            <= 1'b0;

            sync_b0            <= 8'hff;
            sync_b1            <= 8'hff;
            start_locked       <= (ENABLE_START_CODE_ALIGN != 0) ? 1'b0 : 1'b1;
            emit_prefix_count  <= 2'd0;

            dbg_fifo_read_count <= 16'd0;
        end else begin
            wr_gray_sync1_mpeg <= wr_gray;
            wr_gray_sync2_mpeg <= wr_gray_sync1_mpeg;

            DATA_WR <= 1'b0;

            if (emit_prefix_count != 2'd0) begin
                DATA_WR <= 1'b1;

                if (emit_prefix_count == 2'd3) begin
                    DATA_OUT <= 8'h00;
                end else if (emit_prefix_count == 2'd2) begin
                    DATA_OUT <= 8'h00;
                end else begin
                    DATA_OUT <= 8'h01;
                end

                emit_prefix_count <= emit_prefix_count - 2'd1;
                dbg_fifo_read_count <= dbg_fifo_read_count + 16'd1;
            end else begin
                if (!fifo_empty) begin
                    if ((ENABLE_START_CODE_ALIGN != 0) && (!start_locked)) begin
                        sync_b0 <= sync_b1;
                        sync_b1 <= fifo_dout;

                        rd_bin  <= rd_bin_next;
                        rd_gray <= rd_gray_next;

                        if (start_code_found) begin
                            start_locked      <= 1'b1;
                            emit_prefix_count <= 2'd3;
                        end
                    end else begin
                        DATA_OUT <= fifo_dout;
                        DATA_WR  <= 1'b1;

                        rd_bin  <= rd_bin_next;
                        rd_gray <= rd_gray_next;

                        dbg_fifo_read_count <= dbg_fifo_read_count + 16'd1;
                    end
                end
            end
        end
    end

    // -------------------------------------------------------------------------
    // Debug: completed SPI byte count synchronized into clk_mpeg domain
    // -------------------------------------------------------------------------
    reg [2:0] byte_toggle_sync;
    reg       byte_toggle_last;

    always @(posedge clk_mpeg or posedge rst_global) begin
        if (rst_global) begin
            byte_toggle_sync   <= 3'b000;
            byte_toggle_last   <= 1'b0;
            dbg_spi_byte_count <= 16'd0;
        end else begin
            byte_toggle_sync <= {byte_toggle_sync[1:0], byte_toggle_spi};

            if (byte_toggle_sync[2] != byte_toggle_last) begin
                byte_toggle_last   <= byte_toggle_sync[2];
                dbg_spi_byte_count <= dbg_spi_byte_count + 16'd1;
            end
        end
    end

    // -------------------------------------------------------------------------
    // Debug: overflow sticky synchronized into clk_mpeg domain
    // -------------------------------------------------------------------------
    reg [2:0] overflow_toggle_sync;
    reg       overflow_toggle_last;

    always @(posedge clk_mpeg or posedge rst_global) begin
        if (rst_global) begin
            overflow_toggle_sync <= 3'b000;
            overflow_toggle_last <= 1'b0;
            dbg_overflow_sticky  <= 1'b0;
        end else begin
            overflow_toggle_sync <= {overflow_toggle_sync[1:0], overflow_toggle_spi};

            if (overflow_toggle_sync[2] != overflow_toggle_last) begin
                overflow_toggle_last <= overflow_toggle_sync[2];
                dbg_overflow_sticky  <= 1'b1;
            end
        end
    end

endmodule
