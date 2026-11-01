#!/usr/bin/env bash
#
# electric-eye installer: eeye and hellion as systemd services.
#
# Usage:
#   sudo ./install.sh --role rov|topside --placement a|b [options]
#
#   --role rov|topside   which end of the tether this machine is
#   --placement a|b      where eeye (the effects) runs:
#                          a = topside: the ROV sends its camera up, effects
#                              are applied topside
#                          b = on the ROV: the ROV sends the pilot's picture
#                              up (where autonomy will run)
#                        Both ends must be installed with the same placement.
#   --tether IFACE       topside only: give IFACE the BlueROV topside address,
#                        192.168.2.1/24 (or --topside-ip's), with
#                        NetworkManager. Without it the network is left alone.
#
#   The two ends find each other on the network by themselves (hellion's
#   discovery: see docs/hellion.md), so no addresses are needed. To pin
#   them instead -- discovery cannot reach across a router, say:
#   --topside-ip IP      the topside machine's address, which the ROV sends to
#   --rov-ip IP          the ROV's, which the topside talks to
#                        Written into /etc/hellion/hellion.json (the old one
#                        kept as .bak) and remembered for later runs. "auto"
#                        un-pins a remembered one (installs before 0.3.0
#                        pinned theirs): --topside-ip auto --rov-ip auto.
#   --pair NAME          only pair with a hellion installed with the same NAME
#                        (default "default"). Give each vehicle its own when
#                        several share a network. Not a password: anyone on
#                        the network can use any name.
#   --reset-config       replace /etc/hellion/hellion.json (and eeye's
#                        effects config) with fresh ones; the old files are
#                        kept as *.bak
#   --no-packages        skip installing packages
#
# Re-running is the update path: `git pull && sudo ./install.sh`. The role
# and placement are remembered, existing configs are kept, and the services
# are restarted on the new binaries.
#
# What runs where:
#                 ROV                         topside
#   placement a   hellion (camera -> RTP)     hellion (RTP -> bus "camera",
#                                               bus "pilot" -> QGC, loopback),
#                                               eeye (bus "camera" -> "pilot")
#   placement b   eeye (camera -> "pilot"),   hellion (RTP -> QGC, bus
#                 hellion ("pilot" -> RTP)      "pilot", loopback)

set -euo pipefail

log() { printf '\n==> %s\n' "$1"; }
warn() { printf 'WARNING: %s\n' "$1" >&2; }
die() { printf 'ERROR: %s\n' "$1" >&2; exit 1; }

trap 'die "Install failed at line $LINENO. Nothing after that point ran; it is safe to fix the issue and re-run this script."' ERR

ORIG_ARGS="$*"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STATE_FILE=/etc/hellion/install.conf

ROLE=""
PLACEMENT=""
TETHER_IF=""
TOPSIDE_IP=""
ROV_IP=""
PAIR=""
RESET_CONFIG=0
INSTALL_PACKAGES=1
# The value after an option, or a clear error when there is none.
need_value() { [ $# -ge 2 ] && [ -n "$2" ] || die "$1 needs a value (try --help)"; }
while [ $# -gt 0 ]; do
  case "$1" in
    --role) need_value "$@"; ROLE="$2"; shift 2 ;;
    --role=*) ROLE="${1#*=}"; shift ;;
    --placement) need_value "$@"; PLACEMENT="$2"; shift 2 ;;
    --placement=*) PLACEMENT="${1#*=}"; shift ;;
    --tether) need_value "$@"; TETHER_IF="$2"; shift 2 ;;
    --tether=*) TETHER_IF="${1#*=}"; shift ;;
    --topside-ip) need_value "$@"; TOPSIDE_IP="$2"; shift 2 ;;
    --topside-ip=*) TOPSIDE_IP="${1#*=}"; shift ;;
    --rov-ip) need_value "$@"; ROV_IP="$2"; shift 2 ;;
    --rov-ip=*) ROV_IP="${1#*=}"; shift ;;
    --pair) need_value "$@"; PAIR="$2"; shift 2 ;;
    --pair=*) PAIR="${1#*=}"; shift ;;
    --reset-config) RESET_CONFIG=1; shift ;;
    --no-packages) INSTALL_PACKAGES=0; shift ;;
    # The whole header comment, however long it grows.
    -h|--help) awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"; exit 0 ;;
    *) die "Unknown option: $1 (try --help)" ;;
  esac
