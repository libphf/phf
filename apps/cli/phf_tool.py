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

# Load phf.dll from libphf
HERE = os.path.dirname(os.path.abspath(__file__))
DLL_PATH = os.path.abspath(os.path.join(HERE, "..", "..", "libphf", "phf.dll"))

if not os.path.exists(DLL_PATH):
    # Try next to script or in cwd
    DLL_PATH = os.path.abspath("phf.dll")

if not os.path.exists(DLL_PATH):
    raise FileNotFoundError(f"Could not locate phf.dll. Run build.bat in libphf first. Looked in: {DLL_PATH}")

libphf = ctypes.CDLL(DLL_PATH)

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

def encode_image(input_path, output_path=None):
    if not os.path.exists(input_path):
        print(f"Error: File not found: {input_path}")
        return False
        
    if output_path is None:
        base, _ = os.path.splitext(input_path)
        output_path = base + ".phf"

    img_bgr = cv2.imread(input_path, cv2.IMREAD_UNCHANGED)
    if img_bgr is None:
        print(f"Error: Could not read image {input_path}")
        return False

    has_alpha = (img_bgr.ndim == 3 and img_bgr.shape[2] == 4)
    channels = 4 if has_alpha else 3
    h, w = img_bgr.shape[:2]
    
    if channels == 4:
        rgb = cv2.cvtColor(img_bgr, cv2.COLOR_BGRA2RGBA)
    else:
        rgb = cv2.cvtColor(img_bgr, cv2.COLOR_BGR2RGB)
    
    rgb = np.ascontiguousarray(rgb)
    raw_bytes = w * h * channels
    
    print(f"Encoding: {input_path}")
    print(f"  Dimensions: {w}x{h} ({channels} channels) | Raw: {raw_bytes / (1024*1024):.2f} MB")
    
    out_sz = ctypes.c_size_t(0)
    t0 = time.perf_counter()
    ptr = libphf.phf_encode_memory(rgb.ctypes.data, w, h, channels, ctypes.byref(out_sz))
    enc_time = time.perf_counter() - t0
    
    if not ptr:
        print("Error: PHF encoding failed.")
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

def decode_image(input_path, output_path=None):
    if not os.path.exists(input_path):
        print(f"Error: File not found: {input_path}")
        return False
        
    if output_path is None:
        base, _ = os.path.splitext(input_path)
        output_path = base + ".png"

    with open(input_path, "rb") as f:
        phf_bytes = f.read()

    info = PHFInfo()
    rc = libphf.phf_probe_memory(phf_bytes, len(phf_bytes), ctypes.byref(info))
    if rc != 0:
        print(f"Error: Invalid or corrupt PHF file (error code {rc})")
        return False

    w, h, ch = info.width, info.height, info.channels
    dst = np.zeros((h, w, ch), dtype=np.uint8)
    
    print(f"Decoding: {input_path}")
    print(f"  Dimensions: {w}x{h} ({ch} channels) | PHF Size: {len(phf_bytes)/(1024*1024):.2f} MB")
    
    t0 = time.perf_counter()
    dec_rc = libphf.phf_decode_memory(phf_bytes, len(phf_bytes), dst.ctypes.data, w, h, ch)
    dec_time = time.perf_counter() - t0
    
    if dec_rc != 0:
        print(f"Error: Decode failed with code {dec_rc}")
        return False
        
    raw_bytes = w * h * ch
    throughput = (raw_bytes / (1024*1024)) / dec_time
    
    if ch == 4:
        bgr = cv2.cvtColor(dst, cv2.COLOR_RGBA2BGRA)
    else:
        bgr = cv2.cvtColor(dst, cv2.COLOR_RGB2BGR)
        
    cv2.imwrite(output_path, bgr)
    print(f"  -> Saved: {output_path}")
    print(f"  -> Speed: {dec_time*1000:.1f} ms ({throughput:.1f} MB/s)")
    return True

def info_image(input_path):
    if not os.path.exists(input_path):
        print(f"Error: File not found: {input_path}")
        return
        
    with open(input_path, "rb") as f:
        phf_bytes = f.read()

    info = PHFInfo()
    rc = libphf.phf_probe_memory(phf_bytes, len(phf_bytes), ctypes.byref(info))
    if rc != 0:
        print(f"Error: Not a valid PHF file (error {rc})")
        return
        
    raw_sz = info.width * info.height * info.channels
    ratio = raw_sz / len(phf_bytes)
    savings = (1.0 - len(phf_bytes) / raw_sz) * 100.0
    
    print("=" * 50)
    print("PHF File Information:")
    print("=" * 50)
    print(f"File:            {input_path}")
    print(f"Resolution:      {info.width} x {info.height} pixels")
    print(f"Channels:        {info.channels} ({'RGBA' if info.channels == 4 else 'RGB'})")
    print(f"Raw Size:        {raw_sz / (1024*1024):.2f} MB ({raw_sz} bytes)")
    print(f"PHF Size:        {len(phf_bytes) / (1024*1024):.2f} MB ({len(phf_bytes)} bytes)")
    print(f"Compression:     {ratio:.2f}x ({savings:.1f}% space savings)")
    print(f"Lossless:        100% Bit-Exact (Strict)")
    print("=" * 50)

def main():
    if len(sys.argv) < 2:
        print("PHF Utility v1.0 — Pereira-Huffman Format")
        print("Usage:")
        print("  python phf_tool.py encode <input.jpg|png> [output.phf]")
        print("  python phf_tool.py decode <input.phf> [output.png]")
        print("  python phf_tool.py info   <input.phf>")
        return

    cmd = sys.argv[1].lower()
    if cmd == "encode" and len(sys.argv) >= 3:
        out = sys.argv[3] if len(sys.argv) >= 4 else None
        encode_image(sys.argv[2], out)
    elif cmd == "decode" and len(sys.argv) >= 3:
        out = sys.argv[3] if len(sys.argv) >= 4 else None
        decode_image(sys.argv[2], out)
    elif cmd == "info" and len(sys.argv) >= 3:
        info_image(sys.argv[2])
    else:
        print(f"Unknown command: {cmd}")

if __name__ == "__main__":
    main()
