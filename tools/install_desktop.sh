#!/bin/sh
# Add SidPortal to the desktop's application menu: a .desktop entry that starts the
# panel from this checkout, and the icon (tools/assets/icons) in the hicolor theme.
#
#   tools/install_desktop.sh
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
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
Exec=python3 $here/tools/gui.py
Path=$here
Icon=sidportal
Terminal=false
Categories=AudioVideo;Audio;
StartupNotify=true
StartupWMClass=Sidportal
ENTRY
update-desktop-database "$data/applications" 2>/dev/null || true
echo "installed $data/applications/sidportal.desktop"