done

# A re-run reuses the last install's choices for whatever it is not given.
# Read as data, not sourced: it is only ever KEY=value lines.
# Addresses or a pair given now rewrite hellion.json (see put_config).
ADDRS_GIVEN=0
[ -n "$TOPSIDE_IP$ROV_IP$PAIR" ] && ADDRS_GIVEN=1
if [ -r "$STATE_FILE" ]; then
  prev() { sed -n "s/^$1=//p" "$STATE_FILE" | head -1; }
  if [ -z "$ROLE$PLACEMENT" ]; then
    ROLE=$(prev ROLE)
    PLACEMENT=$(prev PLACEMENT)
    log "Using the previous install's choices: --role $ROLE --placement $PLACEMENT"
  fi
  [ -n "$TOPSIDE_IP" ] || TOPSIDE_IP=$(prev TOPSIDE_IP)
  [ -n "$ROV_IP" ] || ROV_IP=$(prev ROV_IP)
  [ -n "$PAIR" ] || PAIR=$(prev PAIR)
fi
# "auto" un-pins an address remembered from an earlier run (installs from
# before discovery all pinned theirs): found by discovery again.
[ "$TOPSIDE_IP" = auto ] && TOPSIDE_IP=""
[ "$ROV_IP" = auto ] && ROV_IP=""
# No addresses: the ends find each other (discovery). Given, they pin it.
for ip in "$TOPSIDE_IP" "$ROV_IP"; do
  [ -z "$ip" ] && continue
  echo "$ip" | grep -Eq '^([0-9]{1,3}\.){3}[0-9]{1,3}$' ||
    die "\"$ip\" is not an IPv4 address like 192.168.2.1 (hostnames do not \
work here: the release binaries are static and cannot look them up)"
done
[ -z "$TOPSIDE_IP" ] || [ "$TOPSIDE_IP" != "$ROV_IP" ] ||
  die "--topside-ip and --rov-ip are the same address"
PAIR=${PAIR:-default}
echo "$PAIR" | grep -Eq '^[A-Za-z0-9_-]{1,31}$' ||
  die "--pair must be a name: letters, digits, '-' and '_', up to 31"

case "$ROLE" in rov|topside) ;; *) die "--role must be rov or topside (try --help)" ;; esac
case "$PLACEMENT" in a|b) ;; *) die "--placement must be a or b (try --help)" ;; esac
if [ -n "$TETHER_IF" ] && [ "$ROLE" != topside ]; then
  die "--tether is for the topside machine. On a BlueROV the ROV's network belongs to BlueOS; leave it alone."
fi
# Checked before anything is changed: a name that is not an interface here
# would otherwise get a profile that never applies (and used to abort the
# install halfway, after the units were written but before they started).
if [ -n "$TETHER_IF" ] && [ ! -e "/sys/class/net/$TETHER_IF" ]; then
  die "--tether $TETHER_IF: no such network interface on this machine. It has: \
$(ls /sys/class/net | grep -vx lo | tr '\n' ' ')"
fi

if [ "$(id -u)" -ne 0 ]; then
  die "This needs root (packages, a system user, /etc, systemd units). Run: sudo $0 $ORIG_ARGS"
fi

