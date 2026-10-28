# PHF (Pure High-Fidelity Format) Specification v1.0

The official file extension is **`.phf`**. The recommended MIME type is **`image/phf`** or **`image/x-phf`**.

---

## 1. Binary Container Layout

Every `.phf` file begins with a fixed **20-byte header**, followed immediately by the length-limited canonical N-Huffman compressed entropy payload:

```
+-------------------------------------------------------------------+
|                     PHF FILE HEADER (20 BYTES)                   |
+-------------------+---------+----------+-------+----------+-------+
| Magic 'PHF1' (4B) | Ver(1B) | Chan(1B) | Fl(1B)| Res(1B)  | W(4B) |
+-------------------+---------+----------+-------+----------+-------+
| Height (4B)       | Payload Size (4B)                             |
+-------------------+-----------------------------------------------+
|                     COMPRESSED N-HUFFMAN PAYLOAD                  |
| (Canonical-15 Tables + Order-1 Bitstream + SIMD MED Residuals)    |
+-------------------------------------------------------------------+
```

### Header Field Specification:

| Byte Offset | Size | Type | Field Name | Description |
| :---: | :---: | :---: | :--- | :--- |
| `0x00` | 4 bytes | ASCII | `magic` | Magic identifier: `0x50 0x48 0x46 0x31` (`"PHF1"`). |
| `0x04` | 1 byte | uint8 | `version` | Format container version. Current version: `1`. |
| `0x05` | 1 byte | uint8 | `channels` | Channel count (`3` for RGB, `4` for RGBA). |
| `0x06` | 1 byte | uint8 | `flags` | Bitmask configuration: <br>• Bit 0: Has Alpha (`1` = RGBA, `0` = RGB)<br>• Bit 1: Spatial MED predictor active (`1` = Yes)<br>• Bit 2: Color decorrelation active (`1` = R-G, G, B-G) |
| `0x07` | 1 byte | uint8 | `reserved` | Reserved for future alignment / color profiles (must be `0x00`). |
| `0x08` | 4 bytes | uint32 | `width` | Image width in pixels (Little-Endian). |
| `0x0C` | 4 bytes | uint32 | `height` | Image height in pixels (Little-Endian). |
| `0x10` | 4 bytes | uint32 | `payload_size` | Exact byte count of the compressed entropy stream. |

---

## 2. Entropy & Residual Pipeline

1. **Header Validation:** Confirm `magic == "PHF1"`, `version == 1`, and file size equals $20 + \text{payload\_size}$.
2. **Entropy Decoding:** The compressed payload is read via a 15-bit canonical Huffman bitstream reader across 256 order-1 context state tables per channel plane.
3. **Inverse Spatial Prediction (MED):**
   For each reconstructed residual pixel at coordinate $(x, y)$, spatial prediction $\hat{X}$ is calculated from causal neighbors:
   $$\hat{X} = \text{clamp}(A + B - C, \min(A, B), \max(A, B))$$
   where $A = P(x-1, y)$, $B = P(x, y-1)$, and $C = P(x-1, y-1)$.
   The original pixel value is recovered via:
   $$X = (\hat{X} + \text{residual}) \pmod{256}$$
4. **Color Re-correlation:**
   - $\text{Green} = G'$
   - $\text{Red} = (R' + G) \pmod{256}$
   - $\text{Blue} = (B' + G) \pmod{256}$

---

## 3. Streaming & Network Transmission

The PHF container is intentionally designed for zero-copy, pipelined network transmission (HTTP chunked streams, WebSockets, WebRTC DataChannels, and UNIX domain sockets).

### 3.1 Instant 20-Byte Header Probe
Unlike chunk-based containers (e.g. PNG chunk trees or TIFF tag directories) that require multiple reads across variable offsets:
- A stream receiver reads exactly the initial **20 bytes** via `phf_probe_memory()` to obtain:
  - Width ($W$) and Height ($H$)
  - Channel count ($C$)
  - Required destination pixel memory: $W \times H \times C$ bytes
  - Total incoming payload size: $20 + \text{payload\_size}$
- Buffer pre-allocation occurs instantly with zero memory reallocations while the network payload continues streaming in.

### 3.2 Planar Stream Independence
The compressed payload contains independent sequential partitions for each color plane:
- Planar layout: Plane 0 ($G$), Plane 1 ($R - G$), Plane 2 ($B - G$), and optional Plane 3 ($\alpha$).
- A multi-threaded streaming consumer can decompress incoming planes concurrently as their respective byte streams arrive over the network, minimizing total frame display latency.

### 3.3 Inter-Frame Delta Protocol (Video Extension)
When deployed in real-time display streaming (e.g., Remote Desktop, medical telemetry, or camera monitoring), the underlying engine supports temporal prediction against the prior frame buffer ($P$-frames):
- Zero-motion tiles require 0 entropy bits.
- Enables steady 60 fps lossless video transmission over standard Gigabit Ethernet networks without compression artifacts.
