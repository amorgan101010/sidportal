#!/usr/bin/env python3
"""SidPortal front panel.

Starts the realtime engine (build/sidportal --realtime) and talks to it over its
stdin/stdout line protocol. The panel mirrors the unit: the 2x16 LCD, the four LEDs,
the four realtime knobs, the rotary encoder and the 16-key data-input pad. Below it are
an on-screen keyboard and a status line; settings sit behind the panel's gear button.

Computer keyboard:
  0-9 / keypad digits   number keys        Return / keypad Enter   ENTER (D)
  BackSpace / Escape    EXIT (A)           Up / Down               UP (B) / DOWN (C)
  Left / Right          LEFT (*) / RIGHT (#)
  - / =                 turn the encoder one detent
  hold A S D F + - / =  turn realtime knob 1-4
"""
from __future__ import annotations

import json
import math
import os
import queue
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk


sys.path.insert(0, str(Path(__file__).resolve().parent))
from assets.hd44780_a00 import FONT_A00  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
KEY_LEGENDS = '123A456B789C*0#D'          # keypad encoder code n -> legend
LED_BITS = (0x40, 0x20, 0x10, 0x08)        # Sync, MIDI, Edit, Ext clk (port A, active low)
RELEASE_GRACE_MS = 40


def xdg_dir(var: str, fallback: str) -> Path:
    base = os.environ.get(var) or str(Path.home() / fallback)
    path = Path(base) / 'sidportal'
    path.mkdir(parents=True, exist_ok=True)
    return path


CONFIG_DIR = xdg_dir('XDG_CONFIG_HOME', '.config')
DATA_DIR = xdg_dir('XDG_DATA_HOME', '.local/share')
CONFIG_PATH = CONFIG_DIR / 'config.json'
NVRAM_PATH = DATA_DIR / 'nvram.bin'

ENGINE_NAMES = {'residfp': 'reSIDfp (detailed filter model)', 'resid': 'reSID (classic)'}

DEFAULT_CONFIG = {
    'audio': 'default',
    'midi_source': '',
    'channel': 1,
    'model': '6581',
    'engine': 'residfp',
    'volume': 1.0,
    'knobs': [128, 128, 128, 128],
    'octave': 4,
    'ext_in': 'none',
    'ext_gain': 1.0,
    'filter_bias': 0.0,
    'bias_cc': -1,
    'noise_gate': False,
    'gate_threshold_db': -60.0,
}

VSID_INSTALL = Path.home() / '.local' / 'opt' / 'sidportal-asid-vice'
VSID_ROM_DIR = DATA_DIR / 'vice-roms'


def load_config() -> dict:
    cfg = dict(DEFAULT_CONFIG)
    try:
        cfg.update(json.loads(CONFIG_PATH.read_text()))
    except (OSError, ValueError):
        pass
    return cfg


def save_config(cfg: dict) -> None:
    tmp = CONFIG_PATH.with_suffix('.tmp')
    tmp.write_text(json.dumps(cfg, indent=2))
    tmp.replace(CONFIG_PATH)


def find_rom() -> Path | None:
    for p in (ROOT / 'roms' / 'firmware.bin',):
        if p.exists():
            return p
    return None


def find_binary() -> Path | None:
    for p in (ROOT / 'build' / 'sidportal', ROOT / 'sidportal'):
        if p.exists():
            return p
    return None


def find_vsid_binary() -> Path | None:
    configured = os.environ.get('SIDPORTAL_VSID')
    candidates = [Path(configured)] if configured else []
    candidates += [VSID_INSTALL / 'bin' / 'vsid']
    found = shutil.which('vsid')
    if found:
        candidates.append(Path(found))
    return next((p for p in candidates if p.is_file() and os.access(p, os.X_OK)), None)


def preset_banks() -> list[Path]:
    found = []
    for d in (ROOT / 'patches', DATA_DIR / 'patches'):
        if d.is_dir():
            found += sorted(d.glob('*.syx'))
    return found


def alsa_midi_sources() -> list[tuple[str, str]]:
    """Readable ALSA sequencer ports as (address, 'client: port') pairs."""
    try:
        out = subprocess.run(['aconnect', '-i'], capture_output=True, text=True, timeout=3).stdout
    except (OSError, subprocess.SubprocessError):
        return []
    sources = []
    client = name = None
    for line in out.splitlines():
        m = re.match(r"client (\d+): '([^']*)'", line)
        if m:
            client, name = m.group(1), m.group(2).strip()
            continue
        m = re.match(r"\s+(\d+) '([^']*)'", line)
        if m and client is not None:
            if client == '0' or name in ('SidPortal', 'Midi Through'):
                continue
            sources.append((f'{client}:{m.group(1)}', f'{name}: {m.group(2).strip()}'))
    return sources


def alsa_pcms(capture: bool) -> list[str]:
    """Useful ALSA PCM names: the sound-server plugins and each card's default devices."""
    try:
        out = subprocess.run(['arecord' if capture else 'aplay', '-L'], capture_output=True,
                             text=True, timeout=3).stdout
    except (OSError, subprocess.SubprocessError):
        out = ''
    names = [ln for ln in out.splitlines() if ln and not ln[0].isspace()]
    keep = [n for n in ('default', 'pipewire', 'pulse', 'jack') if n in names]
    keep += [n for n in names if n.startswith(('sysdefault:', 'front:'))]
    return keep or ['default']


def sidportal_input_port() -> str | None:
    try:
        out = subprocess.run(['aconnect', '-o'], capture_output=True, text=True, timeout=3).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    client = None
    for line in out.splitlines():
        m = re.match(r"client (\d+): 'SidPortal'", line)
        if m:
            client = m.group(1)
            continue
        m = re.match(r"\s+(\d+) 'SidPortal MIDI In", line)
        if m and client:
            return f'{client}:{m.group(1)}'
    return None


def sidportal_vsid_port_index(engine_pid: int) -> int | None:
    """Return VICE's MIDI destination index for this engine's input port."""
    try:
        out = subprocess.run(['aconnect', '-o'], capture_output=True, text=True, timeout=3).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    client_re = re.compile(r"client (\d+): '([^']*)'(?: \[.*?pid=(\d+)\])?")
    port_re = re.compile(r"\s+(\d+) '([^']*)'")
    client_id = None
    client_name = ''
    client_pid = None
    index = 0
    matches = []
    for line in out.splitlines():
        m = client_re.match(line)
        if m:
            client_id, client_name, client_pid = m.group(1), m.group(2).strip(), m.group(3)
            continue
        m = port_re.match(line)
        if m and client_id not in (None, '0'):
            port_name = m.group(2).strip()
            if (client_name == 'SidPortal' and client_pid == str(engine_pid)
                    and port_name.startswith('SidPortal MIDI In')):
                matches.append(index)
            # This is the same order used by asid-vice's ALSA destination enumeration:
            # client ID, then port ID, with the System client excluded.
            index += 1
    return matches[0] if len(matches) == 1 else None