# eeye runs on the ROV in placement b and topside in placement a.
RUN_EEYE=0
{ [ "$ROLE" = rov ] && [ "$PLACEMENT" = b ]; } && RUN_EEYE=1
{ [ "$ROLE" = topside ] && [ "$PLACEMENT" = a ]; } && RUN_EEYE=1
# The loopback device (/dev/video10, for OBS, a browser, anything that
# takes a webcam) is written by hellion topside, in both placements.
NEED_LOOPBACK=0
[ "$ROLE" = topside ] && NEED_LOOPBACK=1

# The person who ran sudo: added to 'video' (for `hellion top`), and the
# build runs as them so their checkout does not fill with root-owned files.
TARGET_USER="${SUDO_USER:-}"
[ "$TARGET_USER" = root ] && TARGET_USER=""

# --- Packages ------------------------------------------------------------

PKGS="build-essential pkg-config libturbojpeg0-dev ffmpeg"
[ "$NEED_LOOPBACK" -eq 1 ] && PKGS="$PKGS v4l2loopback-dkms"

if [ "$INSTALL_PACKAGES" -eq 1 ]; then
  if command -v apt-get >/dev/null 2>&1; then
    if [ "$NEED_LOOPBACK" -eq 1 ] && command -v mokutil >/dev/null 2>&1 &&
       mokutil --sb-state 2>/dev/null | grep -q "SecureBoot enabled"; then
      warn "Secure Boot is on. Installing v4l2loopback-dkms may ask for a \
one-time MOK enrollment password -- don't skip it -- and then you MUST \
reboot and enter the same password on the blue 'MOK Manager' screen. Skip \
either step and the module builds but the kernel refuses to load it \
('modprobe: ... Operation not permitted'). 'sudo mokutil --list-new' \
non-empty means enrollment is still pending."
    fi
    log "Installing packages: $PKGS"
    apt-get update
    # shellcheck disable=SC2086
    if ! apt-get install -y $PKGS; then
      die "Package install failed. If v4l2loopback-dkms failed to build, the \
kernel headers for $(uname -r) are probably missing: \
'sudo apt install raspberrypi-kernel-headers' on a Pi, otherwise \
'sudo apt install linux-headers-\$(uname -r)'. Then re-run."
    fi
  else
    warn "No apt-get here, so packages were not installed. This needs a C \
compiler, make, pkg-config, libjpeg-turbo (turbojpeg) headers and ffmpeg\
$([ "$NEED_LOOPBACK" -eq 1 ] && echo ", plus the v4l2loopback kernel module"). \
Install them with your package manager, then re-run with --no-packages."
    INSTALL_PACKAGES=0
  fi
fi
command -v ffmpeg >/dev/null 2>&1 ||
  die "ffmpeg is not installed; hellion needs it for H.264 and RTP."

# --- Build ---------------------------------------------------------------

log "Building in $SCRIPT_DIR"
if [ -n "$TARGET_USER" ]; then
  sudo -u "$TARGET_USER" make -C "$SCRIPT_DIR" -j"$(nproc)" all
else
  make -C "$SCRIPT_DIR" -j"$(nproc)" all
fi
for exe in eeye hellion; do
  [ -x "$SCRIPT_DIR/builddir/$exe" ] ||
    die "The build finished but builddir/$exe is missing -- please report this."
done

# --- The service user ------------------------------------------------------
#
# One system user for both services: hellion writes eeye's config and reads
# its status beside it, and the two share the frame bus in /dev/shm. Its
# group is 'video' for the camera and the loopback device, and so that the
# bus and status files (mode 0660) are readable by people in 'video'.

log "Setting up the 'eeye' service user"
getent group video >/dev/null || groupadd --system video
if ! getent passwd eeye >/dev/null; then
  useradd --system --gid video --home-dir /var/lib/eeye --no-create-home \
          --shell "$(command -v nologin || echo /bin/false)" \
          --comment "electric-eye services" eeye
