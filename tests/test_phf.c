/*
 * libphf test suite: round-trips, container validation, CRC, and a small
 * corruption fuzz. Usage: test_phf [legacy_sample.phf]
 */
#include "phf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0, g_run = 0;
#define CHECK(cond, ...)                                    \
    do {                                                    \
        g_run++;                                            \
        if (!(cond)) {                                      \
            g_fail++;                                       \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);     \
            printf(__VA_ARGS__);                            \
            printf("\n");                                   \
        }                                                   \
    } while (0)

static uint32_t rs = 12345;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

enum { K_NOISE, K_GRADIENT, K_BLOCKS, K_FLAT, K_XOR_ALPHA, K_COUNT };

static void fill(uint8_t *p, int w, int h, int ch, int kind) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            for (int c = 0; c < ch; c++) {
                uint8_t v;
                switch (kind) {
                case K_NOISE:    v = (uint8_t)rnd(); break;
                case K_GRADIENT: v = (uint8_t)(x * 3 + y * 2 + c * 40 + (int)(rnd() % 7)); break;
                case K_BLOCKS:   v = ((x / 17 + y / 13) % 2) ? 200 : 30; break;
                case K_FLAT:     v = 7; break;
                default:         v = (c == 3) ? ((x > w / 2) ? 255 : 0) : (uint8_t)(x ^ y); break;
                }
                p[((size_t)y * w + x) * ch + c] = v;
            }
}

/* Every mode/effort combination; index 0 is the NULL default. */
static const phf_encode_options_t OPTS[] = {
    { PHF_MODE_AUTO, PHF_EFFORT_BEST }, { PHF_MODE_AUTO, PHF_EFFORT_FAST },
    { PHF_MODE_MED, PHF_EFFORT_BEST },  { PHF_MODE_MED, PHF_EFFORT_FAST },
    { PHF_MODE_PALETTE, PHF_EFFORT_BEST }, { PHF_MODE_PALETTE, PHF_EFFORT_FAST },
};
#define N_OPTS (sizeof(OPTS) / sizeof(OPTS[0]))
static const phf_encode_options_t MED_BEST = { PHF_MODE_MED, PHF_EFFORT_BEST };
static const phf_encode_options_t PAL_BEST = { PHF_MODE_PALETTE, PHF_EFFORT_BEST };

/* NHF3 predictor of an encoded PHF file: 2 = MED, 3 = palette. */
static int file_pred(const uint8_t *e) { return (e[PHF_HEADER_SIZE + 5] >> 1) & 3; }

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void test_roundtrip(void) {
    static const int sizes[][2] = { {1,1}, {1,7}, {7,1}, {2,2}, {15,3}, {16,16}, {17,5},
                                    {31,33}, {64,64}, {257,129}, {640,480} };
    for (size_t si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++)
        for (int ch = 3; ch <= 4; ch++)
            for (int k = 0; k < K_COUNT; k++)
                for (size_t oi = 0; oi < N_OPTS; oi++) {
                    int w = sizes[si][0], h = sizes[si][1];
                    size_t n = (size_t)w * h * ch;
                    uint8_t *img = malloc(n), *dec = malloc(n), *d4 = malloc((size_t)w * h * 4);
                    fill(img, w, h, ch, k);
                    size_t osz = 0;
                    uint8_t *enc = phf_encode_memory(img, (uint32_t)w, (uint32_t)h, (uint8_t)ch,
                                                     oi ? &OPTS[oi] : NULL, &osz);
                    CHECK(enc != NULL, "encode %dx%d ch%d kind%d opt%zu", w, h, ch, k, oi);
                    if (!enc) { free(img); free(dec); free(d4); continue; }

                    int rc = phf_decode_memory(enc, osz, dec, (uint32_t)w, (uint32_t)h, (uint8_t)ch);
                    CHECK(rc == PHF_OK && memcmp(dec, img, n) == 0,
                          "roundtrip %dx%d ch%d kind%d opt%zu rc=%d", w, h, ch, k, oi, rc);

                    rc = phf_decode_memory(enc, osz, dec, (uint32_t)w, (uint32_t)h, 0);
                    CHECK(rc == PHF_OK && memcmp(dec, img, n) == 0, "stride 0 %dx%d ch%d opt%zu", w, h, ch, oi);

                    /* RGB into RGBA must come out opaque in every mode. */
                    memset(d4, 0, (size_t)w * h * 4);
                    rc = phf_decode_memory(enc, osz, d4, (uint32_t)w, (uint32_t)h, 4);
                    int ok = (rc == PHF_OK);
                    for (size_t i = 0; ok && i < (size_t)w * h; i++)
                        for (int c = 0; c < 4; c++) {
                            uint8_t want = c < ch ? img[i * ch + c] : 255;
                            if (d4[i * 4 + c] != want) { ok = 0; break; }
                        }
                    CHECK(ok, "stride 4 %dx%d ch%d kind%d opt%zu rc=%d", w, h, ch, k, oi, rc);

                    phf_free(enc); free(img); free(dec); free(d4);
                }
}

