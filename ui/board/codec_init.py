"""
codec_init.py — Phase 6 startup script (fixed BIT_PATH for ui_dev)
1. Load overlay (Phase 6 bitstream)
2. Initialise ADAU1761 codec via AXI IIC
3. Exit (PL state preserved; run sudo ./audio_dma next)

Usage:
    sudo python3 codec_init.py
    sudo ./audio_dma
"""

import time
from pynq import Overlay
from pynq.lib.iic import AxiIIC

BIT_PATH   = "/home/xilinx/bass-fx/ui_dev/bass_fx_bd.bit"
IIC_BASE   = 0x40800000
IIC_RANGE  = 0x10000
CODEC_ADDR = 0x3B

# ── Load overlay ──────────────────────────────────────────────
print("Loading overlay: {}".format(BIT_PATH))
ol = Overlay(BIT_PATH, ignore_version=True)
print("Overlay loaded.")

# ── AXI IIC helper ────────────────────────────────────────────
iic = ol.axi_iic_0
ffi = iic._ffi

def iic_write(reg, val):
    tx = ffi.new("unsigned char[3]", [0x40, reg, val])
    iic.send(CODEC_ADDR, tx, 3)

# ── ADAU1761 init ─────────────────────────────────────────────
print("Initialising ADAU1761...")

# PLL: 10 MHz MCLK → 49.152 MHz → 48 kHz
iic_write(0x00, 0x0E)

pll = ffi.new("unsigned char[8]",
              [0x40, 0x02, 0x02, 0x71, 0x02, 0x3C, 0x21, 0x03])
iic.send(CODEC_ADDR, pll, 8)

ptr   = ffi.new("unsigned char[2]", [0x40, 0x02])
rxbuf = ffi.new("unsigned char[6]")
locked = False
for _ in range(200):
    iic.send(CODEC_ADDR, ptr, 2, 1)
    iic.receive(CODEC_ADDR, rxbuf, 6)
    if rxbuf[5] & 0x02:
        locked = True
        break
    time.sleep(0.01)

if not locked:
    raise RuntimeError("ADAU1761 PLL did not lock — check MCLK (U5) and I2C (U9/T9)")
print("  PLL locked.")

iic_write(0x00, 0x0F)

# Codec register init
iic_write(0x0A, 0x00)
iic_write(0x0C, 0x00)
iic_write(0x0E, 0xB3)
iic_write(0x0F, 0xB3)
iic_write(0x10, 0x01)
iic_write(0x14, 0x20)
iic_write(0x15, 0x01)
iic_write(0x19, 0x33)
iic_write(0x1C, 0x00)
iic_write(0x1D, 0x00)
iic_write(0x1E, 0x00)
iic_write(0x1F, 0x00)
iic_write(0x23, 0xE5)
iic_write(0x24, 0xE5)
iic_write(0x29, 0x03)
iic_write(0x2A, 0x03)
iic_write(0xF2, 0x01)
iic_write(0xF3, 0x01)
iic_write(0xF5, 0x01)
iic_write(0xF6, 0x01)
iic_write(0xF9, 0x7F)
iic_write(0xFA, 0x03)

# Line in + HP out
iic_write(0x0A, 0x01)
iic_write(0x0B, 0x07)
iic_write(0x0C, 0x01)
iic_write(0x0D, 0x07)
iic_write(0x1C, 0x21)
iic_write(0x1E, 0x41)
iic_write(0x23, 0xE7)
iic_write(0x24, 0xE7)

print("  Codec configured: 48 kHz, line-in, HP out enabled.")
print()
print("Done. Now run:  sudo ./audio_dma")
