"""The scripted WAV output uses the same host gate as realtime and the plugin."""
import array
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import wave

root = Path(__file__).resolve().parent.parent
rom = root / 'roms/firmware.bin'
if not rom.exists():
    sys.exit(77)
binary = os.environ.get('SIDPORTAL_BINARY', str(root / 'build/sidportal'))
with tempfile.TemporaryDirectory() as directory:
    def render(name, *options):
        path = Path(directory) / name
        subprocess.run([binary, '--rom', str(rom), '--fresh', '--render', str(path),
                        '--script', 'wait 13000', *options], check=True, timeout=120,
                       capture_output=True)
        with wave.open(str(path)) as file:
            return array.array('h', file.readframes(file.getnframes()))
    bypass = render('bypass.wav')
    gated = render('gated.wav', '--noise-gate', '--gate-threshold-db', '-18')
    assert len(bypass) == len(gated)
    tail = 48000
    bypass_energy = sum(abs(v) for v in bypass[-tail:])
    gated_energy = sum(abs(v) for v in gated[-tail:])
    assert bypass_energy > 0, 'firmware rendered no idle audio'
    assert gated_energy < bypass_energy * 0.2, (bypass_energy, gated_energy)