static void test_modes(void) {
    enum { W = 96, H = 64 };
    uint8_t img[W * H * 4];
    size_t a, b;

    /* NULL and a zeroed struct are the same default. */
    fill(img, W, H, 3, K_GRADIENT);
    phf_encode_options_t zero;
    memset(&zero, 0, sizeof(zero));
    uint8_t *e1 = phf_encode_memory(img, W, H, 3, NULL, &a), *e2 = phf_encode_memory(img, W, H, 3, &zero, &b);
    CHECK(e1 && e2 && a == b && memcmp(e1, e2, a) == 0, "NULL options == zeroed options");
    phf_free(e1); phf_free(e2);

    /* AUTO: 4 colours scattered at random -> palette (both efforts); noise -> MED. */
    static const uint8_t four[4][3] = { {250, 10, 10}, {10, 200, 30}, {20, 20, 230}, {240, 240, 240} };
    for (int fast = 0; fast <= 1; fast++) {
        const phf_encode_options_t o = { PHF_MODE_AUTO, fast ? PHF_EFFORT_FAST : PHF_EFFORT_BEST };
        for (int i = 0; i < W * H; i++) memcpy(img + i * 3, four[rnd() % 4], 3);
        uint8_t *e = phf_encode_memory(img, W, H, 3, &o, &a);
        CHECK(e && file_pred(e) == 3, "AUTO fast=%d picks palette for 4 scattered colours", fast);
        CHECK(e && e[6] == PHF_FLAG_CRC32, "palette flags = 0x%02x", e ? e[6] : 0);
        phf_free(e);
        fill(img, W, H, 3, K_NOISE);
        e = phf_encode_memory(img, W, H, 3, &o, &a);
        CHECK(e && file_pred(e) == 2, "AUTO fast=%d picks MED for noise", fast);
        phf_free(e);
    }

    /* AUTO + BEST never loses to either forced mode. */
    for (int k = 0; k < K_COUNT; k++) {
        fill(img, W, H, 3, k);
        size_t s_auto, s_med, s_pal;
        const phf_encode_options_t pal = { PHF_MODE_PALETTE, PHF_EFFORT_BEST };
        uint8_t *x = phf_encode_memory(img, W, H, 3, NULL, &s_auto);
        uint8_t *y = phf_encode_memory(img, W, H, 3, &MED_BEST, &s_med);
        uint8_t *z = phf_encode_memory(img, W, H, 3, &pal, &s_pal);
        CHECK(x && y && z && s_auto <= s_med && s_auto <= s_pal, "AUTO kind%d: %zu vs MED %zu / palette %zu",
              k, s_auto, s_med, s_pal);
        phf_free(x); phf_free(y); phf_free(z);
    }

    /* RGBA palette keeps alpha and flags it. */
    fill(img, W, H, 4, K_XOR_ALPHA);
    uint8_t *e = phf_encode_memory(img, W, H, 4, &PAL_BEST, &a);
    uint8_t out[W * H * 4];
    CHECK(e && file_pred(e) == 3 && e[6] == (PHF_FLAG_ALPHA | PHF_FLAG_CRC32), "RGBA palette header");
    CHECK(e && phf_decode_memory(e, a, out, W, H, 4) == PHF_OK && memcmp(out, img, sizeof(out)) == 0,
          "RGBA palette roundtrip");
    phf_free(e);

    /* Invalid options are rejected. */
    phf_encode_options_t bad = { (phf_mode_t)7, PHF_EFFORT_BEST };
    CHECK(phf_encode_memory(img, W, H, 3, &bad, &a) == NULL, "invalid mode");
    bad.mode = PHF_MODE_MED; bad.effort = (phf_effort_t)9;
    CHECK(phf_encode_memory(img, W, H, 3, &bad, &a) == NULL, "invalid effort");
}

