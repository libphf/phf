#ifndef PHF_H
#define PHF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(PHF_BUILD_DLL)
  #define PHF_API __declspec(dllexport)
#elif defined(_WIN32)
  #define PHF_API
#else
  #define PHF_API __attribute__((visibility("default")))
#endif

#define PHF_VERSION 1
#define PHF_MAGIC 0x31464850 /* "PHF1" little-endian */

typedef struct {
    uint32_t width;
    uint32_t height;
    uint8_t  channels;  /* 3 = RGB, 4 = RGBA */
    uint8_t  flags;     /* Bit 0: alpha, Bit 1: MED pred, Bit 2: G-decorr */
    size_t   payload_size;
} phf_info_t;

/**
 * phf_probe_memory:
 *   Reads the 20-byte PHF file header and populates out_info.
 *   Returns 0 on success, or a negative error code.
 */
PHF_API int phf_probe_memory(const uint8_t *data, size_t size, phf_info_t *out_info);

/**
 * phf_encode_memory:
 *   Encodes an RGB or RGBA pixel buffer into the full PHF file container (.phf).
 *   The returned pointer includes the 20-byte header + N-Huffman compressed payload.
 *   Must be freed with phf_free().
 */
PHF_API uint8_t* phf_encode_memory(
    const uint8_t *pixels,
    uint32_t width,
    uint32_t height,
    uint8_t channels,
    size_t *out_size
);

/**
 * phf_decode_memory:
 *   Decodes a complete in-memory PHF file buffer into destination pixel buffer.
 *   Returns 0 on success, or a negative error code.
 */
PHF_API int phf_decode_memory(
    const uint8_t *phf_data,
    size_t phf_size,
    uint8_t *dst_pixels,
    uint32_t width,
    uint32_t height,
    uint8_t channels
);

/**
 * phf_write_file:
 *   Encodes and writes an RGB/RGBA image directly to a .phf file on disk.
 *   Returns 0 on success, or a negative error code.
 */
PHF_API int phf_write_file(
    const char *filepath,
    const uint8_t *pixels,
    uint32_t width,
    uint32_t height,
    uint8_t channels
);

/**
 * phf_read_file:
 *   Reads and decodes a .phf file from disk, allocating and returning the pixel buffer.
 *   Fills out_info with dimensions. Must be freed with phf_free().
 */
PHF_API uint8_t* phf_read_file(const char *filepath, phf_info_t *out_info);

/**
 * phf_free:
 *   Frees memory allocated by libphf.
 */
PHF_API void phf_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* PHF_H */
