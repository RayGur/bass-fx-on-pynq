// clk_oddr.v -- Clock-to-IO output buffer using ODDR primitive
//
// Converts a clock network signal to a regular data output that can
// drive any IO pin. Required because FPGA clock networks cannot
// directly drive IO pins.
//
// Usage in block design:
//   - clk_in  : connect to clk_wiz_10MHz/clk_out1
//   - clk_out : make external, assign to U5 (ADAU1761 MCLK) in XDC
//
// Device: Xilinx 7-series / Zynq (XC7Z020)

`default_nettype none

module clk_oddr (
    input  wire clk_in,   // clock input from MMCM/PLL (10 MHz)
    output wire clk_out   // clock output to IO pin
);

    ODDR #(
        .DDR_CLK_EDGE ("SAME_EDGE"),
        .INIT         (1'b0),
        .SRTYPE       ("SYNC")
    ) u_oddr (
        .Q  (clk_out),  // 1-bit DDR output → IO pin
        .C  (clk_in),   // clock input from clk_wiz
        .CE (1'b1),     // always enabled
        .D1 (1'b1),     // high on rising edge
        .D2 (1'b0),     // low on falling edge → 50% duty cycle
        .R  (1'b0),     // no reset
        .S  (1'b0)      // no set
    );

endmodule

`default_nettype wire
