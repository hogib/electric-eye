#!/usr/bin/env bash
# rov.sh: a bench session on the real vehicle, driven from the laptop.
#
#   bench/rov/rov.sh check                 what is there: link, Pi, camera, tools
#   bench/rov/rov.sh kit                   build/fetch what goes to the Pi
#   bench/rov/rov.sh deploy                copy it to ~/eeye-bench on the Pi (no sudo)
#   bench/rov/rov.sh quiet                 stop the Pi's own eeye/hellion and apt timers (sudo: asks you)
#   bench/rov/rov.sh camera                the camera's formats, and a clip of real frames for `effects`
#   bench/rov/rov.sh effects               what each effect costs on the Pi, 1/2/4 threads
#   bench/rov/rov.sh run CASE [SECONDS] [--g2g]
#   bench/rov/rov.sh all [SECONDS] [--g2g] every case the vehicle can run
#   bench/rov/rov.sh stop                  stop whatever a run left behind (both ends)
#   bench/rov/rov.sh restore               put the Pi back as `quiet` found it (sudo: asks you)
#   bench/rov/rov.sh status                what is running on both ends
#
# Cases (CASE):
#   ee-b           eeye on the Pi (auto visibility), hellion encodes with libx264
#   ee-a-x264      hellion on the Pi encodes the camera's MJPEG with libx264
#   ee-a-h264      hellion on the Pi sends the camera's own H.264 (no ffmpeg)
#   ffmpeg-x264    ffmpeg alone: MJPEG -> libx264 -> RTP, ffmpeg receives
#   ffmpeg-copy    ffmpeg alone: the camera's H.264 -> RTP, ffmpeg receives
#   gst-x264       GStreamer alone: MJPEG -> x264enc -> RTP, GStreamer receives
#   gst-copy       GStreamer alone: the camera's H.264 -> RTP, GStreamer receives
#
# Every case sends 1280x720 at 30 fps as H.264 over RTP to the laptop on
# UDP $PORT, and the laptop decodes it. Each is measured the same way:
# psample.py on the Pi (CPU, memory, temperature, throttling of the
# programs by name), the frame rate of decoded frames on the laptop, and
# with --g2g the latency through the lens (../latency/g2g.py: aim the
# camera at this laptop's screen first).
#
# It never talks to the autopilot, never uses sudo by itself, and uses its
# own ports and frame bus directory, so the laptop's installed
# electric-eye can stay running. On the Pi the installed eeye/hellion hold
# the camera and must be stopped: `quiet` does that and `restore` undoes
# it. Read CHECKLIST.md before a session.
#
# Settings (environment):
#   ROV=bluerov2@192.168.2.10   ssh target; ROV=local runs the "Pi" side on
#                               this machine, for a dry run with a webcam
#   LAPTOP_IP=192.168.2.1       where the Pi sends video
#   CAM_MJPEG=/dev/video0       the camera's MJPEG node on the Pi
#   CAM_H264=/dev/video2        its H.264 node
#   PORT=5710                   RTP port (5610 is the installed hellion's)
#   OUT=~/eeye-results/<date>-rov
#   VERSION=0.3.4               the release whose binaries go to the Pi
set -u
set -o pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
ROV=${ROV:-bluerov2@192.168.2.10}
LAPTOP_IP=${LAPTOP_IP:-192.168.2.1}
CAM_MJPEG=${CAM_MJPEG:-/dev/video0}
CAM_H264=${CAM_H264:-/dev/video2}
PORT=${PORT:-5710}
VERSION=${VERSION:-0.3.4}
OUT=${OUT:-$HOME/eeye-results/$(date +%F)-rov}
KIT=$REPO/builddir/bench/kit
RDIR=eeye-bench               # on the Pi, under its home
LRUN=$OUT/.run                # this end's pid files and private frame bus
ROV_CTL=18091
TOP_CTL=18080
CASES="ee-b ee-a-x264 ee-a-h264 ffmpeg-x264 ffmpeg-copy gst-x264 gst-copy"

say() { printf '%s\n' "$*"; }
die() { printf 'rov.sh: %s\n' "$*" >&2; exit 1; }

