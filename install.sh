#!/bin/sh
# Install lsend on the CardputerZero (Raspberry Pi OS). Run from this folder.
#   sh install.sh            -> installs /usr/local/bin/lsend
#   sh install.sh --service  -> also runs a background receiver at boot
set -e
sudo install -m 755 lsend.py /usr/local/bin/lsend
echo "Installed /usr/local/bin/lsend"

# Create the HTTPS cert now so first launch is instant
lsend scan >/dev/null 2>&1 || true

if [ "$1" = "--service" ]; then
  U="$(id -un)"
  sed "s/__USER__/$U/" lsend.service | sudo tee /etc/systemd/system/lsend.service >/dev/null
  sudo systemctl daemon-reload
  sudo systemctl enable --now lsend
  echo "Receiver service running as $U (files -> ~/Downloads/LocalSend)"
  echo "Note: stop it (sudo systemctl stop lsend) before using the TUI on port 53317."
fi
