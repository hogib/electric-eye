#!/usr/bin/env python3
# /// script
# requires-python = ">=3.9"
# dependencies = ["numpy", "opencv-contrib-python-headless", "scikit-image"]
# ///
"""quality: eeye's auto visibility against published enhancement methods.

    uv run bench/quality/quality.py images --data RAW_DIR [--ref REF_DIR] --out OUT
    uv run bench/quality/quality.py video  --clip CLIP.mp4 --out OUT [--seconds 20]

`images` runs every method over a folder of underwater pictures and scores
each result: UCIQE and UIQM (no reference needed), PSNR and SSIM where
--ref holds a reference picture of the same name (UIEB's do), the number
of ORB keypoints (a stand-in for "can a vision task use this"), and the
time the method took at that picture's size. `video` runs them over a
clip scaled to 1280x720, frame by frame as a live system would, and
measures what stills cannot: flicker, features matched from one frame to
the next, and the time per frame at the size eeye runs at.

The methods
  identity          the input through eeye's own 4:2:2 frame format and
                    back: the floor every eeye figure should be read from
  eeye-auto         eeye's auto visibility, run by ee_run, which links
                    eeye's own sources (make -C bench)
  clahe             CLAHE on L of CIELAB (OpenCV), clip 2.0, 8x8 tiles
  grayworld         gray-world white balance (OpenCV xphoto)
  grayworld-clahe   the two together: what one would write in an afternoon
  he                histogram equalisation of luma
  fusion            colour balance and fusion, after Ancuti et al., IEEE
                    TIP 2018
  udcp              underwater dark channel prior, after Drews et al., 2013
  --external NAME=CMD   any other method: CMD is run once with {in} and
                    {out} replaced by folders of PNGs (same names out as
                    in). For FUnIE-GAN, MLLE and the like; see README.md.

`fusion` and `udcp` are this bench's own implementations from the papers,
in numpy and OpenCV: no public code from the authors of either was found.
They are here as quality references. Their times are those of unoptimised research
code and say nothing about what a tuned version would cost.

UCIQE and UIQM follow the formulas and constants of the papers (Yang and
Sowmya 2015; Panetta, Gao and Agaian 2016). UIQM is computed as the
FUnIE-GAN repository's evaluation code computes it, with 0.114 where that
has 0.144 for blue in the sharpness term, and at each picture's own size
where its script first resizes to 256x256. UCIQE has the same three terms
as the Python port in common use but on CIELAB scaled to 0..1, which
gives values of the size the literature reports. Public versions of both
differ enough to rank pictures differently, so compare the figures within
this bench, not with a table in someone else's paper. Both are known to
reward over-saturated and over-contrasted results; they are reported
because everyone reports them, next to measures that do not have that
flaw.
"""
import argparse
import csv
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile
import time

import cv2
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
EE_RUN = os.path.join(REPO, "builddir", "bench", "ee_run")
EXTS = (".png", ".jpg", ".jpeg", ".bmp", ".tif", ".tiff")


# ---- eeye's frame format ----------------------------------------------------