static void test_header(void) {
    uint8_t img[8 * 8 * 4];
    fill(img, 8, 8, 4, K_GRADIENT);
    size_t osz;
    uint8_t *e3 = phf_encode_memory(img, 8, 8, 3, &MED_BEST, &osz);
    CHECK(memcmp(e3, "PHF1", 4) == 0 && e3[4] == PHF_VERSION && e3[5] == 3, "magic/version/channels");
    CHECK(e3[6] == (PHF_FLAG_MED | PHF_FLAG_DECORR | PHF_FLAG_CRC32), "RGB flags = 0x%02x", e3[6]);
    CHECK(e3[7] == 0, "reserved byte");
    CHECK(le32(e3 + 8) == 8 && le32(e3 + 12) == 8, "dimensions little-endian");
    CHECK(osz == PHF_HEADER_SIZE + le32(e3 + 16) + PHF_CRC_SIZE, "size = 20 + payload + 4");

    phf_info_t info;
    CHECK(phf_probe_memory(e3, PHF_HEADER_SIZE, &info) == PHF_OK && info.width == 8 && info.channels == 3,
          "probe needs only 20 bytes");
    CHECK(phf_probe_memory(e3, 19, &info) == PHF_ERR_TRUNCATED, "probe 19 bytes");
    CHECK(phf_probe_memory(NULL, 20, &info) == PHF_ERR_ARG, "probe NULL");
    phf_free(e3);

    uint8_t *e4 = phf_encode_memory(img, 8, 8, 4, &MED_BEST, &osz);
    CHECK(e4[6] == (PHF_FLAG_ALPHA | PHF_FLAG_MED | PHF_FLAG_DECORR | PHF_FLAG_CRC32), "RGBA flags = 0x%02x", e4[6]);
    phf_free(e4);
}

static void test_limits_and_errors(void) {
    uint8_t px[4] = { 0 }, out[64 * 64 * 4];
    size_t osz;
    CHECK(phf_encode_memory(px, 0, 1, 3, NULL, &osz) == NULL, "width 0");
    CHECK(phf_encode_memory(px, 1, 1, 2, NULL, &osz) == NULL, "channels 2");
    CHECK(phf_encode_memory(px, PHF_MAX_DIM + 1, 1, 3, NULL, &osz) == NULL, "width > 65535");
    CHECK(phf_encode_memory(px, 1, 1, 3, NULL, NULL) == NULL, "NULL out_size");

    uint8_t img[64 * 64 * 3];
    fill(img, 64, 64, 3, K_GRADIENT);
    uint8_t *e = phf_encode_memory(img, 64, 64, 3, &MED_BEST, &osz);
    uint8_t *m = malloc(osz);

    CHECK(phf_decode_memory(e, osz, out, 63, 64, 3) == PHF_ERR_SIZE, "dimension mismatch");
    CHECK(phf_decode_memory(e, osz, out, 64, 64, 2) == PHF_ERR_STRIDE, "stride < channels");
    CHECK(phf_decode_memory(e, osz, NULL, 64, 64, 3) == PHF_ERR_ARG, "NULL dst");
    CHECK(phf_decode_memory(e, osz - 1, out, 64, 64, 3) == PHF_ERR_TRUNCATED, "missing CRC byte");
    CHECK(phf_decode_memory(e, osz - 5, out, 64, 64, 3) == PHF_ERR_TRUNCATED, "truncated payload");

    memcpy(m, e, osz); m[0] = 'X';
    CHECK(phf_decode_memory(m, osz, out, 64, 64, 3) == PHF_ERR_MAGIC, "bad magic");
    memcpy(m, e, osz); m[4] = 2;
    CHECK(phf_decode_memory(m, osz, out, 64, 64, 3) == PHF_ERR_VERSION, "bad version");

    /* payload_size near 2^32: must not wrap (this used to pass on 32-bit/WASM). */
    memcpy(m, e, osz); m[16] = 0xF0; m[17] = 0xFF; m[18] = 0xFF; m[19] = 0xFF;
    CHECK(phf_decode_memory(m, osz, out, 64, 64, 3) == PHF_ERR_TRUNCATED, "huge payload_size");

    /* Header channel count must agree with the payload. */
    memcpy(m, e, osz); m[5] = 4;
    CHECK(phf_decode_memory(m, osz, out, 64, 64, 4) == PHF_ERR_CORRUPT, "channel mismatch");

    /* Single bit flip in the payload is caught by the CRC. */
    memcpy(m, e, osz); m[PHF_HEADER_SIZE + osz / 3] ^= 0x10;
    CHECK(phf_decode_memory(m, osz, out, 64, 64, 3) == PHF_ERR_CHECKSUM, "CRC detects bit flip");

    /* Files without the CRC flag (written before it existed) still decode. */
    memcpy(m, e, osz); m[6] &= (uint8_t)~PHF_FLAG_CRC32;
    CHECK(phf_decode_memory(m, osz - PHF_CRC_SIZE, out, 64, 64, 3) == PHF_OK && memcmp(out, img, sizeof(img)) == 0,
          "legacy file without CRC");

    free(m);
    phf_free(e);
}