# Runs a script (stdin) on the Pi. ROV=local: here, with a home of its own.
if [ "$ROV" = local ]; then
  RHOME=${RHOME:-$OUT/.pi-home}
  mkdir -p "$RHOME"
  LAPTOP_IP=127.0.0.1
  R() { HOME=$RHOME bash -s; }
  RSUDO() { say "(ROV=local: skipping sudo: $*)"; }
  PUT() { mkdir -p "$RHOME/$2" && cp -r "$1"/. "$RHOME/$2/"; }
  GET() { cp -r "$RHOME/$1" "$2" 2>/dev/null; }
else
  SSH=(ssh -o BatchMode=yes -o ConnectTimeout=5 "$ROV")
  R() { "${SSH[@]}" bash -s; }
  # sudo on the Pi asks for a password: a terminal, and you typing it.
  RSUDO() { ssh -t "$ROV" "sudo $*"; }
  PUT() { "${SSH[@]}" "mkdir -p $2" && scp -q -r "$1"/. "$ROV:$2/"; }
  GET() { scp -q -r "$ROV:$1" "$2" 2>/dev/null; }
fi

need_out() { mkdir -p "$OUT" "$LRUN"; }

# ---- check ----------------------------------------------------------------

cmd_check() {
  need_out
  local f=$OUT/inventory.txt
  {
    say "== $(date -Is) check, ROV=$ROV"
    say "-- laptop"
    uname -srm
    ip -br addr | grep -v '^lo' || true
    if [ "$ROV" != local ]; then
      ip -br addr | grep -q "$LAPTOP_IP/" ||
        say "!! no interface here has $LAPTOP_IP: the tether is down or its profile is not up"
      ping -c 2 -W 1 "${ROV#*@}" >/dev/null 2>&1 && say "ping ${ROV#*@}: ok" ||
        say "!! ${ROV#*@} does not answer ping"
    fi
    for t in ffmpeg gst-launch-1.0 uv gh; do
      command -v $t >/dev/null && say "$t: $(command -v $t)" || say "!! $t: not installed here"
    done
    # What this end needs to receive the GStreamer cases.
    if command -v gst-inspect-1.0 >/dev/null; then
      for t in rtph264depay rtpjitterbuffer avdec_h264; do
        gst-inspect-1.0 $t >/dev/null 2>&1 || say "!! gst $t: missing here (gst-plugins-good, gst-libav): the gst-* cases cannot be received"
      done
    fi
    systemctl is-active eeye hellion 2>/dev/null | paste -sd' ' | sed 's/^/installed eeye, hellion here: /'
    say "-- pi"
  } | tee "$f"
  R <<EOF 2>&1 | tee -a "$f" || die "cannot reach $ROV over ssh without a password (ssh-copy-id $ROV, with the Pi owner's say)"
echo "model: \$(tr -d '\\0' < /proc/device-tree/model 2>/dev/null || echo unknown)"
echo "os: \$(. /etc/os-release 2>/dev/null; echo \$PRETTY_NAME), kernel \$(uname -r), \$(uname -m), \$(nproc) cores"
echo "clock: \$(date -Is)  (laptop: $(date -Is))"
skew=\$(( \$(date +%s) - $(date +%s) )); [ \${skew#-} -gt 5 ] && echo "!! the Pi's clock is \${skew}s off the laptop's (no RTC): results are stamped by the laptop, but set it if you compare logs"
echo "temperature: \$(awk '{printf "%.1f C", \$1/1000}' /sys/class/thermal/thermal_zone0/temp 2>/dev/null)"
echo "governor: \$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)"
echo "load: \$(cut -d' ' -f1-3 /proc/loadavg)"
echo "installed eeye, hellion: \$(systemctl is-active eeye hellion 2>/dev/null | paste -sd' ')"
echo "apt timers: \$(systemctl is-active apt-daily.timer apt-daily-upgrade.timer 2>/dev/null | paste -sd' ')"
for p in unattended-upgr apt-get dpkg; do pgrep -x \$p >/dev/null && echo "!! \$p is running: it holds a core, wait for it or stop it before measuring"; done
for t in ffmpeg gst-launch-1.0 v4l2-ctl python3; do command -v \$t >/dev/null && echo "\$t: yes" || echo "!! \$t: not installed on the Pi"; done
command -v gst-inspect-1.0 >/dev/null && for e in x264enc rtph264pay h264parse jpegdec; do gst-inspect-1.0 \$e >/dev/null 2>&1 && echo "gst \$e: yes" || echo "!! gst \$e: missing"; done
echo "video group: \$(id -nG | tr ' ' '\\n' | grep -c '^video\$')"
echo "cameras:"; for d in /dev/video*; do [ -e \$d ] && echo "  \$d \$(cat /sys/class/video4linux/\${d##*/}/name 2>/dev/null)"; done
[ -d ~/$RDIR ] && echo "kit on the Pi: \$(cat ~/$RDIR/KIT 2>/dev/null | head -1)" || echo "kit on the Pi: none (rov.sh deploy)"
EOF
  say; say "saved: $f"
}

# ---- kit, deploy ------------------------------------------------------------

remote_arch() { echo 'uname -m' | R; }

cmd_kit() {
  local arch=${1:-$(remote_arch)} || die "cannot ask the Pi its architecture"
  rm -rf "$KIT"; mkdir -p "$KIT/cases"
  case $arch in
  aarch64)
    make -s -C "$REPO/bench" kit ${CROSS:+CROSS=$CROSS} >/dev/null || die "bench kit did not build (CROSS=?)"
    cp "$REPO"/builddir/bench/kit-arm64/{ee_run,bus_cat,BUILD} "$KIT/"
    if [ -n "${EEYE_BIN:-}" ] && [ -n "${HELLION_BIN:-}" ]; then
      cp "$EEYE_BIN" "$KIT/eeye"; cp "$HELLION_BIN" "$KIT/hellion"
      say "eeye, hellion: $EEYE_BIN, $HELLION_BIN" >"$KIT/KIT"
    else
      local d; d=$(mktemp -d)
      gh release download "v$VERSION" -R hogib/electric-eye -D "$d" \
        -p "eeye-v$VERSION-linux-arm64" -p "hellion-v$VERSION-linux-arm64" -p SHA256SUMS ||
        die "could not download v$VERSION's arm64 binaries (or set EEYE_BIN and HELLION_BIN)"
      (cd "$d" && sha256sum --quiet --ignore-missing -c SHA256SUMS) || die "v$VERSION's binaries do not match SHA256SUMS"
      cp "$d/eeye-v$VERSION-linux-arm64" "$KIT/eeye"; cp "$d/hellion-v$VERSION-linux-arm64" "$KIT/hellion"
      rm -rf "$d"
      say "eeye, hellion: release v$VERSION, linux-arm64, checksums verified" >"$KIT/KIT"
    fi ;;
  x86_64)
    make -s -C "$REPO" BUILD=release BUILDDIR="$REPO/builddir/bench/native" >/dev/null || die "eeye/hellion did not build"
    make -s -C "$REPO/bench" >/dev/null || die "bench tools did not build"
    cp "$REPO"/builddir/bench/native/{eeye,hellion} "$REPO"/builddir/bench/{ee_run,bus_cat} "$KIT/"
    say "eeye, hellion: built here from $(git -C "$REPO" describe --tags --always --dirty), x86-64" >"$KIT/KIT" ;;
  *) die "no kit for a $arch Pi" ;;
  esac
  chmod 755 "$KIT"/{eeye,hellion,ee_run,bus_cat}
  cp "$HERE/psample.py" "$KIT/"
  cp "$HERE"/effects/*.json "$KIT/cases/"
  { say "kit made $(date -Is) from $(git -C "$REPO" describe --tags --always --dirty)"; [ -f "$KIT/BUILD" ] && sed 's/^/bench tools: /' "$KIT/BUILD"; } >>"$KIT/KIT"
  (cd "$KIT" && sha256sum eeye hellion ee_run bus_cat >SHA256SUMS)
  cat "$KIT/KIT"
}

cmd_deploy() {
  [ -f "$KIT/KIT" ] || cmd_kit
  PUT "$KIT" "$RDIR" || die "copy to the Pi failed"
  R <<EOF || die "the kit does not run on the Pi"
cd ~/$RDIR && sha256sum --quiet -c SHA256SUMS && mkdir -p run results && ./hellion --help >/dev/null 2>&1; ./ee_run 2>/dev/null; echo "kit in ~/$RDIR: \$(head -1 KIT)"
EOF
}

# ---- quiet, restore ---------------------------------------------------------

UNITS="eeye.service hellion.service apt-daily.timer apt-daily-upgrade.timer"

cmd_quiet() {
  R <<EOF
mkdir -p ~/$RDIR
# Only on the first quiet of a session: a second one must not record
# "inactive" over what was really there.
[ -f ~/$RDIR/units.was ] || for u in $UNITS; do echo "\$u \$(systemctl is-active \$u 2>/dev/null)"; done > ~/$RDIR/units.was
echo "as found:"; sed 's/^/  /' ~/$RDIR/units.was
EOF
  say "Stopping them on the Pi (sudo will ask for the Pi's password):"
  RSUDO systemctl stop $UNITS
  R <<'EOF'
for p in unattended-upgr apt-get dpkg; do pgrep -x $p >/dev/null && echo "!! $p is still running (stopping the timer does not end it): wait, or end it yourself"; done
echo "now: $(systemctl is-active eeye hellion apt-daily.timer apt-daily-upgrade.timer 2>/dev/null | paste -sd' ')"
EOF
}

cmd_restore() {
  cmd_stop
  local units
  units=$(echo "awk '\$2==\"active\"{print \$1}' ~/$RDIR/units.was 2>/dev/null | paste -sd' '" | R)
  if [ -n "$units" ]; then
    say "Starting what was running on the Pi: $units (sudo will ask)"
    RSUDO systemctl start $units
  else
    say "nothing recorded as running before (no quiet in this session?)"
  fi
  R <<EOF
echo "as found:"; sed 's/^/  /' ~/$RDIR/units.was 2>/dev/null
echo "now:"; for u in $UNITS; do echo "  \$u \$(systemctl is-active \$u 2>/dev/null)"; done
same=yes; while read u was; do [ "\$(systemctl is-active \$u 2>/dev/null)" = "\$was" ] || same=no; done < ~/$RDIR/units.was 2>/dev/null
[ \$same = yes ] && rm -f ~/$RDIR/units.was && echo "the Pi's services are as found" || echo "!! not as found: see above"
echo "left on the Pi: ~/$RDIR (\$(du -sh ~/$RDIR 2>/dev/null | cut -f1)); remove it with: rm -rf ~/$RDIR"
EOF
}

# ---- camera, effects ----------------------------------------------------------

cmd_camera() {
  need_out
  R <<EOF | tee "$OUT/camera.txt"
for d in /dev/video*; do
  [ -e \$d ] || continue
  echo "== \$d  \$(cat /sys/class/video4linux/\${d##*/}/name 2>/dev/null)"
  v4l2-ctl -d \$d --list-formats-ext 2>&1 | grep -E "^\\s*\\[|Size|Interval" | grep -E "\\[|1280x720|1920x1080" -A1 | grep -v '^--' | head -24
  v4l2-ctl -d \$d -L 2>/dev/null | grep -iE "key|bitrate|h264|exposure|gain|white" | head -12
