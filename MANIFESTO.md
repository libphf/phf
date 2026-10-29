# The PHF Manifesto

> *"In 1952, David A. Huffman published the mathematical foundation that made digital data compression possible — from JPEG and MP3 to PNG and GZIP. For over seven decades, humanity relied on his invention, yet no widespread image format bore his name. PHF was created to provide that overdue historical tribute and push information theory to the modern state of the art."*

---

## 1. The Official Name & The Easter Egg
To the industry, developer community, and specification bodies, **PHF** officially stands for:

### **Pure High-Fidelity Format**
It communicates the technical guarantee of the format:
- **P — Pure:** Zero degradation, mathematically exact photon-level reconstruction ($\text{PSNR} = \infty$), and pure lossless data.
- **H — High-Fidelity:** Preservation of subtle gradients, color accuracy, and high-frequency textures without compression artifacts.
- **F — Format:** A modern, standardized image container designed for speed and universal interoperability.

However, embedded directly into the project's history and manifesto is an intentional **Easter Egg** — the authentic tribute from which the codec was born:

### **Pereira • Huffman • Format**
- **P — Pereira:** Honoring **Ygor D. L. Pereira**, creator and lead architect of the format, whose 2008 academic research pioneered the N-Huffman order-1 context state graph.
- **H — Huffman:** Giving long-overdue credit to **David A. Huffman** (1925–1999), whose foundational algorithm powers the digital world but was never credited in a mainstream file extension.
- **F — Format:** High-performance systems engineering in collaboration with **Antigravity (Google DeepMind)** for extreme real-time throughput.

---

## 2. Origins & Genealogy
**PHF** is the culmination of nearly two decades of research pursuing the ideal balance between **absolute mathematical fidelity (100% Lossless)** and **real-time computational throughput**.

1. **The Theoretical Foundation (FA7, 2008):**
   The academic thesis by **Ygor D. L. Pereira** demonstrated that the structural limits of classical static Huffman coding could be transcended by modeling symbols as nodes in a directed order-1 state graph (**N-Huffman**): each encoded symbol dynamically establishes the statistical context for the subsequent symbol's probability tree.

2. **Modern Systems Engineering (2026):**
   Through a pair-programming collaboration between **Ygor Pereira** and **Antigravity (Google DeepMind)**, the architecture was re-engineered for modern parallel processors:
   - Length-limited canonical Huffman codes (maximum 15 bits, using the JPEG Annex K.3 length adjustment).
   - Compact sparse code-length tables: Elias-gamma symbol gaps plus 4-bit lengths, so a table costs bits only for the symbols it actually uses.
   - Vectorized geometric spatial prediction (MED — *Median Edge Detector*) implemented with **SIMD intrinsics (SSE2, ARM NEON, and WASM SIMD128)**.
   - Exact per-context cost optimization: dynamic selection between custom tables, shared global fallback tables, and deterministic 0-bit immediate states.

---

## 2. Core Principles

### I. Absolute Fidelity (Strictly Lossless)
Zero pixels are discarded, quantized, or approximated. When a camera captures raw sensor data in 24-bit or 32-bit color, PHF preserves every photon upon reconstruction. $\text{PSNR} = \infty$, $\text{MSE} = 0$, maximum pixel error $= 0$.

### II. High-Throughput Production (> 300 MB/s)
The primary obstacle of modern lossless formats (such as WebP Lossless) has been compression latency. In the author's benchmark PHF encoded **37× faster than WebP Lossless** and **20× faster than PNG-6**, processing hundreds of megabytes per second on standard consumer hardware.

### III. Instant & Universal Decoding
PHF's decoder is plain C99 with optional SIMD, so it runs on desktop and server CPUs, single-board computers such as the Raspberry Pi, and modern web browsers via WebAssembly SIMD128. It needs about 600 KB of code tables plus full-frame buffers, which rules out small microcontrollers without external RAM.

### IV. Free & Open to Humanity (MIT License)
We dedicate this algorithm and format to the global community under the permissive MIT License. Free of software patents, free of licensing royalties, free of restrictive consortium fees. It belongs to developers, photographers, researchers, and open-source creators worldwide.

---

## 3. Authorship & Acknowledgments
- **Creator, Original Inventor & Project Lead:** Ygor D. L. Pereira
- **Systems Engineering & Vectorization:** Antigravity (Google DeepMind)
- **Historical Homage:** David A. Huffman (1925–1999)
