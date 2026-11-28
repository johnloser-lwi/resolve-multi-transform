#!/usr/bin/env bash
# Install the macOS build into /Library/OFX/Plugins, where DaVinci Resolve
# looks for OFX plugins.
#
#   scripts/install-mac.sh            installs build-mac/Release
#   scripts/install-mac.sh Debug
#
# Build first with scripts/build-mac.sh. Restart Resolve afterwards: it scans
# for plugins only at launch.
#
# Needs your password (sudo) -- unless the installed bundle already belongs to
# you, in which case it is updated in place with no prompt. To set that up once:
#
#   sudo chown -R "$(id -un)":staff /Library/OFX/Plugins/MultiTransform.ofx.bundle
#
# Only this plugin's own folder changes hands; /Library/OFX/Plugins and every
# other plugin in it stay as they are.

set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
config="${1:-Release}"
src="$repo/build-mac/$config/MultiTransform.ofx.bundle"
dest="/Library/OFX/Plugins"
name="MultiTransform.ofx.bundle"
target="$dest/$name"

[ -d "$src" ] || { echo "No build at $src -- run scripts/build-mac.sh first" >&2; exit 1; }

if pgrep -x Resolve >/dev/null; then
    echo "DaVinci Resolve is running. Quit it first, then run this again." >&2
    exit 1
fi

# Writable by you, and nothing inside it that is not: update in place, no sudo.
if [ -d "$target" ] && [ -w "$target" ] && [ -z "$(find "$target" ! -user "$(id -un)" -print -quit)" ]; then
    run=()
    # Replace the contents, not the folder: the folder itself sits in a
    # root-owned directory, so it could not be recreated without sudo.
    rsync -a --delete "$src/" "$target/"
else
    run=(sudo)
    sudo mkdir -p "$dest"
    sudo rm -rf "${target:?}"
    sudo cp -R "$src" "$dest/"
fi

# A bundle copied from elsewhere can carry the quarantine flag, which makes
# Gatekeeper refuse to load it inside Resolve with no visible error. Apple
# Silicon also needs a signature; ad-hoc is enough on this machine.
${run[@]+"${run[@]}"} xattr -dr com.apple.quarantine "$target" 2>/dev/null || true
${run[@]+"${run[@]}"} codesign --force --sign - "$target"

echo "Installed: $target"
echo "Start DaVinci Resolve."
