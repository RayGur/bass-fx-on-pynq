#!/usr/bin/env python3
"""
bass_ui.py — Bass FX PC-side GUI (Tkinter + paramiko)

Guitar pedalboard-style control panel for the PYNQ-Z2 bass effect processor.

Connections (two SSH channels):
    CH1: sudo python3 ~/bass-fx/ui_dev/ctrl_client.py
         stdin → effect commands;  stdout → STATE lines (100 ms)
    CH2: bash ~/bass-fx/ui_dev/start.sh
         runs codec_init.py then audio_dma (blocks until stopped)

Usage: python3 bass_ui.py
Dependency: pip install paramiko
"""

import tkinter as tk
from tkinter import messagebox
import threading
import time
import sys
import math

try:
    import paramiko
except ImportError:
    print("ERROR: missing dependency — pip install paramiko")
    sys.exit(1)

# ── Board paths ───────────────────────────────────────────────
BOARD_WORKDIR = "/home/xilinx/bass-fx/ui_dev"
# sudo -S reads password from stdin; start.sh avoids inner-sudo when root;
# ctrl_client.py stdin receives the password line first, then effect commands.
CTRL_CMD  = f"sudo -S python3 {BOARD_WORKDIR}/ctrl_client.py"
START_CMD = f"sudo -S bash {BOARD_WORKDIR}/start.sh"

# ── Colour palette (dark pedalboard) ─────────────────────────
C_BG        = "#141414"
C_PANEL     = "#1c1c1c"
C_PEDAL     = "#212121"
C_PEDAL_RIM = "#333333"
C_STOMP     = "#2e2e2e"
C_STOMP_ON  = "#3a1010"
C_STOMP_RIM = "#555555"
C_LED_OFF   = "#1a0808"
C_LED_ON    = "#ff2200"
C_LED_RIM   = "#2a0000"
C_TEXT      = "#d8d8d8"
C_DIM       = "#666666"
C_ACCENT    = "#ff5533"   # distortion orange-red
C_BLUE      = "#3399ff"   # wobble blue
C_BTN       = "#2a2a2a"
C_BTN_ON    = "#4a2010"
C_KNOB_BG   = "#383838"
C_KNOB_RIM  = "#4a4a4a"
C_KNOB_PTR  = "#ff5533"
C_TRACK     = "#2a2a2a"

# ── Preset tables ─────────────────────────────────────────────
DIST_PRESETS = [
    {"threshold": 0.5,  "gain": 4},   # L — gentle clip
    {"threshold": 0.2,  "gain": 12},  # H — heavy clip
]
WOBBLE_PRESETS = [
    {"lfo_rate": 1.0, "lfo_depth": 80},   # SLOW — 1 Hz
    {"lfo_rate": 4.0, "lfo_depth": 100},  # FAST — 4 Hz
]
WAH_PRESETS = [6, 4, 0]  # A, B, C → lfo_floor values


