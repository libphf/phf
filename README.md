# PHF — Pereira-Huffman Format
### *The Pure High-Fidelity Lossless Image Format*

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Status: Stable](https://img.shields.io/badge/Status-100%25%20Lossless-green.svg)](SPEC.md)
[![Platform: Cross-Platform](https://img.shields.io/badge/Platform-x86__64%20%7C%20ARM64%20%7C%20WASM-orange.svg)](#building)

**PHF** (*Pereira-Huffman Format* / *Pure High-Fidelity Format*) is a next-generation, high-performance, strictly lossless image codec.

By combining order-1 context-adaptive Huffman coding (N-Huffman) with SIMD-vectorized Median Edge Detection (MED) and Package-Merge canonical length-limiting, PHF delivers **higher compression ratios than WebP Lossless** while operating at **300 to 400 MB/s** throughput.

---

## Benchmark: High-Resolution Photographic Dataset (Unsplash)

Tested across 7 ultra-high-resolution real-world photographs (from 4.4 MP up to 36.2 MP Full-Frame DSLRs), totaling **370.4 MB of raw 24-bit RGB pixel data**:

| Codec | Total Size | Compression Ratio | Space Savings | Total Encode Time | Encode Throughput | Total Decode Time | Decode Throughput | Lossless Verified |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| 🥇 **PHF** | **95.2 MB** | **3.89×** | **74.3%** | **1.28 s** | **290 MB/s** | **0.92 s** | **403 MB/s** | **100% Bit-Exact (0 errors)** |
| 🥈 **WebP-LL** | 104.3 MB | 3.55× | 71.8% | 47.26 s | 8 MB/s | 2.57 s | 144 MB/s | 100% Bit-Exact (0 errors) |
| 🥉 **PNG-6** | 142.6 MB | 2.60× | 61.5% | 24.90 s | 15 MB/s | 1.66 s | 223 MB/s | 100% Bit-Exact (0 errors) |
| **PNG-1** | 155.4 MB | 2.38× | 58.1% | 6.42 s | 58 MB/s | 1.58 s | 234 MB/s | 100% Bit-Exact (0 errors) |
| **QOI** | 169.4 MB | 2.19× | 54.3% | 0.67 s | 551 MB/s | 0.46 s | 808 MB/s | 100% Bit-Exact (0 errors) |

### Key Advantages:
* **Dense Compression:** Produces smaller files than Google's WebP Lossless (**9.1 MB smaller**) and is **33% smaller than standard PNG-6** (47.4 MB smaller).
* **Extreme Encoding Speed:** Encodes **19.5× faster than PNG-6** and **37.0× faster than WebP Lossless**.
* **High-Speed Decoding:** Decodes at **> 400 MB/s**, nearly 2× faster than PNG and 2.8× faster than WebP.
* **100% Mathematically Lossless:** Pixel-perfect reconstruction: $\text{PSNR} = \infty$, $\text{MSE} = 0$, maximum pixel error $= 0$.

---

## Repository Structure

```text
phf/
├── MANIFESTO.md          # Origins, vision, and tribute to David Huffman
├── SPEC.md               # Binary format specification v1.0
├── LICENSE               # Permissive MIT License
│
├── libphf/               # Core C Library
│   ├── include/
│   │   └── phf.h         # Clean public C API
│   ├── src/
│   │   ├── phf.c         # Encoder, decoder, and container logic
│   │   ├── huff.c / .h   # Canonical 15-bit Package-Merge Huffman tree
│   │   ├── bitio.h       # Branchless bitstream reader and writer
│   │   └── phf_simd.h    # SIMD intrinsics (SSE2, AVX2, ARM NEON, WASM)
│   ├── build.bat         # Native Windows MSVC build (phf.dll / phf.lib)
│   └── build_wasm.bat    # WebAssembly build (phf.js / phf.wasm)
│
└── apps/
    ├── cli/
    │   └── phf_tool.py   # CLI tool for encoding, decoding, and inspecting PHF images
    └── web/
        ├── phf-view.js   # <phf-img> Web Component
        └── viewer.html   # Web-based interactive viewer with drag & drop
```

---

## Building libphf

### Windows (MSVC):
```cmd
cd libphf
build.bat
```
Produces `phf.dll` and `phf.lib`.

### WebAssembly (Emscripten):
```cmd
cd libphf
build_wasm.bat
```
Produces `phf.js` and `phf.wasm` (SIMD128 enabled).

---

## Quickstart (CLI)

```bash
# Encode an image to PHF
python apps/cli/phf_tool.py encode photo.jpg photo.phf

# Decode a PHF image back to PNG
python apps/cli/phf_tool.py decode photo.phf photo.png

# Display image metadata and compression statistics
python apps/cli/phf_tool.py info photo.phf
```

---

## C API Example

```c
#include "phf.h"

// Encode RGB image to a .phf file
int err = phf_write_file("output.phf", rgb_buffer, width, height, 3);

// Read and decode a .phf file from disk
phf_info_t info;
uint8_t *decoded_rgb = phf_read_file("output.phf", &info);

if (decoded_rgb) {
    // Process pixels: info.width, info.height, info.channels
    phf_free(decoded_rgb);
}
```

---

## Authors & Acknowledgments

- **Ygor D. L. Pereira** — Original N-Huffman concept and academic research (FA7, 2008).
- **Antigravity (Google DeepMind)** — Codec systems engineering, SIMD vectorization, and WebAssembly architecture.
- **David A. Huffman** (1925–1999) — In enduring tribute to the father of minimum redundancy coding.

---

## License

This project is licensed under the **MIT License** — see the [LICENSE](LICENSE) file for details.
