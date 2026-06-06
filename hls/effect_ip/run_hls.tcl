# run_hls.tcl -- Vitis HLS one-shot script: create project, C Sim, synthesis, export
#
# Usage (Vitis HLS Tcl Console or command line):
#   vitis_hls -f run_hls.tcl
#
# Output: effect_ip_prj/solution1/impl/ip/  (Vivado IP-catalog directory)

# Change to the directory containing this script so relative add_files paths work
cd [file dirname [info script]]

# --- Settings ---
set proj_name  "effect_ip_prj"
set top_func   "process_sample"
set part       "xc7z020clg400-1"
set clk_period 10
;# 10 ns = 100 MHz

# --- Create project ---
open_project -reset $proj_name

set_top $top_func

# Add design source files (three .cpp files compiled into a single IP)
add_files effect_ip.h
add_files process_sample.cpp
add_files distortion.cpp
add_files wobble.cpp

# Add testbench
add_files -tb tb_process_sample.cpp

# --- Create solution ---
open_solution -reset "solution1"
set_part $part
create_clock -period $clk_period -name default

# --- C Simulation ---
# Non-zero return from testbench main() causes Vitis HLS to abort here
csim_design -clean

# --- Synthesis ---
csynth_design

# --- Export RTL as Vivado IP catalog ---
# Output: effect_ip_prj/solution1/impl/ip/
export_design -format ip_catalog -vendor "ee_project" -library "hls" -version "1.0"

puts ""
puts "=== Done ==="
puts "IP location : [pwd]/${proj_name}/solution1/impl/ip/"
puts "Next step   : Add the above directory to Vivado IP Repository, then build Block Design."
puts ""
puts "For INTERFACE.md:"
puts "  AXI-Lite offsets : check ADDR_* defines in ${proj_name}/solution1/impl/ip/drivers/*/src/x${top_func}_hw.h"
puts "  IP base address  : read from Vivado Address Editor after integration"
