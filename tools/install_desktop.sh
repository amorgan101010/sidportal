#!/bin/sh
# Add the JUCE standalone to the desktop's application menu.
#
#   tools/install_desktop.sh [path/to/SidPortal]
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
standalone=${1:-"$here/build/SidPortalPlugin_artefacts/Release/Standalone/SidPortal"}
if [ ! -x "$standalone" ]; then
    echo "JUCE standalone not found: $standalone" >&2
    echo "Build it or pass its executable path to tools/install_desktop.sh." >&2
    exit 1
fi
standalone_dir=$(cd "$(dirname "$standalone")" && pwd)
standalone="$standalone_dir/$(basename "$standalone")"
data=${XDG_DATA_HOME:-$HOME/.local/share}
theme=$data/icons/hicolor
for png in "$here"/tools/assets/icons/sidportal-*.png; do
    size=$(basename "$png" .png | sed 's/.*-//')
    mkdir -p "$theme/${size}x$size/apps"
    cp "$png" "$theme/${size}x$size/apps/sidportal.png"
done
gtk-update-icon-cache -f -t "$theme" 2>/dev/null || true
mkdir -p "$data/applications"
cat > "$data/applications/sidportal.desktop" <<ENTRY
[Desktop Entry]
Type=Application
Name=SidPortal
Comment=SID synthesizer emulator
Exec=$standalone
Path=$standalone_dir
Icon=sidportal
Terminal=false
Categories=AudioVideo;Audio;
StartupNotify=true
StartupWMClass=SidPortal
ENTRY
update-desktop-database "$data/applications" 2>/dev/null || true
echo "installed $data/applications/sidportal.desktop for $standalone"
