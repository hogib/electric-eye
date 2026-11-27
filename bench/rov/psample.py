#!/usr/bin/env python3
"""psample: what some processes cost, read from /proc once a second.

    psample.py --seconds 60 --csv out.csv --json out.json NAME [NAME ...]

Runs on the ROV's Pi (standard library only). Each NAME is a program name
as /proc/<pid>/comm has it (eeye, hellion, ffmpeg, gst-launch-1.0); every
process of that name is followed, new ones are picked up as they start,
and their figures are summed per name. The same method for electric-eye
and for the stacks it is compared with, which `hellion bench` cannot
follow.

Per second: CPU of one core (%), resident memory (MB), the machine's whole
CPU (%), temperature, clock, and the Pi firmware's throttle flags where
the kernel shows them. The JSON is the summary: mean and highest per name.
"""
import argparse
import glob
import json
import os
import time

TICK = os.sysconf("SC_CLK_TCK")
PAGE = os.sysconf("SC_PAGE_SIZE")


def read(path):
    try:
        with open(path) as f:
            return f.read()
    except OSError:
        return None


def procs(names):
    """{name: {pid: (cpu_ticks, rss_bytes)}} for the processes now alive."""
    out = {n: {} for n in names}
    for d in glob.glob("/proc/[0-9]*"):
        comm = read(d + "/comm")
        if comm is None:
            continue
        comm = comm.strip()
        if comm not in out:
            continue
        stat = read(d + "/stat")
        if not stat:
            continue
        # The command may hold spaces and brackets: split after the last ")".
        f = stat[stat.rindex(")") + 2:].split()
        out[comm][int(d[6:])] = (int(f[11]) + int(f[12]), int(f[21]) * PAGE)
    return out


def machine_ticks():
    f = read("/proc/stat").split("\n", 1)[0].split()[1:]
    v = [int(x) for x in f]
    idle = v[3] + (v[4] if len(v) > 4 else 0)
    return sum(v[:8]), idle


def temperature():
    best = None
    for z in sorted(glob.glob("/sys/class/thermal/thermal_zone*")):
        kind = (read(z + "/type") or "").strip()
        t = read(z + "/temp")
        if t is None:
            continue
        c = int(t) / 1000.0
        if "cpu" in kind or "soc" in kind or "x86_pkg" in kind:
            return c
        if best is None:
            best = c
    return best


def clock_mhz():
    f = read("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq")
    return int(f) / 1000.0 if f else None


def throttled():
    """The Pi firmware's flags as an int, or None where there are none."""
    for p in glob.glob("/sys/devices/platform/soc*/*firmware*/get_throttled") + \
            glob.glob("/sys/devices/platform/soc/soc:firmware/get_throttled"):
        v = read(p)
        if v:
            try:
                return int(v.strip(), 16)
            except ValueError:
                pass
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=60)
    ap.add_argument("--csv")
    ap.add_argument("--json")
    ap.add_argument("names", nargs="+")
    a = ap.parse_args()

    cores = os.cpu_count() or 1
    csv = open(a.csv, "w") if a.csv else None
    if csv:
        csv.write("t_s,name,value,value2\n")
    series = {n: {"cpu": [], "rss": []} for n in a.names}
    sysrow = {"cpu": [], "temp": [], "mhz": []}
    flags = 0
    prev = procs(a.names)
    prev_m = machine_ticks()
    t0 = prev_t = time.monotonic()
    while True:
        left = prev_t + 1.0 - time.monotonic()
        if left > 0:
            time.sleep(left)
        now = time.monotonic()
        if now - t0 > a.seconds + 0.5:
            break
        dt = now - prev_t
        cur = procs(a.names)
        for n in a.names:
            ticks = 0
            for pid, (cpu, _) in cur[n].items():
                # A process first seen now is charged from its start: it
                # started within this second.
                ticks += cpu - prev[n].get(pid, (0, 0))[0]
            cpu_pct = 100.0 * ticks / TICK / dt
            rss = sum(r for _, r in cur[n].values()) / 1e6
            series[n]["cpu"].append(cpu_pct)
            series[n]["rss"].append(rss)
            if csv:
                csv.write("%.1f,%s,%.2f,%.2f\n" % (now - t0, n, cpu_pct, rss))
        m = machine_ticks()
        total = m[0] - prev_m[0]
        busy = total - (m[1] - prev_m[1])
        sys_pct = 100.0 * cores * busy / total if total else 0.0
        temp, mhz, thr = temperature(), clock_mhz(), throttled()
        sysrow["cpu"].append(sys_pct)
        if temp is not None:
            sysrow["temp"].append(temp)
        if mhz is not None:
            sysrow["mhz"].append(mhz)
        if thr is not None:
            flags |= thr
        if csv:
            csv.write("%.1f,machine_cpu_pct,%.2f,\n" % (now - t0, sys_pct))
            if temp is not None:
                csv.write("%.1f,temp_c,%.1f,\n" % (now - t0, temp))
            if mhz is not None:
                csv.write("%.1f,cpu_mhz,%.0f,\n" % (now - t0, mhz))
            if thr is not None:
                csv.write("%.1f,throttled,%d,\n" % (now - t0, thr))
            csv.flush()
        prev, prev_m, prev_t = cur, m, now

    def stat(v):
        return {"mean": round(sum(v) / len(v), 2), "max": round(max(v), 2),
                "min": round(min(v), 2)} if v else None

    out = {
        "seconds": round(time.monotonic() - t0, 1),
        "samples": len(sysrow["cpu"]),
        "cores": cores,
        "processes": {n: {"cpu_pct": stat(s["cpu"]), "rss_mb": stat(s["rss"]),
                          "seen": any(x > 0 for x in s["rss"])}
                      for n, s in series.items()},
        "all_named_cpu_pct": stat([sum(series[n]["cpu"][i] for n in a.names)
                                   for i in range(len(sysrow["cpu"]))]),
        "machine_cpu_pct": stat(sysrow["cpu"]),
        "temp_c": stat(sysrow["temp"]),
        "cpu_mhz": stat(sysrow["mhz"]),
        # Bits 0-3: under-voltage, frequency capped, throttled, soft
        # temperature limit, at some moment of this run.
        "throttled_now_bits": flags & 0xf,
    }
    text = json.dumps(out, indent=1)
    if a.json:
        with open(a.json, "w") as f:
            f.write(text + "\n")
    print(text)


if __name__ == "__main__":
    main()