# --- Engine -------------------------------------------------------------------------

class Engine:
    def __init__(self, binary: Path, rom: Path, cfg: dict):
        self.binary, self.rom, self.cfg = binary, rom, cfg
        self.proc: subprocess.Popen[str] | None = None
        self.lines: queue.Queue[str] = queue.Queue()

    def start(self) -> None:
        args = [str(self.binary), '--rom', str(self.rom), '--nvram', str(NVRAM_PATH),
                '--realtime', '--audio', self.cfg['audio']]
        if self.cfg['model'] == '8580':
            args.append('--8580')
        args += ['--engine', self.cfg.get('engine', 'residfp')]
        if self.cfg.get('ext_in', 'none') not in ('', 'none'):
            args += ['--ext-in', self.cfg['ext_in']]
        args += ['--ext-gain', f"{self.cfg.get('ext_gain', 1.0):.3f}"]
        if self.cfg.get('noise_gate', False):
            args.append('--noise-gate')
        args += ['--gate-threshold-db', str(self.cfg.get('gate_threshold_db', -60.0))]
        args += ['--filter-bias', f"{self.cfg.get('filter_bias', 0.0):.3f}"]
        if self.cfg.get('bias_cc', -1) >= 0:
            args += ['--bias-cc', str(self.cfg['bias_cc'])]
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     text=True, bufsize=1)
        threading.Thread(target=self._read, args=(self.proc,), daemon=True).start()
        self.send(f'volume {self.cfg["volume"]:.3f}')

    def _read(self, proc: subprocess.Popen[str]) -> None:
        assert proc.stdout is not None
        for line in proc.stdout:
            self.lines.put(line.rstrip('\n'))
        self.lines.put('exited')

    def send(self, line: str) -> None:
        if self.proc and self.proc.poll() is None and self.proc.stdin:
            try:
                self.proc.stdin.write(line + '\n')
                self.proc.stdin.flush()
            except (BrokenPipeError, OSError):
                pass

    def stop(self) -> None:
        if not self.proc:
            return
        self.send('quit')
        try:
            self.proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.proc = None


# --- Panel widgets ----------------------------------------------------------------------