fi
WAS_IN_VIDEO=1
if [ -n "$TARGET_USER" ]; then
  id -nG "$TARGET_USER" | grep -qw video || WAS_IN_VIDEO=0
  usermod -aG video "$TARGET_USER"
else
  warn "Not run via sudo, so no login user was added to 'video'. Do it by \
hand for whoever will run 'hellion top': sudo usermod -aG video <user>"
fi

# Installed copies rather than paths into this checkout: a home directory is
# often 0700, which the service user cannot enter.
log "Installing eeye and hellion to /usr/local/bin"
install -m 0755 "$SCRIPT_DIR/builddir/eeye" "$SCRIPT_DIR/builddir/hellion" /usr/local/bin/

# --- v4l2loopback (topside) --------------------------------------------------

if [ "$NEED_LOOPBACK" -eq 1 ]; then
  log "Loading v4l2loopback (/dev/video10) now and on every boot"
  echo v4l2loopback > /etc/modules-load.d/v4l2loopback.conf
  echo 'options v4l2loopback video_nr=10 card_label="hellion" exclusive_caps=1' \
    > /etc/modprobe.d/v4l2loopback.conf
  if lsmod | grep -q '^v4l2loopback'; then
    [ -e /dev/video10 ] || warn "v4l2loopback is loaded, but with different \
options (there is no /dev/video10). Reboot, or 'sudo rmmod v4l2loopback && \
sudo modprobe v4l2loopback', to pick up the new ones."
  elif ! modprobe v4l2loopback; then
    warn "v4l2loopback would not load. hellion runs without it (its loopback \
sink retries); fix it to get /dev/video10. See the Secure Boot note above."
  fi
fi

# --- Configs -------------------------------------------------------------------

# Writes $2 to $1 unless $1 exists, which is kept -- except with
# --reset-config, or when $5 is 1 (new addresses were given), when it is
# replaced and the old one kept as .bak.
put_config() {
  local dest="$1" content="$2" owner="$3" mode="$4" force="${5:-0}"
  if [ -e "$dest" ] && [ "$(cat "$dest")" = "$content" ]; then
    log "$dest is up to date"
    return 0
  fi
  if [ -e "$dest" ] && [ "$RESET_CONFIG" -eq 0 ] && [ "$force" -eq 0 ]; then
    log "Keeping $dest (--reset-config replaces it)"
    return 0
  fi
  if [ -e "$dest" ]; then
    cp -p "$dest" "$dest.bak"
    log "Replacing $dest (the old one is $dest.bak)"
  else
    log "Writing $dest"
  fi
  printf '%s\n' "$content" > "$dest.new"
  chown "$owner" "$dest.new"
  chmod "$mode" "$dest.new"
  mv -f "$dest.new" "$dest"
}

install -d -m 0755 /etc/hellion
# The examples find the other end by discovery ("auto"); a pinned address
# replaces that, and a pair other than the default is added.
hellion_json() {
  local sedargs=()
  [ -n "$ROV_IP" ] && sedargs+=(-e "s/\"peer\": \"auto\"/\"peer\": \"$ROV_IP:8091\"/")
  [ -n "$TOPSIDE_IP" ] && sedargs+=(-e "s/rtp:auto:/rtp:$TOPSIDE_IP:/g")
  [ "$PAIR" != default ] &&
    sedargs+=(-e "s/^\(  \"role\": \"[a-z]*\",\)\$/\1\n  \"pair\": \"$PAIR\",/")
  sedargs+=(-e "")
  sed "${sedargs[@]}" "$SCRIPT_DIR/hellion/examples/$PLACEMENT-$ROLE.json"
}
put_config /etc/hellion/hellion.json "$(hellion_json)" root:root 0644 "$ADDRS_GIVEN"
if [ -e /etc/hellion/hellion.json ] &&
   ! grep -q "\"role\": *\"$ROLE\"" /etc/hellion/hellion.json; then
  warn "/etc/hellion/hellion.json is not a $ROLE config. It was kept; \
re-run with --reset-config to replace it with hellion/examples/$PLACEMENT-$ROLE.json."
fi

