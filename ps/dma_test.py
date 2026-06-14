#!/usr/bin/env python3
# sudo python3 ~/dma_test.py
from pynq import Overlay, allocate, MMIO
import numpy as np, time

BIT = "/home/xilinx/bass_fx_bd.bit"
N = 256

DMA_MM2S_CR = 0x00
DMA_MM2S_SR = 0x04
DMA_MM2S_SA = 0x18
DMA_MM2S_LEN = 0x28
DMA_S2MM_CR = 0x30
DMA_S2MM_SR = 0x34
DMA_S2MM_DA = 0x48
DMA_S2MM_LEN = 0x58
IOC = 1 << 12
RS = 1

ol = Overlay(BIT, ignore_version=True)
dma = MMIO(0x41E00000, 0x10000)
eff = MMIO(0x40020000, 0x10000)

eff.write(0x10, N)
eff.write(0x18, 0)
eff.write(0x20, 0)
eff.write(0x00, 0x81)
print(f"effect CTRL={hex(eff.read(0x00))}  n_samples={eff.read(0x10)}")

in_buf = allocate(shape=(N * 2,), dtype=np.int32)
out_buf = allocate(shape=(N * 2,), dtype=np.int32)
in_buf[:] = np.arange(N * 2, dtype=np.int32) * 100
out_buf[:] = np.int32(-0x55555556)  # 0xAAAAAAAA sentinel
in_buf.flush()
out_buf.flush()

in_p = in_buf.physical_address
out_p = out_buf.physical_address
print(f"in  phys=0x{in_p:08x}  out phys=0x{out_p:08x}")

dma.write(DMA_MM2S_CR, 1 << 2)
dma.write(DMA_S2MM_CR, 1 << 2)
time.sleep(0.01)
dma.write(DMA_MM2S_CR, RS)
dma.write(DMA_S2MM_CR, RS)
print(
    f"post-reset SR: MM2S={hex(dma.read(DMA_MM2S_SR))} S2MM={hex(dma.read(DMA_S2MM_SR))}"
)

dma.write(DMA_S2MM_DA, out_p)
dma.write(DMA_S2MM_LEN, N * 2 * 4)
dma.write(DMA_MM2S_SA, in_p)
dma.write(DMA_MM2S_LEN, N * 2 * 4)

t0 = time.time()
while not (dma.read(DMA_MM2S_SR) & IOC):
    if time.time() - t0 > 5:
        print("MM2S TIMEOUT")
        break
dma.write(DMA_MM2S_SR, IOC)

t0 = time.time()
while not (dma.read(DMA_S2MM_SR) & IOC):
    if time.time() - t0 > 5:
        print("S2MM TIMEOUT")
        break
s2mm_sr = dma.read(DMA_S2MM_SR)
print(f"S2MM SR={hex(s2mm_sr)}  LEN_rem={dma.read(DMA_S2MM_LEN)}")
print(f"  halted={s2mm_sr&1} idle={(s2mm_sr>>1)&1} int_err={(s2mm_sr>>4)&1} slv_err={(s2mm_sr>>5)&1} dec_err={(s2mm_sr>>6)&1} ioc={(s2mm_sr>>12)&1}")
dma.write(DMA_S2MM_SR, IOC)

out_buf.invalidate()
print(f"in_buf [:8] = {list(in_buf[:8])}")
print(f"out_buf[:8] = {list(out_buf[:8])}")

# Check alternating pattern
sentinel = int(np.int32(-0x55555556))  # 0xAAAAAAAA
ok_even  = all(int(out_buf[i]) == int(in_buf[i]) & 0xFFFFFF for i in range(0, N*2, 2))
all_odd_sentinel = all(int(out_buf[i]) == sentinel           for i in range(1, N*2, 2))
print(f"even positions match input (L-ch): {ok_even}")
print(f"odd  positions all sentinel  (R-ch): {all_odd_sentinel}")
num_written  = sum(1 for i in range(N*2) if int(out_buf[i]) != sentinel)
num_sentinel = sum(1 for i in range(N*2) if int(out_buf[i]) == sentinel)
print(f"total written={num_written}  sentinel={num_sentinel}  (expected written={N*2})")