done
cd ~/$RDIR || exit 1
if systemctl is-active -q eeye hellion 2>/dev/null; then echo "!! the installed eeye/hellion hold the camera: rov.sh quiet first, then camera again for the clip"; exit 0; fi
# 90 real frames for the effects timing: synthetic ones mislead (docs/effects.md).
ffmpeg -hide_banner -loglevel error -y -f v4l2 -input_format mjpeg -video_size 1280x720 -framerate 30 -i $CAM_MJPEG \
  -frames:v 120 -vf "select=gte(n\\,30),scale=1280:720:out_range=pc" -pix_fmt yuvj422p -f rawvideo run/clip.yuv &&
  ffmpeg -hide_banner -loglevel error -y -f rawvideo -pix_fmt yuvj422p -s 1280x720 -i run/clip.yuv -frames:v 1 -update 1 run/clip.jpg &&
  echo "clip: \$(( \$(stat -c %s run/clip.yuv) / 1843200 )) frames of 1280x720 from $CAM_MJPEG in ~/$RDIR/run/clip.yuv" ||
  echo "!! could not take a clip from $CAM_MJPEG"
EOF
  GET "$RDIR/run/clip.jpg" "$OUT/camera-frame.jpg" && say "a frame of it: $OUT/camera-frame.jpg"
}