# ─────────────────────────────────────────────────────────────
class Knob(tk.Canvas):
    """
    Rotary knob: drag up to increase, drag down to decrease.
    Draws a 270° arc track with pointer showing current value.
    """
    SIZE   = 76   # canvas width/height
    RADIUS = 28   # knob circle radius

    def __init__(self, parent, label, min_val, max_val, init_val,
                 fmt="{:.1f}", on_change=None, **kwargs):
        super().__init__(parent,
                         width=self.SIZE, height=self.SIZE + 26,
                         bg=C_PEDAL, highlightthickness=0, **kwargs)
        self.label     = label
        self.min_val   = float(min_val)
        self.max_val   = float(max_val)
        self.value     = float(init_val)
        self.fmt       = fmt
        self.on_change = on_change
        self._drag_y   = None

        self._draw()
        self.bind("<ButtonPress-1>",   self._press)
        self.bind("<B1-Motion>",       self._drag)
        self.bind("<ButtonRelease-1>", self._release)

    # ── Drawing ───────────────────────────────────────────────

    def _val_to_angle_deg(self, val):
        """Map value → canvas angle (degrees from east, CCW positive)."""
        frac = (val - self.min_val) / (self.max_val - self.min_val)
        # sweep: 225° (7 o'clock) → -45° (5 o'clock), clockwise = 270°
        return 225.0 - frac * 270.0

    def _draw(self):
        self.delete("all")
        cx = self.SIZE // 2
        cy = self.RADIUS + 6
        r  = self.RADIUS

        # Track arc (full sweep, dark)
        self.create_arc(cx - r, cy - r, cx + r, cy + r,
                        start=225, extent=-270,
                        style=tk.ARC, outline=C_TRACK, width=4)

        # Active arc (start → value, accent colour)
        frac = (self.value - self.min_val) / (self.max_val - self.min_val)
        if frac > 0.001:
            self.create_arc(cx - r, cy - r, cx + r, cy + r,
                            start=225, extent=-frac * 270,
                            style=tk.ARC, outline=C_ACCENT, width=4)

        # Knob body
        ir = r - 7
        self.create_oval(cx - ir, cy - ir, cx + ir, cy + ir,
                         fill=C_KNOB_BG, outline=C_KNOB_RIM, width=1)

        # Pointer
        ang = math.radians(self._val_to_angle_deg(self.value))
        tip = ir - 3
        px  = cx + tip * math.cos(ang)
        py  = cy - tip * math.sin(ang)
        self.create_line(cx, cy, px, py,
                         fill=C_KNOB_PTR, width=2, capstyle=tk.ROUND)

        # Centre dot
        self.create_oval(cx - 2, cy - 2, cx + 2, cy + 2,
                         fill=C_DIM, outline="")

        # Label above
        self.create_text(cx, 2, text=self.label,
                         fill=C_DIM, font=("Arial", 7), anchor="n")

        # Value below
        self.create_text(cx, self.SIZE + 14,
                         text=self.fmt.format(self.value),
                         fill=C_TEXT, font=("Courier", 9, "bold"),
                         anchor="center")

    # ── Interaction ───────────────────────────────────────────

    def set_value(self, val, notify=True):
        self.value = max(self.min_val, min(self.max_val, float(val)))
        self._draw()
        if notify and self.on_change:
            self.on_change(self.value)

    def _press(self, e):
        self._drag_y = e.y

    def _drag(self, e):
        if self._drag_y is None:
            return
        dy = self._drag_y - e.y          # positive = drag up = increase
        step = (self.max_val - self.min_val) / 120.0
        self.set_value(self.value + dy * step)
        self._drag_y = e.y

    def _release(self, _e):
        self._drag_y = None


