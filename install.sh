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
# adds the invoking user to the 'video' group, configures the wired
# interface so a direct tether works, and installs + enables the systemd
# service.
#
# Pass --no-network to skip the networking step.
#
# Safe to re-run: every step here is idempotent (apt install of an
# installed package, usermod -aG on an existing membership, systemctl
# enable on an already-enabled unit are all no-ops).

set -euo pipefail

log() { printf '\n==> %s\n' "$1"; }
warn() { printf 'WARNING: %s\n' "$1" >&2; }
die() { printf 'ERROR: %s\n' "$1" >&2; exit 1; }

trap 'die "Install failed at line $LINENO. Nothing after that point ran; it is safe to fix the issue and re-run this script."' ERR

SETUP_NETWORK=1
for arg in "$@"; do
  case "$arg" in
    --no-network) SETUP_NETWORK=0 ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) die "Unknown option: $arg (try --help)" ;;
  esac
done

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
if ! apt-get install -y build-essential pkg-config \
    libturbojpeg0-dev v4l2loopback-dkms; then
  die "Package install failed. If it was specifically v4l2loopback-dkms \
that failed to build, you're most likely missing kernel headers for your \
running kernel ($(uname -r)) -- try:
  sudo apt install raspberrypi-kernel-headers
(or, on a non-RPi-branded kernel: sudo apt install linux-headers-\$(uname -r))
then re-run this script."
fi

log "Building electric_eye in $SCRIPT_DIR"
make -C "$SCRIPT_DIR" BUILDDIR="$BUILD_DIR" -j"$(nproc)"

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

# --- Wired networking for the tether ---------------------------------
#
# The drone and the topside machine are joined by a direct Ethernet cable:
# no router, no DHCP server, no DNS. NetworkManager's default wired profile
# is ipv4=auto/ipv6=auto, and on such a cable BOTH fail -- DHCP has nothing
# to answer it, and IPv6 "auto" waits for a router advertisement that never
# arrives. NetworkManager needs at least one address family to succeed, so
# it tears the connection down, removing the address, and retries. The
# result is the interface flapping in and out of "connected" with outages
# of a minute or more, while the physical link never drops at all.
#
# The fix is one setting: ipv6.method=link-local. A link-local address
# needs no server, so that family always succeeds, the connection stays
# activated, and the address persists. IPv4 is deliberately left on DHCP --
# it keeps working normally on a bench LAN, and is simply allowed to fail
# on a bare cable (ipv4.may-fail defaults to yes).
#
# That link-local address is exactly what tools/eeye-net discovers over.
setup_network() {
  if ! command -v nmcli >/dev/null 2>&1; then
    warn "NetworkManager not found, so the wired interface was left alone. \
On Raspberry Pi OS Bullseye and older this machine probably uses dhcpcd \
instead. The tether needs the wired interface to keep an IPv6 link-local \
address when no DHCP server answers; configure that by hand, or re-run \
with --no-network to silence this."
    return 0
  fi
  if ! nmcli -t general status >/dev/null 2>&1; then
    warn "NetworkManager is installed but not responding, so the wired \
interface was left alone. Start it (systemctl enable --now NetworkManager) \
and re-run this script."
    return 0
  fi

  # Work on connection PROFILES, not devices. A device reports only the
  # connection currently active on it, so keying off that would create a
  # duplicate profile every time this ran with the cable unplugged -- and
  # NetworkManager happily allows duplicate names.
  local profiles
  profiles=$(nmcli -t -f NAME,TYPE connection show 2>/dev/null \
             | awk -F: '$2 == "802-3-ethernet" { print $1 }')

  if [ -z "$profiles" ]; then
    # No wired profile at all (a Pi that has never had a cable plugged in).
    # Deliberately not pinned to an interface name: a profile bound to one
    # silently stops applying the day a USB adapter is used instead.
    log "No wired profile exists; creating 'eeye-tether'"
    if nmcli connection add type ethernet con-name eeye-tether \
         ipv4.method auto ipv6.method link-local \
         connection.autoconnect yes connection.autoconnect-priority 50 \
         >/dev/null 2>&1; then
      profiles="eeye-tether"
    else
      warn "Could not create a wired profile. Set one up by hand: \
nmcli connection add type ethernet con-name eeye-tether ifname '*' \
ipv4.method auto ipv6.method link-local"
      return 0
    fi
  fi

  # Fix every wired profile, so whichever one activates on the tether
  # behaves. Modifying is idempotent, which keeps re-running this script
  # safe.
  local name
  while IFS= read -r name; do
    [ -n "$name" ] || continue
    if nmcli connection modify "$name" ipv6.method link-local >/dev/null 2>&1; then
      log "Wired profile '$name': ipv6.method set to link-local"
    else
      warn "Could not modify wired profile '$name'. Set it by hand: \
nmcli connection modify '$name' ipv6.method link-local"
    fi
  done <<< "$profiles"

  # Activate only where nothing is already up. Reactivating a live
  # connection drops it, and this script is very often run over SSH --
  # quite possibly over the very interface being changed.
  local dev state
  while IFS= read -r dev; do
    [ -n "$dev" ] || continue
    state=$(nmcli -t -f GENERAL.STATE device show "$dev" 2>/dev/null | cut -d: -f2-)
    # Matched on NetworkManager's numeric state, not its words: the string
    # "disconnected" contains "connected", so a glob for the latter also
    # catches the former and would skip exactly the devices worth bringing
    # up. 100 = connected, 30 = disconnected, 20 = unavailable (no cable).
    case "$state" in
      100*)
        warn "$dev is already connected, so it was not restarted. The change \
applies on the next reboot or cable replug. Reactivating it now would drop \
that connection, which is a bad idea if you are currently on it."
        ;;
      20*)
        log "$dev has no cable; the profile will apply when one is plugged in"
        ;;
      *)
        if nmcli device connect "$dev" >/dev/null 2>&1; then
          log "$dev activated"
        else
          warn "$dev is configured but would not activate. It should come up \
on its own once a cable is connected."
        fi
        ;;
    esac
  done <<< "$(nmcli -t -f DEVICE,TYPE,STATE device status 2>/dev/null \
              | awk -F: '$2 == "ethernet" && $3 != "unmanaged" { print $1 }')"
}

if [ "$SETUP_NETWORK" -eq 1 ]; then
  log "Configuring wired networking for the tether"
  setup_network
else
  log "Skipping tether networking (--no-network)"
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
  Tether:  the wired interface now keeps an IPv6 link-local address even
           with no DHCP server, which is what topside discovery uses.
           Run the same setup on the TOPSIDE machine -- 'tools/eeye-net fix'
           prints the command for it.

If you weren't already in the 'video' group before this install, log out
and back in (or reboot) before running 'eeye' by hand from a terminal --
the systemd service itself doesn't need that, since it picks up group
membership fresh on every start.
EOF
