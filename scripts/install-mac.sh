#!/usr/bin/env bash
# Install the macOS build into /Library/OFX/Plugins, where DaVinci Resolve
# looks for OFX plugins. Asks for your password (sudo) for the copy.
#
#   scripts/install-mac.sh            installs build-mac/Release
#   scripts/install-mac.sh Debug
#
# Build first with scripts/build-mac.sh. Restart Resolve afterwards: it scans
# for plugins only at launch.

set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
config="${1:-Release}"
src="$repo/build-mac/$config/MultiTransform.ofx.bundle"
dest="/Library/OFX/Plugins"
name="MultiTransform.ofx.bundle"

[ -d "$src" ] || { echo "No build at $src -- run scripts/build-mac.sh first" >&2; exit 1; }

if pgrep -x Resolve >/dev/null; then
    echo "DaVinci Resolve is running. Quit it first, then run this again." >&2
    exit 1
fi

sudo mkdir -p "$dest"
sudo rm -rf "${dest:?}/$name"
sudo cp -R "$src" "$dest/"

# A bundle copied from elsewhere can carry the quarantine flag, which makes
# Gatekeeper refuse to load it inside Resolve with no visible error. Apple
# Silicon also needs a signature; ad-hoc is enough on this machine.
sudo xattr -dr com.apple.quarantine "$dest/$name" 2>/dev/null || true
sudo codesign --force --sign - "$dest/$name"

echo "Installed: $dest/$name"
echo "Start DaVinci Resolve."
