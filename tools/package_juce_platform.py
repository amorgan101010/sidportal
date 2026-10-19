#!/usr/bin/env python3
"""Package the Linux or Windows JUCE build into a portable ZIP and checksum."""

from __future__ import annotations

import argparse
import hashlib
import shutil
import stat
import struct
import zipfile
from pathlib import Path


def is_elf(path: Path) -> bool:
    try:
        with path.open("rb") as f:
            return f.read(4) == b"\x7fELF"
    except OSError:
        return False


def assert_elf_x64(path: Path) -> None:
    with path.open("rb") as f:
        header = f.read(20)
    if len(header) < 20 or header[:4] != b"\x7fELF" or header[4] != 2:
        raise SystemExit(f"expected a 64-bit Linux ELF binary: {path}")
    endian = "<" if header[5] == 1 else ">" if header[5] == 2 else None
    if endian is None or struct.unpack(endian + "H", header[18:20])[0] != 62:
        raise SystemExit(f"expected an x86_64 Linux binary: {path}")


def pe_machine(path: Path) -> int | None:
    try:
        with path.open("rb") as f:
            if f.read(2) != b"MZ":
                return None
            f.seek(0x3C)
            offset = struct.unpack("<I", f.read(4))[0]
            f.seek(offset)
            if f.read(4) != b"PE\0\0":
                return None
            return struct.unpack("<H", f.read(2))[0]
    except (OSError, struct.error):
        return None


def assert_pe_x64(path: Path) -> None:
    if pe_machine(path) != 0x8664:
        raise SystemExit(f"expected an x86_64 Windows executable: {path}")


def copy_tree(source: Path, destination: Path) -> None:
    shutil.copytree(source, destination, symlinks=False)
    for path in destination.rglob("*"):
        if path.is_symlink():
            raise SystemExit(f"symlink is not supported in portable package: {path}")


def reject_firmware(root: Path) -> None:
    extensions = {".bin", ".rom", ".syx"}
    for path in root.rglob("*"):
        if path.is_file() and path.suffix.lower() in extensions:
            raise SystemExit(f"firmware or patch data found in package: {path}")


def linux_readme() -> str:
    return """SidPortal for Linux (x86_64)

This package contains the JUCE standalone app, VST3 instrument, and the Linux
command-line realtime/WAV renderer. SidStation firmware is not included.

On first launch, select your own verified SidStation OS r34 firmware image when
the picker opens. SidPortal copies it to:

  $XDG_DATA_HOME/sidportal/firmware.bin

If XDG_DATA_HOME is unset, the path is ~/.local/share/sidportal/firmware.bin.
You can also set SIDPORTAL_ROM to choose a firmware file explicitly.

Run the standalone app with ./SidPortal. The VST3 bundle is SidPortal.vst3;
install it in your DAW's VST3 plugin folder. For realtime or WAV jobs, the
command-line renderer is ./sidportal. These binaries are unsigned.

"""


def windows_readme() -> str:
    return """SidPortal for Windows (x64)

This package contains the JUCE standalone app and VST3 instrument.
SidStation firmware is not included. On first launch, select your own verified
SidStation OS r34 firmware image when the picker opens. SidPortal copies it to:

  %APPDATA%\\SidPortal\\firmware.bin

You can also set SIDPORTAL_ROM to choose a firmware file explicitly.

Run SidPortal.exe to open the standalone app. Install SidPortal.vst3 in your
DAW's VST3 plugin folder and rescan. These binaries are unsigned.
"""


def zip_tree(package: Path, archive: Path) -> None:
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for path in sorted(package.rglob("*")):
            arcname = path.relative_to(package.parent).as_posix()
            info = zipfile.ZipInfo(arcname + ("/" if path.is_dir() else ""))
            info.create_system = 3
            mode = path.lstat().st_mode
            info.external_attr = (stat.S_IMODE(mode) & 0xFFFF) << 16
            if path.is_dir():
                zf.writestr(info, b"")
            else:
                zf.writestr(info, path.read_bytes())


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def package(platform: str, build_dir: Path, output_dir: Path) -> tuple[Path, Path]:
    artefacts = build_dir / "SidPortalPlugin_artefacts" / "Release"
    vst3 = artefacts / "VST3" / "SidPortal.vst3"
    standalone = artefacts / "Standalone" / ("SidPortal.exe" if platform == "windows" else "SidPortal")
    if not vst3.is_dir():
        raise SystemExit(f"missing VST3 bundle: {vst3}")
    if not standalone.is_file():
        raise SystemExit(f"missing standalone executable: {standalone}")

    if platform == "linux":
        assert_elf_x64(standalone)
        cli = build_dir / "sidportal"
        if not cli.is_file():
            raise SystemExit(f"missing Linux realtime engine: {cli}")
        assert_elf_x64(cli)
        plugin_binaries = [p for p in vst3.rglob("*") if p.is_file() and is_elf(p)]
        if not plugin_binaries:
            raise SystemExit(f"no Linux ELF plugin binary found in {vst3}")
        for binary in plugin_binaries:
            assert_elf_x64(binary)
        platform_name = "Linux-x86_64"
        readme = linux_readme()
    else:
        assert_pe_x64(standalone)
        plugin_binaries = [p for p in vst3.rglob("*") if p.is_file() and pe_machine(p) is not None]
        if not plugin_binaries:
            raise SystemExit(f"no Windows PE plugin binary found in {vst3}")
        for binary in plugin_binaries:
            assert_pe_x64(binary)
        platform_name = "Windows-x64"
        readme = windows_readme()

    package_name = f"SidPortal-{platform_name}"
    output_dir.mkdir(parents=True, exist_ok=True)
    stage_parent = output_dir / ".stage"
    if stage_parent.exists():
        shutil.rmtree(stage_parent)
    package_root = stage_parent / package_name
    package_root.mkdir(parents=True)
    copy_tree(vst3, package_root / "SidPortal.vst3")
    shutil.copy2(standalone, package_root / standalone.name)
    (package_root / f"README-{platform}.txt").write_text(readme, encoding="utf-8")

    if platform == "linux":
        shutil.copy2(cli, package_root / "sidportal")

    reject_firmware(package_root)
    archive = output_dir / f"{package_name}.zip"
    checksum = Path(str(archive) + ".sha256")
    archive.unlink(missing_ok=True)
    checksum.unlink(missing_ok=True)
    zip_tree(package_root, archive)
    digest = sha256(archive)
    checksum.write_text(f"{digest}  {archive.name}\n", encoding="ascii")
    shutil.rmtree(stage_parent)
    return archive, checksum


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("platform", choices=("linux", "windows"))
    parser.add_argument("build_dir", type=Path)
    parser.add_argument("output_dir", type=Path)
    args = parser.parse_args()
    archive, checksum = package(args.platform, args.build_dir, args.output_dir)
    print(f"created {archive} and {checksum}")


if __name__ == "__main__":
    main()