def to_yuv422(bgr):
    """BGR -> planar full-range YUV 4:2:2 (JFIF matrix), as eeye holds it."""
    ycc = cv2.cvtColor(bgr, cv2.COLOR_BGR2YCrCb)
    y = ycc[:, :, 0]
    cr = ycc[:, :, 1].astype(np.uint16)
    cb = ycc[:, :, 2].astype(np.uint16)
    v = ((cr[:, 0::2] + cr[:, 1::2] + 1) // 2).astype(np.uint8)
    u = ((cb[:, 0::2] + cb[:, 1::2] + 1) // 2).astype(np.uint8)
    return y.tobytes() + u.tobytes() + v.tobytes()


def from_yuv422(buf, w, h):
    n = w * h
    y = np.frombuffer(buf, np.uint8, n, 0).reshape(h, w)
    u = np.frombuffer(buf, np.uint8, n // 2, n).reshape(h, w // 2)
    v = np.frombuffer(buf, np.uint8, n // 2, n + n // 2).reshape(h, w // 2)
    ycc = np.dstack([y, np.repeat(v, 2, axis=1), np.repeat(u, 2, axis=1)])
    return cv2.cvtColor(ycc, cv2.COLOR_YCrCb2BGR)


def even(bgr):
    h, w = bgr.shape[:2]
    return np.ascontiguousarray(bgr[:h - h % 2, :w - w % 2])


def ee_run(frames, config, still, repeat=1, fps=30):
    """Runs frames (same size) through ee_run. Returns (outputs, ms/frame)."""
    if not os.access(EE_RUN, os.X_OK):
        sys.exit("quality: %s is not built: make -C bench" % EE_RUN)
    h, w = frames[0].shape[:2]
    with tempfile.TemporaryDirectory() as d:
        cfg, fin, fout = (os.path.join(d, n) for n in ("c.json", "in", "out"))
        with open(cfg, "w") as f:
            json.dump(config, f)
        with open(fin, "wb") as f:
            for fr in frames:
                f.write(to_yuv422(fr))
        cmd = [EE_RUN, "-c", cfg, "-s", "%dx%d" % (w, h), "-r", str(fps)]
        if still:
            cmd += ["--still", "--repeat", str(repeat)]
        env = dict(os.environ, OMP_WAIT_POLICY="passive")
        r = subprocess.run(cmd + [fin, fout], capture_output=True, text=True,
                           env=env)
        if r.returncode != 0:
            sys.exit("quality: ee_run failed: " + r.stderr.strip())
        timing = json.loads(r.stdout.strip().splitlines()[-1])
        size = w * h * 2
        with open(fout, "rb") as f:
            data = f.read()
    outs = [from_yuv422(data[i * size:(i + 1) * size], w, h)
            for i in range(len(frames))]
    return outs, timing


# ---- the other methods --------------------------------------------------------

def m_clahe(bgr, state=None):
    lab = cv2.cvtColor(bgr, cv2.COLOR_BGR2LAB)
    lab[:, :, 0] = cv2.createCLAHE(2.0, (8, 8)).apply(lab[:, :, 0])
    return cv2.cvtColor(lab, cv2.COLOR_LAB2BGR)


def m_grayworld(bgr, state=None):
    return cv2.xphoto.createGrayworldWB().balanceWhite(bgr)


def m_grayworld_clahe(bgr, state=None):
    return m_clahe(m_grayworld(bgr))


def m_he(bgr, state=None):
    ycc = cv2.cvtColor(bgr, cv2.COLOR_BGR2YCrCb)
    ycc[:, :, 0] = cv2.equalizeHist(ycc[:, :, 0])
    return cv2.cvtColor(ycc, cv2.COLOR_YCrCb2BGR)


def m_udcp(bgr, state=None):
    """Drews et al. 2013: the dark channel over green and blue only.

    As the paper's eq. 10 has it: 15x15 patches, no factor on the dark
    channel, and for the water's colour the brightest pixel among the top
    0.1% of the dark channel. The paper names neither a floor for the
    transmission nor a recovery formula; this uses He et al.'s (0.1, and
    the image model inverted), and a guided filter where the paper refines
    by matting, as implementations of it generally do."""
    img = bgr.astype(np.float32) / 255.0
    kernel = cv2.getStructuringElement(cv2.MORPH_RECT, (15, 15))
    dark = cv2.erode(np.minimum(img[:, :, 0], img[:, :, 1]), kernel)
    n = max(1, dark.size // 1000)
    idx = np.argpartition(dark.ravel(), -n)[-n:]
    cand = img.reshape(-1, 3)[idx]
    a = np.maximum(cand[cand.sum(axis=1).argmax()], 0.05)
    norm = img / a
    t = 1.0 - cv2.erode(np.minimum(norm[:, :, 0], norm[:, :, 1]), kernel)
    grey = cv2.cvtColor(bgr, cv2.COLOR_BGR2GRAY).astype(np.float32) / 255.0
    t = cv2.ximgproc.guidedFilter(grey, t.astype(np.float32), 30, 1e-3)
    t = np.clip(t, 0.1, 1.0)[:, :, None]
    out = (img - a) / t + a
    return (np.clip(out, 0, 1) * 255).astype(np.uint8)


def _pyr_gauss(x, levels):
    out = [x]
    for _ in range(levels - 1):
        out.append(cv2.pyrDown(out[-1]))
    return out


def _pyr_laplace(x, levels):
    g = _pyr_gauss(x, levels)
    out = []
    for i in range(levels - 1):
        up = cv2.pyrUp(g[i + 1], dstsize=(g[i].shape[1], g[i].shape[0]))
        out.append(g[i] - up)
    out.append(g[-1])
    return out


# The paper gives no value for the gamma of its first input; public ports
# use 1.2, 1.3 and 2. The defaults here are the setting that came closest
# to UIEB's references on a 60-picture subset (README.md has the sweep), so
# that the baseline is the method at its best and not at our guess.
# FUSION_GAMMA and FUSION_NORM in the environment set them.
FUSION_GAMMA = float(os.environ.get("FUSION_GAMMA", "1.2"))
FUSION_NORM = os.environ.get("FUSION_NORM", "robust")


def m_fusion(bgr, state=None):
    """Ancuti et al. 2018: white balance with red compensation (eq. 4,
    alpha 1) and gray world, then a multiscale fusion of a gamma-corrected
    and a sharpened version (eq. 6), weighted by Laplacian contrast,
    saliency and saturation (eq. 7, delta 0.1).

    The paper leaves three things open, and they are choices here: the
    gamma (FUSION_GAMMA above), the Gaussian of the sharpening (sigma 20,
    as one MATLAB port has it) and the relative scale of the three weights
    (each on the 0..1 scale of the picture). No code from the authors was
    found; the public ports read for this each depart from the paper in
    ways this does not (the red compensation's formula, a contrast weight
    that is not a Laplacian, histogram equalisation in the sharpening)."""
    img = bgr.astype(np.float32) / 255.0
    b, g, r = cv2.split(img)
    r = r + (g.mean() - r.mean()) * (1.0 - r) * g  # alpha = 1
    wb = np.clip(cv2.merge([b, g, r]), 0, 1)
    means = wb.reshape(-1, 3).mean(axis=0)
    wb = np.clip(wb * (means.mean() / np.maximum(means, 1e-3)), 0, 1)

    in1 = np.power(wb, FUSION_GAMMA)
    detail = wb - cv2.GaussianBlur(wb, (0, 0), 20)
    # N{} of eq. 6: one shift and scale for all three channels. "minmax"
    # is the paper's wording, to the full range; "robust" stretches
    # between the 1st and 99th percentiles instead, so that a few extreme
    # pixels do not leave the rest of the detail nearly flat.
    if FUSION_NORM == "minmax":
        lo, hi = float(detail.min()), float(detail.max())
    else:
        lo, hi = (float(v) for v in np.percentile(detail, [1, 99]))
    detail = np.clip((detail - lo) / max(hi - lo, 1e-6), 0, 1)
    in2 = (wb + detail) / 2.0  # normalised unsharp masking

    def weights(x):
        lum = cv2.cvtColor(x, cv2.COLOR_BGR2GRAY)
        contrast = np.abs(cv2.Laplacian(lum, cv2.CV_32F))
        # Achanta's saliency: distance in Lab of the slightly blurred
        # picture from its mean colour, with L on 0..1 like the rest.
        lab = cv2.cvtColor(x, cv2.COLOR_BGR2LAB)
        sal = np.sqrt(((cv2.GaussianBlur(lab, (5, 5), 0) -
                        lab.reshape(-1, 3).mean(axis=0)) ** 2).sum(axis=2))
        sal = sal / 100.0
        sat = np.sqrt(((x - lum[:, :, None]) ** 2).mean(axis=2))
        return contrast + sal + sat + 0.1

    w1, w2 = weights(in1), weights(in2)
    total = w1 + w2
    w1, w2 = w1 / total, w2 / total
    # By size: 7 levels at 600x800, the paper's example.
    levels = max(2, int(math.log2(min(img.shape[:2]))) - 2)
    gw1, gw2 = _pyr_gauss(w1, levels), _pyr_gauss(w2, levels)
    l1, l2 = _pyr_laplace(in1, levels), _pyr_laplace(in2, levels)
    fused = [gw1[i][:, :, None] * l1[i] + gw2[i][:, :, None] * l2[i]
             for i in range(levels)]
    out = fused[-1]
    for i in range(levels - 2, -1, -1):
        out = cv2.pyrUp(out, dstsize=(fused[i].shape[1],
                                      fused[i].shape[0])) + fused[i]
    return (np.clip(out, 0, 1) * 255).astype(np.uint8)


PY_METHODS = {
    "clahe": m_clahe,
    "grayworld": m_grayworld,
    "grayworld-clahe": m_grayworld_clahe,
    "he": m_he,
    "fusion": m_fusion,
    "udcp": m_udcp,
}
EE_METHODS = {
    "identity": {"chain": []},
    "eeye-auto": {"auto_visibility": True, "chain": []},
}
ALL = list(EE_METHODS) + list(PY_METHODS)


# ---- metrics ------------------------------------------------------------------

def uciqe(bgr):
    lab = cv2.cvtColor(bgr, cv2.COLOR_BGR2LAB).astype(np.float64)
    L = lab[:, :, 0] / 255.0
    a = lab[:, :, 1] / 255.0 - 0.5
    b = lab[:, :, 2] / 255.0 - 0.5
    chroma = np.sqrt(a * a + b * b)
    s = np.sort(L.ravel())
    k = max(1, len(s) // 100)
    con = s[-k:].mean() - s[:k].mean()
    sat = np.where(L > 0, chroma / np.maximum(L, 1e-6), 0).mean()
    return 0.4680 * chroma.std() + 0.2745 * con + 0.2576 * sat


def _trimmed_mean(x, lo=0.1, hi=0.1):
    x = np.sort(x.ravel())
    k = len(x)
    a, b = int(math.ceil(lo * k)), int(math.floor(hi * k))
    return x[a + 1:k - b].sum() / max(1, k - a - b)


def _blocks(x, size):
    h, w = x.shape[:2]
    k2, k1 = h // size, w // size
    x = x[:k2 * size, :k1 * size]
    if x.ndim == 2:
        x = x.reshape(k2, size, k1, size).swapaxes(1, 2).reshape(k2, k1, -1)
    else:
        x = x.reshape(k2, size, k1, size, -1).swapaxes(1, 2).reshape(k2, k1, -1)
    return x, k1 * k2


def _eme(x, size=10):
    bl, n = _blocks(x, size)
    if n == 0:
        return 0.0
    hi, lo = bl.max(axis=2), bl.min(axis=2)
    ok = (lo > 0) & (hi > 0)
    return 2.0 / n * np.log(hi[ok] / lo[ok]).sum()


def uiqm(bgr):
    x = bgr.astype(np.float64)
    b, g, r = x[:, :, 0], x[:, :, 1], x[:, :, 2]
    # Colourfulness.
    rg, yb = r - g, (r + g) / 2 - b
    mrg, myb = _trimmed_mean(rg), _trimmed_mean(yb)
    srg, syb = ((rg - mrg) ** 2).mean(), ((yb - myb) ** 2).mean()
    uicm = -0.0268 * math.hypot(mrg, myb) + 0.1586 * math.sqrt(srg + syb)
    # Sharpness: EME of each channel's Sobel edge map times the channel.
    uism = 0.0
    for ch, lam in ((r, 0.299), (g, 0.587), (b, 0.114)):
        mag = np.hypot(cv2.Sobel(ch, cv2.CV_64F, 1, 0),
                       cv2.Sobel(ch, cv2.CV_64F, 0, 1))
        if mag.max() > 0:
            mag *= 255.0 / mag.max()
        uism += lam * _eme(mag * ch)
    # Contrast: logAMEE over blocks.
    bl, n = _blocks(x, 10)
    hi, lo = bl.max(axis=2), bl.min(axis=2)
    top, bot = hi - lo, hi + lo
    ok = (top > 0) & (bot > 0)
    ratio = top[ok] / bot[ok]
    uiconm = -1.0 / max(n, 1) * (ratio * np.log(ratio)).sum()
    return 0.0282 * uicm + 0.2953 * uism + 3.5753 * uiconm


def psnr_ssim(out, ref):
    from skimage.metrics import structural_similarity
    if ref.shape != out.shape:
        ref = cv2.resize(ref, (out.shape[1], out.shape[0]),
                         interpolation=cv2.INTER_AREA)
    mse = ((out.astype(np.float64) - ref) ** 2).mean()
    psnr = 99.0 if mse == 0 else 10 * math.log10(255.0 ** 2 / mse)
    ssim = structural_similarity(out, ref, channel_axis=2, data_range=255)
    return psnr, ssim


_orb = None


def orb(bgr):
    global _orb
    _orb = _orb or cv2.ORB_create(2000)
    return _orb.detectAndCompute(cv2.cvtColor(bgr, cv2.COLOR_BGR2GRAY), None)


def orb_matches(d1, d2):
    """Matches that pass the ratio test, between two frames' descriptors."""
    if d1 is None or d2 is None or len(d1) < 2 or len(d2) < 2:
        return 0
    pairs = cv2.BFMatcher(cv2.NORM_HAMMING).knnMatch(d1, d2, k=2)
    return sum(1 for p in pairs if len(p) == 2 and
               p[0].distance < 0.75 * p[1].distance)


def tile_means(bgr, n=8):
    y = cv2.cvtColor(bgr, cv2.COLOR_BGR2GRAY).astype(np.float32)
    return cv2.resize(y, (n, n), interpolation=cv2.INTER_AREA)


# ---- running ------------------------------------------------------------------

def timed(fn, img, repeat=3):
    best, out = None, None
    for _ in range(repeat):
        t0 = time.perf_counter()
        out = fn(img)
        t = (time.perf_counter() - t0) * 1000
        best = t if best is None else min(best, t)
    return out, best


def run_external(name, cmd, images):
    """images: {filename: bgr}. Returns {filename: bgr}, or None on failure."""
    with tempfile.TemporaryDirectory() as d:
        din, dout = os.path.join(d, "in"), os.path.join(d, "out")
        os.makedirs(din)
        os.makedirs(dout)
        for fn, img in images.items():
            cv2.imwrite(os.path.join(din, fn), img)
        t0 = time.perf_counter()
        r = subprocess.run(cmd.replace("{in}", din).replace("{out}", dout),
                           shell=True)
        ms = (time.perf_counter() - t0) * 1000 / max(1, len(images))
        if r.returncode != 0:
            print("quality: external method %s failed (exit %d)" %
                  (name, r.returncode), file=sys.stderr)
            return None, None
        outs = {}
        for fn, img in images.items():
            o = cv2.imread(os.path.join(dout, fn))
            if o is None:
                print("quality: %s wrote no %s" % (name, fn), file=sys.stderr)
                return None, None
            if o.shape != img.shape:
                o = cv2.resize(o, (img.shape[1], img.shape[0]))
            outs[fn] = o
    return outs, ms


def summarise(rows, keys, out_dir, title, note):
    methods = []
    for r in rows:
        if r["method"] not in methods:
            methods.append(r["method"])
    table = []
    for m in methods:
        mine = [r for r in rows if r["method"] == m]
        row = {"method": m, "n": len(mine)}
        for k in keys:
            v = [r[k] for r in mine if r.get(k) is not None]
            row[k] = round(float(np.mean(v)), 4) if v else None
        table.append(row)
    lines = ["# " + title, "", note, "",
             "| method | n | " + " | ".join(keys) + " |",
             "|---|---|" + "---|" * len(keys)]
    for row in table:
        lines.append("| %s | %d | %s |" % (
            row["method"], row["n"],
            " | ".join("-" if row[k] is None else "%.3f" % row[k]
                       for k in keys)))
    text = "\n".join(lines) + "\n"
    with open(os.path.join(out_dir, "summary.md"), "w") as f:
        f.write(text)
    with open(os.path.join(out_dir, "summary.json"), "w") as f:
        json.dump(table, f, indent=1)
    print(text)


def cmd_images(a):
    os.makedirs(a.out, exist_ok=True)
    names = sorted(n for n in os.listdir(a.data) if n.lower().endswith(EXTS))
    if a.limit:
        names = names[:a.limit]
    if not names:
        sys.exit("quality: no pictures in " + a.data)
    methods = a.methods.split(",") if a.methods else ALL
    externals = dict(e.split("=", 1) for e in a.external)
    keys = ["uciqe", "uiqm", "psnr", "ssim", "orb_keypoints", "ms"]
    rows, inputs, refs = [], {}, {}
    for i, name in enumerate(names):
        img = cv2.imread(os.path.join(a.data, name))
        if img is None:
            continue
        img = even(img)
        stem = os.path.splitext(name)[0] + ".png"
        inputs[stem] = img
        ref = None
        if a.ref:
            for e in EXTS:
                p = os.path.join(a.ref, os.path.splitext(name)[0] + e)
                if os.path.exists(p):
                    ref = even(cv2.imread(p))
                    break
        refs[stem] = ref
        for m in methods:
            if m in EE_METHODS:
                outs, timing = ee_run([img], EE_METHODS[m], still=True,
                                      repeat=5)
                out, ms = outs[0], timing["ms_mean"]
            elif m in PY_METHODS:
                out, ms = timed(PY_METHODS[m], img)
            else:
                sys.exit("quality: no method called " + m)
            rows.append(score(name, m, img, out, ref, ms))
            if a.save:
                os.makedirs(os.path.join(a.out, m), exist_ok=True)
                cv2.imwrite(os.path.join(a.out, m, stem), out)
        print("\r%d/%d" % (i + 1, len(names)), end="", file=sys.stderr)
    print(file=sys.stderr)
    for m, cmd in externals.items():
        outs, ms = run_external(m, cmd, inputs)
        if outs is None:
            continue
        for stem, out in outs.items():
            rows.append(score(stem, m, inputs[stem], out, refs[stem], ms))
    with open(os.path.join(a.out, "per-image.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, ["image", "method", "width", "height"] + keys)
        w.writeheader()
        w.writerows(rows)
    summarise(rows, keys, a.out, "Enhancement on stills: " + a.data,
              "Means over the pictures. uciqe, uiqm: higher is called better "
              "(see the caveats in quality.py). psnr, ssim: against the "
              "reference picture, where there is one. orb_keypoints: of 2000 "
              "asked for. ms: per picture at its own size, on this machine; "
              "an external method's includes its start-up.")


def score(name, method, img, out, ref, ms):
    kp, _ = orb(out)
    row = {"image": name, "method": method, "width": img.shape[1],
           "height": img.shape[0], "uciqe": uciqe(out), "uiqm": uiqm(out),
           "orb_keypoints": len(kp), "ms": ms, "psnr": None, "ssim": None}
    if ref is not None:
        row["psnr"], row["ssim"] = psnr_ssim(out, ref)
    return row


def cmd_video(a):
    os.makedirs(a.out, exist_ok=True)
    cap = cv2.VideoCapture(a.clip)
    fps = cap.get(cv2.CAP_PROP_FPS) or 30.0
    frames = []
    while len(frames) < int(a.seconds * fps):
        ok, fr = cap.read()
        if not ok:
            break
        frames.append(cv2.resize(fr, (1280, 720),
                                 interpolation=cv2.INTER_AREA))
    if len(frames) < 3:
        sys.exit("quality: could not read frames from " + a.clip)
    methods = a.methods.split(",") if a.methods else ALL
    keys = ["flicker", "flicker_vs_input", "flicker_rel_vs_input", "matches",
            "matches_vs_input", "uciqe", "ms_mean", "ms_p99"]
    rows, base = [], {}
    for m in ["identity"] + [x for x in methods if x != "identity"]:
        if m in EE_METHODS:
            outs, timing = ee_run(frames, EE_METHODS[m], still=False, fps=fps)
            ms_mean, ms_p99 = timing["ms_mean"], timing["ms_p99"]
        else:
            outs, times = [], []
            for fr in frames:
                t0 = time.perf_counter()
                outs.append(PY_METHODS[m](fr))
                times.append((time.perf_counter() - t0) * 1000)
            ms_mean = float(np.mean(times))
            ms_p99 = float(np.percentile(times, 99))
        # Flicker: how much each of 8x8 tiles' brightness moves from one
        # frame to the next, in grey levels. Scene motion is in it too,
        # which is why it is also given against the input's own.
        tiles = [tile_means(o) for o in outs]
        flicker = float(np.mean([np.abs(tiles[i] - tiles[i - 1]).mean()
                                 for i in range(1, len(tiles))]))
        # The same against the picture's own contrast (the spread of its
        # tiles): a method that doubles contrast doubles the plain figure
        # for the same scene motion without flickering at all.
        rel = flicker / max(float(np.mean([t.std() for t in tiles])), 1e-6)
        desc = [orb(o)[1] for o in outs[::3]]
        matches = float(np.mean([orb_matches(desc[i - 1], desc[i])
                                 for i in range(1, len(desc))]))
        if m == "identity":
            base = {"flicker": flicker, "matches": matches, "rel": rel}
        rows.append({
            "method": m, "flicker": flicker,
            "flicker_vs_input": flicker / max(base["flicker"], 1e-6),
            "flicker_rel_vs_input": rel / max(base["rel"], 1e-6),
            "matches": matches,
            "matches_vs_input": matches / max(base["matches"], 1e-6),
            "uciqe": float(np.mean([uciqe(o) for o in outs[::10]])),
            "ms_mean": ms_mean, "ms_p99": ms_p99})
        if a.save:
            h, w = outs[0].shape[:2]
            vw = cv2.VideoWriter(os.path.join(a.out, m + ".mp4"),
                                 cv2.VideoWriter_fourcc(*"mp4v"), fps, (w, h))
            for o in outs:
                vw.write(o)
            vw.release()
        print("%s done" % m, file=sys.stderr)
    if methods and "identity" not in methods:
        rows = [r for r in rows if r["method"] != "identity"] or rows
    summarise(rows, keys, a.out,
              "Enhancement on video: %s (%d frames, 1280x720)" %
              (a.clip, len(frames)),
              "flicker: mean change of an 8x8 grid of tile brightnesses "
              "between consecutive frames, in grey levels (lower is "
              "steadier); _vs_input is that over the untouched clip's, so 1.0 "
              "adds none; _rel_ divides each by its own picture's contrast "
              "first, so a contrast gain alone does not count as flicker. "
              "matches: ORB features matched between frames three "
              "apart (higher helps tracking); _vs_input likewise. ms: per "
              "1280x720 frame on this machine, mean and 99th percentile.")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    i = sub.add_parser("images")
    i.add_argument("--data", required=True)
    i.add_argument("--ref")
    i.add_argument("--out", required=True)
    i.add_argument("--methods")
    i.add_argument("--external", action="append", default=[],
                   metavar="NAME=CMD")
    i.add_argument("--limit", type=int, default=0)
    i.add_argument("--save", action="store_true",
                   help="keep every method's pictures")
    v = sub.add_parser("video")
    v.add_argument("--clip", required=True)
    v.add_argument("--out", required=True)
    v.add_argument("--methods")
    v.add_argument("--seconds", type=float, default=20)
    v.add_argument("--save", action="store_true")
    a = ap.parse_args()
    (cmd_images if a.cmd == "images" else cmd_video)(a)


if __name__ == "__main__":
    main()
