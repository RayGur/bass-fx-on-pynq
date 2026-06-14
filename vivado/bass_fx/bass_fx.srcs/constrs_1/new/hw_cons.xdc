# Switches (sw_tri_io = AXI GPIO tristate port name)
set_property PACKAGE_PIN M20 [get_ports {sw_tri_io[0]}]
set_property PACKAGE_PIN M19 [get_ports {sw_tri_io[1]}]
set_property IOSTANDARD LVCMOS33 [get_ports {sw_tri_io[0]}]
set_property IOSTANDARD LVCMOS33 [get_ports {sw_tri_io[1]}]

# Buttons
set_property PACKAGE_PIN D19 [get_ports {btn_tri_io[0]}]
set_property PACKAGE_PIN D20 [get_ports {btn_tri_io[1]}]
set_property PACKAGE_PIN L20 [get_ports {btn_tri_io[2]}]
set_property PACKAGE_PIN L19 [get_ports {btn_tri_io[3]}]
set_property IOSTANDARD LVCMOS33 [get_ports {btn_tri_io[0]}]
set_property IOSTANDARD LVCMOS33 [get_ports {btn_tri_io[1]}]
set_property IOSTANDARD LVCMOS33 [get_ports {btn_tri_io[2]}]
set_property IOSTANDARD LVCMOS33 [get_ports {btn_tri_io[3]}]

# LEDs
set_property PACKAGE_PIN R14 [get_ports {led_tri_io[0]}]
set_property PACKAGE_PIN P14 [get_ports {led_tri_io[1]}]
set_property PACKAGE_PIN N16 [get_ports {led_tri_io[2]}]
set_property PACKAGE_PIN M14 [get_ports {led_tri_io[3]}]
set_property IOSTANDARD LVCMOS33 [get_ports {led_tri_io[0]}]
set_property IOSTANDARD LVCMOS33 [get_ports {led_tri_io[1]}]
set_property IOSTANDARD LVCMOS33 [get_ports {led_tri_io[2]}]
set_property IOSTANDARD LVCMOS33 [get_ports {led_tri_io[3]}]

# Audio codec (ADAU1761) I2S + MCLK
# Note: BD wrapper adds _0 suffix to audio_codec_ctrl ports
set_property PACKAGE_PIN U5  [get_ports audio_clk_10MHz]
set_property PACKAGE_PIN R18 [get_ports bclk_0]
set_property PACKAGE_PIN T17 [get_ports lrclk_0]
set_property PACKAGE_PIN G18 [get_ports sdata_o_0]
set_property PACKAGE_PIN F17 [get_ports sdata_i_0]
set_property PACKAGE_PIN M17 [get_ports {codec_address_0[0]}]
set_property PACKAGE_PIN M18 [get_ports {codec_address_0[1]}]
set_property IOSTANDARD LVCMOS33 [get_ports audio_clk_10MHz]
set_property IOSTANDARD LVCMOS33 [get_ports bclk_0]
set_property IOSTANDARD LVCMOS33 [get_ports lrclk_0]
set_property IOSTANDARD LVCMOS33 [get_ports sdata_o_0]
set_property IOSTANDARD LVCMOS33 [get_ports sdata_i_0]
set_property IOSTANDARD LVCMOS33 [get_ports {codec_address_0[0]}]
set_property IOSTANDARD LVCMOS33 [get_ports {codec_address_0[1]}]

# DRC bypass for unused GPIO bits (bits 2-5 of sw, bits 4-5 of btn/led)
# These exist because arduino board interface forced 6-bit width
set_property SEVERITY {Warning} [get_drc_checks NSTD-1]
set_property SEVERITY {Warning} [get_drc_checks UCIO-1]

# RGB LEDs (LD4=bits[2:0], LD5=bits[5:3]) — Phase 4
# BD wrapper generates port name: rgbleds_tri_o_tri_o (output-only GPIO RTL interface)
# Source pins: PYNQ-Z2 official base.xdc; Bank 13 (LVCMOS33); no conflict with existing pins
set_property PACKAGE_PIN L15 [get_ports {rgbleds_tri_o_tri_o[0]}]
set_property PACKAGE_PIN G17 [get_ports {rgbleds_tri_o_tri_o[1]}]
set_property PACKAGE_PIN N15 [get_ports {rgbleds_tri_o_tri_o[2]}]
set_property PACKAGE_PIN G14 [get_ports {rgbleds_tri_o_tri_o[3]}]
set_property PACKAGE_PIN L14 [get_ports {rgbleds_tri_o_tri_o[4]}]
set_property PACKAGE_PIN M15 [get_ports {rgbleds_tri_o_tri_o[5]}]
set_property IOSTANDARD LVCMOS33 [get_ports {rgbleds_tri_o_tri_o[0]}]
set_property IOSTANDARD LVCMOS33 [get_ports {rgbleds_tri_o_tri_o[1]}]
set_property IOSTANDARD LVCMOS33 [get_ports {rgbleds_tri_o_tri_o[2]}]
set_property IOSTANDARD LVCMOS33 [get_ports {rgbleds_tri_o_tri_o[3]}]
set_property IOSTANDARD LVCMOS33 [get_ports {rgbleds_tri_o_tri_o[4]}]
set_property IOSTANDARD LVCMOS33 [get_ports {rgbleds_tri_o_tri_o[5]}]

# ADAU1761 I2C (PL pins U9=SCL, T9=SDA)
set_property PACKAGE_PIN U9 [get_ports audio_i2c_scl_io]
set_property PACKAGE_PIN T9 [get_ports audio_i2c_sda_io]
set_property IOSTANDARD LVCMOS33 [get_ports audio_i2c_scl_io]
set_property IOSTANDARD LVCMOS33 [get_ports audio_i2c_sda_io]
set_property PULLUP true [get_ports audio_i2c_scl_io]
set_property PULLUP true [get_ports audio_i2c_sda_io]
