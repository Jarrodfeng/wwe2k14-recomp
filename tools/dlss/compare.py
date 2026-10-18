"""Session 24 (DLSS): exact quality comparison of replay frames rendered in several arms (tools\\dlss\\s24_frames.ps1).

For every guest frame present in the reference arm and a test arm: PSNR and SSIM (luma) over the whole picture and
inside the wrestler mask (pixels where the reference differs from the mask arm, rendered with the character shaders
skipped; dilated). Arms rendered smaller than the reference (e.g. native 720p) are also scored as plain upscales
(bilinear, bicubic: PIL, i.e. what a screen scaler or a simple resize does).

Usage: py tools\\dlss\\compare.py TAG [--ref native2] [--mask mask2] [--low native1] [--arms a,b] [--crops F,F,...]
       [--groups NAME=A-B,...] [--out DIR]
Prints a table per frame group and writes out\\dlss_s24\\frames\\TAG\\compare.csv; --crops writes side-by-side crops
(reference | plain bicubic | each test arm) around the wrestlers to out\\dlss_s24\\crops\\TAG\\ (local only).
"""
import argparse
import csv
import glob
import os
import re

import numpy as np
from PIL import Image
from scipy import ndimage

ROOT = r"C:\Users\Jarrod\wwe2k14-recomp\out\dlss_s24"
DEFAULT_GROUPS = ("brock_still=2400-2800,bryan_pan=5771-5891,heavy_shot=7537-7867,match=10000-10800,"
                  "slam=10850-10872,takedown=11262-11278,match_late=11600-11800")


def load_arm(tag, arm):
    files = glob.glob(os.path.join(ROOT, "frames", tag, arm, "*_clip_h_f*.png"))
    out = {}
    for f in files:
        m = re.search(r"_f(\d+)\.png$", f)
        if m:
            out[int(m.group(1))] = f
    return out


def luma(a):
    return a[..., 0] * 0.299 + a[..., 1] * 0.587 + a[..., 2] * 0.114


def psnr(a, b, mask=None):
    d = (a - b) ** 2
    if mask is not None:
        if mask.sum() < 64:
            return float("nan")
        mse = d[mask].mean()
    else:
        mse = d.mean()
    return float("inf") if mse <= 0 else 10 * np.log10(255.0 ** 2 / mse)


