#!/usr/bin/env bash
set -euo pipefail

RULE_SRC="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/99-teensy-ota.rules"
RULE_DST="/etc/udev/rules.d/99-teensy-ota.rules"

if [[ ${EUID} -ne 0 ]]; then
  echo "Installing Teensy udev rules requires sudo/root." >&2
  exec sudo "$0" "$@"
fi

install -m 0644 "$RULE_SRC" "$RULE_DST"
udevadm control --reload-rules
udevadm trigger --attr-match=idVendor=16c0 || true
udevadm settle || true

# Ensure the normal serial node is usable by future shells even without uaccess.
if getent passwd taha >/dev/null && getent group dialout >/dev/null; then
  usermod -aG dialout taha || true
fi

cat <<'MSG'
Installed /etc/udev/rules.d/99-teensy-ota.rules and reloaded udev.
Unplug/replug the Teensy or press its program button if permissions do not update immediately.
If your current shell was opened before dialout membership existed, open a new shell or run: newgrp dialout
MSG