class Lcd:
    BG = '#a3bd3c'
    OFF = '#9db73a'
    ON = '#1c2410'

    def __init__(self, canvas: tk.Canvas, box: tuple[int, int, int, int]):
        self.canvas = canvas
        x0, y0, x1, y1 = box
        canvas.create_rectangle(x0, y0, x1, y1, fill=self.BG, outline='')
        # Faint backlight hot spot toward the centre.
        canvas.create_rectangle(x0, y0, x1, y0 + 3, fill='#b4cc4e', outline='')
        # Whole-pixel geometry: 3 px dots on a 4 px pitch, one dot of space between
        # characters and two between the lines. Fractional positions round unevenly.
        pitch, dot = 4, 3
        width, height = 95 * pitch, 18 * pitch
        ox = int(x0 + (x1 - x0 - width) // 2)
        oy = int(y0 + (y1 - y0 - height) // 2)
        self.dots: list[list[list[list[int]]]] = []
        for row in range(2):
            row_cells = []
            for col in range(16):
                cell = []
                for py in range(8):
                    line = []
                    for px in range(5):
                        x = ox + (col * 6 + px) * pitch
                        y = oy + (row * 10 + py) * pitch
                        line.append(canvas.create_rectangle(x, y, x + dot, y + dot, fill=self.OFF, outline=''))
                    cell.append(line)
                row_cells.append(cell)
            self.dots.append(row_cells)
        self.lit = [[[0] * 8 for _ in range(16)] for _ in range(2)]
        self.state = None
        self.blink_phase = False

    def update(self, chars: bytes, cgram: bytes, on: bool, cursor: bool, blink: bool, crow: int, ccol: int):
        self.state = (chars, cgram, on, cursor, blink, crow, ccol)
        self._draw()

    def tick_blink(self):
        self.blink_phase = not self.blink_phase
        if self.state and (self.state[3] or self.state[4]):
            self._draw()

    def _glyph(self, code: int, cgram: bytes) -> list[int]:
        if code < 16:
            base = (code & 7) * 8
            # CGRAM rows are bit 4 = leftmost column; return bit 0 = leftmost like the ROM font.
            return [int(f'{cgram[base + i] & 0x1F:05b}'[::-1], 2) for i in range(8)]
        return list(FONT_A00[code * 8: code * 8 + 8])

    def _draw(self):
        chars, cgram, on, cursor, blink, crow, ccol = self.state
        for row in range(2):
            for col in range(16):
                rows = self._glyph(chars[row * 16 + col], cgram) if on else [0] * 8
                if on and row == crow and col == ccol:
                    if cursor:
                        rows[7] = 0x1F
                    if blink and self.blink_phase:
                        rows = [0x1F] * 8
                old = self.lit[row][col]
                if rows == old:
                    continue
                cell = self.dots[row][col]
                for py in range(8):
                    if rows[py] == old[py]:
                        continue
                    for px in range(5):
                        bit = bool(rows[py] >> px & 1)
                        if bit != bool(old[py] >> px & 1):
                            self.canvas.itemconfigure(cell[py][px], fill=self.ON if bit else self.OFF)
                self.lit[row][col] = rows


# The artwork is bundled (tools/assets/panel, rendered by tools/export_assets.py), so the
# panel needs nothing beyond Tk. Tk's X11 canvas draws lines and ovals without
# antialiasing, which is why the knob pointers, LEDs and dimple are images too.
ART = Path(__file__).resolve().parent / 'assets' / 'panel'
LAYOUT = json.loads((ART / 'layout.json').read_text())
PANEL_W, PANEL_H = LAYOUT['width'], LAYOUT['height']
_photos: dict = {}


def art(name: str) -> tk.PhotoImage:
    if name not in _photos:
        _photos[name] = tk.PhotoImage(file=str(ART / '1x' / f'{name}.png'))
    return _photos[name]


def knob_frame(radius: int, value: int) -> tk.PhotoImage:
    """One frame of a knob's sprite sheet: the knob with its pointer at `value`."""
    key = ('knob', radius, value)
    if key not in _photos:
        sheet = art(f'knobs_{radius}')
        f = LAYOUT['knob_frame'][str(radius)]
        x, y = (value % 16) * f, (value // 16) * f
        frame = tk.PhotoImage(width=f, height=f)
        frame.tk.call(frame, 'copy', sheet, '-from', x, y, x + f, y + f)
        _photos[key] = frame
    return _photos[key]


class Led:
    def __init__(self, canvas: tk.Canvas, x: int, y: int):
        self.canvas = canvas
        self.item = canvas.create_image(x, y, image=art('led_off'))
        self.on = None

    def set(self, on: bool):
        if on == self.on:
            return
        self.on = on
        self.canvas.itemconfigure(self.item, image=art('led_on' if on else 'led_off'))


class Knob:
    """A realtime potentiometer: drag up/down or use the wheel; 0-255 like the A/D."""

    def __init__(self, canvas: tk.Canvas, cx: int, cy: int, r: int, value: int, on_change,
                 on_double=None, group: str = ''):
        self.canvas, self.cx, self.cy, self.r = canvas, cx, cy, r
        self.value = value
        self.on_change = on_change
        tag = f'knob{id(self)}'
        self.tag = tag
        tags = (tag, group) if group else tag
        self.item = canvas.create_image(cx, cy, tags=tags)   # body and pointer, one image per value
        if on_double:
            canvas.tag_bind(tag, '<Double-Button-1>', lambda e: on_double())
        canvas.tag_bind(tag, '<ButtonPress-1>', self._press)
        canvas.tag_bind(tag, '<B1-Motion>', self._drag)
        canvas.tag_bind(tag, '<Button-4>', lambda e: self.nudge(4))
        canvas.tag_bind(tag, '<Button-5>', lambda e: self.nudge(-4))
        canvas.tag_bind(tag, '<Enter>', lambda e: canvas.configure(cursor='sb_v_double_arrow'))
        canvas.tag_bind(tag, '<Leave>', lambda e: canvas.configure(cursor=''))
        self._drag_y = 0
        self._drag_v = value
        self._draw()

    def _press(self, e):
        self._drag_y, self._drag_v = e.y, self.value

    def _drag(self, e):
        scale = 0.25 if e.state & 0x1 else 1.0   # Shift: fine
        self.set(self._drag_v + (self._drag_y - e.y) * scale)

    def nudge(self, delta: int):
        self.set(self.value + delta)

    def set(self, v):
        v = max(0, min(255, int(round(v))))
        if v != self.value:
            self.value = v
            self._draw()
            self.on_change(v)

    def show(self, v):
        """Move the pointer without reporting a change (the value came from elsewhere)."""
        self.value = max(0, min(255, int(round(v))))
        self._draw()

    def _draw(self):
        r, v = self.r, self.value
        self.canvas.itemconfigure(self.item, image=knob_frame(r, v))


class Encoder:
    """The rotary encoder: 24 detents per turn; drag around it or use the wheel."""
    DETENT = 15.0

    def __init__(self, canvas: tk.Canvas, cx: int, cy: int, r: int, image, on_step):
        self.canvas, self.cx, self.cy, self.r = canvas, cx, cy, r
        self.on_step = on_step
        self.angle = -90.0
        self.acc = 0.0
        tag = 'encoder'
        canvas.create_image(cx, cy, image=image, tags=tag)
        self.dimple = canvas.create_image(0, 0, image=art('dimple'),
                                          tags=tag)
        canvas.tag_bind(tag, '<ButtonPress-1>', self._press)
        canvas.tag_bind(tag, '<B1-Motion>', self._drag)
        canvas.tag_bind(tag, '<Button-4>', lambda e: self.step(1))
        canvas.tag_bind(tag, '<Button-5>', lambda e: self.step(-1))
        canvas.tag_bind(tag, '<Enter>', lambda e: canvas.configure(cursor='exchange'))
        canvas.tag_bind(tag, '<Leave>', lambda e: canvas.configure(cursor=''))
        self._last = None
        self._draw()

    def _mouse_angle(self, e):
        return math.degrees(math.atan2(e.y - self.cy, e.x - self.cx))

    def _press(self, e):
        self._last = self._mouse_angle(e)

    def _drag(self, e):
        a = self._mouse_angle(e)
        d = (a - self._last + 540) % 360 - 180
        self._last = a
        self.angle += d
        self.acc += d
        steps = int(self.acc / self.DETENT)
        if steps:
            self.acc -= steps * self.DETENT
            self.on_step(steps)
        self._draw()

    def step(self, n: int):
        self.angle += n * self.DETENT
        self.on_step(n)
        self._draw()

    def _draw(self):
        a = math.radians(self.angle)
        d = self.r * 0.62
        x, y = self.cx + d * math.cos(a), self.cy + d * math.sin(a)
        self.canvas.coords(self.dimple, x, y)


class SlideSwitch:
    """A two-position slide switch: click it (or a legend) to move the slider."""

    def __init__(self, canvas: tk.Canvas, cx: int, cy: int, travel: float, image, right: bool, on_change):
        self.canvas, self.cx, self.cy, self.travel = canvas, cx, cy, travel
        self.right = right
        self.on_change = on_change
        self.item = canvas.create_image(0, 0, image=image, tags='chipswitch')
        # The slot and legends are printed on the panel, and an unfilled canvas item takes
        # no clicks, so hit-test the switch area on the canvas itself.
        w, h = LAYOUT['switch_slot']
        self.area = (cx - w / 2 - 6, cy - h / 2 - 22, cx + w / 2 + 6, cy + h / 2 + 4)
        canvas.bind('<ButtonPress-1>', self._press, add='+')
        canvas.tag_bind('chipswitch', '<Enter>', lambda e: canvas.configure(cursor='hand2'))
        canvas.tag_bind('chipswitch', '<Leave>', lambda e: canvas.configure(cursor=''))
        self._draw()

    def _press(self, e):
        x0, y0, x1, y1 = self.area
        if not (x0 <= e.x <= x1 and y0 <= e.y <= y1):
            return
        # Clicking either half picks that side; clicking the slider flips it.
        target = e.x > self.cx
        if abs(e.x - self.cx - (self.travel if self.right else -self.travel)) < 14:
            target = not self.right
        self.set(target)

    def set(self, right: bool, notify: bool = True):
        if right == self.right:
            return
        self.right = right
        self._draw()
        if notify:
            self.on_change(right)

    def _draw(self):
        self.canvas.coords(self.item, self.cx + (self.travel if self.right else -self.travel), self.cy)


class Piano:
    """Three octaves of mouse-playable keys under the panel."""
    WHITE = [0, 2, 4, 5, 7, 9, 11]
    BLACK = {1: 0, 3: 1, 6: 3, 8: 4, 10: 5}

    def __init__(self, parent, app: 'App'):
        self.app = app
        self.octaves = 3
        w = PANEL_W
        self.canvas = tk.Canvas(parent, width=w, height=86, bg='#1b1b1d', highlightthickness=0)
        self.canvas.pack(fill='x')
        self.keys: dict[int, int] = {}
        self.down: int | None = None
        x0, kw = 120, (w - 240) / (7 * self.octaves)
        self.canvas.create_text(60, 43, text='OCTAVE', fill='#9a9aa0', font=('DejaVu Sans', 8, 'bold'))
        self.oct_label = self.canvas.create_text(60, 60, text='', fill='#e6e6e6', font=('DejaVu Sans', 10, 'bold'))
        for txt, dx, d in (('◀', -26, -1), ('▶', 26, 1)):
            t = self.canvas.create_text(60 + dx, 60, text=txt, fill='#e6e6e6', font=('DejaVu Sans', 10))
            self.canvas.tag_bind(t, '<Button-1>', lambda e, d=d: self.shift(d))
        for o in range(self.octaves):
            for i, semi in enumerate(self.WHITE):
                x = x0 + (o * 7 + i) * kw
                k = self.canvas.create_rectangle(x, 8, x + kw - 1, 80, fill='#f4f2ec', outline='#555')
                self.keys[k] = o * 12 + semi
        for o in range(self.octaves):
            for semi, after in self.BLACK.items():
                x = x0 + (o * 7 + after + 1) * kw - kw * 0.3
                k = self.canvas.create_rectangle(x, 8, x + kw * 0.6, 54, fill='#151515', outline='#000')
                self.keys[k] = o * 12 + semi
        self.canvas.bind('<ButtonPress-1>', self._press)
        self.canvas.bind('<B1-Motion>', self._motion)
        self.canvas.bind('<ButtonRelease-1>', self._release)
        self.shift(0)

    def shift(self, d: int):
        self.app.cfg['octave'] = max(0, min(7, self.app.cfg['octave'] + d))
        o = self.app.cfg['octave']
        self.canvas.itemconfigure(self.oct_label, text=f'C{o - 1}')

    def _key_at(self, e):
        hits = [k for k in self.canvas.find_overlapping(e.x, e.y, e.x, e.y) if k in self.keys]
        return hits[-1] if hits else None

    def _note(self, k):
        return max(0, min(127, (self.app.cfg['octave']) * 12 + self.keys[k]))

    def _press(self, e):
        k = self._key_at(e)
        if k is not None:
            self._on(k)

    def _motion(self, e):
        k = self._key_at(e)
        if k != self.down:
            self._off()
            if k is not None:
                self._on(k)

    def _release(self, e):
        self._off()

    def _on(self, k):
        self.down = k
        self.canvas.itemconfigure(k, fill='#e0a43a')
        self.app.note(self._note(k), True)

    def _off(self):
        if self.down is None:
            return
        k = self.down
        black = self.keys[k] % 12 in self.BLACK
        self.canvas.itemconfigure(k, fill='#151515' if black else '#f4f2ec')
        self.app.note(self._note(k), False)
        self.down = None


# --- Application ------------------------------------------------------------------------

class App:
    def __init__(self, root: tk.Tk, binary: Path, rom: Path):
        self.root = root
        self.cfg = load_config()
        self.first_run = not NVRAM_PATH.exists()
        self.vsid_proc: subprocess.Popen | None = None
        self.asid_mode = False
        root.title('SidPortal')
        root.configure(bg='#1b1b1d')
        root.resizable(False, False)

        self.canvas = tk.Canvas(root, width=PANEL_W, height=PANEL_H, highlightthickness=0, bg='#c8ccd0')
        self.canvas.pack()
        self.images = {'panel': art('panel')}
        self.canvas.create_image(0, 0, image=self.images['panel'], anchor='nw')
        L = LAYOUT

        self.subtitle = self.canvas.create_image(*L['subtitle'], anchor='nw')
        self.lcd = Lcd(self.canvas, L['lcd_glass'])
        self.leds = [Led(self.canvas, x, y) for x, y in L['leds']]

        self.knobs = []
        for i, (cx, cy) in enumerate(L['knobs']):
            self.knobs.append(Knob(self.canvas, cx, cy, L['knob_r'], self.cfg['knobs'][i], lambda v, i=i: self.knob_changed(i, v)))

        # Right-hand column: output volume, chip switch and filter character. These are host
        # controls, not the unit's A/D knobs, so they stay out of self.knobs.
        sr = L['small_knob_r']
        self.volume_knob = Knob(self.canvas, *L['volume'], sr, self._volume_to_knob(self.cfg['volume']), self._volume_changed)
        sw = L['switch_slot'][0]
        self.images['slider'] = art('slider')
        self.chip_switch = SlideSwitch(self.canvas, *L['chip_switch'], sw / 2 - 17, self.images['slider'],
                                       self.cfg['model'] == '8580', self._model_changed)
        self.canvas.create_image(*L['character_legend_at'], image=art('character'), anchor='nw', tags='character')
        self.character_knob = Knob(self.canvas, *L['character'], sr, self._bias_to_knob(self.cfg.get('filter_bias', 0.0)), self._bias_changed,
                                   on_double=lambda: self.character_knob.set(128), group='character')

        self.images['encoder'] = art('encoder')
        self.encoder = Encoder(self.canvas, *L['encoder'], L['encoder_r'], self.images['encoder'],
                               lambda n: self.engine.send(f'enc {n}'))

        self.key_items = []
        ox, oy = L['keypad_origin']
        for code, legend in enumerate(KEY_LEGENDS):
            row, col = divmod(code, 4)
            up, down = art(f'key{code}_up'), art(f'key{code}_down')
            self.images[f'key{code}'] = (up, down)
            item = self.canvas.create_image(ox + col * L['key_pitch'], oy + row * L['key_pitch'],
                                            image=up, anchor='nw')
            self.canvas.tag_bind(item, '<ButtonPress-1>', lambda e, c=code: self.key(c, True))
            self.canvas.tag_bind(item, '<ButtonRelease-1>', lambda e, c=code: self.key(c, False))
            self.key_items.append(item)
        self.keys_down: set[int] = set()

        self.piano = Piano(root, self)
        self._build_bar()
        self._update_bias_state()

        self.engine = Engine(binary, rom, self.cfg)
        self.engine.start()
        self.ready_at: float | None = None
        self.pending_bank: Path | None = None
        if self.first_run:
            banks = [b for b in preset_banks() if 'Presets' in b.name]
            if banks:
                self.pending_bank = banks[0]

        # Keyboard.
        self.held: dict[str, int] = {}
        self.release_jobs: dict[str, str] = {}
        root.bind('<KeyPress>', self._key_press)
        root.bind('<KeyRelease>', self._key_release)
        root.protocol('WM_DELETE_WINDOW', self.quit)

        self.root.after(16, self.poll)
        self.root.after(400, self.blink)
        self.root.after(1500, self.connect_saved_midi)

    # -- I/O bar ------------------------------------------------------------------------
    def _build_bar(self):
        style = ttk.Style()
        try:
            style.theme_use('clam')
        except tk.TclError:
            pass
        bg = '#1b1b1d'
        style.configure('Bar.TFrame', background=bg)
        style.configure('Bar.TLabel', background=bg, foreground='#d8d8dc')
        style.configure('Bar.TRadiobutton', background=bg, foreground='#d8d8dc')
        style.configure('Bar.TButton', padding=(8, 2))

        # Settings that live in the Setup dialog; the variables exist from the start.
        self.midi_var = tk.StringVar()
        self.channel_var = tk.IntVar(value=self.cfg['channel'])
        self.out_var = tk.StringVar(value=self.cfg['audio'])
        self.ext_var = tk.StringVar(value=self.cfg.get('ext_in', 'none'))
        self.ext_gain_var = tk.DoubleVar(value=self.cfg.get('ext_gain', 1.0))
        self.noise_gate_var = tk.BooleanVar(value=self.cfg.get('noise_gate', False))
        self.gate_threshold_var = tk.DoubleVar(value=self.cfg.get('gate_threshold_db', -60.0))
        self.bias_cc_var = tk.StringVar(value=self._cc_text(self.cfg.get('bias_cc', -1)))
        self.engine_var = tk.StringVar(value=ENGINE_NAMES.get(self.cfg.get('engine'), ENGINE_NAMES['residfp']))
        self.midi_sources: list[tuple[str, str]] = []
        self.setup_win: tk.Toplevel | None = None

        # The settings menu hangs off the gear button on the panel (or a right-click on it).
        self.menu = tk.Menu(self.root, tearoff=False)
        self.menu.add_command(label='ASID Mode', command=self.enter_asid_mode)
        self.menu.add_command(label='Exit ASID Mode / Restart', command=self.exit_asid_mode)
        self.menu.add_separator()
        self.menu.add_command(label='Play SID tune…', command=self.play_sid_tune)
        self.menu.add_command(label='Stop VSID', command=self.stop_vsid, state='disabled')
        self.stop_vsid_menu_index = self.menu.index('end')
        self.menu.add_separator()
        self.menu.add_command(label='Load SysEx…', command=self.load_syx)
        self.menu.add_command(label='Setup…', command=self.open_setup)
        self.menu.add_separator()
        self.menu.add_command(label='Quit', command=self.quit)
        gx, gy = LAYOUT['gear']
        self.images['gear'] = (art('gear_up'), art('gear_down'))
        gear = self.canvas.create_image(gx, gy, image=self.images['gear'][0], tags='gear')
        self.canvas.tag_bind('gear', '<ButtonPress-1>', lambda e: self._open_menu(gear))
        self.canvas.tag_bind('gear', '<Enter>', lambda e: self.canvas.configure(cursor='hand2'))
        self.canvas.tag_bind('gear', '<Leave>', lambda e: self.canvas.configure(cursor=''))
        self.canvas.bind('<ButtonPress-3>', lambda e: self.menu.tk_popup(e.x_root, e.y_root), add='+')

        bar = ttk.Frame(self.root, style='Bar.TFrame', padding=(10, 4))
        bar.pack(fill='x')
        self.status_var = tk.StringVar(value='Starting…')
        ttk.Label(bar, textvariable=self.status_var, style='Bar.TLabel').pack(side='right', padx=4)

    def enter_asid_mode(self, on_ready=None):
        # From the top menu, DOWN twice selects ASID and ENTER starts it.
        if self.asid_mode:
            if on_ready:
                on_ready()
            return
        if self.ready_at is None or time.monotonic() < self.ready_at + 18:
            self.status_var.set('Waiting for SidPortal to finish starting…')
            self.root.after(250, lambda: self.enter_asid_mode(on_ready))
            return
        keys = [KEY_LEGENDS.index('C'), KEY_LEGENDS.index('C'), KEY_LEGENDS.index('D')]

        def press_next(index: int):
            if index >= len(keys):
                return
            code = keys[index]
            self.key(code, True)
            # The ASID receive loop treats a still-held keypad key as its exit request.
            # Release ENTER before that loop's first poll, while keeping the navigation
            # keys long enough for the normal menu scanner to register them.
            hold_ms = 60 if code == KEY_LEGENDS.index('D') else 80
            self.root.after(hold_ms, lambda: self.key(code, False))
            if index + 1 < len(keys):
                self.root.after(180, lambda: press_next(index + 1))
            else:
                def ready():
                    self.asid_mode = True
                    if on_ready:
                        on_ready()
                self.root.after(100, ready)

        press_next(0)

    def play_sid_tune(self):
        default_library = Path.home() / 'Music' / 'HVSC_85' / 'C64Music'
        configured_library = self.cfg.get('sid_library')
        start = Path(configured_library) if configured_library else default_library
        if not start.is_dir():
            start = default_library if default_library.is_dir() else Path.home() / 'Music'
        chosen = filedialog.askopenfilename(
            parent=self.root, title='Choose a SID tune from HVSC', initialdir=start,
            filetypes=[('SID tunes', '*.sid *.SID'), ('All files', '*')])
        if not chosen:
            return
        tune = Path(chosen)

        vsid = find_vsid_binary()
        if not vsid:
            messagebox.showerror(
                'VSID not found',
                'Install the ASID-enabled VICE build or set SIDPORTAL_VSID to its vsid binary.')
            return
        roms = {name: VSID_ROM_DIR / filename for name, filename in {
            'kernal': 'kernal_generic.rom', 'basic': 'basic_generic.rom',
            'chargen': 'chargen_openroms.rom'}.items()}
        missing = [str(path) for path in roms.values() if not path.is_file()]
        if missing:
            messagebox.showerror(
                'VSID ROMs not found',
                'The open VICE ROM set is incomplete:\n\n' + '\n'.join(missing))
            return
        library_root = next((p for p in (tune.parent, *tune.parents) if p.name == 'C64Music'), tune.parent)
        self.cfg['sid_library'] = str(library_root)
        save_config(self.cfg)

        def start_player():
            if self.ready_at is None or time.monotonic() < self.ready_at + 18:
                self.status_var.set('Waiting for SidPortal to finish starting…')
                self.root.after(250, start_player)
                return

            def start_after_stop():
                if self.asid_mode:
                    self._launch_vsid(tune, vsid, roms)
                else:
                    self.enter_asid_mode(lambda: self._launch_vsid(tune, vsid, roms))

            self._stop_vsid_then(start_after_stop)

        start_player()

    def _launch_vsid(self, tune: Path, vsid: Path, roms: dict[str, Path]):
        engine_pid = self.engine.proc.pid if self.engine.proc else -1
        port_index = sidportal_vsid_port_index(engine_pid)
        if port_index is None:
            messagebox.showerror(
                'SidPortal MIDI input not found',
                'Could not identify this SidPortal instance in the ALSA MIDI destination list.')
            return
        args = [str(vsid), '-sounddev', 'asid', '-soundarg', str(port_index),
                '-kernal', str(roms['kernal']), '-basic', str(roms['basic']),
                '-chargen', str(roms['chargen']), str(tune)]
        env = os.environ.copy()
        env['HVSC_BASE'] = self.cfg.get('sid_library', str(tune.parent))
        try:
            proc = subprocess.Popen(args, cwd=str(tune.parent),
                                    env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except OSError as exc:
            messagebox.showerror('Could not start VSID', str(exc))
            return
        self.vsid_proc = proc
        self.menu.entryconfigure(self.stop_vsid_menu_index, state='normal')
        self.status_var.set(f'VSID: {tune.name}')
        self.root.after(500, lambda: self._watch_vsid(proc))

    def _watch_vsid(self, proc: subprocess.Popen):
        if proc.poll() is None:
            self.root.after(500, lambda: self._watch_vsid(proc))
            return
        if self.vsid_proc is proc:
            self.vsid_proc = None
            self.menu.entryconfigure(self.stop_vsid_menu_index, state='disabled')
            self.status_var.set('Ready')

    def _stop_vsid_then(self, callback=None):
        proc = self.vsid_proc
        if proc is None or proc.poll() is not None:
            self.vsid_proc = None
            self.menu.entryconfigure(self.stop_vsid_menu_index, state='disabled')
            if callback:
                callback()
            return
        self.vsid_proc = None
        self.menu.entryconfigure(self.stop_vsid_menu_index, state='disabled')
        self.status_var.set('Stopping VSID…')
        try:
            proc.send_signal(signal.SIGINT)
        except ProcessLookupError:
            pass
        self._wait_vsid_stop(proc, time.monotonic() + 4, callback)

    def _wait_vsid_stop(self, proc: subprocess.Popen, deadline: float, callback=None):
        if proc.poll() is not None:
            if callback:
                callback()
            elif self.root.winfo_exists():
                self.status_var.set('Ready')
            return
        if time.monotonic() >= deadline:
            proc.terminate()
            self.root.after(500, lambda: self._force_stop_vsid(proc, callback))
            return
        self.root.after(100, lambda: self._wait_vsid_stop(proc, deadline, callback))

    def _force_stop_vsid(self, proc: subprocess.Popen, callback=None):
        if proc.poll() is None:
            proc.kill()
            self.root.after(100, lambda: self._force_stop_vsid(proc, callback))
        elif callback:
            callback()
        elif self.root.winfo_exists():
            self.status_var.set('Ready')

    def stop_vsid(self):
        self._stop_vsid_then()

    def exit_asid_mode(self):
        self._stop_vsid_then(self._restart_firmware)

    def _restart_firmware(self):
        self.asid_mode = False
        self.ready_at = time.monotonic()
        self.status_var.set('Restarting firmware…')
        self.engine.send('reset')
        self.root.after(18000, lambda: self.status_var.set('Ready')
                        if self.root.winfo_exists() and not self.asid_mode else None)

    def _open_menu(self, gear):
        self.canvas.itemconfigure(gear, image=self.images['gear'][1])
        gx, gy = LAYOUT['gear']
        x = self.canvas.winfo_rootx() + gx - 150
        y = self.canvas.winfo_rooty() + gy + 16
        self.root.update_idletasks()
        try:
            self.menu.tk_popup(x, y)
        finally:
            self.menu.grab_release()
            self.canvas.itemconfigure(gear, image=self.images['gear'][0])

    @staticmethod
    def _cc_text(cc: int) -> str:
        return 'none' if cc is None or cc < 0 else str(cc)

    def open_setup(self):
        if self.setup_win is not None and self.setup_win.winfo_exists():
            self.setup_win.lift()
            return
        w = tk.Toplevel(self.root)
        self.setup_win = w
        w.title('SidPortal Setup')
        w.transient(self.root)
        w.resizable(False, False)
        f = ttk.Frame(w, padding=14)
        f.pack(fill='both', expand=True)
        row = 0

        def heading(text):
            nonlocal row
            ttk.Label(f, text=text, font=('DejaVu Sans', 10, 'bold')).grid(
                row=row, column=0, columnspan=3, sticky='w', pady=(8 if row else 0, 4))
            row += 1

        def field(label, widget, extra=None):
            nonlocal row
            ttk.Label(f, text=label).grid(row=row, column=0, sticky='w', padx=(0, 10), pady=3)
            widget.grid(row=row, column=1, sticky='we', pady=3)
            if extra is not None:
                extra.grid(row=row, column=2, sticky='w', padx=(8, 0))
            row += 1

        heading('MIDI')
        midi_box = ttk.Combobox(f, textvariable=self.midi_var, width=34, state='readonly',
                                postcommand=self.refresh_midi_sources)
        midi_box.bind('<<ComboboxSelected>>', lambda e: self.connect_midi())
        field('Input', midi_box)
        field('Keyboard channel', ttk.Spinbox(f, from_=1, to=16, width=4, textvariable=self.channel_var,
                                              command=self._channel_changed))
        cc_box = ttk.Spinbox(f, values=['none'] + [str(i) for i in range(128)], width=6,
                             textvariable=self.bias_cc_var, command=self._bias_cc_changed)
        cc_box.bind('<Return>', lambda e: self._bias_cc_changed())
        cc_box.bind('<FocusOut>', lambda e: self._bias_cc_changed())
        self.learn_btn = ttk.Button(f, text='Learn', command=self._learn_bias_cc)
        field('Filter character CC', cc_box, self.learn_btn)

        heading('Sound')
        engine_box = ttk.Combobox(f, textvariable=self.engine_var, width=34, state='readonly',
                                  values=list(ENGINE_NAMES.values()))
        engine_box.bind('<<ComboboxSelected>>', lambda e: self._engine_changed())
        field('SID emulation', engine_box)

        heading('Audio')
        out_box = ttk.Combobox(f, textvariable=self.out_var, width=34,
                               postcommand=lambda: out_box.configure(values=alsa_pcms(False)))
        out_box.bind('<<ComboboxSelected>>', lambda e: self._audio_out_changed())
        out_box.bind('<Return>', lambda e: self._audio_out_changed())
        field('Output', out_box)
        ext_box = ttk.Combobox(f, textvariable=self.ext_var, width=34,
                               postcommand=lambda: ext_box.configure(values=['none'] + alsa_pcms(True)))
        ext_box.bind('<<ComboboxSelected>>', lambda e: self._ext_in_changed())
        ext_box.bind('<Return>', lambda e: self._ext_in_changed())
        field('Ext in (through the filter)', ext_box)
        field('Ext in gain', ttk.Scale(f, from_=0.0, to=4.0, variable=self.ext_gain_var,
                                       command=lambda v: self._ext_gain_changed()))
        field('Host noise gate', ttk.Checkbutton(f, text='On', variable=self.noise_gate_var,
                                                command=self._gate_changed))
        gate_scale = ttk.Scale(f, from_=-90.0, to=-18.0, variable=self.gate_threshold_var,
                               command=lambda v: self._gate_changed())
        gate_value = ttk.Label(f, text=f"{self.gate_threshold_var.get():.1f} dBFS")
        self.gate_value = gate_value
        field('Gate threshold (-90 to -18 dBFS)', gate_scale, gate_value)

        ttk.Button(f, text='Close', command=w.destroy).grid(row=row, column=0, columnspan=3, sticky='e', pady=(12, 0))
        f.columnconfigure(1, weight=1)

    def _gate_changed(self):
        enabled = self.noise_gate_var.get()
        threshold = round(max(-90.0, min(-18.0, self.gate_threshold_var.get())), 1)
        self.cfg['noise_gate'] = enabled
        self.cfg['gate_threshold_db'] = threshold
        save_config(self.cfg)
        if hasattr(self, 'gate_value') and self.gate_value.winfo_exists():
            self.gate_value.configure(text=f'{threshold:.1f} dBFS')
        self.engine.send(f'gatethreshold {threshold:.1f}')
        self.engine.send(f'noisegate {int(enabled)}')

    def _bias_cc_changed(self):
        text = self.bias_cc_var.get().strip()
        cc = int(text) if text.isdigit() and int(text) < 128 else -1
        self.bias_cc_var.set(self._cc_text(cc))
        if cc != self.cfg.get('bias_cc', -1):
            self.cfg['bias_cc'] = cc
            save_config(self.cfg)
        self.engine.send(f'biascc {cc}')

    def _learn_bias_cc(self):
        self.engine.send('learn bias')
        self.learn_btn.configure(text='Move a control…')

    def refresh_midi_sources(self):
        self.midi_sources = alsa_midi_sources()
        self.midi_box['values'] = ['(none)'] + [n for _, n in self.midi_sources]

    def connect_midi(self, name: str | None = None):
        name = name if name is not None else self.midi_var.get()
        dest = sidportal_input_port()
        if not dest:
            self.status_var.set('MIDI port not ready')
            return
        # Drop existing subscriptions to our input, then make the new one.
        for addr, _ in alsa_midi_sources():
            subprocess.run(['aconnect', '-d', addr, dest], capture_output=True)
        self.cfg['midi_source'] = '' if name in ('', '(none)') else name
        for addr, n in self.midi_sources or alsa_midi_sources():
            if n == name:
                r = subprocess.run(['aconnect', addr, dest], capture_output=True, text=True)
                self.status_var.set('MIDI: ' + name.split(':')[0] if r.returncode == 0 else 'MIDI connect failed')
                break
        save_config(self.cfg)

    def connect_saved_midi(self):
        name = self.cfg.get('midi_source')
        if not name:
            return
        self.midi_sources = alsa_midi_sources()
        if any(n == name for _, n in self.midi_sources):
            self.midi_var.set(name)
            self.connect_midi(name)
        elif self.ready_at is None:
            self.root.after(1000, self.connect_saved_midi)

    def _audio_out_changed(self):
        dev = self.out_var.get().strip() or 'default'
        if dev == self.cfg['audio']:
            return
        if not messagebox.askokcancel('Audio output', f'Restart the SidPortal on {dev}?\n\n'
                                      'The machine reboots; patches and settings are kept.'):
            self.out_var.set(self.cfg['audio'])
            return
        self.cfg['audio'] = dev
        save_config(self.cfg)
        self.engine.stop()
        self.engine = Engine(self.engine.binary, self.engine.rom, self.cfg)
        self.engine.start()
        self.root.after(1500, self.connect_saved_midi)

    def _ext_in_changed(self):
        dev = self.ext_var.get().strip() or 'none'
        self.cfg['ext_in'] = dev
        save_config(self.cfg)
        self.engine.send(f'extin {dev}')

    def _ext_gain_changed(self):
        self.cfg['ext_gain'] = round(float(self.ext_gain_var.get()), 3)
        self.engine.send(f'extgain {self.cfg["ext_gain"]:.3f}')

    # Filter character -0.5..+0.5 on the knob's 0..255, centred on 128.
    @staticmethod
    def _bias_to_knob(v: float) -> int:
        return max(0, min(255, round(128 + v * 254)))

    @staticmethod
    def _knob_to_bias(k: int) -> float:
        return max(-0.5, min(0.5, (k - 128) / 254))

    # Volume 0..3 with an audio taper: unity sits a little past half way.
    @staticmethod
    def _volume_to_knob(v: float) -> int:
        return max(0, min(255, round(255 * math.sqrt(max(0.0, v) / 3.0))))

    @staticmethod
    def _knob_to_volume(k: int) -> float:
        return 3.0 * (k / 255) ** 2

    def _bias_changed(self, k: int):
        self.cfg['filter_bias'] = round(self._knob_to_bias(k), 3)
        self.engine.send(f'filterbias {self.cfg["filter_bias"]:.3f}')

    def _update_bias_state(self):
        # reSID's character control moves the 6581's filter only; reSIDfp has one for each chip.
        idle = self.cfg['model'] == '8580' and self.cfg.get('engine') == 'resid'
        self.canvas.itemconfigure('character', state='hidden' if idle else 'normal')
        m = self.cfg['model']
        self.canvas.itemconfigure(self.subtitle, image=art(f'subtitle_{m}'))

    def _engine_changed(self):
        name = next(k for k, v in ENGINE_NAMES.items() if v == self.engine_var.get())
        if name == self.cfg.get('engine'):
            return
        self.cfg['engine'] = name
        self.engine.send(f'engine {name}')
        self._update_bias_state()
        save_config(self.cfg)

    def _channel_changed(self):
        self.cfg['channel'] = max(1, min(16, int(self.channel_var.get() or 1)))

    def _model_changed(self, is_8580: bool):
        self.cfg['model'] = '8580' if is_8580 else '6581'
        self.engine.send(f'model {self.cfg["model"]}')
        self._update_bias_state()
        save_config(self.cfg)

    def _volume_changed(self, k: int):
        self.cfg['volume'] = round(self._knob_to_volume(k), 3)
        self.engine.send(f'volume {self.cfg["volume"]:.3f}')

    def load_syx(self, path: Path | None = None):
        if path is None:
            start = ROOT / 'patches' if (ROOT / 'patches').is_dir() else Path.home()
            chosen = filedialog.askopenfilename(title='Send SysEx to the SidPortal', initialdir=start,
                                                filetypes=[('SysEx', '*.syx'), ('All files', '*')])
            if not chosen:
                return
            path = Path(chosen)
            if not messagebox.askokcancel(
                    'Load SysEx',
                    f'Send {path.name} to the SidPortal?\n\nAn all-patch dump replaces every patch in memory.'):
                return
        self.engine.send(f'syx {path}')

    # -- Panel actions ------------------------------------------------------------------
    def key(self, code: int, down: bool):
        if down and code in self.keys_down:
            return
        if not down and code not in self.keys_down:
            return
        if down and self.asid_mode:
            self.keys_down.add(code)
            self.canvas.itemconfigure(self.key_items[code], image=self.images[f'key{code}'][1])
            # Route the panel exit through the host reset path instead of relying on
            # the firmware's keypad poll and its timing while ASID data is arriving.
            self.asid_mode = False
            self._stop_vsid_then(self._restart_firmware)
            return
        (self.keys_down.add if down else self.keys_down.discard)(code)
        up_img, down_img = self.images[f'key{code}']
        self.canvas.itemconfigure(self.key_items[code], image=down_img if down else up_img)
        self.engine.send(f'{"down" if down else "up"} {code}')

    def knob_changed(self, i: int, v: int):
        self.cfg['knobs'][i] = v
        self.engine.send(f'knob {i + 1} {v}')

    def note(self, n: int, on: bool):
        ch = self.cfg['channel'] - 1
        if on:
            self.engine.send(f'midi {0x90 | ch:02X} {n:02X} 64')
        else:
            self.engine.send(f'midi {0x80 | ch:02X} {n:02X} 00')

    # -- Computer keyboard ----------------------------------------------------------------
    KEYMAP = {
        **{str(d): KEY_LEGENDS.index(str(d)) for d in range(10)},
        **{f'KP_{d}': KEY_LEGENDS.index(str(d)) for d in range(10)},
        'Return': KEY_LEGENDS.index('D'), 'KP_Enter': KEY_LEGENDS.index('D'),
        'BackSpace': KEY_LEGENDS.index('A'), 'Escape': KEY_LEGENDS.index('A'),
        'Up': KEY_LEGENDS.index('B'), 'Down': KEY_LEGENDS.index('C'),
        'Left': KEY_LEGENDS.index('*'), 'Right': KEY_LEGENDS.index('#'),
    }
    KNOB_KEYS = {'a': 0, 's': 1, 'd': 2, 'f': 3}

    def _key_press(self, e):
        if isinstance(e.widget, (ttk.Combobox, ttk.Spinbox, tk.Entry)):
            return
        sym = e.keysym if len(e.keysym) > 1 else e.keysym.lower()
        job = self.release_jobs.pop(sym, None)
        if job:
            self.root.after_cancel(job)   # auto-repeat: the key never really came up
        if sym in ('minus', 'equal', 'KP_Subtract', 'KP_Add'):
            d = 1 if sym in ('equal', 'KP_Add') else -1
            knobs = [self.KNOB_KEYS[k] for k in self.held if k in self.KNOB_KEYS]
            if knobs:
                for i in knobs:
                    self.knobs[i].nudge(4 * d)
            else:
                self.encoder.step(d)
            return
        if sym in self.held:
            return
        if sym in self.KEYMAP:
            self.held[sym] = self.KEYMAP[sym]
            self.key(self.KEYMAP[sym], True)
        elif sym in self.KNOB_KEYS:
            self.held[sym] = -1

    def _key_release(self, e):
        sym = e.keysym if len(e.keysym) > 1 else e.keysym.lower()
        if sym not in self.held:
            return

        def release():
            self.release_jobs.pop(sym, None)
            code = self.held.pop(sym, None)
            if code is not None and code >= 0:
                self.key(code, False)
        self.release_jobs[sym] = self.root.after(RELEASE_GRACE_MS, release)

    # -- Engine output ----------------------------------------------------------------------
    def poll(self):
        lcd_line = None
        try:
            while True:
                line = self.engine.lines.get_nowait()
                if line.startswith('lcd '):
                    lcd_line = line
                elif line.startswith('leds '):
                    v = int(line.split()[1], 16)
                    for led, bit in zip(self.leds, LED_BITS):
                        led.set(not (v & bit))
                elif line.startswith('syx '):
                    left = int(line.split()[1])
                    self.status_var.set(f'Receiving SysEx: {left} bytes' if left else 'Ready')
                elif line.startswith('biascc '):
                    cc = int(line.split()[1])
                    self.cfg['bias_cc'] = cc
                    save_config(self.cfg)
                    self.bias_cc_var.set(self._cc_text(cc))
                    if self.setup_win is not None and self.setup_win.winfo_exists():
                        self.learn_btn.configure(text='Learn')
                    self.status_var.set(f'Filter character on CC {cc}')
                elif line.startswith('filterbias '):
                    self.cfg['filter_bias'] = float(line.split()[1])
                    self.character_knob.show(self._bias_to_knob(self.cfg['filter_bias']))
                elif line.startswith('extin '):
                    dev = line[6:]
                    self.status_var.set('Ext in: off' if dev == 'none' else f'Ext in: {dev}')
                elif line == 'ready':
                    self.ready_at = time.monotonic()
                    self.status_var.set('Ready')
                    for i, k in enumerate(self.knobs):
                        self.engine.send(f'knob {i + 1} {k.value}')
                elif line.startswith('error'):
                    self.status_var.set(line[6:60])
                    messagebox.showerror('SidPortal engine', line[6:])
                elif line == 'exited':
                    self.status_var.set('Engine stopped')
        except queue.Empty:
            pass
        if lcd_line:
            p = lcd_line.split()
            self.lcd.update(bytes.fromhex(p[1]), bytes.fromhex(p[2]), p[3] == '1', p[4] == '1',
                            p[5] == '1', int(p[6]), int(p[7]))
        # First run: the RAM is empty, so send the factory bank once the boot screen is past.
        if self.pending_bank and self.ready_at and time.monotonic() - self.ready_at > 14:
            self.engine.send(f'syx {self.pending_bank}')
            self.pending_bank = None
        self.root.after(16, self.poll)

    def blink(self):
        self.lcd.tick_blink()
        self.root.after(400, self.blink)

    def quit(self):
        try:
            save_config(self.cfg)
        except OSError:
            pass
        proc = self.vsid_proc
        self.vsid_proc = None
        if proc and proc.poll() is None:
            try:
                proc.send_signal(signal.SIGINT)
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.terminate()
                try:
                    proc.wait(timeout=1)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=1)
            except ProcessLookupError:
                pass
        self.engine.stop()
        self.root.destroy()


def main():
    binary = find_binary()
    rom = find_rom()
    root = tk.Tk(className='sidportal')   # WM_CLASS Sidportal, as the start-menu entry expects
    icons = [tk.PhotoImage(master=root, file=str(path))
             for path in sorted((Path(__file__).resolve().parent / 'assets' / 'icons').glob('sidportal-*.png'))]
    if icons:
        root.iconphoto(True, *icons)
    if not binary or not rom:
        root.withdraw()
        messagebox.showerror('SidPortal', 'Build the engine (cmake --build build) and import the firmware '
                             'with tools/roms.py first.' if not binary else
                             'Firmware not found: run tools/roms.py to import the firmware.')
        return 1
    App(root, binary, rom)
    root.mainloop()
    return 0


if __name__ == '__main__':
    sys.exit(main())