def ssim_map(x, y):
    # Gaussian-window SSIM on luma (sigma 1.5, the usual constants), 0..255.
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    mx, my = ndimage.gaussian_filter(x, 1.5), ndimage.gaussian_filter(y, 1.5)
    sxx = ndimage.gaussian_filter(x * x, 1.5) - mx * mx
    syy = ndimage.gaussian_filter(y * y, 1.5) - my * my
    sxy = ndimage.gaussian_filter(x * y, 1.5) - mx * my
    return ((2 * mx * my + c1) * (2 * sxy + c2)) / ((mx * mx + my * my + c1) * (sxx + syy + c2))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tag")
    ap.add_argument("--ref", default="native2")
    ap.add_argument("--mask", default="mask2")
    ap.add_argument("--low", default="native1", help="arm rendered small: scored as bilinear / bicubic upscales")
    ap.add_argument("--arms", default="", help="test arms (default: every other arm folder)")
    ap.add_argument("--groups", default=DEFAULT_GROUPS)
    ap.add_argument("--crops", default="", help="guest frames to crop (comma-separated)")
    ap.add_argument("--crop-size", type=int, default=480, help="crop width/height in reference pixels")
    ap.add_argument("--ref-tag", default="", help="take the reference, mask and low arms from this batch")
    ap.add_argument("--ref-resize", default="", help="WxH: resample the reference to this size (Lanczos), e.g. a "
                    "native 4K render downscaled to 2560x1440 = a supersampled, anti-aliased reference")
    ap.add_argument("--mask-base", default="", help="TAG/ARM the mask arm is compared with (default: the reference)")
    a = ap.parse_args()
    tag_dir = os.path.join(ROOT, "frames", a.tag)
    ref_tag = a.ref_tag or a.tag
    ref = load_arm(ref_tag, a.ref)
    mask_arm = load_arm(ref_tag, a.mask) if a.mask else {}
    arms = [x for x in a.arms.split(",") if x] or sorted(
        d for d in os.listdir(tag_dir) if os.path.isdir(os.path.join(tag_dir, d)) and d not in (a.ref, a.mask))
    loaded = {arm: load_arm(a.tag, arm) for arm in arms}
    if a.ref_tag and a.low and a.low not in loaded:
        arms.append(a.low)
        loaded[a.low] = load_arm(ref_tag, a.low)
    groups = []
    for g in a.groups.split(","):
        name, rng = g.split("=")
        lo, hi = rng.split("-")
        groups.append((name, int(lo), int(hi)))
    crops = {int(x) for x in a.crops.split(",") if x}
    rows = []
    mask_base = load_arm(*a.mask_base.split("/")) if a.mask_base else None
    for f in sorted(ref):
        ref_img = Image.open(ref[f]).convert("RGB")
        if a.ref_resize:
            rw, rh = (int(v) for v in a.ref_resize.lower().split("x"))
            ref_img = ref_img.resize((rw, rh), Image.LANCZOS)
        r = np.asarray(ref_img, np.float64)
        h, w = r.shape[:2]
        mask = None
        if f in mask_arm and (mask_base is None or f in mask_base):
            m = np.asarray(Image.open(mask_arm[f]).convert("RGB"), np.float64)
            base = r if mask_base is None else np.asarray(Image.open(mask_base[f]).convert("RGB"), np.float64)
            mask = np.abs(base - m).max(axis=2) > 12
            mask = ndimage.binary_dilation(mask, iterations=3)
        rl = luma(r)
        variants = {}
        for arm in arms:
            if f not in loaded[arm]:
                continue
            im = Image.open(loaded[arm][f]).convert("RGB")
            if im.size != (w, h):
                if arm == a.low:
                    variants[arm + ":bilinear"] = im.resize((w, h), Image.BILINEAR)
                    variants[arm + ":bicubic"] = im.resize((w, h), Image.BICUBIC)
                continue
            variants[arm] = im
        crop_imgs = []
        for name, im in variants.items():
            t = np.asarray(im, np.float64)
            sm = ssim_map(rl, luma(t))
            row = {"frame": f, "arm": name, "psnr": psnr(r, t), "ssim": float(sm.mean()),
                   "mask_px": int(mask.sum()) if mask is not None else 0,
                   "psnr_mask": psnr(r, t, mask) if mask is not None else float("nan"),
                   "ssim_mask": float(sm[mask].mean()) if mask is not None and mask.sum() >= 64 else float("nan")}
            rows.append(row)
            crop_imgs.append((name, im))
        if f in crops and crop_imgs:
            if mask is not None and mask.any():
                ys, xs = np.nonzero(mask)
                cy, cx = int(np.median(ys)), int(np.median(xs))
            else:
                cy, cx = h // 2, w // 2
            s = a.crop_size
            x0, y0 = max(0, min(w - s, cx - s // 2)), max(0, min(h - s, cy - s // 2))
            box = (x0, y0, x0 + s, y0 + s)
            panels = [("reference " + a.ref, ref_img)] + crop_imgs
            panels = [(n, im) for n, im in panels if not n.endswith(":bilinear")]
            sheet = Image.new("RGB", (len(panels) * (s + 8), s), (255, 255, 255))
            for i, (n, im) in enumerate(panels):
                sheet.paste(im.crop(box), (i * (s + 8), 0))
            od = os.path.join(ROOT, "crops", a.tag)
            os.makedirs(od, exist_ok=True)
            sheet.save(os.path.join(od, f"f{f}_" + "_".join(n.replace(":", "-").replace(" ", "-") for n, _ in panels)
                                    + ".png"))
    with open(os.path.join(tag_dir, "compare.csv"), "w", newline="") as fh:
        wr = csv.DictWriter(fh, fieldnames=["frame", "arm", "psnr", "ssim", "mask_px", "psnr_mask", "ssim_mask"])
        wr.writeheader()
        for row in rows:
            wr.writerow(row)
    names = sorted({row["arm"] for row in rows})
    print(f"reference {a.ref}, mask {a.mask}: {len(ref)} frames; PSNR dB (higher = closer to native), SSIM")
    for gname, lo, hi in groups:
        sel = [row for row in rows if lo <= row["frame"] <= hi]
        if not sel:
            continue
        frames = sorted({row["frame"] for row in sel})
        print(f"\n{gname} (frames {frames[0]}..{frames[-1]}, {len(frames)}):")
        print(f"  {'arm':28s} {'PSNR':>7s} {'SSIM':>7s} {'PSNR wr':>8s} {'SSIM wr':>8s}")
        for n in names:
            rs = [row for row in sel if row["arm"] == n]
            if not rs:
                continue
            def mean(k):
                v = [row[k] for row in rs if np.isfinite(row[k])]
                return float(np.mean(v)) if v else float("nan")
            print(f"  {n:28s} {mean('psnr'):7.2f} {mean('ssim'):7.4f} {mean('psnr_mask'):8.2f} {mean('ssim_mask'):8.4f}")


if __name__ == "__main__":
    main()
