#!/bin/sh
set -eu

platform=${1:?usage: sidportal-test linux|windows}
artifact_dir=/artifacts
testdata=/testdata

case "$platform" in
    linux)
        archive=SidPortal-Linux-x86_64.zip
        hostcheck="$artifact_dir/sidportal_vst3_hostcheck"
        package=SidPortal-Linux-x86_64
        ;;
    windows)
        archive=SidPortal-Windows-x64.zip
        hostcheck="$artifact_dir/sidportal_vst3_hostcheck.exe"
        package=SidPortal-Windows-x64
        ;;
    *) echo "unknown platform: $platform" >&2; exit 2 ;;
esac

test -f "$artifact_dir/$archive"
test -f "$artifact_dir/$archive.sha256"
test -f "$hostcheck"
(cd "$artifact_dir" && sha256sum -c "$archive.sha256")

unpacked=$(mktemp -d /tmp/sidportal-artifact.XXXXXX)
trap 'rm -rf "$unpacked"' EXIT HUP INT TERM
unzip -q "$artifact_dir/$archive" -d "$unpacked"
root="$unpacked/$package"
plugin="$root/SidPortal.vst3"
test -d "$plugin"

cat > /tmp/sidportal-asound.conf <<'EOF'
pcm.!default { type null }
EOF
export ALSA_CONFIG_PATH=/tmp/sidportal-asound.conf

rom_path=
preset_path=
if [ -f "$testdata/roms/firmware.bin" ]; then
    rom_path="$testdata/roms/firmware.bin"
    preset_path=$(find "$testdata/patches" -maxdepth 1 -type f -iname '*Presets*.syx' -print -quit 2>/dev/null || true)
else
    echo "No local firmware found; VST3 audio will use its load/callback smoke check only."
fi

if [ "$platform" = linux ]; then
    test -x "$root/SidPortal"
    test -x "$root/sidportal"
    test ! -e "$root/tools/gui.py"
    export HOME=/tmp/sidportal-home
    export SIDPORTAL_DATA_DIR=/tmp/sidportal-first-run-data
    rm -rf "$SIDPORTAL_DATA_DIR"
    mkdir -p "$HOME"

    xvfb-run -a sh -c '
        "$1" >/tmp/sidportal-linux-gui.log 2>&1 &
        app=$!
        found=
        picker=
        for attempt in $(seq 1 30); do
            sleep 1
            if ! kill -0 "$app" 2>/dev/null; then break; fi
            tree=$(xwininfo -root -tree 2>/dev/null || true)
            echo "$tree" | grep -qi sidportal && found=yes
            echo "$tree" | grep -Fqi "Select SidStation OS r34 firmware" && picker=yes
            if [ -n "$found" ] && [ -n "$picker" ]; then break; fi
        done
        if [ -z "$found" ] || [ -z "$picker" ]; then
            cat /tmp/sidportal-linux-gui.log
            xwininfo -root -tree 2>/dev/null || true
            kill "$app" 2>/dev/null || true
            wait "$app" 2>/dev/null || true
            echo "SidPortal standalone did not show its first-launch firmware picker" >&2
            exit 1
        fi
        kill -TERM "$app" 2>/dev/null || true
        wait "$app" 2>/dev/null || true
    ' sh "$root/SidPortal"

    if [ -n "$rom_path" ]; then
        export SIDPORTAL_ROM="$rom_path"
        if [ -n "$preset_path" ]; then export SIDPORTAL_PRESETS="$preset_path"; fi
        set --
    else
        set -- --load-only
    fi
    xvfb-run -a "$hostcheck" "$plugin" "$@"
else
    export WINEPREFIX=/tmp/sidportal-wine
    export WINEDEBUG=-all
    export HOME=/tmp/sidportal-home
    rm -rf "$WINEPREFIX"
    mkdir -p "$WINEPREFIX" "$HOME"
    unset SIDPORTAL_ROM SIDPORTAL_PRESETS
    to_win_path() { printf 'Z:%s\n' "$(printf '%s' "$1" | sed 's,/,\\,g')"; }
    export SIDPORTAL_DATA_DIR=$(to_win_path /tmp/sidportal-first-run-data)
    mkdir -p /tmp/sidportal-first-run-data

    xvfb-run -a sh -c '
        wine64 "$1" >/tmp/sidportal-windows-gui.log 2>&1 &
        app=$!
        found=
        picker=
        for attempt in $(seq 1 30); do
            sleep 1
            tree=$(xwininfo -root -tree 2>/dev/null || true)
            echo "$tree" | grep -qi sidportal && found=yes
            echo "$tree" | grep -Fqi "Select SidStation OS r34 firmware" && picker=yes
            if [ -n "$found" ] && [ -n "$picker" ]; then break; fi
            if ! kill -0 "$app" 2>/dev/null && [ -z "$found" ]; then break; fi
        done
        if [ -z "$found" ] || [ -z "$picker" ]; then
            cat /tmp/sidportal-windows-gui.log
            xwininfo -root -tree 2>/dev/null || true
            wineserver -k 2>/dev/null || true
            echo "SidPortal Windows standalone did not show its first-launch firmware picker under Wine" >&2
            exit 1
        fi
        wineserver -k 2>/dev/null || true
    ' sh "$root/SidPortal.exe"

    plugin_win=$(to_win_path "$plugin")
    if [ -n "$rom_path" ]; then
        export SIDPORTAL_ROM=$(to_win_path "$rom_path")
        if [ -n "$preset_path" ]; then export SIDPORTAL_PRESETS=$(to_win_path "$preset_path"); fi
        set --
    else
        set -- --load-only
    fi
    xvfb-run -a wine64 "$hostcheck" "$plugin_win" "$@"
fi

echo "$platform CI artifact smoke checks passed."
