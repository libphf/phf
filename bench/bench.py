#!/usr/bin/env python3
"""
Reproducible lossless benchmark: PHF vs PNG (levels 1 and 6) vs WebP Lossless.

Usage:
  python bench/bench.py [--repeat N] image1.png image2.jpg ...

Every codec runs in this same process on the same decoded 8-bit RGB/RGBA pixels,
and every decode is checked bit-exact against the source. PNG and WebP come from
OpenCV (libpng / libwebp). PHF uses the libphf shared library, so its thread count
depends on how libphf was built (CMake prints "OpenMP enabled" when it is used)
and on OMP_NUM_THREADS. Set OMP_NUM_THREADS=1 for a single-threaded comparison.
"""
import argparse
import ctypes
import os
import platform
import sys
import time

import cv2
import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "apps", "cli"))
import phf_tool  # noqa: E402  (loads libphf)

lib = phf_tool.libphf


def phf_encode(rgb, ch):
    h, w = rgb.shape[:2]
    out_sz = ctypes.c_size_t(0)
    ptr = lib.phf_encode_memory(rgb.ctypes.data, w, h, ch, ctypes.byref(out_sz))
    if not ptr:
        raise RuntimeError("phf_encode_memory failed")
    data = ctypes.string_at(ptr, out_sz.value)
    lib.phf_free(ptr)
    return data


def phf_decode(data, w, h, ch):
    dst = np.empty((h, w, ch), dtype=np.uint8)
    rc = lib.phf_decode_memory(data, len(data), dst.ctypes.data, w, h, ch)
    if rc != 0:
        raise RuntimeError(phf_tool.error_text(rc))
    return dst


def cv_codec(ext, params):
    def enc(bgr, ch):
        ok, buf = cv2.imencode(ext, bgr, params)
        if not ok:
            raise RuntimeError(f"cv2.imencode({ext}) failed")
        return buf.tobytes()

    def dec(data, w, h, ch):
        return cv2.imdecode(np.frombuffer(data, np.uint8), cv2.IMREAD_UNCHANGED)
    return enc, dec


CODECS = [
    ("PHF", "rgb", phf_encode, phf_decode),
    ("WebP-LL", "bgr", *cv_codec(".webp", [cv2.IMWRITE_WEBP_QUALITY, 101])),
    ("PNG-6", "bgr", *cv_codec(".png", [cv2.IMWRITE_PNG_COMPRESSION, 6])),
    ("PNG-1", "bgr", *cv_codec(".png", [cv2.IMWRITE_PNG_COMPRESSION, 1])),
]


def best_time(fn, repeat):
    best, result = float("inf"), None
    for _ in range(repeat):
        t0 = time.perf_counter()
        result = fn()
        best = min(best, time.perf_counter() - t0)
    return best, result


def compare(src, out, ch):
    """'exact', 'alpha0' (differs only in the color of fully transparent pixels,
    which WebP Lossless drops unless run in its `exact` mode), or 'mismatch'."""
    if out is None or out.shape != src.shape:
        return "mismatch"
    diff = (out != src).any(axis=2)
    if not diff.any():
        return "exact"
    if ch == 4 and (out[..., 3] == src[..., 3]).all() and not (diff & (src[..., 3] != 0)).any():
        return "alpha0"
    return "mismatch"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("images", nargs="+")
    ap.add_argument("--repeat", type=int, default=3, help="runs per measurement; the fastest is kept")
    args = ap.parse_args()

    totals = {name: {"size": 0, "enc": 0.0, "dec": 0.0, "errors": 0, "alpha0": 0} for name, *_ in CODECS}
    raw_total = 0
    used = 0

    for path in args.images:
        rgb, ch = phf_tool.load_rgb(path)
        if rgb is None:
            print(f"skip {path}: {ch}", file=sys.stderr)
            continue
        h, w = rgb.shape[:2]
        bgr = cv2.cvtColor(rgb, cv2.COLOR_RGBA2BGRA if ch == 4 else cv2.COLOR_RGB2BGR)
        raw_total += w * h * ch
        used += 1
        print(f"{path}: {w}x{h}x{ch}", file=sys.stderr)

        for name, space, enc, dec in CODECS:
            src = rgb if space == "rgb" else bgr
            t_enc, data = best_time(lambda: enc(src, ch), args.repeat)
            t_dec, out = best_time(lambda: dec(data, w, h, ch), args.repeat)
            verdict = compare(src, out, ch)
            if verdict == "alpha0":
                totals[name]["alpha0"] += 1
            elif verdict != "exact":
                totals[name]["errors"] += 1
            totals[name]["size"] += len(data)
            totals[name]["enc"] += t_enc
            totals[name]["dec"] += t_dec

    if not used:
        sys.exit("no usable images")

    mb = raw_total / 1e6
    print()
    print(f"{used} image(s), {mb:.1f} MB raw | {platform.platform()} | {platform.processor() or platform.machine()}")
    print(f"OMP_NUM_THREADS={os.environ.get('OMP_NUM_THREADS', 'unset')} | OpenCV {cv2.__version__} | best of {args.repeat}")
    print()
    print("| Codec | Total Size | Ratio | Encode | Encode MB/s | Decode | Decode MB/s | Bit-exact |")
    print("| :--- | ---: | ---: | ---: | ---: | ---: | ---: | :---: |")
    for name, *_ in sorted(CODECS, key=lambda c: totals[c[0]]["size"]):
        t = totals[name]
        if t["errors"]:
            exact = f"NO ({t['errors']} image(s))"
        elif t["alpha0"]:
            exact = f"yes* ({t['alpha0']} image(s))"
        else:
            exact = "yes"
        print(f"| {name} | {t['size'] / 1e6:.2f} MB | {raw_total / t['size']:.2f}x | {t['enc']:.2f} s | "
              f"{mb / t['enc']:.0f} | {t['dec']:.2f} s | {mb / t['dec']:.0f} | "
              f"{exact} |")
    if any(t["alpha0"] for t in totals.values()):
        print()
        print("\\* pixels identical except the RGB of fully transparent (alpha = 0) pixels.")


if __name__ == "__main__":
    main()