cmd_effects() {
  need_out
  say "Timing every effect on the Pi, on its camera's own frames (about 2 minutes)..."
  R <<EOF >"$OUT/effects.jsonl" || die "effects run failed (rov.sh deploy and rov.sh camera first?)"
cd ~/$RDIR || exit 1
[ -s run/clip.yuv ] || { echo "no clip: rov.sh camera" >&2; exit 1; }
temp() { awk '{printf "%.1f", \$1/1000}' /sys/class/thermal/thermal_zone0/temp 2>/dev/null || echo null; }
for threads in 1 2 4; do
  for c in cases/*.json; do
    t0=\$(temp)
    # Twice through: a video run for the smoothed path, as eeye runs it.
    r=\$(OMP_NUM_THREADS=\$threads OMP_WAIT_POLICY=passive ./ee_run -c \$c -s 1280x720 run/clip.yuv -) || continue
    echo "{\\"case\\": \\"\$(basename \$c .json)\\", \\"omp_threads\\": \$threads, \\"temp_before_c\\": \$t0, \\"temp_after_c\\": \$(temp), \\"timing\\": \$r}"
  done
done
EOF
  python3 - "$OUT/effects.jsonl" <<'EOF' | tee "$OUT/effects.txt"
import json, sys
rows = [json.loads(l) for l in open(sys.argv[1]) if l.strip()]
cases = sorted({r["case"] for r in rows})
print("ms per 1280x720 frame on the Pi: mean (p99), by OpenMP threads; budget 33.3 ms at 30 fps")
print("%-16s %18s %18s %18s" % ("chain", "1 thread", "2 threads", "4 threads"))
for c in cases:
    cells = []
    for t in (1, 2, 4):
        m = [r for r in rows if r["case"] == c and r["omp_threads"] == t]
        cells.append("%7.2f (%6.2f)" % (m[0]["timing"]["ms_mean"], m[0]["timing"]["ms_p99"]) if m else "-")
    print("%-16s %18s %18s %18s" % (c, *cells))
temps = [r["temp_after_c"] for r in rows if r["temp_after_c"] is not None]
if temps: print("temperature during the run: up to %.1f C" % max(temps))
EOF
  say "saved: $OUT/effects.jsonl, effects.txt"
}

# ---- run ----------------------------------------------------------------------

# The Pi's side of a case: a shell script that starts it, leaving pid files.
rov_script() {
  local case=$1
  cat <<EOF
cd ~/$RDIR/run || exit 1
rm -f *.pid; mkdir -p rec
export EEYE_BUS_DIR=/dev/shm/eeye-bench-bus OMP_WAIT_POLICY=passive
rm -rf \$EEYE_BUS_DIR; mkdir -p \$EEYE_BUS_DIR
start() { name=\$1; shift; nohup "\$@" > \$name.log 2>&1 < /dev/null & echo \$! > \$name.pid; }
X264="-c:v libx264 -preset ultrafast -tune zerolatency -b:v 4000k -maxrate 4000k -bufsize 2000k -g 30 -pix_fmt yuv420p"
RTP="-f rtp -pkt_size 1200 -sdp_file /dev/null rtp://$LAPTOP_IP:$PORT"
EOF
  local hcommon="\"name\":\"rov\",\"role\":\"rov\",\"control\":\"$ROV_CTL\",\"discovery\":false,\"record_dir\":\"@RUN@/rec\",\"snapshot_dir\":\"@RUN@/rec\""
  case $case in
  ee-b)
    cat <<EOF
cat > eeye.json <<'J'
{"auto_visibility": true, "chain": [], "capture_device": "$CAM_MJPEG", "capture_width": 1280, "capture_height": 720, "output_device": "", "bus_raw": ""}
J
cat > hellion.json <<'J'
{$hcommon,"placement":"b","pipelines":[{"name":"pilot-up","source":"bus:pilot","framerate":30,"sinks":["rtp:$LAPTOP_IP:$PORT"]}]}
J
sed -i "s|@RUN@|\$PWD|g" hellion.json
start eeye ../eeye eeye.json
sleep 2
start hellion ../hellion -c hellion.json
EOF
    ;;
  ee-a-x264)
    cat <<EOF
cat > hellion.json <<'J'
{$hcommon,"placement":"a","pipelines":[{"name":"camera-up","source":"v4l2:$CAM_MJPEG","width":1280,"height":720,"framerate":30,"sinks":["rtp:$LAPTOP_IP:$PORT"]}]}
J
sed -i "s|@RUN@|\$PWD|g" hellion.json
start hellion ../hellion -c hellion.json
EOF
    ;;
  ee-a-h264)
    cat <<EOF
cat > hellion.json <<'J'
{$hcommon,"placement":"a","pipelines":[{"name":"camera-up","source":"v4l2:$CAM_H264","format":"h264","width":1280,"height":720,"framerate":30,"sinks":["rtp:$LAPTOP_IP:$PORT"]}]}
J
sed -i "s|@RUN@|\$PWD|g" hellion.json
start hellion ../hellion -c hellion.json
EOF
    ;;
  ffmpeg-x264)
    echo "start ffmpeg ffmpeg -hide_banner -nostdin -loglevel warning -f v4l2 -input_format mjpeg -video_size 1280x720 -framerate 30 -i $CAM_MJPEG \$X264 \$RTP" ;;
  ffmpeg-copy)
    echo "start ffmpeg ffmpeg -hide_banner -nostdin -loglevel warning -f v4l2 -input_format h264 -video_size 1280x720 -framerate 30 -i $CAM_H264 -c:v copy -bsf:v dump_extra=freq=keyframe \$RTP" ;;
  gst-x264)
    echo "start gst gst-launch-1.0 -q v4l2src device=$CAM_MJPEG ! image/jpeg,width=1280,height=720,framerate=30/1 ! jpegdec ! videoconvert ! x264enc tune=zerolatency speed-preset=ultrafast bitrate=4000 key-int-max=30 ! video/x-h264,profile=baseline ! rtph264pay config-interval=-1 pt=96 mtu=1200 ! udpsink host=$LAPTOP_IP port=$PORT sync=false" ;;
  gst-copy)
    echo "start gst gst-launch-1.0 -q v4l2src device=$CAM_H264 ! video/x-h264,width=1280,height=720,framerate=30/1 ! h264parse ! rtph264pay config-interval=-1 pt=96 mtu=1200 ! udpsink host=$LAPTOP_IP port=$PORT sync=false" ;;
  *) return 1 ;;
  esac
  echo 'sleep 1; for f in *.pid; do kill -0 $(cat $f) 2>/dev/null || { echo "!! ${f%.pid} did not stay up:"; tail -5 ${f%.pid}.log; }; done'
}

# The names psample follows on the Pi for a case.
rov_names() {
  case $1 in
  ee-b) echo "eeye hellion ffmpeg" ;;
  ee-a-x264) echo "hellion ffmpeg" ;;
  ee-a-h264) echo "hellion" ;;
  ffmpeg-*) echo "ffmpeg" ;;
  gst-*) echo "gst-launch-1.0" ;;
  esac
}

# This end's receiver for a case: a command writing decoded GRAY8 1280x720
# frames to stdout. electric-eye's is its own topside hellion, read off
# the frame bus; the others decode with the stack that sent.
receiver() {
  case $1 in
  ee-*) echo "EEYE_BUS_DIR=$LRUN/bus $REPO/builddir/bench/bus_cat bench --timeout 30" ;;
  ffmpeg-*) echo "ffmpeg -hide_banner -nostdin -loglevel error -protocol_whitelist file,udp,rtp -fflags nobuffer -flags low_delay -max_delay 0 -reorder_queue_size 0 -i $LRUN/bench.sdp -vf scale=1280:720 -pix_fmt gray -f rawvideo -" ;;
  gst-*) echo "gst-launch-1.0 -q udpsrc port=$PORT caps=application/x-rtp,media=video,clock-rate=90000,encoding-name=H264,payload=96 ! rtpjitterbuffer latency=0 ! rtph264depay ! avdec_h264 ! videoconvert ! videoscale ! video/x-raw,format=GRAY8,width=1280,height=720 ! fdsink fd=1 sync=false" ;;
  esac
}

cmd_stop() {
  # By pid, never by pattern: a pattern matches the shell that carries it.
  echo "cd ~/$RDIR/run 2>/dev/null && { for f in *.pid; do [ -f \$f ] && kill -INT \$(cat \$f) 2>/dev/null; done; for i in \$(seq 30); do n=0; for f in *.pid; do [ -f \$f ] && kill -0 \$(cat \$f) 2>/dev/null && n=1; done; [ \$n = 0 ] && break; sleep 0.1; done; for f in *.pid; do [ -f \$f ] && kill -KILL \$(cat \$f) 2>/dev/null; rm -f \$f; done; }; true" | R
  local f pid i
  for f in "$LRUN"/*.pid; do
    [ -f "$f" ] || continue
    pid=$(cat "$f")
    kill -INT "$pid" 2>/dev/null
    # hellion takes a second or two to stop its decoder: let it, so no
    # ffmpeg of its is left behind.
    for i in $(seq 40); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
    kill -KILL "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
    rm -f "$f"
  done
}

cmd_run() {
  local case=${1:-} seconds=60 g2g=no a
  shift || true
  for a in "$@"; do
    case $a in --g2g) g2g=yes ;; *[!0-9]* | '') die "run: what is \"$a\"?" ;; *) seconds=$a ;; esac
  done
  echo " $CASES " | grep -q " $case " || die "run: CASE is one of: $CASES"
  need_out
  [ -x "$REPO/builddir/bench/bus_cat" ] || make -s -C "$REPO/bench" >/dev/null || die "bench tools did not build"
  [ -x "$REPO/builddir/hellion" ] || make -s -C "$REPO" >/dev/null || die "hellion did not build here"
  local d=$OUT/$case
  mkdir -p "$d"; rm -f "$d"/*
  cmd_stop
  say "== $case, ${seconds}s$([ $g2g = yes ] && echo ', with latency through the lens')"

  # This end first, so nothing sent is missed.
  cat >"$LRUN/bench.sdp" <<EOF
v=0
o=- 0 0 IN IP4 127.0.0.1
s=bench
c=IN IP4 0.0.0.0
t=0 0
m=video $PORT RTP/AVP 96
a=rtpmap:96 H264/90000
a=fmtp:96 packetization-mode=1
EOF
  case $case in ee-*)
    rm -rf "$LRUN/bus"; mkdir -p "$LRUN/bus" "$LRUN/rec"
    cat >"$LRUN/top.json" <<EOF
{"name":"bench-topside","role":"topside","placement":"a","control":"$TOP_CTL","discovery":false,
 "record_dir":"$LRUN/rec","snapshot_dir":"$LRUN/rec",
 "pipelines":[{"name":"down","source":"rtp:$PORT","width":1280,"height":720,"sinks":["bus:bench"]}]}
EOF
    EEYE_BUS_DIR=$LRUN/bus "$REPO/builddir/hellion" -c "$LRUN/top.json" >"$d/laptop-hellion.log" 2>&1 &
    echo $! >"$LRUN/hellion.pid"
    sleep 1
    ;;
  esac

  rov_script "$case" | R | tee "$d/start.txt"
  grep -q '^!!' "$d/start.txt" && { cmd_stop; die "$case did not start on the Pi (see above; $d/start.txt)"; }

  local recv; recv=$(receiver "$case")
  local warm=6
  if [ $g2g = yes ]; then
    say "the pattern goes full-screen now: 20s of calibration, then ${seconds}s. Esc ends it early."
    (cd "$REPO" && uv run --quiet bench/latency/g2g.py --size 1280x720 --seconds "$seconds" --label "$case" \
      --source "$recv" --json "$d/latency.json" --csv "$d/latency.csv") >"$d/g2g.txt" 2>&1 &
    echo $! >"$LRUN/g2g.pid"
    warm=22
  else
    (bash -c "$recv" | python3 "$HERE/../latency/fcount.py" --size 1280x720 --skip $warm --seconds "$seconds" >"$d/decoded.json") 2>"$d/receiver.log" &
    echo $! >"$LRUN/recv.pid"
  fi
  sleep $warm

  # The measured window.
  local names; names=$(rov_names "$case")
  R >"$d/pi.json" 2>"$d/pi.err" <<EOF
cd ~/$RDIR && python3 psample.py --seconds $seconds --csv run/psample.csv $names
EOF
  case $case in ee-*) curl -s -m 3 "http://127.0.0.1:$TOP_CTL/api/status" >"$d/laptop-status.json" ;; esac
  [ "$ROV" != local ] || true
  echo "curl -s -m 3 http://127.0.0.1:$ROV_CTL/api/status 2>/dev/null; echo; tail -3 ~/$RDIR/run/eeye.log 2>/dev/null | grep effects:" | R >"$d/pi-status.txt" 2>/dev/null
  [ $g2g = yes ] && wait "$(cat "$LRUN/g2g.pid")" 2>/dev/null
  [ $g2g = yes ] || { sleep 1; wait "$(cat "$LRUN/recv.pid")" 2>/dev/null; }
  cmd_stop
  GET "$RDIR/run/psample.csv" "$d/pi-psample.csv"
  for f in eeye hellion ffmpeg gst; do GET "$RDIR/run/$f.log" "$d/pi-$f.log"; done
  python3 "$HERE/summary.py" "$OUT" "$case"
}

cmd_all() {
  need_out
  local c have_h264 have_gst
  have_h264=$(echo "v4l2-ctl -d $CAM_H264 --list-formats 2>/dev/null | grep -c H264" | R)
  have_gst=$(echo "command -v gst-launch-1.0 >/dev/null && gst-inspect-1.0 rtph264pay >/dev/null 2>&1 && echo 1 || echo 0" | R)
  for c in $CASES; do
    case $c in *h264 | *copy) [ "${have_h264:-0}" -gt 0 ] || { say "-- $c skipped: $CAM_H264 offers no H.264"; continue; } ;; esac
    case $c in gst-*) [ "${have_gst:-0}" = 1 ] || { say "-- $c skipped: GStreamer (gst-launch-1.0, rtph264pay) is not on the Pi"; continue; } ;; esac
    cmd_run "$c" "$@"
    # Let it cool to a like start: heat carries from one case into the next.
    echo 'for i in $(seq 30); do t=$(awk "{print int(\$1/1000)}" /sys/class/thermal/thermal_zone0/temp 2>/dev/null || echo 0); [ $t -lt 65 ] && break; sleep 5; done; echo "pi at ${t} C"' | R
  done
  python3 "$HERE/summary.py" "$OUT"
}

cmd_status() {
  say "-- pi"
  R <<EOF
cd ~/$RDIR/run 2>/dev/null && for f in *.pid; do [ -f \$f ] && { kill -0 \$(cat \$f) 2>/dev/null && echo "  \${f%.pid} running (\$(cat \$f))" || echo "  \${f%.pid} gone"; }; done
echo "  installed eeye, hellion: \$(systemctl is-active eeye hellion 2>/dev/null | paste -sd' ')"
echo "  \$(awk '{printf "%.1f C", \$1/1000}' /sys/class/thermal/thermal_zone0/temp 2>/dev/null), load \$(cut -d' ' -f1 /proc/loadavg)"
EOF
  say "-- laptop"
  local f
  for f in "$LRUN"/*.pid; do
    [ -f "$f" ] && { kill -0 "$(cat "$f")" 2>/dev/null && say "  $(basename "$f" .pid) running" || say "  $(basename "$f" .pid) gone"; }
  done
  say "  results: $OUT"
}

case ${1:-} in
check) cmd_check ;;
kit) shift; cmd_kit "$@" ;;
deploy) cmd_deploy ;;
quiet) cmd_quiet ;;
restore) cmd_restore ;;
camera) cmd_camera ;;
effects) cmd_effects ;;
run) shift; cmd_run "$@" ;;
all) shift; cmd_all "$@" ;;
stop) cmd_stop ;;
status) cmd_status ;;
*) sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