# ─────────────────────────────────────────────────────────────
class App:
    def __init__(self, root: tk.Tk):
        self.root = root
        root.title("Bass FX")
        root.configure(bg=C_BG)
        root.resizable(False, False)

        # SSH / channel state
        self.ssh          = None
        self._board_pass  = ""       # stored at connect; used for sudo -S
        self.ctrl_stdin   = None
        self.ctrl_stdout  = None
        self.fx_chan      = None
        self._stop_ev     = threading.Event()
        self._ctrl_lock   = threading.Lock()

        # Effect enable state (driven by both GPIO sw and UI stomps)
        self.dist_en    = tk.BooleanVar(value=False)
        self.wobble_en  = tk.BooleanVar(value=False)

        # GPIO sync helpers
        self._last_sw        = "--"   # last seen sw string ("01" etc.)
        self._gpio_updating  = False  # True while syncing from GPIO; suppresses send_cmd

        # Preset indices (tracked for STATE sync)
        self.dist_preset_idx    = 0
        self.wobble_preset_idx  = 0
        self.wah_idx            = 0

        self._build_ui()

    # ── UI construction ───────────────────────────────────────

    def _build_ui(self):
        self._build_topbar()
        self._build_board()
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

    def _build_topbar(self):
        top = tk.Frame(self.root, bg=C_PANEL, padx=10, pady=6)
        top.pack(fill=tk.X)

        def lbl(text):
            return tk.Label(top, text=text, bg=C_PANEL, fg=C_DIM, font=("Arial", 9))

        def entry(var, width, show=None):
            kw = {}
            if show:
                kw["show"] = show
            return tk.Entry(top, textvariable=var, width=width,
                            bg="#252525", fg=C_TEXT, insertbackground=C_TEXT,
                            relief=tk.FLAT, font=("Courier", 9),
                            highlightthickness=1, highlightcolor="#444444",
                            highlightbackground="#333333", **kw)

        self.host_var = tk.StringVar(value="")
        self.user_var = tk.StringVar(value="xilinx")
        self.pass_var = tk.StringVar(value="")

        lbl("Host:").pack(side=tk.LEFT)
        entry(self.host_var, 15).pack(side=tk.LEFT, padx=(2, 8))
        lbl("User:").pack(side=tk.LEFT)
        entry(self.user_var, 8).pack(side=tk.LEFT, padx=(2, 8))
        lbl("Pass:").pack(side=tk.LEFT)
        entry(self.pass_var, 8, show="*").pack(side=tk.LEFT, padx=(2, 8))

        self.conn_btn = self._mkbtn(top, "Connect", self._connect,
                                    bg="#1e2e1e", fg="#66cc66")
        self.conn_btn.pack(side=tk.LEFT, padx=(0, 4))

        self.start_btn = self._mkbtn(top, "▶ Start FX", self._start_fx,
                                     bg="#1a2a3a", fg=C_BLUE, state=tk.DISABLED)
        self.start_btn.pack(side=tk.LEFT, padx=4)

        self.stop_btn = self._mkbtn(top, "■ Stop", self._stop_fx,
                                    bg="#2a1a1a", fg="#ff6644", state=tk.DISABLED)
        self.stop_btn.pack(side=tk.LEFT, padx=4)

        # Right side: sw indicator + status
        self.sw_var = tk.StringVar(value="SW:--")
        tk.Label(top, textvariable=self.sw_var, bg=C_PANEL, fg=C_DIM,
                 font=("Courier", 8)).pack(side=tk.RIGHT, padx=6)

        self.status_var = tk.StringVar(value="● Disconnected")
        self.status_lbl = tk.Label(top, textvariable=self.status_var,
                                   bg=C_PANEL, fg=C_DIM, font=("Courier", 9))
        self.status_lbl.pack(side=tk.RIGHT, padx=10)

    def _mkbtn(self, parent, text, cmd, bg=C_BTN, fg=C_TEXT, state=tk.NORMAL):
        return tk.Button(parent, text=text, command=cmd,
                         bg=bg, fg=fg,
                         activebackground="#3a3a3a", activeforeground=C_TEXT,
                         relief=tk.FLAT, font=("Arial", 9, "bold"),
                         padx=8, pady=2, state=state)

    def _build_board(self):
        board = tk.Frame(self.root, bg=C_BG, padx=20, pady=16)
        board.pack(fill=tk.BOTH, expand=True)

        self._build_dist_pedal(board)

        # Centre divider
        tk.Frame(board, bg="#2a2a2a", width=2).pack(
            side=tk.LEFT, fill=tk.Y, padx=16, pady=8)

        self._build_wobble_pedal(board)

    # ── Pedal helpers ─────────────────────────────────────────

    def _pedal_frame(self, parent, title, color):
        outer = tk.Frame(parent, bg=C_PEDAL_RIM, padx=2, pady=2)
        outer.pack(side=tk.LEFT, fill=tk.Y)
        inner = tk.Frame(outer, bg=C_PEDAL, padx=16, pady=14)
        inner.pack(fill=tk.BOTH, expand=True)
        tk.Label(inner, text=title, bg=C_PEDAL, fg=color,
                 font=("Arial", 14, "bold")).pack(pady=(0, 4))
        return inner

    def _led(self, parent, var):
        c = tk.Canvas(parent, width=18, height=18, bg=C_PEDAL,
                      highlightthickness=0)
        c.pack(pady=2)

        def redraw(*_):
            c.delete("all")
            fill = C_LED_ON if var.get() else C_LED_OFF
            c.create_oval(1, 1, 17, 17, fill=fill,
                          outline=C_LED_RIM, width=1)

        var.trace_add("write", redraw)
        redraw()

    def _stomp(self, parent, var, cmd_name):
        """Large rubber stomp switch — click to toggle effect on/off."""
        c = tk.Canvas(parent, width=108, height=108, bg=C_PEDAL,
                      highlightthickness=0, cursor="hand2")
        c.pack(pady=6)

        def redraw(*_):
            c.delete("all")
            on  = var.get()
            bg  = C_STOMP_ON if on else C_STOMP
            # Shadow
            c.create_oval(7, 7, 103, 105, fill="#111111", outline="")
            # Outer ring
            c.create_oval(4, 4, 104, 104,
                          fill="#1a1a1a", outline=C_STOMP_RIM, width=2)
            # Rubber pad
            c.create_oval(12, 12, 96, 96, fill=bg,
                          outline="#666666" if on else "#444444", width=2)
            # Specular highlight
            c.create_arc(22, 14, 58, 38, start=30, extent=120,
                         style=tk.ARC, outline="#ffffff", width=1)
            # Label
            txt = "ON" if on else "OFF"
            col = C_ACCENT if on else C_DIM
            c.create_text(54, 54, text=txt, fill=col,
                          font=("Arial", 13, "bold"))

        def toggle(_e):
            if not self.ssh or self._gpio_updating:
                return
            var.set(not var.get())
            self.send_cmd(f"{cmd_name} {1 if var.get() else 0}")
            # Note: GPIO sw is master; if sw disagrees, audio_dma overrides
            # within ~5.33 ms and the next STATE line will correct the display.

        c.bind("<ButtonPress-1>", toggle)
        var.trace_add("write", redraw)
        redraw()

    def _preset_row(self, parent, labels, on_select, default=0):
        """Row of small labelled preset buttons; highlights active one."""
        row = tk.Frame(parent, bg=C_PEDAL)
        row.pack(pady=4)
        btns = []

        def make_cb(i):
            def cb():
                for j, b in enumerate(btns):
                    b.config(
                        bg=C_BTN_ON if j == i else C_BTN,
                        fg=C_ACCENT if j == i else C_DIM,
                    )
                on_select(i)
            return cb

        for i, lbl in enumerate(labels):
            b = tk.Button(row, text=lbl, width=6,
                          bg=C_BTN_ON if i == default else C_BTN,
                          fg=C_ACCENT if i == default else C_DIM,
                          activebackground="#5a2a2a", activeforeground=C_ACCENT,
                          relief=tk.FLAT, font=("Arial", 9, "bold"),
                          command=make_cb(i))
            b.pack(side=tk.LEFT, padx=3)
            btns.append(b)

        return btns

    # ── Distortion pedal ──────────────────────────────────────

    def _build_dist_pedal(self, parent):
        f = self._pedal_frame(parent, "DISTORTION", C_ACCENT)

        self._led(f, self.dist_en)
        self._stomp(f, self.dist_en, "dist_en")

        knob_row = tk.Frame(f, bg=C_PEDAL)
        knob_row.pack(pady=2)

        self.thr_knob = Knob(knob_row, "THRESH", 0.05, 0.90,
                             DIST_PRESETS[0]["threshold"],
                             fmt="{:.2f}",
                             on_change=lambda v: self.send_cmd(
                                 f"threshold {v:.4f}"))
        self.thr_knob.pack(side=tk.LEFT, padx=10)

        self.gain_knob = Knob(knob_row, "GAIN", 1, 20,
                              DIST_PRESETS[0]["gain"],
                              fmt="{:.0f}x",
                              on_change=lambda v: self.send_cmd(
                                  f"gain {int(round(v))}"))
        self.gain_knob.pack(side=tk.LEFT, padx=10)

        tk.Label(f, text="PRESET", bg=C_PEDAL, fg=C_DIM,
                 font=("Arial", 8)).pack(pady=(6, 0))
        self.dist_preset_btns = self._preset_row(
            f, ["L", "H"], self._apply_dist_preset)

    # ── Wobble pedal ──────────────────────────────────────────

    def _build_wobble_pedal(self, parent):
        f = self._pedal_frame(parent, "WOBBLE", C_BLUE)

        self._led(f, self.wobble_en)
        self._stomp(f, self.wobble_en, "wobble_en")

        knob_row = tk.Frame(f, bg=C_PEDAL)
        knob_row.pack(pady=2)

        self.rate_knob = Knob(knob_row, "RATE", 0.5, 8.0,
                              WOBBLE_PRESETS[0]["lfo_rate"],
                              fmt="{:.1f}Hz",
                              on_change=lambda v: self.send_cmd(
                                  f"lfo_rate {v:.2f}"))
        self.rate_knob.pack(side=tk.LEFT, padx=10)

        self.depth_knob = Knob(knob_row, "DEPTH", 0, 100,
                               WOBBLE_PRESETS[0]["lfo_depth"],
                               fmt="{:.0f}%",
                               on_change=lambda v: self.send_cmd(
                                   f"lfo_depth {int(round(v))}"))
        self.depth_knob.pack(side=tk.LEFT, padx=10)

        tk.Label(f, text="WAH DEPTH", bg=C_PEDAL, fg=C_DIM,
                 font=("Arial", 8)).pack(pady=(6, 0))
        self.wah_btns = self._preset_row(
            f, ["A", "B", "C"], self._apply_wah_preset)

        tk.Label(f, text="SPEED PRESET", bg=C_PEDAL, fg=C_DIM,
                 font=("Arial", 8)).pack(pady=(6, 0))
        self.wobble_preset_btns = self._preset_row(
            f, ["SLOW", "FAST"], self._apply_wobble_preset)

    # ── Preset handlers ───────────────────────────────────────

    def _apply_dist_preset(self, idx):
        self.dist_preset_idx = idx
        p = DIST_PRESETS[idx]
        self.thr_knob.set_value(p["threshold"], notify=False)
        self.gain_knob.set_value(p["gain"],      notify=False)
        self.send_cmd(f"threshold {p['threshold']:.4f}")
        self.send_cmd(f"gain {p['gain']}")

    def _apply_wobble_preset(self, idx):
        self.wobble_preset_idx = idx
        p = WOBBLE_PRESETS[idx]
        self.rate_knob.set_value(p["lfo_rate"],   notify=False)
        self.depth_knob.set_value(p["lfo_depth"], notify=False)
        self.send_cmd(f"lfo_rate {p['lfo_rate']:.2f}")
        self.send_cmd(f"lfo_depth {p['lfo_depth']}")

    def _apply_wah_preset(self, idx):
        self.wah_idx = idx
        self.send_cmd(f"lfo_floor {WAH_PRESETS[idx]}")

    # ── SSH connect / disconnect ───────────────────────────────

    def _set_status(self, text, color=C_DIM):
        self.root.after(0, lambda t=text, c=color: (
            self.status_var.set(t),
            self.status_lbl.config(fg=c),
        ))

    def _connect(self):
        host = self.host_var.get().strip()
        user = self.user_var.get().strip()
        pwd  = self.pass_var.get()
        if not host:
            messagebox.showerror("Bass FX", "Enter the board hostname / IP.")
            return
        self._set_status("● Connecting…", "#ffaa44")

        def _worker():
            try:
                client = paramiko.SSHClient()
                client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
                client.connect(host, username=user, password=pwd,
                               timeout=10, banner_timeout=15)
                self.ssh = client
                self._board_pass = pwd   # stored for sudo -S use
                self.root.after(0, self._on_connected)
            except Exception as exc:
                self.root.after(0, lambda e=exc:
                                self._set_status(f"● Error: {e}", "#ff4444"))

        threading.Thread(target=_worker, daemon=True).start()

    def _on_connected(self):
        self._set_status("● Connected", "#44dd44")
        self.conn_btn.config(text="Disconnect", command=self._disconnect,
                             bg="#2a1a1a", fg="#ff7755")
        self.start_btn.config(state=tk.NORMAL)

    def _disconnect(self):
        """Initiate disconnect: disable buttons, stop FX then close SSH in background."""
        self._set_status("● Disconnecting…", "#ffaa44")
        self.conn_btn.config(state=tk.DISABLED)
        self.start_btn.config(state=tk.DISABLED)
        self.stop_btn.config(state=tk.DISABLED)
        threading.Thread(target=self._run_disconnect, daemon=True).start()

    def _run_disconnect(self):
        """Background: stop FX (blocking), then close SSH."""
        self._run_stop_fx()          # sends quit + pkill, waits for completion
        if self.ssh:
            try:
                self.ssh.close()
            except Exception:
                pass
            self.ssh = None
        self._set_status("● Disconnected", C_DIM)
        self.root.after(0, lambda: (
            self.conn_btn.config(text="Connect", command=self._connect,
                                 bg="#1e2e1e", fg="#66cc66", state=tk.NORMAL),
            self.start_btn.config(state=tk.DISABLED),
            self.stop_btn.config(state=tk.DISABLED),
        ))

    # ── Start / stop FX ───────────────────────────────────────

    def _start_fx(self):
        if not self.ssh:
            return
        self._set_status("● Starting codec…", "#ffaa44")
        self.start_btn.config(state=tk.DISABLED)
        self.stop_btn.config(state=tk.NORMAL)
        self._stop_ev.clear()
        threading.Thread(target=self._run_start_fx, daemon=True).start()

    def _run_start_fx(self):
        try:
            pwd = self._board_pass

            # CH2: start.sh via sudo -S (send password, then start.sh takes over)
            fx_stdin, fx_stdout, fx_stderr = self.ssh.exec_command(
                START_CMD, timeout=None)
            fx_stdin.write(pwd + "\n")   # sudo -S reads this for authentication
            fx_stdin.flush()
            self.fx_chan = fx_stdout.channel

            codec_ready = threading.Event()

            def _monitor_fx():
                try:
                    for line in iter(fx_stdout.readline, ""):
                        if self._stop_ev.is_set():
                            break
                        stripped = line.rstrip()
                        print(f"[start.sh] {stripped}")
                        if any(kw in stripped for kw in
                               ("Codec ready", "audio loop", "Done.")):
                            codec_ready.set()
                except Exception:
                    pass

            threading.Thread(target=_monitor_fx, daemon=True).start()

            # Wait up to 20 s for codec; proceed anyway on timeout
            got_ready = codec_ready.wait(20)
            if not got_ready:
                print("[bass_ui] codec ready timeout — proceeding")

            if self._stop_ev.is_set():
                return

            # Brief settle after audio_dma starts
            time.sleep(1.0)

            # CH1: ctrl_client.py via sudo -S
            # sudo -S reads password from first stdin line, then hands
            # remaining stdin to python3 ctrl_client.py.
            ctrl_stdin, ctrl_stdout, ctrl_stderr = self.ssh.exec_command(
                CTRL_CMD, timeout=None)
            ctrl_stdin.write(pwd + "\n")  # sudo -S password — must come first
            ctrl_stdin.flush()
            with self._ctrl_lock:
                self.ctrl_stdin  = ctrl_stdin
                self.ctrl_stdout = ctrl_stdout

            self._set_status("● Running", "#44dd44")

            # Push current UI state to board
            threading.Thread(target=self._push_all_params, daemon=True).start()

            # Start STATE reader
            threading.Thread(target=self._state_reader, daemon=True).start()

        except Exception as exc:
            self._set_status(f"● FX error: {exc}", "#ff4444")
            print(f"[bass_ui] start error: {exc}")

    def _push_all_params(self):
        """Send full parameter set to ctrl_client.py after connect."""
        time.sleep(0.5)   # let ctrl_client.py initialise
        p = DIST_PRESETS[self.dist_preset_idx]
        self.send_cmd(f"threshold {p['threshold']:.4f}")
        self.send_cmd(f"gain {p['gain']}")
        q = WOBBLE_PRESETS[self.wobble_preset_idx]
        self.send_cmd(f"lfo_rate {q['lfo_rate']:.2f}")
        self.send_cmd(f"lfo_depth {q['lfo_depth']}")
        self.send_cmd(f"lfo_floor {WAH_PRESETS[self.wah_idx]}")
        self.send_cmd(f"dist_en {1 if self.dist_en.get() else 0}")
        self.send_cmd(f"wobble_en {1 if self.wobble_en.get() else 0}")

    def _state_reader(self):
        """Background thread: parse STATE lines from ctrl_client.py stdout."""
        try:
            for line in iter(self.ctrl_stdout.readline, ""):
                if self._stop_ev.is_set():
                    break
                line = line.strip()
                if line.startswith("STATE "):
                    self._parse_state(line)
        except Exception as exc:
            print(f"[bass_ui] state reader: {exc}")

    def _parse_state(self, line):
        """Parse 'STATE sw=01 dist_preset=0 wobble_preset=1 wah=0' and update UI."""
        parts = {}
        for tok in line.split()[1:]:
            if "=" in tok:
                k, v = tok.split("=", 1)
                parts[k] = v

        # sw → stomp buttons (GPIO is master)
        sw = parts.get("sw", "--")
        self.root.after(0, lambda s=sw: self.sw_var.set(f"SW:{s}"))
        if sw != self._last_sw and len(sw) == 2 and sw.isdigit():
            self._last_sw = sw
            self.root.after(0, lambda s=sw: self._sync_sw_to_ui(s))

        # preset indices → button highlights + knobs (btn[0/1/2] changes)
        try:
            dp  = int(parts.get("dist_preset",   -1))
            wp  = int(parts.get("wobble_preset",  -1))
            wah = int(parts.get("wah",            -1))
            if dp in (0, 1) and dp != self.dist_preset_idx:
                self.root.after(0, lambda i=dp:  self._set_dist_preset_btn(i))
            if wp in (0, 1) and wp != self.wobble_preset_idx:
                self.root.after(0, lambda i=wp:  self._set_wobble_preset_btn(i))
            if wah in (0, 1, 2) and wah != self.wah_idx:
                self.root.after(0, lambda i=wah: self._set_wah_preset_btn(i))
        except ValueError:
            pass

    def _sync_sw_to_ui(self, sw_str):
        """Reflect board GPIO sw[1:0] in stomp button state (no commands sent)."""
        # sw_str = "XY" where X=sw[1]=wobble, Y=sw[0]=dist
        sw0 = bool(int(sw_str[1]))   # sw[0] → dist_en
        sw1 = bool(int(sw_str[0]))   # sw[1] → wobble_en
        self._gpio_updating = True
        try:
            self.dist_en.set(sw0)
            self.wobble_en.set(sw1)
        finally:
            self._gpio_updating = False

    def _set_dist_preset_btn(self, idx):
        """Highlight dist preset button and sync knobs (no command sent)."""
        self.dist_preset_idx = idx
        for j, b in enumerate(self.dist_preset_btns):
            b.config(bg=C_BTN_ON if j == idx else C_BTN,
                     fg=C_ACCENT if j == idx else C_DIM)
        p = DIST_PRESETS[idx]
        self.thr_knob.set_value(p["threshold"], notify=False)
        self.gain_knob.set_value(p["gain"],      notify=False)

    def _set_wobble_preset_btn(self, idx):
        """Highlight wobble preset button and sync knobs (no command sent)."""
        self.wobble_preset_idx = idx
        for j, b in enumerate(self.wobble_preset_btns):
            b.config(bg=C_BTN_ON if j == idx else C_BTN,
                     fg=C_ACCENT if j == idx else C_DIM)
        q = WOBBLE_PRESETS[idx]
        self.rate_knob.set_value(q["lfo_rate"],   notify=False)
        self.depth_knob.set_value(q["lfo_depth"], notify=False)

    def _set_wah_preset_btn(self, idx):
        """Highlight wah preset button (no command sent)."""
        self.wah_idx = idx
        for j, b in enumerate(self.wah_btns):
            b.config(bg=C_BTN_ON if j == idx else C_BTN,
                     fg=C_ACCENT if j == idx else C_DIM)

    def _stop_fx(self):
        """Initiate stop: disable buttons immediately, do teardown in background."""
        self._set_status("● Stopping…", "#ffaa44")
        self.root.after(0, lambda: (
            self.stop_btn.config(state=tk.DISABLED),
            self.start_btn.config(state=tk.DISABLED),
        ))
        threading.Thread(target=self._run_stop_fx, daemon=True).start()

    def _run_stop_fx(self):
        """Background: send quit to ctrl_client (it kills audio_dma), then fallback pkill."""
        # 1. Send quit command — ctrl_client.py runs as root and pkills audio_dma then exits
        with self._ctrl_lock:
            stdin = self.ctrl_stdin
        if stdin:
            try:
                stdin.write("quit\n")
                stdin.flush()
            except Exception:
                pass

        # 2. Give ctrl_client time to kill audio_dma and exit cleanly
        time.sleep(0.5)

        self._stop_ev.set()

        # 3. Close ctrl channel (cleanup regardless of whether quit succeeded)
        with self._ctrl_lock:
            try:
                if self.ctrl_stdin:
                    self.ctrl_stdin.channel.shutdown_write()
            except Exception:
                pass
            self.ctrl_stdin  = None
            self.ctrl_stdout = None

        # 4. Fallback pkill — kill both processes, wait for completion
        if self.ssh:
            for target in ("ctrl_client.py", "audio_dma"):
                try:
                    ki, ko, _ = self.ssh.exec_command(
                        f"sudo -S pkill -f {target} 2>/dev/null; true")
                    ki.write(self._board_pass + "\n")
                    ki.flush()
                    ko.channel.recv_exit_status()   # block until pkill finishes
                    print(f"[bass_ui] pkill {target}: done")
                except Exception as e:
                    print(f"[bass_ui] pkill {target}: {e}")

        self.fx_chan = None
        self._last_sw = "--"
        self._set_status("● Stopped", C_DIM)
        self.root.after(0, lambda: (
            self.start_btn.config(state=tk.NORMAL if self.ssh else tk.DISABLED),
            self.stop_btn.config(state=tk.DISABLED),
            self.sw_var.set("SW:--"),
        ))

    # ── Send command to ctrl_client.py ────────────────────────

    def send_cmd(self, cmd: str):
        if self._stop_ev.is_set():
            return
        with self._ctrl_lock:
            stdin = self.ctrl_stdin
        if not stdin:
            return
        try:
            stdin.write(cmd + "\n")
            stdin.flush()
        except Exception as exc:
            print(f"[bass_ui] send_cmd '{cmd}': {exc}")

    # ── Window close ─────────────────────────────────────────

    def _on_close(self):
        """Window close: stop FX and close SSH before destroying window."""
        self._set_status("● Closing…", "#ffaa44")
        self.conn_btn.config(state=tk.DISABLED)
        self.start_btn.config(state=tk.DISABLED)
        self.stop_btn.config(state=tk.DISABLED)

        def _worker():
            self._run_stop_fx()
            if self.ssh:
                try:
                    self.ssh.close()
                except Exception:
                    pass
                self.ssh = None
            self.root.after(0, self.root.destroy)

        threading.Thread(target=_worker, daemon=False).start()


# ─────────────────────────────────────────────────────────────
def main():
    root = tk.Tk()
    root.minsize(680, 500)
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
