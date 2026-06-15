#!/usr/bin/env python3
"""
ctrl_client.py — Bass FX AXI-Lite control client (board-side, 14.6)

Run: sudo python3 ctrl_client.py

Protocol (stdin, one command per line):
    dist_en 1
    wobble_en 0
    threshold 0.30       # float → Q1.23 int
    gain 8               # integer 1–20
    lfo_rate 2.0         # Hz → phase increment (round(hz * 2^32 / 48000))
    lfo_depth 80         # integer 0–100
    lfo_floor 6          # integer 0–15

stdout (every 100 ms):
    STATE sw=01 dist_preset=0 wobble_preset=1 wah=0

Manages /tmp/bass_ui_active sentinel (audio_dma skips sw polling when present).
"""

import sys
import os
import mmap
import struct
import threading
import time
import signal
import atexit

# ── AXI-Lite register map ──────────────────────────────────────
EFFECT_BASE  = 0x40020000
EFFECT_SIZE  = 0x1000  # 4 KB — covers all effect registers

OFFSET_DIST_EN   = 0x18
OFFSET_WOBBLE_EN = 0x20
OFFSET_THRESHOLD = 0x28
OFFSET_GAIN      = 0x30
OFFSET_LFO_RATE  = 0x38
OFFSET_LFO_DEPTH = 0x40
OFFSET_LFO_FLOOR = 0x48

# ── GPIO (sw[1:0] input) ───────────────────────────────────────
GPIO0_BASE  = 0x40000000
GPIO0_SIZE  = 0x1000
GPIO_DATA   = 0x000  # ch1: sw[1:0]

# ── Sentinel ───────────────────────────────────────────────────
SENTINEL = "/tmp/bass_ui_active"

# ── Tracked state (for STATE output) ──────────────────────────
dist_preset   = 0   # 0=L, 1=H
wobble_preset = 0   # 0=slow, 1=fast
wah_depth     = 0   # 0=A, 1=B, 2=C

# ── /dev/mem mmap ─────────────────────────────────────────────
_devmem_fd  = None
_effect_mm  = None
_gpio0_mm   = None
_lock       = threading.Lock()   # protect mmap reads/writes


def init_mmap():
    global _devmem_fd, _effect_mm, _gpio0_mm
    _devmem_fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
    _effect_mm = mmap.mmap(_devmem_fd, EFFECT_SIZE,
                           flags=mmap.MAP_SHARED,
                           prot=mmap.PROT_READ | mmap.PROT_WRITE,
                           offset=EFFECT_BASE)
    _gpio0_mm  = mmap.mmap(_devmem_fd, GPIO0_SIZE,
                           flags=mmap.MAP_SHARED,
                           prot=mmap.PROT_READ | mmap.PROT_WRITE,
                           offset=GPIO0_BASE)


def _reg_write(mm, offset, value):
    mm.seek(offset)
    mm.write(struct.pack('<I', int(value) & 0xFFFFFFFF))


def _reg_read(mm, offset):
    mm.seek(offset)
    return struct.unpack('<I', mm.read(4))[0]


def write_effect(offset, value):
    with _lock:
        _reg_write(_effect_mm, offset, value)


def read_gpio_sw():
    with _lock:
        return _reg_read(_gpio0_mm, GPIO_DATA) & 0x3


# ── Command dispatch ───────────────────────────────────────────
# lfo_floor → wah preset index table
_WAH_FLOOR_TO_IDX = {6: 0, 4: 1, 0: 2}


def handle_command(line):
    global dist_preset, wobble_preset, wah_depth
    parts = line.strip().split()
    if len(parts) < 2:
        return
    cmd, arg = parts[0], parts[1]
    try:
        if cmd == 'dist_en':
            write_effect(OFFSET_DIST_EN, int(arg) & 1)
        elif cmd == 'wobble_en':
            write_effect(OFFSET_WOBBLE_EN, int(arg) & 1)
        elif cmd == 'threshold':
            write_effect(OFFSET_THRESHOLD, int(float(arg) * (1 << 23)))
        elif cmd == 'gain':
            write_effect(OFFSET_GAIN, int(arg))
        elif cmd == 'lfo_rate':
            # Hz → phase increment: round(hz * 2^32 / 48000)
            write_effect(OFFSET_LFO_RATE, round(float(arg) * (1 << 32) / 48000))
        elif cmd == 'lfo_depth':
            write_effect(OFFSET_LFO_DEPTH, int(arg))
        elif cmd == 'lfo_floor':
            val = int(arg)
            write_effect(OFFSET_LFO_FLOOR, val)
            wah_depth = _WAH_FLOOR_TO_IDX.get(val, 0)
        elif cmd == 'dist_preset':
            dist_preset = int(arg) & 1
        elif cmd == 'wobble_preset':
            wobble_preset = int(arg) & 1
    except (ValueError, OverflowError) as e:
        sys.stderr.write("[ctrl_client] bad cmd: {} ({})\n".format(line.strip(), e))
        sys.stderr.flush()


# ── Stdin reader thread ────────────────────────────────────────
def stdin_reader():
    try:
        for line in sys.stdin:
            if line.strip():
                handle_command(line)
    except Exception:
        pass


# ── Cleanup ───────────────────────────────────────────────────
def cleanup():
    try:
        os.unlink(SENTINEL)
    except OSError:
        pass
    try:
        if _effect_mm:
            _effect_mm.close()
        if _gpio0_mm:
            _gpio0_mm.close()
        if _devmem_fd is not None:
            os.close(_devmem_fd)
    except Exception:
        pass


def _sig_handler(sig, frame):
    cleanup()
    sys.exit(0)


# ── Main ──────────────────────────────────────────────────────
def main():
    # Touch sentinel — audio_dma will skip sw polling while this exists
    open(SENTINEL, 'w').close()
    atexit.register(cleanup)
    signal.signal(signal.SIGTERM, _sig_handler)
    signal.signal(signal.SIGINT,  _sig_handler)

    init_mmap()

    # Stdin reader (daemon — exits when main exits)
    t = threading.Thread(target=stdin_reader, daemon=True)
    t.start()

    # Main polling loop: output STATE every 100 ms
    while True:
        sw  = read_gpio_sw()
        sw1 = (sw >> 1) & 1
        sw0 = sw & 1
        sys.stdout.write(
            "STATE sw={}{} dist_preset={} wobble_preset={} wah={}\n".format(
                sw1, sw0, dist_preset, wobble_preset, wah_depth))
        sys.stdout.flush()
        time.sleep(0.1)


if __name__ == '__main__':
    main()
