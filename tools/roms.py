#!/usr/bin/env python3
"""Import and verify the OS r34 firmware the emulator runs.

Accepts either 128 KB EPROM dump of OS r34 (the program at $8000, or the "reloc" dump
with it at $0000), a bare 32 KB program image, or a ZIP holding any of them; files are
recognised by size and SHA-1, not by name. The 32 KB program is written to
roms/firmware.bin.

  python3 tools/roms.py --import-zip /path/to/firmware_dumps.zip
  python3 tools/roms.py --import /path/to/dump.bin
  python3 tools/roms.py            # verify roms/firmware.bin
"""
from __future__ import annotations

import argparse
import hashlib
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ROM_PATH = ROOT / 'roms' / 'firmware.bin'
PROGRAM_SHA1 = '5abd531e083e36851e1506a136c06e1b06f524b2'   # the 32 KB $8000-$FFFF image
KNOWN_DUMPS = {
    '7e1117907e24a0eb973590b6384210f355901af4': 0x8000,     # 128 KB dump, program at $8000
    '6456aeb829b13bc867812f83729baccb48058865': 0x0000,     # 128 KB 'reloc' dump, program at $0000
}


def program_from(data: bytes) -> bytes | None:
    if len(data) == 0x8000:
        prog = data
    elif len(data) == 0x20000:
        offset = KNOWN_DUMPS.get(hashlib.sha1(data).hexdigest())
        if offset is None:
            offset = 0x8000 if data[0xFFFE:0x10000] == b'\x80\x00' else 0
        prog = data[offset:offset + 0x8000]
    else:
        return None
    return prog if hashlib.sha1(prog).hexdigest() == PROGRAM_SHA1 else None


def store(prog: bytes) -> None:
    ROM_PATH.parent.mkdir(exist_ok=True)
    ROM_PATH.write_bytes(prog)
    print(f'imported {ROM_PATH.relative_to(ROOT)} (SHA-1 {PROGRAM_SHA1})')


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--import-zip', type=Path)
    ap.add_argument('--import', dest='import_file', type=Path)
    args = ap.parse_args()

    if args.import_zip:
        with zipfile.ZipFile(args.import_zip) as z:
            for name in z.namelist():
                if name.startswith('__MACOSX') or not name.lower().endswith('.bin'):
                    continue
                prog = program_from(z.read(name))
                if prog:
                    store(prog)
                    return 0
        print('no OS r34 image found in the ZIP', file=sys.stderr)
        return 1
    if args.import_file:
        prog = program_from(args.import_file.read_bytes())
        if not prog:
            print(f'{args.import_file} is not an OS r34 image (size or SHA-1 mismatch)', file=sys.stderr)
            return 1
        store(prog)
        return 0
    if not ROM_PATH.exists():
        print(f'{ROM_PATH.relative_to(ROOT)} is missing; import it with --import-zip or --import', file=sys.stderr)
        return 1
    if program_from(ROM_PATH.read_bytes()) is None:
        print(f'{ROM_PATH.relative_to(ROOT)} does not match OS r34', file=sys.stderr)
        return 1
    print(f'{ROM_PATH.relative_to(ROOT)} OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
