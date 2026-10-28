#!/usr/bin/env python3
"""
PHF (Pereira-Huffman Format) Command-Line Utility
Author: Ygor Pereira & Antigravity (Google DeepMind)
License: MIT
"""
import sys
import os
import time
import ctypes
import cv2
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

if sys.platform == "win32":
    LIB_NAMES = ["phf.dll"]
elif sys.platform == "darwin":
    LIB_NAMES = ["libphf.dylib"]
else:
    LIB_NAMES = ["libphf.so"]

PHF_FLAG_ALPHA = 0x01
PHF_FLAG_MED = 0x02
PHF_FLAG_DECORR = 0x04
PHF_FLAG_CRC32 = 0x08

PHF_ERRORS = {
    -1: "invalid argument",
    -2: "not a PHF file (bad magic)",
    -3: "unsupported PHF version",
    -4: "dimension mismatch",
    -5: "file is truncated",
    -6: "stride smaller than channel count",
    -7: "corrupt payload",
    -8: "checksum mismatch (file is corrupted)",
    -9: "out of memory",
    -10: "I/O error",
}


def find_library():
    """PHF_LIB env var, then CMake build dirs, then libphf/ (build.bat), then cwd."""
    env = os.environ.get("PHF_LIB")
    if env:
        return [env]
    dirs = [
        os.path.join(ROOT, "build"),
        os.path.join(ROOT, "build", "Release"),
        os.path.join(ROOT, "libphf"),
        os.getcwd(),
    ]
    return [os.path.join(d, n) for d in dirs for n in LIB_NAMES]


def load_library():
    candidates = find_library()
    for path in candidates:
        if os.path.exists(path):
            return ctypes.CDLL(path)
    sys.exit("Error: could not locate the libphf shared library. Build it with\n"
             "  cmake -S . -B build && cmake --build build --config Release\n"
             "or set PHF_LIB to its path. Looked in:\n  " + "\n  ".join(candidates))


libphf = load_library()

# Define ctypes prototypes
class PHFInfo(ctypes.Structure):
    _fields_ = [
        ("width", ctypes.c_uint32),
        ("height", ctypes.c_uint32),
        ("channels", ctypes.c_uint8),
        ("flags", ctypes.c_uint8),
        ("payload_size", ctypes.c_size_t),
    ]

libphf.phf_probe_memory.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(PHFInfo)]
libphf.phf_probe_memory.restype = ctypes.c_int

libphf.phf_encode_memory.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint8, ctypes.POINTER(ctypes.c_size_t)]
libphf.phf_encode_memory.restype = ctypes.c_void_p

libphf.phf_decode_memory.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint8]
libphf.phf_decode_memory.restype = ctypes.c_int

libphf.phf_free.argtypes = [ctypes.c_void_p]
libphf.phf_free.restype = None


def error_text(rc):
    return f"{PHF_ERRORS.get(rc, 'unknown error')} (code {rc})"


def load_rgb(input_path):
    """Returns (pixels, channels) as a contiguous 8-bit RGB/RGBA array, or (None, reason)."""
    img = cv2.imread(input_path, cv2.IMREAD_UNCHANGED)
    if img is None:
        return None, "could not read image"
    if img.dtype != np.uint8:
        # PHF stores 8 bits per channel; truncating would silently lose data.
        return None, f"{img.dtype} samples are not supported (PHF is 8 bits per channel)"
    if img.ndim == 2:
        # Grayscale is stored as RGB (R = G = B); decoding yields a 3-channel image.
        return np.ascontiguousarray(cv2.cvtColor(img, cv2.COLOR_GRAY2RGB)), 3
    if img.shape[2] == 4:
        return np.ascontiguousarray(cv2.cvtColor(img, cv2.COLOR_BGRA2RGBA)), 4
    if img.shape[2] == 3:
        return np.ascontiguousarray(cv2.cvtColor(img, cv2.COLOR_BGR2RGB)), 3
    return None, f"unsupported channel count {img.shape[2]}"


def encode_image(input_path, output_path=None):
    if not os.path.exists(input_path):
        print(f"Error: File not found: {input_path}")
        return False

    if output_path is None:
        base, _ = os.path.splitext(input_path)
        output_path = base + ".phf"

    rgb, channels = load_rgb(input_path)
    if rgb is None:
        print(f"Error: {input_path}: {channels}")
        return False
    h, w = rgb.shape[:2]
    raw_bytes = w * h * channels

    print(f"Encoding: {input_path}")
    print(f"  Dimensions: {w}x{h} ({channels} channels) | Raw: {raw_bytes / (1024*1024):.2f} MB")

    out_sz = ctypes.c_size_t(0)
    t0 = time.perf_counter()
    ptr = libphf.phf_encode_memory(rgb.ctypes.data, w, h, channels, ctypes.byref(out_sz))
    enc_time = time.perf_counter() - t0

    if not ptr:
        print("Error: PHF encoding failed (max 65535 px per side and 67,108,864 px total).")
        return False

    phf_bytes = ctypes.string_at(ptr, out_sz.value)
    libphf.phf_free(ptr)

    with open(output_path, "wb") as f:
        f.write(phf_bytes)

    comp_bytes = len(phf_bytes)
    ratio = raw_bytes / comp_bytes
    savings = (1.0 - comp_bytes / raw_bytes) * 100.0
    throughput = (raw_bytes / (1024*1024)) / enc_time

    print(f"  -> Saved: {output_path}")
    print(f"  -> Size: {comp_bytes / (1024*1024):.2f} MB (Ratio: {ratio:.2f}x, Space Savings: {savings:.1f}%)")
    print(f"  -> Speed: {enc_time*1000:.1f} ms ({throughput:.1f} MB/s)")
    return True


