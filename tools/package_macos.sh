#!/bin/sh
# Package the universal JUCE VST3 and standalone app from a CMake Release build.
set -eu

build=${1:-build-macos}
out=${2:-dist}
artefacts="$build/SidPortalPlugin_artefacts/Release"
vst3="$artefacts/VST3/SidPortal.vst3"
app="$artefacts/Standalone/SidPortal.app"

for bundle in "$vst3" "$app"; do
    if [ ! -d "$bundle" ]; then
        echo "missing build product: $bundle" >&2
        exit 1
    fi
done

for executable in "$vst3/Contents/MacOS/SidPortal" "$app/Contents/MacOS/SidPortal"; do
    if [ ! -f "$executable" ]; then
        echo "missing bundle executable: $executable" >&2
        exit 1
    fi
    architectures=$(lipo -archs "$executable")
    for arch in arm64 x86_64; do
        case " $architectures " in
            *" $arch "*) ;;
            *) echo "$executable is missing the $arch architecture (has: $architectures)" >&2; exit 1 ;;
        esac
    done
done

mkdir -p "$out"
stage=$(mktemp -d "${TMPDIR:-/tmp}/sidportal-macos.XXXXXX")
trap 'rm -rf "$stage"' EXIT HUP INT TERM
package="$stage/SidPortal-macOS"
mkdir -p "$package"
ditto "$vst3" "$package/SidPortal.vst3"
ditto "$app" "$package/SidPortal.app"
cat > "$package/README-macOS.txt" <<'EOF'
SidPortal for macOS

This universal package contains a standalone app and a VST3 instrument.
It does not include SidStation firmware. On first launch, SidPortal prompts you
to select your own OS r34 firmware image if it cannot find one. The selected
image is copied to:

  ~/Library/Application Support/SidPortal/firmware.bin

You can also place a verified 32 KB firmware image there before launching, or
set SIDPORTAL_ROM to the firmware file before launching SidPortal or the DAW.

Patch memory starts empty on first launch. Load your SidStation patch bank with
the gear menu's Load SysEx command. The standalone and DAW projects retain patch
memory when closed normally.

For ASID playback, choose Enter ASID Mode in the gear menu and route an external
ASID MIDI stream to the standalone's selected MIDI input or to the plugin from
your DAW. Stop the MIDI source before choosing Exit ASID Mode / Restart.

Choose Noise Gate from the gear menu to adjust the host noise gate and its threshold.
The gate is off by default.

Copy SidPortal.app to Applications to use the standalone app. Install
SidPortal.vst3 in ~/Library/Audio/Plug-Ins/VST3 and restart or rescan your DAW.

These builds are unsigned. If macOS blocks the standalone app, Control-click it
in Finder, choose Open, and confirm the prompt.
EOF

if find "$package" -type f \( -name 'firmware.bin' -o -name '*.rom' -o -name '*.syx' \) | grep -q .; then
    echo "firmware or patch data found in package" >&2
    exit 1
fi

archive="$out/SidPortal-macOS-universal.zip"
rm -f "$archive" "$archive.sha256"
ditto -c -k --norsrc --keepParent "$package" "$archive"
shasum -a 256 "$archive" > "$archive.sha256"
echo "created $archive and checksum"
