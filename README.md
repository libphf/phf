# PHF — Pure High-Fidelity Format
### *Next-Generation Ultra-Fast Lossless Image Format*

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Status: Stable](https://img.shields.io/badge/Status-100%25%20Lossless-green.svg)](SPEC.md)
[![Platform: Cross-Platform](https://img.shields.io/badge/Platform-x86__64%20%7C%20ARM64%20%7C%20WASM-orange.svg)](#building)

**PHF** (**P**ure **H**igh-Fidelity **F**ormat, with roots in the **P**ereira-**H**uffman **F**ormat) is a next-generation, high-performance, strictly lossless image codec.

By combining order-1 context-adaptive Huffman coding (N-Huffman) with SIMD-vectorized Median Edge Detection (MED) and length-limited (15-bit) canonical codes, PHF delivered **higher compression ratios than WebP Lossless** at **~300–400 MB/s** in the benchmark below.

PHF has two coding modes, and the encoder can pick between them:

| Mode | Best for | How |
| :--- | :--- | :--- |
| **MED** | photos, camera, video | R−G/G/B−G planes, MED prediction, N-Huffman per plane |
| **Palette** | graphics, screenshots, UI, few colours | each pixel coded whole as an index into the image's colours, with the same N-Huffman model (the monograph's "compression by pixel") |

On graphics the palette mode is about 70–75% smaller than MED and decodes 2–3× faster; on
photos MED wins. Files say which mode they use, so decoders need no option.

---

## Benchmark: High-Resolution Photographic Dataset (Unsplash)

Tested across 7 ultra-high-resolution real-world photographs (from 4.4 MP up to 36.2 MP Full-Frame DSLRs), totaling **370.4 MB of raw 24-bit RGB pixel data**:

> **About these numbers:** they were measured by the author; the CPU, the PHF thread count
> (libphf can use OpenMP for its color planes) and the codec versions were not recorded.
> Re-run the comparison on your own hardware and images with
> [`bench/bench.py`](#testing--benchmarking), which checks every decode bit-exact.

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

## Built for Streaming & Real-Time Throughput

Traditional lossless image formats like PNG and WebP Lossless were designed solely as static storage containers. Their compression pipelines are too computationally heavy to be used in real-time network streaming pipelines.

Because PHF inherits the architecture of the **NHF3 video streaming engine**, it is natively engineered for live network transmission, camera sensor capture, and low-latency display:

| Streaming Metric | PNG-6 | WebP-LL | 🥇 PHF |
| :--- | :---: | :---: | :---: |
| **Encode Latency (1080p 24-bit, 6.2 MB)** | ~415 ms *(~2 fps)* | ~780 ms *(~1 fps)* | **~21 ms *(~45 fps)*** |
| **Encode Throughput** | 15 MB/s | 8 MB/s | **290 MB/s** |
| **Decode Throughput** | 223 MB/s | 144 MB/s | **> 400 MB/s** |
| **Instant Header Probe** | Variable chunks (IHDR/IDAT) | RIFF header parse | **Fixed 20-byte probe (0 allocations)** |
| **Planar Streams** | ❌ Monolithic Deflate stream | ❌ Monolithic VP8L stream | **✅ Length-prefixed, independently decodable planes** |
| **Inter-Frame Delta (P-frames)** | ❌ Unsupported | ❌ Unsupported | **⚠️ In the NHF3 core engine only (not in `.phf` v1)** |
| **Integrity Check** | ✅ CRC-32 per chunk | ⚠️ None for the image data | **✅ CRC-32 of the payload** |

### Streaming Architectural Strengths:
1. **Low Encode Latency:** at the 290 MB/s measured above, a 1080p frame (6.2 MB) encodes in about 21 ms and a 720p frame (2.8 MB) in about 10 ms: enough for lossless 30 fps at 1080p or 60 fps at 720p. Latencies in the table are frame size ÷ measured throughput.
2. **20-Byte Instant Probe:** Over TCP sockets, HTTP chunked streams, or WebRTC data channels, the receiver only needs the first 20 bytes to immediately ascertain resolution, channel count, and payload size—pre-allocating GPU/Canvas buffers before the payload finishes downloading.
3. **Independent Planes:** the color planes ($R-G$, $G$, $B-G$, and $\alpha$) are stored as separate length-prefixed streams, so each can be entropy-decoded on its own (libphf decodes them in parallel when built with OpenMP). The format allows starting on a plane as soon as it arrives; the current `phf_decode_memory()` API still takes the complete file.
4. **Target Applications:**
   - **High-Fidelity Remote Desktop / VNC:** Crisp, uncorrupted text and UI streaming without JPEG/H.264 chroma blur or ringing artifacts.
   - **Medical & Scientific Imaging:** Live transmission of endoscopy, surgical, and radiology feeds where mathematical losslessness is required; the payload CRC-32 detects corrupted files instead of silently decoding them.
   - **High-Speed Industrial Inspection:** Machine vision camera feeds operating at 60 to 120 FPS.

---

## Repository Structure

```text
phf/
├── MANIFESTO.md          # Origins, vision, and tribute to David Huffman
├── SPEC.md               # Byte-level format specification v1.0
├── LICENSE               # Permissive MIT License
├── CMakeLists.txt        # Cross-platform build (library + tests)
│
├── libphf/               # Core C Library
│   ├── include/
│   │   └── phf.h         # Public C API, flags and error codes
│   ├── src/
│   │   ├── phf.c         # Encoder, decoder, and container logic
│   │   ├── huff.c / .h   # Canonical length-limited (15-bit) Huffman codes
│   │   ├── bitio.h       # Bitstream reader and writer
│   │   ├── nhf3_core.h   # Internal NHF3 engine API
│   │   └── phf_simd.h    # SIMD prediction (SSE2, ARM NEON, WASM SIMD128, scalar)
│   ├── build.bat         # Windows MSVC shortcut (phf.dll)
│   └── build_wasm.bat/.sh# WebAssembly build -> apps/web/phf.js + phf.wasm
│
├── tests/
│   └── test_phf.c        # Round-trip, container, CRC and corruption tests
├── bench/
│   └── bench.py          # Reproducible PHF vs PNG vs WebP Lossless benchmark
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

### Any platform (CMake 3.16+):
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```
Produces `libphf.so` / `libphf.dylib` / `phf.dll` in `build/` (`build/Release/` with Visual Studio).
OpenMP is used automatically when the compiler provides it (`-DPHF_OPENMP=OFF` to disable).
Other options: `-DPHF_BUILD_SHARED=OFF` (static library), `-DPHF_SANITIZE=ON` (ASan + UBSan).

### Windows shortcut (MSVC):
```cmd
cd libphf
build.bat
```
Produces `libphf/phf.dll` and `phf.lib`. Finds Visual Studio through `vswhere` if `cl.exe` is not already in `PATH`.

### WebAssembly (Emscripten):
```bash
source /path/to/emsdk/emsdk_env.sh   # or emsdk_env.bat on Windows
./libphf/build_wasm.sh               # or libphf\build_wasm.bat
```
Writes `apps/web/phf.js` and `apps/web/phf.wasm` (SIMD128 enabled). After rebuilding, bump
`BUILD_VERSION` in `phf-view.js` and the `?v=` strings in `viewer.html` to bust browser caches.

---

## Testing & Benchmarking

```bash
ctest --test-dir build -C Release --output-on-failure
```
Runs round-trips over many sizes and image types (RGB/RGBA, stride 3/4) in every mode and
effort, mode-selection checks, header and error checks, CRC detection, a corruption fuzz of
both modes, file I/O, and decoding of `apps/web/sample.phf`.
CI runs the suite on Linux (with OpenMP and with sanitizers), macOS and Windows.

```bash
pip install opencv-python numpy
python bench/bench.py photo1.png photo2.png ...
OMP_NUM_THREADS=1 python bench/bench.py photo1.png ...   # single-threaded PHF
```
Prints a Markdown table (size, ratio, encode/decode time and MB/s, bit-exact check) for
PHF, WebP Lossless, PNG-6 and PNG-1, plus the machine and thread settings used.

---

## Quickstart (CLI)

Requires the shared library (see above; or point `PHF_LIB` at it) and `pip install opencv-python numpy`.

```bash
# Encode an image to PHF (8-bit RGB, RGBA or grayscale; grayscale is stored as RGB).
# The default mode is auto: it also tries the palette mode when the image has at most
# 4096 colours and keeps the smaller file.
python apps/cli/phf_tool.py encode photo.png photo.phf
python apps/cli/phf_tool.py encode logo.png logo.phf --mode=palette   # or --mode=med
python apps/cli/phf_tool.py encode frame.png frame.phf --fast         # one encode, no trials

# Decode a PHF image back to PNG
python apps/cli/phf_tool.py decode photo.phf photo.png

# Show metadata and verify the file decodes and its CRC matches
python apps/cli/phf_tool.py info photo.phf
```

JPEG inputs work too: PHF stores the decoded JPEG pixels losslessly (it cannot undo the JPEG's own loss).
16-bit images are rejected rather than truncated.

---

## C API Example

```c
#include "phf.h"

// Encode RGB image to a .phf file (max 65535 px per side, 2^26 px total).
// NULL options = { PHF_MODE_AUTO, PHF_EFFORT_BEST }: best size for files.
int err = phf_write_file("output.phf", rgb_buffer, width, height, 3, NULL);
if (err != PHF_OK) { /* PHF_ERR_* code */ }

// Streaming: fix the mode up front and encode once per frame.
phf_encode_options_t live = { PHF_MODE_MED, PHF_EFFORT_FAST };   // PALETTE for screen content
size_t size;
uint8_t *frame_phf = phf_encode_memory(rgb_frame, width, height, 3, &live, &size);
phf_free(frame_phf);

// Read and decode a .phf file from disk (CRC-32 is verified when present)
phf_info_t info;
uint8_t *decoded_rgb = phf_read_file("output.phf", &info);

if (decoded_rgb) {
    // Process pixels: info.width, info.height, info.channels
    phf_free(decoded_rgb);
}

// Or decode from memory straight into an RGBA buffer (alpha = 255 for RGB files)
int rc = phf_decode_memory(file_bytes, file_size, rgba, info.width, info.height, 4);
if (rc == PHF_ERR_CHECKSUM) { /* file is corrupted */ }
```

---

## Authors & Acknowledgments

- **Ygor D. L. Pereira** — Original N-Huffman concept and academic research (FA7, 2008).
- **Antigravity (Google DeepMind)** — Codec systems engineering, SIMD vectorization, and WebAssembly architecture.
- **Claude (Anthropic)** — Code review, container hardening and CRC-32 integrity, byte-level specification, test suite, CMake/CI, and benchmark tooling.
- **David A. Huffman** (1925–1999) — In enduring tribute to the father of minimum redundancy coding.

---

## License

This project is licensed under the **MIT License** — see the [LICENSE](LICENSE) file for details.
