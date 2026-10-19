#!/usr/bin/env python3
"""Firmware-level checks: boot to the top menu, receive the factory bank over SysEx,
select a patch, play A4 and check the SID frequency, and watch the Edit LED.
Skipped (exit 77) without roms/firmware.bin; the SysEx part needs the factory bank
(a .syx with "Presets" in its name) in patches/."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(os.environ.get('SIDPORTAL_ROOT', Path(__file__).resolve().parent.parent))
BINARY = os.environ.get('SIDPORTAL_BINARY', str(ROOT / 'build' / 'sidportal'))
TO_P00 = 'enc -99;wait 300'
ROM = ROOT / 'roms' / 'firmware.bin'
PRESETS = next(iter(sorted((ROOT / 'patches').glob('*Presets*.syx'))), None)

failures = 0


def run(script, *extra, nvram=None):
    args = [BINARY, '--rom', str(ROM), '--script', script, *extra]
    if nvram:
        args += ['--nvram', nvram]
    return subprocess.run(args, capture_output=True, text=True, timeout=120, check=True).stdout


def check(ok, what, detail=''):
    global failures
    print(('ok   ' if ok else 'FAIL ') + what + ('' if ok else f'\n{detail}'))
    failures += not ok


if not ROM.exists():
    print('skipped: no ROM')
    sys.exit(77)

out = run('wait 13000;lcd', '--fresh')
check('Patch' in out and 'System' in out, 'boots to the top menu', out)

# MIDI arriving during boot once wedged the machine: a byte latched before the firmware
# listens leaves a half message, and the next status byte makes it restart itself.
out = run('wait 3000;note 60 100 100;wait 10000;note 64 100 100;wait 1000;lcd', '--fresh')
check('crashed' not in out and 'Patch' in out, 'notes during boot neither crash nor restart it', out)

import array, wave


def render(script, *extra, nvram=None, tail=None):
    with tempfile.TemporaryDirectory() as tmp:
        wav = Path(tmp) / 'out.wav'
        run(script, '--render', str(wav), *extra, nvram=nvram)
        w = wave.open(str(wav))
        pcm = array.array('h', w.readframes(w.getnframes()))
    return pcm[-int(tail * 48000):] if tail else pcm


def peak(pcm):
    return max(abs(v) for v in pcm)


def asid_pitch(pcm, rate=48000):
    """Find the strongest autocorrelation period in the A4 search window."""
    segment = pcm[-rate // 2:]
    mean = sum(segment) / len(segment)
    centered = [sample - mean for sample in segment]
    best_lag, best_corr = 0, float('-inf')
    for lag in range(rate // 500, rate // 350 + 1):
        count = len(centered) - lag
        left = centered[:count]
        right = centered[lag:]
        norm = (sum(v * v for v in left) * sum(v * v for v in right)) ** 0.5
        corr = sum(a * b for a, b in zip(left, right)) / norm if norm else 0
        if corr > best_corr:
            best_lag, best_corr = lag, corr
    return rate / best_lag if best_lag else 0


with tempfile.TemporaryDirectory() as tmp:
    wav = Path(tmp) / 'asid.wav'
    # ASID slots 0, 1, 4, 5, 21 and 22: voice 1 A4, fast attack, full sustain,
    # saw + gate, and master volume 15. A repeated update commits the buffered writes.
    frame = 'F0 2D 4E 33 00 00 03 21 00 00 00 56 1C 09 70 0F 21 F7'
    lcd = 'F0 2D 4F ' + ' '.join(f'{ord(c):02X}' for c in 'ASID FRAME TEST'.ljust(32)) + ' F7'
    lcd_updates = ';'.join(f'midi {frame}' for _ in range(16))
    script = ('wait 12500;press down;wait 300;press down;wait 300;press enter;wait 300;'
              f'midi {frame};wait 20;midi {frame};wait 20;midi {lcd};{lcd_updates};wait 1200;lcd')
    out = run(script, '--fresh', '--render', str(wav))
    check('ASID FRAME TEST' in out, 'ASID mode displays the received text', out)
    check('crashed' not in out and 'midi in:' not in out,
          'ASID MIDI arrives without receiver drops or overruns', out)
    with wave.open(str(wav)) as w:
        pcm = array.array('h', w.readframes(w.getnframes()))
    hz = asid_pitch(pcm)
    check(438 <= hz <= 443, 'ASID register updates play A4 at 440 Hz', f'{hz:.1f} Hz')
    print(f'ASID measurement: {hz:.1f} Hz; 18-byte update takes 5.8 ms at 3125 bytes/s; 0 drops/overruns')


for engine in ('residfp', 'resid'):
    e = ('--engine', engine)
    # Power-on: the DC step when the firmware first sets the volume must stay muted.
    pcm = render('wait 12000', '--fresh', *e)
    check(peak(pcm) < 2000, f'{engine}: boot is free of the power-on thump', peak(pcm))
    # Switching chips steps the output's DC level; the switch must be faded, not thumped.
    pcm = render('wait 12500;model 8580;wait 800;model 6581;wait 800', '--fresh', *e, tail=1.6)
    check(peak(pcm) < 2000, f'{engine}: switching SID chips does not thump', peak(pcm))

# Switching engines live is faded the same way.
pcm = render('wait 12500;engine resid;wait 800;engine residfp;wait 800', '--fresh', tail=1.6)
check(peak(pcm) < 2000, 'switching SID engines does not thump', peak(pcm))

# Ext In: noise into the SID's external input comes out through the filter, so opening
# the cutoff (CC27) on the idle patch must raise the level and brighten it.
with tempfile.TemporaryDirectory() as tmp:
    import random, struct
    wavemod = wave
    noise = Path(tmp) / 'noise.wav'
    rnd = random.Random(1)
    with wavemod.open(str(noise), 'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(48000)
        w.writeframes(b''.join(struct.pack('<h', int(max(-32767, min(32767, rnd.gauss(0, 8000)))))
                               for _ in range(48000 * 3)))
    out_wav = Path(tmp) / 'ext.wav'
    run(f'wait 13000;extin {noise};midi B0 1B 00;wait 1000;midi B0 1B 7F;wait 1000', '--fresh', '--render', str(out_wav))
    import array
    w = wavemod.open(str(out_wav))
    pcm = array.array('h', w.readframes(w.getnframes()))
    rate = 48000
    def rms(a, b):
        seg = pcm[int(a * rate):int(b * rate)]
        return (sum(v * v for v in seg) / len(seg)) ** 0.5
    end = len(pcm) / rate
    closed, opened = rms(end - 1.7, end - 1.1), rms(end - 0.7, end - 0.1)
    check(opened > 3 * closed > 0, 'Ext In passes through the filter (cutoff opens it up)',
          f'closed {closed:.0f} open {opened:.0f}')

    # Chip character: at the same cutoff, a darker setting leaves less high end.
    def high_share(bias, *extra):
        path = Path(tmp) / f'bias{bias}.wav'
        run(f'wait 13000;filterbias {bias};extin {noise};midi B0 1B 40;wait 1000', '--fresh', '--render', str(path),
            *extra)
        w = wavemod.open(str(path))
        p = array.array('h', w.readframes(w.getnframes()))[-24000:]
        diff = [p[i] - p[i - 1] for i in range(1, len(p))]   # first difference ~ treble energy
        return (sum(v * v for v in diff) / sum(v * v for v in p)) ** 0.5
    for extra in (('--engine', 'residfp'), ('--engine', 'resid'), ('--engine', 'residfp', '--8580')):
        dark, bright = high_share(-0.5, *extra), high_share(0.5, *extra)
        check(bright > 1.5 * dark, f'filter character makes {" ".join(extra[1:])} darker or brighter',
              f'dark {dark:.3f} bright {bright:.3f}')

if PRESETS:
    with tempfile.TemporaryDirectory() as tmp:
        nvram = str(Path(tmp) / 'nvram.bin')
        out = run(f'wait 3000;note 60 100 100;wait 11000;syx {PRESETS};wait 500;press enter;wait 800;{TO_P00};wait 1500;lcd',
                  '--fresh', nvram=nvram)
        check('P00 Anpanman' in out, 'presets load after notes were played during boot', out)

    with tempfile.TemporaryDirectory() as tmp:
        nvram = str(Path(tmp) / 'nvram.bin')
        run(f'wait 12000;syx {PRESETS};save', '--fresh', nvram=nvram)
        out = run(f'wait 12000;press enter;wait 1000;{TO_P00};wait 1500;lcd', nvram=nvram)
        check('P00 Anpanman' in out, 'factory bank received, P00 is Anpanman', out)

        # A held note keeps sounding across an engine switch: the registers are replayed.
        def rms(seg):
            return (sum(v * v for v in seg) / len(seg)) ** 0.5
        pcm = render(f'wait 12000;press enter;wait 1000;{TO_P00};wait 1500;midi 90 45 64;wait 800;'
                     'engine resid;wait 800;engine residfp;wait 800', nvram=nvram, tail=2.4)
        before, mid, after = (rms(pcm[i * 38400 + 19200:(i + 1) * 38400]) for i in range(3))
        check(mid > 0.3 * before and after > 0.3 * before > 0, 'a held note survives engine switches',
              f'{before:.0f} {mid:.0f} {after:.0f}')

        out = run(f'wait 12000;press enter;wait 1000;{TO_P00};wait 1500;press enter;wait 500;'
                  'leds;note 69 100 400;press right;wait 500;leds', '--trace-sid', nvram=nvram)
        freqs = set()
        regs = [0] * 32
        for line in out.splitlines():
            if line.startswith('sid '):
                reg, val = line.split()[2].split('=')
                regs[int(reg, 16)] = int(val, 16)
                freqs.add(regs[0] | regs[1] << 8)
        hz = [f * 1e6 / 2 ** 24 for f in freqs]
        check(any(abs(h - 440) < 2 for h in hz), 'A4 sets voice 1 to 440 Hz with a 1 MHz SID clock',
              sorted(round(h, 1) for h in hz)[-10:])
        leds = [int(line.split()[1], 16) for line in out.splitlines() if line.startswith('leds')]
        check(len(leds) == 2 and leds[0] & 0x10 and not leds[1] & 0x10, 'Edit LED lights in edit mode', leds)
else:
    print('skipped SysEx checks: no factory bank (*Presets*.syx) in patches/')

sys.exit(1 if failures else 0)
