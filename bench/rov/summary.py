#!/usr/bin/env python3
"""summary: one line per case from a session's results folder.

    summary.py RESULTS_DIR [CASE]

Reads what rov.sh run left in RESULTS_DIR/<case>/ and prints (and, for the
whole session, writes RESULTS_DIR/summary.md and summary.json) the Pi's
cost and what arrived on the laptop. A figure that was not measured is
shown as "-", never filled in.
"""
import json
import os
import sys


def load(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def case_row(d, name):
    pi = load(os.path.join(d, "pi.json")) or {}
    dec = load(os.path.join(d, "decoded.json")) or {}
    lat = load(os.path.join(d, "latency.json")) or {}
    top = load(os.path.join(d, "laptop-status.json")) or {}
    row = {"case": name}
    procs = pi.get("processes", {})
    row["pi_cpu_pct"] = (pi.get("all_named_cpu_pct") or {}).get("mean")
    row["pi_cpu_pct_max"] = (pi.get("all_named_cpu_pct") or {}).get("max")
    row["pi_cpu_by_program"] = {k: v["cpu_pct"]["mean"] for k, v in procs.items()
                                if v.get("seen") and v.get("cpu_pct")}
    rss = [v["rss_mb"]["max"] for v in procs.values() if v.get("seen") and v.get("rss_mb")]
    row["pi_rss_mb"] = round(sum(rss), 1) if rss else None
    row["pi_machine_cpu_pct"] = (pi.get("machine_cpu_pct") or {}).get("mean")
    row["pi_temp_c_max"] = (pi.get("temp_c") or {}).get("max")
    row["pi_throttled_bits"] = pi.get("throttled_now_bits")
    row["not_seen"] = [k for k, v in procs.items() if not v.get("seen")]
    row["decoded_fps"] = dec.get("fps", lat.get("source_fps"))
    row["longest_gap_ms"] = dec.get("longest_gap_ms")
    for k in ("p50", "p95", "p99", "min", "max"):
        row["latency_ms_" + k] = lat.get("latency_ms_" + k)
    row["latency_frames"] = lat.get("frames_read")
    row["latency_unreadable"] = lat.get("frames_unreadable")
    # electric-eye's own link figures, where the receiver is hellion.
    for p in top.get("pipelines", []):
        for e in p.get("endpoints", []):
            if e.get("role") == "source":
                row["link_kbps"] = e.get("kbps")
                row["link_loss_pct"] = e.get("loss_pct")
                row["link_lost"] = e.get("lost")
    return row


def fmt(v, spec="%.1f"):
    return "-" if v is None else spec % v


def line(r):
    by = ", ".join("%s %.1f" % kv for kv in sorted(r["pi_cpu_by_program"].items()))
    return "| %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
        r["case"], fmt(r["pi_cpu_pct"]), by or "-", fmt(r["pi_rss_mb"]),
        fmt(r["pi_temp_c_max"]), fmt(r["decoded_fps"]),
        fmt(r["latency_ms_p50"]), fmt(r["latency_ms_p95"]),
        fmt(r.get("link_loss_pct"), "%.2f"))


HEAD = ("| case | Pi CPU % of a core | by program | Pi RSS MB | Pi max °C | "
        "decoded fps | latency p50 ms | p95 ms | link loss % |\n"
        "|---|---|---|---|---|---|---|---|---|")


def main():
    out = sys.argv[1]
    only = sys.argv[2] if len(sys.argv) > 2 else None
    names = [only] if only else sorted(
        n for n in os.listdir(out)
        if os.path.isfile(os.path.join(out, n, "pi.json")))
    rows = [case_row(os.path.join(out, n), n) for n in names]
    text = HEAD + "\n" + "\n".join(line(r) for r in rows)
    notes = []
    for r in rows:
        if r["not_seen"]:
            notes.append("%s: no process named %s was seen on the Pi" %
                         (r["case"], ", ".join(r["not_seen"])))
        if r["pi_throttled_bits"]:
            notes.append("%s: the Pi was throttled during the run (bits %d): "
                         "repeat it cooler" % (r["case"], r["pi_throttled_bits"]))
        if r["decoded_fps"] is None:
            notes.append("%s: no decoded frames were counted on the laptop" % r["case"])
    print(text)
    for n in notes:
        print("!! " + n)
    if not only:
        with open(os.path.join(out, "summary.md"), "w") as f:
            f.write(text + "\n" + "".join("\n- " + n for n in notes) + "\n")
        with open(os.path.join(out, "summary.json"), "w") as f:
            json.dump(rows, f, indent=1)
        print("saved: %s/summary.md, summary.json" % out)


if __name__ == "__main__":
    main()
