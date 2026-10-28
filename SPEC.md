# PHF (Pure High-Fidelity Format) Specification v1.0

The official file extension is **`.phf`**. The recommended MIME type is **`image/phf`** or **`image/x-phf`**.

This document describes every byte a decoder needs. The reference implementation is
`libphf/src/phf.c` and `libphf/src/huff.c`; where this text and the code disagree, that is
a bug to be reported.

Conventions: `u8`/`u16`/`u32` are unsigned integers; **LE** = little-endian, **BE** =
big-endian. Bitstreams are read **MSB-first** (the first bit is bit 7 of the first byte).
All pixel arithmetic is modulo 256.

---

## 1. File Layout

```
+----------------------+-------------------------------+-----------------------+
| PHF header (20 B)    | payload: NHF3 packet          | CRC-32 (4 B, optional)|
|                      | (payload_size bytes)          | present if flag bit 3 |
+----------------------+-------------------------------+-----------------------+
```

### 1.1 PHF Header (20 bytes)

| Offset | Size | Type | Field | Description |
| :---: | :---: | :---: | :--- | :--- |
| `0x00` | 4 | ASCII | `magic` | `"PHF1"` (`0x50 0x48 0x46 0x31`). |
| `0x04` | 1 | u8 | `version` | Container version. Must be `1`. |
| `0x05` | 1 | u8 | `channels` | `3` (RGB) or `4` (RGBA). |
| `0x06` | 1 | u8 | `flags` | See 1.2. |
| `0x07` | 1 | u8 | `reserved` | Writers must write `0x00`; readers ignore it. |
| `0x08` | 4 | u32 LE | `width` | Image width in pixels. |
| `0x0C` | 4 | u32 LE | `height` | Image height in pixels. |
| `0x10` | 4 | u32 LE | `payload_size` | Byte length of the NHF3 packet that follows. |

### 1.2 Flags

| Bit | Mask | Name | Meaning |
| :---: | :---: | :--- | :--- |
| 0 | `0x01` | `ALPHA` | Set when `channels == 4`. |
| 1 | `0x02` | `MED` | MED spatial prediction is used. |
| 2 | `0x04` | `DECORR` | R−G / B−G color decorrelation is used. |
| 3 | `0x08` | `CRC32` | A 4-byte CRC-32 follows the payload (see 1.4). |
| 4–7 | | | Reserved, must be `0`. |

A v1 writer produces `0x0E` for RGB and `0x0F` for RGBA. Bits 0–2 are descriptive: the
decoder takes the actual coding modes from the NHF3 packet header (section 2), and they
must agree with it. Files written before bit 3 existed carry no CRC; files from early
encoder builds also carry `0x04`/`0x05` (the MED bit was not set) and are still valid.

### 1.3 Size Limits

The NHF3 packet stores 16-bit dimensions, so: `1 <= width, height <= 65535` and
`width * height <= 2^26` (67,108,864 pixels). Encoders reject larger images.

### 1.4 Validation and Integrity

A decoder must check, in this order:

1. `magic`, `version`, and `channels ∈ {3, 4}`.
2. `20 + payload_size <= file_size` (plus 4 when `CRC32` is set). Compute this without
   integer overflow, e.g. `payload_size <= file_size - 20`. Bytes after the payload
   (and CRC) are ignored.
3. If `CRC32` is set: the u32 LE after the payload must equal the CRC-32 of the
   `payload_size` payload bytes. CRC-32 here is the IEEE 802.3 / zlib CRC (reflected,
   polynomial `0xEDB88320`, initial value and final XOR `0xFFFFFFFF`).
4. The NHF3 packet's width, height and channel count must equal the PHF header's,
   and its P-frame bit must be `0`.

---

## 2. NHF3 Packet (the payload)

### 2.1 Packet Header (`11 + 4·ch` bytes)

| Offset | Size | Type | Field | Description |
| :---: | :---: | :---: | :--- | :--- |
| 0 | 4 | ASCII | `magic` | `"NHF3"`. |
| 4 | 1 | u8 | `version` | `1`. |
| 5 | 1 | u8 | `mode` | bit 0: P-frame (must be 0 in PHF); bits 1–2: predictor (`0` none, `1` left, `2` MED, `3` palette); bit 3: color decorrelation. PHF v1 writers use `0x0C` (MED + decorrelation). |
| 6 | 2 | u16 BE | `width` | |
| 8 | 2 | u16 BE | `height` | |
| 10 | 1 | u8 | `ch` | Channel count (3 or 4 in PHF). |
| 11 | 4·ch | u32 BE[ch] | `plane_len` | Byte length of each plane stream. |

The plane streams follow, concatenated in plane order. The sum of `plane_len` plus the
header must equal `payload_size` exactly. Each plane can be decoded independently once its
bytes are available.

PHF v1 writers only produce predictor `2` with decorrelation. The reference decoder also
accepts predictors `0`/`1` from the NHF3 engine; the palette mode (`3`) is not part of this
specification, and independent decoders may reject it.

