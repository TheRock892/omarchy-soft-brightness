#!/bin/sh
# Builds and installs omarchy-soft-brightness for the current user.
# No root needed: everything runs as a normal Wayland client, installed
# under ~/.local/bin (no system-wide files, no systemd unit).
#
# Usage: sh install.sh
# Uninstall: rm ~/.local/bin/omarchy-soft-brightness ~/.local/bin/omarchy-soft-brightnessd

set -e

cd "$(dirname "$0")"

make

mkdir -p "$HOME/.local/bin"
install -m 755 omarchy-soft-brightnessd "$HOME/.local/bin/omarchy-soft-brightnessd"
install -m 755 omarchy-soft-brightness "$HOME/.local/bin/omarchy-soft-brightness"

echo "Installato in ~/.local/bin/omarchy-soft-brightness"
echo "Verifica che ~/.local/bin sia nel tuo PATH, poi prova:"
echo "  omarchy-soft-brightness status"
echo "  omarchy-soft-brightness 50%"