static void test_fuzz(void) {
    enum { W = 37, H = 23, ITERS = 4000 };
    uint8_t img[W * H * 4], dst[W * H * 4];
    int accepted_crc = 0, total = 0;
    for (int kind = 0; kind < K_COUNT; kind++)
        for (int ch = 3; ch <= 4; ch++)
            for (int pal = 0; pal <= 1; pal++) {
            fill(img, W, H, ch, kind);
            size_t osz;
            uint8_t *e = phf_encode_memory(img, W, H, (uint8_t)ch, pal ? &PAL_BEST : &MED_BEST, &osz);
            uint8_t *m = malloc(osz);
            for (int it = 0; it < ITERS; it++) {
                memcpy(m, e, osz);
                int with_crc = it & 1;
                if (!with_crc) m[6] &= (uint8_t)~PHF_FLAG_CRC32;
                size_t len = with_crc ? osz : osz - PHF_CRC_SIZE;
                int flips = 1 + (int)(rnd() % 4);
                for (int f = 0; f < flips; f++) {
                    size_t p = PHF_HEADER_SIZE + rnd() % (osz - PHF_HEADER_SIZE - PHF_CRC_SIZE);
                    m[p] ^= (uint8_t)(1u << (rnd() % 8));
                }
                if (rnd() % 8 == 0) len = PHF_HEADER_SIZE + rnd() % (len - PHF_HEADER_SIZE);
                int rc = phf_decode_memory(m, len, dst, W, H, 4);
                /* Two flips of the same bit cancel out; only count real corruption. */
                int changed = len != osz || memcmp(m, e, osz) != 0;
                if (with_crc && changed) { total++; if (rc == PHF_OK) accepted_crc++; }
            }
            free(m);
            phf_free(e);
        }
    CHECK(accepted_crc == 0, "CRC let %d/%d corrupted files through", accepted_crc, total);
}

static void test_sample(const char *path) {
    phf_info_t info;
    uint8_t *px = phf_read_file(path, &info);
    CHECK(px != NULL, "phf_read_file(%s)", path);
    if (px) CHECK(info.width > 0 && info.height > 0 && (info.channels == 3 || info.channels == 4), "sample info");
    phf_free(px);
}

static void test_file_io(void) {
    uint8_t img[33 * 17 * 4];
    fill(img, 33, 17, 4, K_XOR_ALPHA);
    const char *path = "test_phf_tmp.phf";
    CHECK(phf_write_file(path, img, 33, 17, 4, NULL) == PHF_OK, "phf_write_file");
    phf_info_t info;
    uint8_t *px = phf_read_file(path, &info);
    CHECK(px && info.width == 33 && info.height == 17 && info.channels == 4 && memcmp(px, img, sizeof(img)) == 0,
          "phf_read_file roundtrip");
    phf_free(px);
    remove(path);
    CHECK(phf_read_file("does/not/exist.phf", &info) == NULL, "missing file");
}

int main(int argc, char **argv) {
    test_roundtrip();
    test_modes();
    test_header();
    test_limits_and_errors();
    test_fuzz();
    test_file_io();
    if (argc > 1) test_sample(argv[1]);
    printf("%d/%d checks passed\n", g_run - g_fail, g_run);
    return g_fail ? 1 : 0;
}