### 2.2 Planes and Color Transform

With decorrelation, the planes are, in order:

| Plane | Content |
| :---: | :--- |
| 0 | `R − G` |
| 1 | `G` |
| 2 | `B − G` |
| 3 | `A` (RGBA only, not transformed) |

Without decorrelation, the planes are R, G, B (and A) unchanged. Reconstruction:
`G = plane1`, `R = plane0 + G`, `B = plane2 + G` (mod 256).

### 2.3 Spatial Prediction

Each plane `P` (size `w × h`, row-major) is replaced by a residual plane `E`. Let
`X = P(x, y)`, `a = P(x−1, y)` (left), `b = P(x, y−1)` (up), `c = P(x−1, y−1)` (up-left):

| Position | Prediction `X̂` |
| :--- | :--- |
| `(0, 0)` | `0` |
| `y = 0, x > 0` | `a` |
| `x = 0, y > 0` | `b` |
| otherwise, predictor `2` (MED) | `min(a,b)` if `c >= max(a,b)`; `max(a,b)` if `c <= min(a,b)`; else `a + b − c` |
| otherwise, predictor `1` (left) | `a` |

`E(x, y) = (X − X̂) mod 256` and the decoder recovers `X = (X̂ + E(x, y)) mod 256`, in
raster order. Predictor `0` leaves the plane unchanged.

---

## 3. Plane Stream (order-1 N-Huffman)

A plane stream encodes the `n = w·h` residual bytes `E[0..n−1]` (raster order). The
**context** of each symbol is the previous symbol in the stream. The stream ends with zero
bits up to the next byte boundary.

### 3.1 Layout

```
first      : 8 bits        first symbol E[0]
rle        : 1 bit         1 = run-length mode (3.4)
if rle:    run class codes for t = 0, then t = 1:
             single : 1 bit
             if single: cls : 5 bits   (every run uses class cls, 0 code bits)
             else:      code table (3.2), only symbols 0..31 allowed
has_fb     : 1 bit         1 = a shared "fallback" code table follows
if has_fb: code table (3.2) over symbols 0..255
contexts   : 256 descriptors, for c = 0..255:
             used : 1 bit
             if used: kind : 2 bits
                 0 = SINGLE   : sym : 8 bits (the next symbol is always sym, 0 bits)
                 1 = OWN      : code table (3.2) for this context
                 2 = FALLBACK : use the fallback table (requires has_fb)
                 3 = invalid
symbols    : 3.3 (plain) or 3.4 (rle)
```

A context `c` that is not `used` must never occur during decoding.

### 3.2 Code Table

A table gives a code length (1–15) for each symbol that has a code:

```
k − 1      : 8 bits        k = number of coded symbols (decoders require k >= 2)
k entries, in increasing symbol order:
  gap      : Elias-gamma   symbol − previous symbol (previous starts at −1)
  len      : 4 bits        1..15
```

Elias-gamma for `v >= 1`: `m = floor(log2 v)` zero bits, then `v` in `m + 1` bits (the
leading bit is the 1). Readers reject `m > 8` and symbols above 255.

The lengths must form a **complete** prefix code (Kraft sum `Σ 2^(−len) = 1`). Codes are
**canonical** as in DEFLATE: shorter codes first, ties by increasing symbol value, each
code being the previous code + 1 (shifted left when the length grows). Codes are read
MSB-first.

### 3.3 Plain Mode (`rle = 0`)

`E[0] = first`. For `i = 1..n−1`, with `ctx = E[i−1]`: if `ctx` is SINGLE, `E[i]` is its
`sym`; otherwise decode one symbol with the context's table (OWN or fallback).

### 3.4 Run-Length Mode (`rle = 1`)

The plane is a sequence of `(symbol, run)` pairs whose runs sum to exactly `n`. Within the
sequence, consecutive symbols always differ.

1. The first pair's symbol is `first`.
2. Every later symbol is decoded as in 3.3, using the previous **pair's** symbol as the
   context.
3. After each symbol `s`, its run is read with run table `t = (s != 0) ? 1 : 0`: a class
   `k` (0–31, either the fixed `cls` or decoded with the table), then `k` raw bits `x`;
   `run = 2^k + x`. A run that would pass `n` is an error.

---

## 4. Streaming Notes

- **Header probe:** the first 20 bytes give width, height, channel count, the required
  output size (`width · height · stride`) and the total file size, so a receiver can
  allocate buffers before the rest arrives (`phf_probe_memory()`).
- **Independent planes:** after `20 + 11 + 4·ch` bytes the receiver knows where each plane
  starts and ends, and each plane decodes on its own. The reference API
  (`phf_decode_memory()`) currently needs the complete file; incremental decoding is a
  possible future API, not something v1 provides.
- **Inter-frame (P-frame) coding** exists in the underlying NHF3 engine (`nhf3_encode` with
  a previous frame), but it is **not** part of the PHF v1 container: PHF files are always
  stand-alone images.