if [ "$RUN_EEYE" -eq 1 ]; then
  install -d -m 0775 -o eeye -g video /var/lib/eeye
  # Where the frames come from differs by placement; the loopback device is
  # hellion's (topside), so eeye writes none. In placement A, eeye's input
  # is already the camera's frame on the bus ("camera"), so republishing it
  # as "raw" would be a full-frame copy per frame of a stream that exists.
  if [ "$PLACEMENT" = a ]; then
    source_keys='  "capture_source": "bus",
  "capture_stream": "camera",
  "bus_raw": "",'
  else
    source_keys='  "capture_device": "/dev/video0",'
  fi
  put_config /var/lib/eeye/effects.json "{
  \"chain\": [
    {\"effect\": \"blur\"},
    {\"effect\": \"sobel\"}
  ],
  \"record_path\": \"\",
$source_keys
  \"output_device\": \"\"
}" eeye:video 0664
fi

# --- systemd units ---------------------------------------------------------------

log "Installing the systemd units"
install -m 0644 "$SCRIPT_DIR/hellion/hellion.service" /etc/systemd/system/hellion.service
if [ "$RUN_EEYE" -eq 1 ]; then
  install -m 0644 "$SCRIPT_DIR/eeye.service" /etc/systemd/system/eeye.service
elif [ -e /etc/systemd/system/eeye.service ]; then
  # An earlier install ran eeye here. In this placement it must not: on the
  # ROV in placement a it would hold the camera hellion needs.
  log "Removing eeye.service: in placement $PLACEMENT eeye does not run on the $ROLE"
  systemctl disable --now eeye >/dev/null 2>&1 || true
  rm -f /etc/systemd/system/eeye.service
fi
systemctl daemon-reload

printf 'ROLE=%s\nPLACEMENT=%s\nTOPSIDE_IP=%s\nROV_IP=%s\nPAIR=%s\n' "$ROLE" \
  "$PLACEMENT" "$TOPSIDE_IP" "$ROV_IP" "$PAIR" > "$STATE_FILE"

# --- Tether network (topside, only when asked) -----------------------------------
#
# BlueROV addressing: topside 192.168.2.1, ROV 192.168.2.2, both /24, set by
# hand (there is no DHCP server on a tether). ipv6 link-local as well, which
# needs no server either: with every address family satisfied,
# NetworkManager keeps the connection up instead of tearing it down to
# retry a DHCP that will never answer.
setup_tether() {
  local ifname="$1"
  if ! command -v nmcli >/dev/null 2>&1 || ! nmcli -t general status >/dev/null 2>&1; then
    warn "NetworkManager is not available, so $ifname was left alone. Give \
it ${TOPSIDE_IP:-192.168.2.1}/24 by hand (with your distro's network config, so it survives \
a reboot)."
    return 0
  fi
  if nmcli -t -f NAME connection show | grep -qx hellion-tether; then
    nmcli connection modify hellion-tether connection.interface-name "$ifname" \
      ipv4.method manual ipv4.addresses "${TOPSIDE_IP:-192.168.2.1}/24" ipv6.method link-local
  else
    nmcli connection add type ethernet con-name hellion-tether ifname "$ifname" \
      ipv4.method manual ipv4.addresses "${TOPSIDE_IP:-192.168.2.1}/24" ipv6.method link-local \
      connection.autoconnect yes connection.autoconnect-priority 50 >/dev/null
  fi
  log "NetworkManager profile 'hellion-tether': $ifname = ${TOPSIDE_IP:-192.168.2.1}/24"
  # The profile outranks the interface's other profiles (priority 50), so
  # on the next connect it wins. Right for a port that is the tether; wrong
  # for one that is also the machine's LAN.
  if ip -4 -o addr show dev "$ifname" 2>/dev/null | grep -q 'inet ' &&
     ! ip -4 -o addr show dev "$ifname" | grep -q 'inet 192\.168\.2\.'; then
    warn "$ifname has another address right now ($(ip -4 -o addr show dev "$ifname" | \
awk '{print $4}' | head -1)). From its next reconnect it will be the tether \
(192.168.2.1, no DHCP), not that network. If $ifname is your LAN or internet \
port, undo it: sudo nmcli connection delete hellion-tether"
  fi
  # Activating drops whatever is up on that interface now, which may be
  # the very connection this script is running over.
  local state
  state=$(nmcli -t -f GENERAL.STATE device show "$ifname" 2>/dev/null | cut -d: -f2-) || state=""
  case "$state" in
    100*) warn "$ifname is connected now, so the new profile was not \
activated (that could cut off this session). It applies on the next replug \
or reboot, or now with: nmcli connection up hellion-tether" ;;
    *) nmcli connection up hellion-tether >/dev/null 2>&1 ||
         log "$ifname is not up yet; the profile applies when the tether is plugged in" ;;
  esac
}
[ -n "$TETHER_IF" ] && setup_tether "$TETHER_IF"