def probe(phf_bytes):
    info = PHFInfo()
    rc = libphf.phf_probe_memory(phf_bytes, len(phf_bytes), ctypes.byref(info))
    return rc, info


def decode_image(input_path, output_path=None):
    if not os.path.exists(input_path):
        print(f"Error: File not found: {input_path}")
        return False

    if output_path is None:
        base, _ = os.path.splitext(input_path)
        output_path = base + ".png"

    with open(input_path, "rb") as f:
        phf_bytes = f.read()

    rc, info = probe(phf_bytes)
    if rc != 0:
        print(f"Error: Invalid PHF file: {error_text(rc)}")
        return False

    w, h, ch = info.width, info.height, info.channels
    if ch not in (3, 4):
        print(f"Error: Invalid PHF file: unsupported channel count {ch}")
        return False
    dst = np.zeros((h, w, ch), dtype=np.uint8)

    print(f"Decoding: {input_path}")
    print(f"  Dimensions: {w}x{h} ({ch} channels) | PHF Size: {len(phf_bytes)/(1024*1024):.2f} MB")

    t0 = time.perf_counter()
    dec_rc = libphf.phf_decode_memory(phf_bytes, len(phf_bytes), dst.ctypes.data, w, h, ch)
    dec_time = time.perf_counter() - t0

    if dec_rc != 0:
        print(f"Error: Decode failed: {error_text(dec_rc)}")
        return False

    raw_bytes = w * h * ch
    throughput = (raw_bytes / (1024*1024)) / dec_time

    if ch == 4:
        bgr = cv2.cvtColor(dst, cv2.COLOR_RGBA2BGRA)
    else:
        bgr = cv2.cvtColor(dst, cv2.COLOR_RGB2BGR)

    if not cv2.imwrite(output_path, bgr):
        print(f"Error: Could not write {output_path}")
        return False
    print(f"  -> Saved: {output_path}")
    print(f"  -> Speed: {dec_time*1000:.1f} ms ({throughput:.1f} MB/s)")
    return True


def info_image(input_path):
    if not os.path.exists(input_path):
        print(f"Error: File not found: {input_path}")
        return False

    with open(input_path, "rb") as f:
        phf_bytes = f.read()

    rc, info = probe(phf_bytes)
    if rc != 0:
        print(f"Error: Not a valid PHF file: {error_text(rc)}")
        return False

    raw_sz = info.width * info.height * info.channels
    ratio = raw_sz / len(phf_bytes) if raw_sz else 0.0
    savings = (1.0 - len(phf_bytes) / raw_sz) * 100.0 if raw_sz else 0.0

    flag_names = [name for bit, name in ((PHF_FLAG_ALPHA, "alpha"), (PHF_FLAG_MED, "MED"),
                                         (PHF_FLAG_DECORR, "decorrelation"), (PHF_FLAG_CRC32, "CRC-32"))
                  if info.flags & bit]

    # Full decode to report whether the file is actually intact.
    integrity = "unsupported channel count"
    ok = False
    if info.channels in (3, 4):
        dst = np.empty((info.height, info.width, info.channels), dtype=np.uint8)
        dec_rc = libphf.phf_decode_memory(phf_bytes, len(phf_bytes), dst.ctypes.data,
                                          info.width, info.height, info.channels)
        ok = dec_rc == 0
        if ok:
            integrity = "decodes OK, CRC-32 verified" if info.flags & PHF_FLAG_CRC32 \
                else "decodes OK (no checksum in file: corruption cannot be ruled out)"
        else:
            integrity = f"FAILED: {error_text(dec_rc)}"

    print("=" * 50)
    print("PHF File Information:")
    print("=" * 50)
    print(f"File:            {input_path}")
    print(f"Resolution:      {info.width} x {info.height} pixels")
    print(f"Channels:        {info.channels} ({'RGBA' if info.channels == 4 else 'RGB'})")
    print(f"Flags:           0x{info.flags:02x} ({', '.join(flag_names) or 'none'})")
    print(f"Raw Size:        {raw_sz / (1024*1024):.2f} MB ({raw_sz} bytes)")
    print(f"PHF Size:        {len(phf_bytes) / (1024*1024):.2f} MB ({len(phf_bytes)} bytes)")
    print(f"Compression:     {ratio:.2f}x ({savings:.1f}% space savings)")
    print(f"Integrity:       {integrity}")
    print("=" * 50)
    return ok


def main():
    if len(sys.argv) < 3:
        print("PHF Utility v1.0 — Pereira-Huffman Format")
        print("Usage:")
        print("  python phf_tool.py encode <input.jpg|png> [output.phf]")
        print("  python phf_tool.py decode <input.phf> [output.png]")
        print("  python phf_tool.py info   <input.phf>")
        return 2

    cmd = sys.argv[1].lower()
    out = sys.argv[3] if len(sys.argv) >= 4 else None
    if cmd == "encode":
        ok = encode_image(sys.argv[2], out)
    elif cmd == "decode":
        ok = decode_image(sys.argv[2], out)
    elif cmd == "info":
        ok = info_image(sys.argv[2])
    else:
        print(f"Unknown command: {cmd}")
        return 2
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
