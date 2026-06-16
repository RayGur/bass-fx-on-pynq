#!/bin/bash
# start.sh — Bass FX startup (14.5)
# Combines codec init and audio DMA into one command.
#
# Interactive usage (as xilinx):
#   bash ~/bass-fx/ui_dev/start.sh
#
# bass_ui.py usage (password via stdin):
#   sudo -S bash ~/bass-fx/ui_dev/start.sh   (password sent on stdin)
#
# When running as root the inner sudo calls are omitted to avoid
# nested-sudo auth issues in non-TTY SSH sessions.

DIR="$(cd "$(dirname "$0")" && pwd)"

# Clean up stale bass_ui sentinel from any previous crashed session
rm -f /tmp/bass_ui_active 2>/dev/null || true

echo "[start] Initialising codec..."

if [ "$(id -u)" = "0" ]; then
    # Already root (e.g. called via sudo -S from bass_ui.py)
    python3 "$DIR/codec_init.py"
    echo "[start] Codec ready. Starting audio DMA loop..."
    exec "$DIR/audio_dma"
else
    # Interactive session as xilinx — use sudo normally
    sudo python3 "$DIR/codec_init.py"
    echo "[start] Codec ready. Starting audio DMA loop..."
    exec sudo "$DIR/audio_dma"
fi