# --- Start ---------------------------------------------------------------------

log "Starting the services"
units="hellion"
[ "$RUN_EEYE" -eq 1 ] && units="eeye hellion"
# shellcheck disable=SC2086
systemctl enable $units >/dev/null 2>&1
# restart, not start: on a re-run they are already running the old binaries.
# reset-failed first: a unit that kept failing (a broken config, say) is
# held by systemd's start limit until this clears it.
# shellcheck disable=SC2086
systemctl reset-failed $units >/dev/null 2>&1 || true
# shellcheck disable=SC2086
systemctl restart $units
sleep 3
for u in $units; do
  if systemctl is-active --quiet "$u"; then
    log "$u is running"
  else
    warn "$u is not running: see journalctl -u $u"
  fi
done

if [ "$ROLE" = topside ]; then
  where="Page:      http://localhost:8080 (or this machine's address, port 8080)
  QGC:       video arrives on UDP 5600 (QGC's default)"
  [ "$NEED_LOOPBACK" -eq 1 ] && where="$where
  Webcam:    /dev/video10, for OBS or a browser"
  [ -n "$ROV_IP" ] && addr="The ROV: pinned at $ROV_IP." ||
    addr="The ROV: found on the network by itself (pair \"$PAIR\"); hellion top shows it."
else
  where="Control:   port 8091 (the topside page reaches this ROV through it)"
  [ -n "$TOPSIDE_IP" ] && addr="Topside: pinned at $TOPSIDE_IP." ||
    addr="Topside: found on the network by itself (pair \"$PAIR\"); hellion top shows it."
fi
other="--role $([ "$ROLE" = rov ] && echo topside || echo rov) --placement $PLACEMENT"
[ "$PAIR" != default ] && other="$other --pair $PAIR"
[ -n "$TOPSIDE_IP" ] && other="$other --topside-ip $TOPSIDE_IP"
[ -n "$ROV_IP" ] && other="$other --rov-ip $ROV_IP"
eeye_lines=""
[ "$RUN_EEYE" -eq 1 ] && eeye_lines="
  Effects:   /var/lib/eeye/effects.json (live; the topside page edits it)
  eeye logs: journalctl -u eeye -f"

cat <<EOF

Installed: $ROLE, placement $PLACEMENT.

  Dashboard: hellion top        Pre-dive: hellion check
  Logs:      journalctl -u hellion -f
  Config:    /etc/hellion/hellion.json (restart hellion after editing)$eeye_lines
  $where

$addr
Install the other end with: $other
EOF
if [ "$WAS_IN_VIDEO" -eq 0 ]; then
  echo "$TARGET_USER was just added to 'video': log out and back in before running 'hellion top'."
fi
