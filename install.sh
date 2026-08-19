#!/usr/bin/env bash
#
# Electric Eye installer.
#
# Usage:
#   git clone <this repo>
#   cd electric-eye
#   sudo ./install.sh
#
# Installs build/runtime dependencies, builds the project in place (this
# checkout is the install -- there is no separate copy-to-/opt step, so
# `git pull && sudo ./install.sh` is also the update path), configures
# v4l2loopback to load automatically on every boot with the right options,
# adds the invoking user to the 'video' group, and installs + enables the
# systemd service.
#
# Safe to re-run: every step here is idempotent (apt install of an
# installed package, usermod -aG on an existing membership, systemctl
# enable on an already-enabled unit are all no-ops).

set -euo pipefail

log() { printf '\n==> %s\n' "$1"; }
warn() { printf 'WARNING: %s\n' "$1" >&2; }
die() { printf 'ERROR: %s\n' "$1" >&2; exit 1; }

trap 'die "Install failed at line $LINENO. Nothing after that point ran; it is safe to fix the issue and re-run this script."' ERR

if [ "$(id -u)" -ne 0 ]; then
  die "This needs root, for package installs, /etc/modules-load.d, /etc/modprobe.d, and the systemd unit. Run: sudo $0"
fi

# The user to add to the 'video' group and to report status back to.
# $SUDO_USER is unset if this was run as root directly (su, or already root)
# rather than via sudo -- in that case there is no clear "which user" to
# pick, so that step is skipped further down with a warning instead of
# guessing.
TARGET_USER="${SUDO_USER:-}"

command -v apt-get >/dev/null 2>&1 || die "apt-get not found. This installer targets Raspberry Pi OS / Debian / Ubuntu; on anything else, follow the manual steps in eeye.c's setup comment and eeye.service instead."

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/builddir"

if command -v mokutil >/dev/null 2>&1 && mokutil --sb-state 2>/dev/null | grep -q "SecureBoot enabled"; then
  warn "Secure Boot is on. Installing v4l2loopback-dkms below may pop up a \
one-time MOK enrollment password prompt -- don't skip it. Afterward you \
MUST reboot: a blue 'MOK Manager' screen will appear during boot asking \
for that same password to actually enroll the signing key. Skip either \
step and the module builds fine but the kernel refuses to load it, which \
looks exactly like a permissions error later ('modprobe: could not \
insert ... Operation not permitted') rather than what it actually is. If \
you miss the prompt, check 'sudo mokutil --list-new' -- non-empty output \
means enrollment is still pending, and rebooting will show the MOK \
Manager screen again."
fi

log "Installing build and runtime dependencies"
apt-get update
if ! apt-get install -y build-essential meson ninja-build pkg-config \
    libturbojpeg0-dev v4l2loopback-dkms; then
  die "Package install failed. If it was specifically v4l2loopback-dkms \
that failed to build, you're most likely missing kernel headers for your \
running kernel ($(uname -r)) -- try:
  sudo apt install raspberrypi-kernel-headers
(or, on a non-RPi-branded kernel: sudo apt install linux-headers-\$(uname -r))
then re-run this script."
fi

log "Building electric_eye in $SCRIPT_DIR"
if [ -d "$BUILD_DIR" ]; then
  meson setup --reconfigure "$BUILD_DIR" "$SCRIPT_DIR"
else
  meson setup "$BUILD_DIR" "$SCRIPT_DIR"
fi
meson compile -C "$BUILD_DIR"

EXE_PATH="$BUILD_DIR/eeye"
[ -x "$EXE_PATH" ] || die "Build succeeded but $EXE_PATH is missing -- unexpected, please report this."

log "Configuring v4l2loopback to load automatically on boot"
cat > /etc/modules-load.d/v4l2loopback.conf <<'EOF'
v4l2loopback
EOF
cat > /etc/modprobe.d/v4l2loopback.conf <<'EOF'
options v4l2loopback video_nr=10 card_label="VirtualCam" exclusive_caps=1
EOF

if lsmod | grep -q '^v4l2loopback'; then
  warn "v4l2loopback is already loaded. modprobe cannot change a loaded \
module's parameters -- if it was loaded with different options before \
this install (a different video_nr, for instance), reboot or manually \
'rmmod v4l2loopback' and 'modprobe v4l2loopback' to pick up the new ones."
else
  modprobe v4l2loopback video_nr=10 card_label="VirtualCam" exclusive_caps=1
fi

if [ -n "$TARGET_USER" ]; then
  log "Adding $TARGET_USER to the 'video' group"
  usermod -aG video "$TARGET_USER"
else
  warn "Could not determine which user to add to the 'video' group (not \
run via sudo?). Do it manually: sudo usermod -aG video <your-user>"
fi

log "Installing the systemd service"
sed \
  -e "s#/opt/electric-eye/builddir/eeye#$EXE_PATH#" \
  -e "s#/opt/electric-eye/eeye_config.json#$SCRIPT_DIR/eeye_config.json#" \
  -e "s#WorkingDirectory=/opt/electric-eye#WorkingDirectory=$SCRIPT_DIR#" \
  -e "s/^User=pi/User=${TARGET_USER:-root}/" \
  "$SCRIPT_DIR/eeye.service" > /etc/systemd/system/eeye.service

systemctl daemon-reload
systemctl enable --now eeye

log "Done"
cat <<EOF

Electric Eye is installed and running as a systemd service.

  Status:  systemctl status eeye
  Logs:    journalctl -u eeye -f
  Config:  edit $SCRIPT_DIR/eeye_config.json -- changes apply live, no restart needed
  Camera:  point another app at /dev/video10 to view the effects feed

If you weren't already in the 'video' group before this install, log out
and back in (or reboot) before running 'eeye' by hand from a terminal --
the systemd service itself doesn't need that, since it picks up group
membership fresh on every start.
EOF
