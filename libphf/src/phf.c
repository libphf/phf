#include "../include/phf.h"
#include "nhf3_core.h"
#include "bitio.h"
#include "huff.h"
#include "phf_simd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _OPENMP
#include <omp.h>
#endif

#define MAX_PIXELS (1u << 26)

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */
static int ilog2_u32(uint32_t v) { int r = 0; while (v >>= 1) r++; return r; }
static int gamma_bits(uint32_t v) { return 2 * ilog2_u32(v) + 1; }

/* Elias-gamma for v >= 1 (v <= 256 in practice). */
static void put_gamma(BitWriter *w, uint32_t v) {
    int nb = ilog2_u32(v);
    bw_put(w, 0, nb);
    bw_put(w, v, nb + 1);
}

static int get_gamma_max(BitReader *r, uint32_t *out, int max_nb) {
    int nb = 0;
    while (br_get(r, 1) == 0) {
        if (++nb > max_nb) return 0;
    }
    uint32_t v = 1u << nb;
    if (nb) v |= br_get(r, nb);
    *out = v;
    return 1;
}

static int get_gamma(BitReader *r, uint32_t *out) {
    return get_gamma_max(r, out, 8);
}

/* Sparse code-length table: (k-1):8, then per used symbol gamma(gap) + len:4.
 * Cost grows with the number of symbols actually used, not with 256. */
static int table_bits(const uint8_t *len) {
    int bits = 8, prev = -1;
    for (int s = 0; s < 256; s++) {
        if (len[s]) { bits += gamma_bits((uint32_t)(s - prev)) + 4; prev = s; }
    }
    return bits;
}

static void put_table(BitWriter *w, const uint8_t *len) {
    int k = 0, prev = -1;
    for (int s = 0; s < 256; s++) if (len[s]) k++;
    bw_put(w, (uint32_t)(k - 1), 8);
    for (int s = 0; s < 256; s++) {
        if (len[s]) { put_gamma(w, (uint32_t)(s - prev)); bw_put(w, len[s], 4); prev = s; }
    }
}

static int get_table(BitReader *r, uint8_t *len) {
    memset(len, 0, 256);
    int k = (int)br_get(r, 8) + 1;
    int sym = -1;
    for (int j = 0; j < k; j++) {
        uint32_t g;
        if (!get_gamma(r, &g)) return 0;
        sym += (int)g;
        if (sym > 255) return 0;
        len[sym] = (uint8_t)br_get(r, 4);
        if (!len[sym]) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Plane codec: order-1 context (the N-Huffman graph), per-context decision  */
/*                                                                           */
/* Stream layout:                                                            */
/*   first symbol (8) | rle flag (1) | [run tables] | fallback flag (1) |    */
/*   [fallback table] | 256 context descriptors | payload                    */
/* With rle=1 the plane is a list of (symbol, repeat) pairs: consecutive     */
/* symbols always differ, so the graph never needs "repeat" transitions, and */
/* the repeat count is sent as log2 class (Huffman, one table for symbol 0   */
/* and one for the rest) plus raw extra bits.                                */
/* ------------------------------------------------------------------------- */
enum { K_UNUSED = 0, K_SINGLE = 1, K_OWN = 2, K_FALLBACK = 3 };
#define RUN_CLASSES 32

typedef struct {
    int single;           /* only one class in use: 0 bits for the class */
    int cls;              /* that class */
    uint8_t len[256];     /* code lengths (classes < 32) */
    uint16_t code[RUN_CLASSES];
} RunEnc;

static void run_enc_build(RunEnc *e, const uint32_t *hist) {
    int used = 0, last = 0;
    memset(e, 0, sizeof(*e));
    for (int k = 0; k < RUN_CLASSES; k++) if (hist[k]) { used++; last = k; }
    if (used < 2) { e->single = 1; e->cls = last; return; }
    huff_build_lengths(hist, RUN_CLASSES, HUFF_MAXLEN, e->len);
    huff_build_codes(e->len, RUN_CLASSES, e->code);
}

static uint8_t *encode_stream(const uint8_t *v, uint32_t m, const uint32_t *runs,
                              size_t *out_size, NHF3PlaneStats *st) {
    uint32_t *freq = (uint32_t *)calloc(65536, sizeof(uint32_t));
    uint8_t *lens = (uint8_t *)calloc(65536, 1);
    uint16_t *codes = (uint16_t *)calloc(65536, sizeof(uint16_t));
    if (!freq || !lens || !codes) { free(freq); free(lens); free(codes); return NULL; }

    uint8_t kind[256], ssym[256];
    uint32_t distinct[256];
    uint64_t owncost[256];
    uint8_t gl[256];
    uint16_t gcode[256];
    memset(kind, 0, sizeof(kind));
    memset(gl, 0, sizeof(gl));

    /* 1. Transition counts: the monograph's graph (over the token stream). */
    uint8_t prev = v[0];
    uint32_t i = 1;
    for (; i + 3 < m; i += 4) {
        uint8_t s0 = v[i];
        uint8_t s1 = v[i + 1];
        uint8_t s2 = v[i + 2];
        uint8_t s3 = v[i + 3];
        freq[((uint32_t)prev << 8) | s0]++;
        freq[((uint32_t)s0 << 8) | s1]++;
        freq[((uint32_t)s1 << 8) | s2]++;
        freq[((uint32_t)s2 << 8) | s3]++;
        prev = s3;
    }
    for (; i < m; i++) { freq[((uint32_t)prev << 8) | v[i]]++; prev = v[i]; }

    for (int c = 0; c < 256; c++) {
        distinct[c] = 0;
        ssym[c] = 0;
        const uint32_t *f = &freq[c * 256];
        for (int s = 0; s < 256; s++) if (f[s]) { distinct[c]++; ssym[c] = (uint8_t)s; }
        if (distinct[c] == 1) kind[c] = K_SINGLE;         /* 0 bits: "immediate state" */
        else if (distinct[c] >= 2) {
            kind[c] = K_FALLBACK;
            uint8_t *L = &lens[c * 256];
            huff_build_lengths(f, 256, HUFF_MAXLEN, L);
            uint64_t bits = (uint64_t)table_bits(L);
            for (int s = 0; s < 256; s++) bits += (uint64_t)f[s] * L[s];
            owncost[c] = bits;
        }
    }

    /* 2. Decide own table vs shared fallback table. */
    int any_fb = 0;
    for (int it = 0; it < 3; it++) {
        uint32_t g[256];
        for (int s = 0; s < 256; s++) g[s] = 1; /* smoothing: fallback covers all 256 symbols */
        for (int c = 0; c < 256; c++) {
            if (kind[c] != K_FALLBACK) continue;
            for (int s = 0; s < 256; s++) g[s] += freq[c * 256 + s];
        }
        huff_build_lengths(g, 256, HUFF_MAXLEN, gl);
        if (it == 2) break; /* last pass only rebuilds gl for the final set */
        for (int c = 0; c < 256; c++) {
            if (distinct[c] < 2) continue;
            uint64_t fb = 0;
            for (int s = 0; s < 256; s++) fb += (uint64_t)freq[c * 256 + s] * gl[s];
            kind[c] = (owncost[c] < fb) ? K_OWN : K_FALLBACK;
        }
    }
    for (int c = 0; c < 256; c++) if (kind[c] == K_FALLBACK) any_fb = 1;

    /* Run-length classes */
    RunEnc re[2];
    if (runs) {
        uint32_t hist[2][RUN_CLASSES];
        memset(hist, 0, sizeof(hist));
        for (uint32_t i = 0; i < m; i++) hist[v[i] != 0][ilog2_u32(runs[i])]++;
        run_enc_build(&re[0], hist[0]);
        run_enc_build(&re[1], hist[1]);
    }

    /* 3. Emit header. */
    BitWriter w;
    bw_init(&w, (size_t)m / 2 + 4096);
    bw_put(&w, v[0], 8);
    bw_put(&w, runs ? 1u : 0u, 1);
    if (runs) {
        for (int t = 0; t < 2; t++) {
            bw_put(&w, (uint32_t)re[t].single, 1);
            if (re[t].single) bw_put(&w, (uint32_t)re[t].cls, 5);
            else put_table(&w, re[t].len);
        }
    }
    bw_put(&w, (uint32_t)any_fb, 1);
    if (any_fb) { put_table(&w, gl); huff_build_codes(gl, 256, gcode); }

    NHF3PlaneStats local;
    memset(&local, 0, sizeof(local));
    for (int c = 0; c < 256; c++) {
        if (kind[c] == K_UNUSED) { bw_put(&w, 0, 1); local.ctx_unused++; continue; }
        bw_put(&w, 1, 1);
        bw_put(&w, (uint32_t)(kind[c] - 1), 2);
        if (kind[c] == K_SINGLE) {
            bw_put(&w, ssym[c], 8);
            memset(&lens[c * 256], 0, 256);
            memset(&codes[c * 256], 0, 256 * sizeof(uint16_t));
            local.ctx_single++;
        } else if (kind[c] == K_OWN) {
            put_table(&w, &lens[c * 256]);
            huff_build_codes(&lens[c * 256], 256, &codes[c * 256]);
            local.ctx_own++;
        } else {
            memcpy(&lens[c * 256], gl, 256);
            memcpy(&codes[c * 256], gcode, 256 * sizeof(uint16_t));
            local.ctx_fallback++;
        }
    }
    local.header_bits = (uint64_t)w.pos * 8 + (uint64_t)w.n;

    /* 4. Payload: reuse freq buffer as packed (len << 16 | code) to cut memory
     * lookups from two disjoint arrays down to a single 32-bit load, and use
     * 32-bit bswap store. */
    uint32_t *codelen = freq;
    for (int idx = 0; idx < 65536; idx++) {
        codelen[idx] = ((uint32_t)lens[idx] << 16) | codes[idx];
    }

    if (!runs) {
        size_t need_bytes = (size_t)m * 2 + 64;
        if (w.pos + need_bytes <= w.cap || bw_reserve(&w, need_bytes)) {
            uint8_t * const wb = w.buf;
            size_t wpos = w.pos;
            uint64_t wacc = w.acc;
            int wn = w.n;
            prev = v[0];
            for (uint32_t i = 1; i < m; i++) {
                uint8_t s = v[i];
                uint32_t cl = codelen[((uint32_t)prev << 8) | s];
                int l = (int)(cl >> 16);
                wacc = (wacc << l) | (cl & 0xFFFFu);
                wn += l;
                if (wn >= 32) {
                    uint32_t top = (uint32_t)(wacc >> (wn - 32));
                    uint32_t be = NHF3_BSWAP32(top);
                    memcpy(wb + wpos, &be, 4);
                    wpos += 4;
                    wn -= 32;
                }
                prev = s;
            }
            w.pos = wpos;
            w.acc = wacc;
            w.n = wn;
        }
    } else {
#define PUT_RUN(i)                                                              \
    do {                                                                        \
        const RunEnc *e_ = &re[v[i] != 0];                                      \
        int k_ = ilog2_u32(runs[i]);                                            \
        if (!e_->single) bw_put(&w, e_->code[k_], e_->len[k_]);                 \
        bw_put(&w, runs[i] - (1u << k_), k_);                                   \
    } while (0)

        PUT_RUN(0);
        prev = v[0];
        for (uint32_t i = 1; i < m; i++) {
            uint8_t s = v[i];
            uint32_t idx = (uint32_t)prev * 256u + s;
            if (lens[idx]) bw_put(&w, codes[idx], lens[idx]);
            PUT_RUN(i);
            prev = s;
        }
#undef PUT_RUN
    }
    local.data_bits = ((uint64_t)w.pos * 8 + (uint64_t)w.n) - local.header_bits;
    bw_flush(&w);

    free(freq); free(lens); free(codes);
    if (w.oom) { free(w.buf); return NULL; }
    if (st) *st = local;
    *out_size = w.pos;
    return w.buf;
}

NHF3_API uint8_t *nhf3_plane_encode(const uint8_t *sym, uint32_t n, size_t *out_size, NHF3PlaneStats *st) {
    if (!sym || !n || !out_size) return NULL;

    uint32_t reps = 0;
    for (uint32_t i = 1; i < n; i++) reps += (sym[i] == sym[i - 1]);

    /* For large video planes (n >= 64000), avoid encoding both non-RLE and RLE
     * when one regime clearly dominates:
     * - If reps >= 45% of n (typical P-frame or flat anime region), RLE always wins.
     * - If reps < 18% of n (noisy texture), non-RLE always wins because RLE pays >= 1 bit/run. */
    int try_raw = 1, try_rle = ((uint64_t)reps * 20 >= n);
    if (n >= 64000) {
        if ((uint64_t)reps * 100 >= (uint64_t)n * 45) try_raw = 0;
        else if ((uint64_t)reps * 100 < (uint64_t)n * 30) try_rle = 0;
    }

    size_t sa = 0;
    NHF3PlaneStats sta;
    memset(&sta, 0, sizeof(sta));
    uint8_t *best = NULL;
    if (try_raw) {
        best = encode_stream(sym, n, NULL, &sa, &sta);
    }

    if (try_rle) {
        uint8_t *v = (uint8_t *)malloc(n);
        uint32_t *runs = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
        if (v && runs) {
            uint32_t m = 0;
            for (uint32_t i = 0; i < n; i++) {
                if (m && v[m - 1] == sym[i]) runs[m - 1]++;
                else { v[m] = sym[i]; runs[m] = 1; m++; }
            }
            size_t sb = 0;
            NHF3PlaneStats stb;
            memset(&stb, 0, sizeof(stb));
            uint8_t *alt = encode_stream(v, m, runs, &sb, &stb);
            if (alt && (!best || sb < sa)) { free(best); best = alt; sa = sb; sta = stb; }
            else free(alt);
        }
        free(v);
        free(runs);
    }
    if (!best && !try_raw) {
        best = encode_stream(sym, n, NULL, &sa, &sta);
    }
    if (!best) return NULL;
    if (st) *st = sta;
    *out_size = sa;
    return best;
}

static int read_run_table(BitReader *r, int *single, int *cls, HuffDec *d) {
    *single = (int)br_get(r, 1);
    if (*single) { *cls = (int)br_get(r, 5); return 1; }
    uint8_t len[256];
    if (!get_table(r, len)) return 0;
    for (int s = RUN_CLASSES; s < 256; s++) if (len[s]) return 0;
    return huff_build_decoder(len, RUN_CLASSES, d);
}

NHF3_API int nhf3_plane_decode(const uint8_t *in, size_t in_size, uint8_t *out, uint32_t n) {
    if (!in || !out || !n) return NHF3_ERR_ARG;
    if (in_size < 1) return NHF3_ERR_TRUNCATED;

    HuffDec *dec = (HuffDec *)malloc(259 * sizeof(HuffDec)); /* 0..255 ctx, 256 fallback, 257/258 runs */
    if (!dec) return NHF3_ERR_MEM;
    uint8_t kind[256], ssym[256], len[256];
    memset(kind, 0, sizeof(kind));
    memset(ssym, 0, sizeof(ssym));
    int rc = NHF3_OK;
    int rsingle[2] = { 1, 1 }, rcls[2] = { 0, 0 };

    BitReader r;
    br_init(&r, in, in_size);
    uint8_t first = (uint8_t)br_get(&r, 8);
    int rle = (int)br_get(&r, 1);
    if (rle) {
        for (int t = 0; t < 2; t++)
            if (!read_run_table(&r, &rsingle[t], &rcls[t], &dec[257 + t])) { rc = NHF3_ERR_CORRUPT; goto done; }
    }
    int have_fb = (int)br_get(&r, 1);
    if (have_fb) {
        if (!get_table(&r, len) || !huff_build_decoder(len, 256, &dec[256])) { rc = NHF3_ERR_CORRUPT; goto done; }
    }
    HuffDec *ctx_dec[256];
    memset(ctx_dec, 0, sizeof(ctx_dec));
    for (int c = 0; c < 256; c++) {
        if (!br_get(&r, 1)) continue;
        uint32_t k = br_get(&r, 2);
        if (k == 0) { kind[c] = K_SINGLE; ssym[c] = (uint8_t)br_get(&r, 8); }
        else if (k == 1) {
            if (!get_table(&r, len) || !huff_build_decoder(len, 256, &dec[c])) { rc = NHF3_ERR_CORRUPT; goto done; }
            kind[c] = K_OWN;
            ctx_dec[c] = &dec[c];
        } else if (k == 2) {
            if (!have_fb) { rc = NHF3_ERR_CORRUPT; goto done; }
            kind[c] = K_FALLBACK;
            ctx_dec[c] = &dec[256];
        } else { rc = NHF3_ERR_CORRUPT; goto done; }
    }
    if (br_overrun(&r)) { rc = NHF3_ERR_TRUNCATED; goto done; }

    {
        uint8_t prev = first;
        uint32_t pos = 0;
        /* Keep BitReader state in local scalar registers (address never taken)
         * so writes to uint8_t *out cannot force memory reloads via C char-aliasing. */
        uint64_t acc = r.acc;
        int nbits = r.n;
        size_t bpos = r.pos;
        size_t bvpos = r.vpos;
        const size_t bsize = r.size;
        const uint8_t * const bp = r.p;

#define LOCAL_FILL()                                                            \
    do {                                                                        \
        if (bpos + 8 <= bsize) {                                                \
            while (nbits <= 32) {                                               \
                uint32_t b32;                                                   \
                memcpy(&b32, bp + bpos, 4);                                     \
                uint64_t b_ = (uint64_t)NHF3_BSWAP32(b32);                      \
                bpos += 4; bvpos += 4;                                          \
                acc |= b_ << (32 - nbits);                                      \
                nbits += 32;                                                    \
            }                                                                   \
        } else {                                                                \
            while (nbits <= 56) {                                               \
                uint64_t b_ = (bpos < bsize) ? bp[bpos++] : 0;                  \
                bvpos++;                                                        \
                acc |= b_ << (56 - nbits);                                      \
                nbits += 8;                                                     \
            }                                                                   \
        }                                                                       \
    } while (0)

#define LOCAL_DECODE(d_, out_s_)                                                \
    do {                                                                        \
        if (nbits < 16) LOCAL_FILL();                                           \
        uint32_t v_ = (uint32_t)(acc >> 48);                                    \
        uint16_t f_ = (d_)->fast[v_ >> (16 - HUFF_FAST_BITS)];                  \
        if (f_) {                                                               \
            int l_ = f_ >> 8;                                                   \
            acc <<= l_; nbits -= l_;                                            \
            out_s_ = f_ & 0xFF;                                                 \
        } else {                                                                \
            out_s_ = -1;                                                        \
            for (int l_ = HUFF_FAST_BITS + 1; l_ <= (d_)->maxlen; l_++) {       \
                if (v_ < (d_)->limit[l_]) {                                     \
                    acc <<= l_; nbits -= l_;                                    \
                    out_s_ = (d_)->sym[(d_)->base[l_] + (int32_t)(v_ >> (16 - l_))]; \
                    break;                                                      \
                }                                                               \
            }                                                                   \
        }                                                                       \
    } while (0)

#define GET_RUN(sym_, outv_)                                                    \
    do {                                                                        \
        int t_ = ((sym_) != 0);                                                 \
        int k_;                                                                 \
        if (rsingle[t_]) k_ = rcls[t_];                                         \
        else LOCAL_DECODE(&dec[257 + t_], k_);                                  \
        if (k_ < 0 || k_ >= RUN_CLASSES) { rc = NHF3_ERR_CORRUPT; goto done; }  \
        uint32_t ex_ = 0;                                                       \
        if (k_) {                                                               \
            if (nbits < k_) LOCAL_FILL();                                       \
            ex_ = (uint32_t)(acc >> (64 - k_));                                 \
            acc <<= k_; nbits -= k_;                                            \
        }                                                                       \
        outv_ = (uint32_t)(1u << k_) | ex_;                                     \
    } while (0)

        if (rle) {
            uint32_t run;
            GET_RUN(first, run);
            if (run > n) { rc = NHF3_ERR_CORRUPT; goto done; }
            memset(out, first, run);
            pos = run;
            while (pos < n) {
                const HuffDec *d = ctx_dec[prev];
                int s;
                if (d) {
                    LOCAL_DECODE(d, s);
                    if (s < 0) { rc = NHF3_ERR_CORRUPT; goto done; }
                } else if (kind[prev] == K_SINGLE) {
                    s = ssym[prev];
                } else { rc = NHF3_ERR_CORRUPT; goto done; }
                GET_RUN(s, run);
                if (run > n - pos) { rc = NHF3_ERR_CORRUPT; goto done; }
                memset(out + pos, s, run);
                pos += run;
                prev = (uint8_t)s;
            }
        } else {
            out[0] = first;
            pos = 1;
            while (pos < n) {
                const HuffDec *d = ctx_dec[prev];
                int s;
                if (d) {
                    LOCAL_DECODE(d, s);
                    if (s < 0) { rc = NHF3_ERR_CORRUPT; goto done; }
                } else if (kind[prev] == K_SINGLE) {
                    s = ssym[prev];
                } else { rc = NHF3_ERR_CORRUPT; goto done; }
                out[pos++] = (uint8_t)s;
                prev = (uint8_t)s;
            }
        }
        r.acc = acc; r.n = nbits; r.pos = bpos; r.vpos = bvpos;
#undef LOCAL_FILL
#undef LOCAL_DECODE
#undef GET_RUN
        if (br_overrun(&r)) rc = NHF3_ERR_TRUNCATED;
    }
done:
    free(dec);
    return rc;
}

/* ------------------------------------------------------------------------- */
/* Reversible transforms                                                     */
/* ------------------------------------------------------------------------- */
static inline uint8_t med(int a, int b, int c) {
    int mx = a > b ? a : b, mn = a > b ? b : a;
    if (c >= mx) return (uint8_t)mn;
    if (c <= mn) return (uint8_t)mx;
    return (uint8_t)(a + b - c);
}

/* Neighbours with the border rules documented in the review: a=left (or up at
 * x=0, or 0 at the origin), b=up (or a), c=up-left (or b). */
static inline uint8_t predict(const uint8_t *p, int x, int y, int w, int mode) {
    int a = x ? p[-1] : (y ? p[-w] : 0);
    if (mode == NHF3_PRED_LEFT) return (uint8_t)a;
    int b = y ? p[-w] : a;
    int c = (x && y) ? p[-w - 1] : b;
    return med(a, b, c);
}

static void spatial_forward(const uint8_t *src, uint8_t *res, int w, int h, int mode) {
    if (mode == NHF3_PRED_LEFT) nhf3_spatial_forward_left(src, res, w, h);
    else if (mode == NHF3_PRED_MED) nhf3_spatial_forward_med(src, res, w, h);
}

static void spatial_inverse(uint8_t *buf, int w, int h, int mode) {
    if (mode == NHF3_PRED_LEFT) nhf3_spatial_inverse_left(buf, w, h);
    else if (mode == NHF3_PRED_MED) nhf3_spatial_inverse_med(buf, w, h);
}

/* ------------------------------------------------------------------------- */
/* Whole-pixel (palette) N-Huffman mode (NHF3_PRED_PIXEL / --pixelmode)      */
/* ------------------------------------------------------------------------- */
static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : (x > y);
}

static void put_table_gen(BitWriter *w, const uint32_t *syms, const uint8_t *lens, uint32_t k, int symbits) {
    bw_put(w, k - 1, symbits);
    int64_t prev = -1;
    for (uint32_t j = 0; j < k; j++) {
        put_gamma(w, (uint32_t)((int64_t)syms[j] - prev));
        bw_put(w, lens[j], 4);
        prev = syms[j];
    }
}

static int get_table_gen(BitReader *r, uint32_t *syms, uint8_t *lens, uint32_t P, int symbits, uint32_t *out_k) {
    uint32_t k = br_get(r, symbits) + 1;
    if (k < 2 || k > P || k > 16384 || br_overrun(r)) return 0;
    int64_t sym = -1;
    for (uint32_t j = 0; j < k; j++) {
        uint32_t g;
        if (!get_gamma_max(r, &g, symbits)) return 0;
        sym += (int64_t)g;
        if (sym >= (int64_t)P) return 0;
        uint8_t l = (uint8_t)br_get(r, 4);
        if (!l || br_overrun(r)) return 0;
        syms[j] = (uint32_t)sym;
        lens[j] = l;
    }
    *out_k = k;
    return 1;
}

static uint8_t *encode_pixel_stream(const uint32_t *v, uint32_t m, const uint32_t *runs,
                                    uint32_t P, size_t *out_size, NHF3PlaneStats *st) {
    int symbits = ilog2_u32(P - 1) + 1;
    uint32_t t = (m > 1) ? (m - 1) : 0;

    uint64_t *keys = (uint64_t *)malloc((t ? t : 1) * sizeof(uint64_t));
    uint32_t *rs = (uint32_t *)malloc((t ? t : 1) * sizeof(uint32_t));
    uint32_t *rc = (uint32_t *)malloc((t ? t : 1) * sizeof(uint32_t));
    uint8_t *L = (uint8_t *)calloc(t ? t : 1, 1);
    uint16_t *C = (uint16_t *)malloc((t ? t : 1) * sizeof(uint16_t));
    uint32_t *ctx_start = (uint32_t *)calloc(P, sizeof(uint32_t));
    uint32_t *ctx_k = (uint32_t *)calloc(P, sizeof(uint32_t));
    uint32_t *ctx_tot = (uint32_t *)calloc(P, sizeof(uint32_t));
    uint8_t *kind = (uint8_t *)calloc(P, 1);
    uint64_t *owncost = (uint64_t *)calloc(P, sizeof(uint64_t));
    uint8_t *out_buf = NULL;

    if (!keys || !rs || !rc || !L || !C || !ctx_start || !ctx_k || !ctx_tot || !kind || !owncost) goto fail;

    for (uint32_t i = 1; i < m; i++) keys[i - 1] = ((uint64_t)v[i - 1] << 32) | v[i];
    if (t >= 512) {
        /* O(t + P) 2-pass counting sort over [0..P-1] instead of O(t log t) qsort */
        uint64_t *tmp_k = (uint64_t *)malloc(t * sizeof(uint64_t));
        uint32_t *cnt_p = (uint32_t *)calloc(P + 1, sizeof(uint32_t));
        if (tmp_k && cnt_p) {
            for (uint32_t i = 0; i < t; i++) cnt_p[(uint32_t)(keys[i] & 0xFFFFFFFFu) + 1]++;
            for (uint32_t i = 1; i <= P; i++) cnt_p[i] += cnt_p[i - 1];
            for (uint32_t i = 0; i < t; i++) tmp_k[cnt_p[(uint32_t)(keys[i] & 0xFFFFFFFFu)]++] = keys[i];

            memset(cnt_p, 0, (P + 1) * sizeof(uint32_t));
            for (uint32_t i = 0; i < t; i++) cnt_p[(uint32_t)(tmp_k[i] >> 32) + 1]++;
            for (uint32_t i = 1; i <= P; i++) cnt_p[i] += cnt_p[i - 1];
            for (uint32_t i = 0; i < t; i++) keys[cnt_p[(uint32_t)(tmp_k[i] >> 32)]++] = tmp_k[i];
            free(tmp_k);
            free(cnt_p);
        } else {
            free(tmp_k);
            free(cnt_p);
            qsort(keys, t, sizeof(uint64_t), cmp_u64);
        }
    } else if (t > 1) {
        qsort(keys, t, sizeof(uint64_t), cmp_u64);
    }

    uint32_t nr = 0;
    for (uint32_t i = 0; i < t;) {
        uint32_t j = i + 1;
        while (j < t && keys[j] == keys[i]) j++;
        uint32_t c = (uint32_t)(keys[i] >> 32);
        uint32_t s = (uint32_t)(keys[i] & 0xFFFFFFFFu);
        uint32_t cnt = j - i;
        if (ctx_k[c] == 0) ctx_start[c] = nr;
        rs[nr] = s;
        rc[nr] = cnt;
        nr++;
        ctx_k[c]++;
        ctx_tot[c] += cnt;
        i = j;
    }

    uint64_t cost_fb1 = 0;
    int any_fb1 = 0;
    for (uint32_t c = 0; c < P; c++) {
        uint32_t k = ctx_k[c], st_idx = ctx_start[c];
        if (k == 0) { kind[c] = K_UNUSED; continue; }
        if (k == 1) { kind[c] = K_SINGLE; continue; }
        if (k <= 16384) {
            huff_build_lengths(&rc[st_idx], (int)k, HUFF_MAXLEN, &L[st_idx]);
            uint64_t hb = (uint64_t)symbits;
            int64_t prev_s = -1;
            uint64_t db = 0;
            for (uint32_t j = 0; j < k; j++) {
                hb += (uint64_t)gamma_bits((uint32_t)((int64_t)rs[st_idx + j] - prev_s)) + 4;
                prev_s = rs[st_idx + j];
                db += (uint64_t)rc[st_idx + j] * L[st_idx + j];
            }
            owncost[c] = hb + db;
        } else {
            owncost[c] = ~(uint64_t)0;
        }
        uint64_t fb1 = (uint64_t)ctx_tot[c] * (uint64_t)symbits;
        if (owncost[c] < fb1) { kind[c] = K_OWN; cost_fb1 += owncost[c]; }
        else { kind[c] = K_FALLBACK; cost_fb1 += fb1; any_fb1 = 1; }
    }

    int fb_mode = any_fb1 ? 1 : 0;
    uint8_t gl[256];
    uint16_t gcode[256];
    memset(gl, 0, sizeof(gl));
    if (P <= 256) {
        uint8_t kind2[256];
        for (uint32_t c = 0; c < P; c++) kind2[c] = (ctx_k[c] >= 2) ? K_FALLBACK : kind[c];
        for (int it = 0; it < 3; it++) {
            uint32_t g[256];
            for (uint32_t s = 0; s < P; s++) g[s] = 1;
            for (uint32_t c = 0; c < P; c++) {
                if (kind2[c] != K_FALLBACK) continue;
                uint32_t st_idx = ctx_start[c], k = ctx_k[c];
                for (uint32_t j = 0; j < k; j++) g[rs[st_idx + j]] += rc[st_idx + j];
            }
            huff_build_lengths(g, (int)P, HUFF_MAXLEN, gl);
            if (it == 2) break;
            for (uint32_t c = 0; c < P; c++) {
                if (ctx_k[c] < 2) continue;
                uint64_t fb = 0;
                uint32_t st_idx = ctx_start[c], k = ctx_k[c];
                for (uint32_t j = 0; j < k; j++) fb += (uint64_t)rc[st_idx + j] * gl[rs[st_idx + j]];
                kind2[c] = (owncost[c] < fb) ? K_OWN : K_FALLBACK;
            }
        }
        uint64_t cost_fb2 = 0;
        int any_fb2 = 0;
        for (uint32_t c = 0; c < P; c++) {
            if (ctx_k[c] < 2) continue;
            if (kind2[c] == K_OWN) cost_fb2 += owncost[c];
            else {
                any_fb2 = 1;
                uint32_t st_idx = ctx_start[c], k = ctx_k[c];
                for (uint32_t j = 0; j < k; j++) cost_fb2 += (uint64_t)rc[st_idx + j] * gl[rs[st_idx + j]];
            }
        }
        if (any_fb2) {
            cost_fb2 += (uint64_t)table_bits(gl);
            if (cost_fb2 < cost_fb1) {
                memcpy(kind, kind2, P);
                fb_mode = 2;
                huff_build_codes(gl, (int)P, gcode);
            }
        } else {
            memcpy(kind, kind2, P);
            fb_mode = 0;
        }
    }

    RunEnc re[2];
    if (runs) {
        uint32_t hist[2][RUN_CLASSES];
        memset(hist, 0, sizeof(hist));
        for (uint32_t i = 0; i < m; i++) hist[v[i] != 0][ilog2_u32(runs[i])]++;
        run_enc_build(&re[0], hist[0]);
        run_enc_build(&re[1], hist[1]);
    }

    BitWriter w;
    bw_init(&w, (size_t)m / 2 + 4096);
    bw_put(&w, v[0], symbits);
    bw_put(&w, runs ? 1u : 0u, 1);
    if (runs) {
        for (int r_t = 0; r_t < 2; r_t++) {
            bw_put(&w, (uint32_t)re[r_t].single, 1);
            if (re[r_t].single) bw_put(&w, (uint32_t)re[r_t].cls, 5);
            else put_table(&w, re[r_t].len);
        }
    }
    bw_put(&w, (uint32_t)fb_mode, 2);
    if (fb_mode == 2) put_table(&w, gl);

    NHF3PlaneStats local;
    memset(&local, 0, sizeof(local));
    for (uint32_t c = 0; c < P; c++) {
        if (kind[c] == K_UNUSED) { bw_put(&w, 0, 1); local.ctx_unused++; continue; }
        bw_put(&w, 1, 1);
        bw_put(&w, (uint32_t)(kind[c] - 1), 2);
        if (kind[c] == K_SINGLE) {
            bw_put(&w, rs[ctx_start[c]], symbits);
            local.ctx_single++;
        } else if (kind[c] == K_OWN) {
            uint32_t st_idx = ctx_start[c], k = ctx_k[c];
            put_table_gen(&w, &rs[st_idx], &L[st_idx], k, symbits);
            huff_build_codes(&L[st_idx], (int)k, &C[st_idx]);
            local.ctx_own++;
        } else {
            local.ctx_fallback++;
        }
    }
    local.header_bits = (uint64_t)w.pos * 8 + (uint64_t)w.n;

#define PUT_PIX_RUN(idx_)                                                       \
    do {                                                                        \
        if (runs) {                                                             \
            const RunEnc *e_ = &re[v[idx_] != 0];                               \
            int k_ = ilog2_u32(runs[idx_]);                                     \
            if (!e_->single) bw_put(&w, e_->code[k_], e_->len[k_]);             \
            bw_put(&w, runs[idx_] - (1u << k_), k_);                            \
        }                                                                       \
    } while (0)

    PUT_PIX_RUN(0);
    for (uint32_t i = 1; i < m; i++) {
        uint32_t prev = v[i - 1], s = v[i];
        uint8_t knd = kind[prev];
        if (knd == K_OWN) {
            uint32_t lo = ctx_start[prev], hi = lo + ctx_k[prev] - 1;
            while (lo < hi) {
                uint32_t mid = (lo + hi) >> 1;
                if (rs[mid] < s) lo = mid + 1;
                else hi = mid;
            }
            bw_put(&w, C[lo], L[lo]);
        } else if (knd == K_FALLBACK) {
            if (fb_mode == 1) bw_put(&w, s, symbits);
            else bw_put(&w, gcode[s], gl[s]);
        }
        PUT_PIX_RUN(i);
    }
#undef PUT_PIX_RUN

    local.data_bits = ((uint64_t)w.pos * 8 + (uint64_t)w.n) - local.header_bits;
    bw_flush(&w);
    if (!w.oom) {
        if (st) *st = local;
        *out_size = w.pos;
        out_buf = w.buf;
    } else {
        free(w.buf);
    }

fail:
    free(keys); free(rs); free(rc); free(L); free(C);
    free(ctx_start); free(ctx_k); free(ctx_tot); free(kind); free(owncost);
    return out_buf;
}

static uint8_t *pixel_plane_encode(const uint8_t *pal, uint32_t P, int ch,
                                   const uint32_t *sym, uint32_t n,
                                   size_t *out_size, NHF3PlaneStats *st) {
    size_t pal_hdr = 3 + (size_t)P * (size_t)ch;
    if (P == 1) {
        uint8_t *res = (uint8_t *)malloc(pal_hdr);
        if (!res) return NULL;
        res[0] = (uint8_t)(P >> 16); res[1] = (uint8_t)(P >> 8); res[2] = (uint8_t)P;
        memcpy(res + 3, pal, (size_t)ch);
        if (st) { memset(st, 0, sizeof(*st)); st->header_bits = pal_hdr * 8; }
        *out_size = pal_hdr;
        return res;
    }

    uint32_t reps = 0;
    for (uint32_t i = 1; i < n; i++) reps += (sym[i] == sym[i - 1]);
    int try_raw = 1, try_rle = ((uint64_t)reps * 20 >= n);
    if (n >= 64000) {
        if ((uint64_t)reps * 100 >= (uint64_t)n * 45) try_raw = 0;
        else if ((uint64_t)reps * 100 < (uint64_t)n * 30) try_rle = 0;
    }

    size_t sa = 0;
    NHF3PlaneStats sta;
    memset(&sta, 0, sizeof(sta));
    uint8_t *best = NULL;
    if (try_raw) {
        best = encode_pixel_stream(sym, n, NULL, P, &sa, &sta);
    }

    if (try_rle) {
        uint32_t *v = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
        uint32_t *runs = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
        if (v && runs) {
            uint32_t m = 0;
            for (uint32_t i = 0; i < n; i++) {
                if (m && v[m - 1] == sym[i]) runs[m - 1]++;
                else { v[m] = sym[i]; runs[m] = 1; m++; }
            }
            if (m >= 2) {
                size_t sb = 0;
                NHF3PlaneStats stb;
                memset(&stb, 0, sizeof(stb));
                uint8_t *alt = encode_pixel_stream(v, m, runs, P, &sb, &stb);
                if (alt && (!best || sb < sa)) { free(best); best = alt; sa = sb; sta = stb; }
                else free(alt);
            }
        }
        free(v);
        free(runs);
    }
    if (!best && !try_raw) {
        best = encode_pixel_stream(sym, n, NULL, P, &sa, &sta);
    }
    if (!best) return NULL;

    uint8_t *res = (uint8_t *)malloc(pal_hdr + sa);
    if (!res) { free(best); return NULL; }
    res[0] = (uint8_t)(P >> 16); res[1] = (uint8_t)(P >> 8); res[2] = (uint8_t)P;
    memcpy(res + 3, pal, (size_t)P * (size_t)ch);
    memcpy(res + pal_hdr, best, sa);
    free(best);
    sta.header_bits += pal_hdr * 8;
    if (st) *st = sta;
    *out_size = pal_hdr + sa;
    return res;
}

static int pixel_plane_decode(const uint8_t *in, size_t in_size, uint8_t *dst,
                              uint32_t n, int ch, int stride, int pf) {
    if (in_size < 3) return NHF3_ERR_TRUNCATED;
    uint32_t P = ((uint32_t)in[0] << 16) | ((uint32_t)in[1] << 8) | in[2];
    if (P == 0 || P > n) return NHF3_ERR_CORRUPT;
    size_t pal_bytes = (size_t)P * (size_t)ch;
    if (in_size - 3 < pal_bytes) return NHF3_ERR_TRUNCATED;
    const uint8_t *pal = in + 3;
    size_t rem = in_size - 3 - pal_bytes;

    if (P == 1) {
        if (rem != 0) return NHF3_ERR_CORRUPT;
        for (uint32_t i = 0; i < n; i++) {
            uint8_t *d = dst + (size_t)i * stride;
            if (pf) for (int c = 0; c < ch; c++) d[c] = (uint8_t)(d[c] + pal[c]);
            else    for (int c = 0; c < ch; c++) d[c] = pal[c];
        }
        return NHF3_OK;
    }
    if (rem < 1) return NHF3_ERR_TRUNCATED;

    int symbits = ilog2_u32(P - 1) + 1;
    uint8_t *kind = (uint8_t *)calloc(P, 1);
    uint32_t *ctx_val = (uint32_t *)calloc(P, sizeof(uint32_t));
    uint32_t *tmp_syms = (uint32_t *)malloc(16384 * sizeof(uint32_t));
    uint8_t *tmp_lens = (uint8_t *)malloc(16384);
    HuffRankDec *own_dec = NULL;
    uint32_t *sym_pool = NULL;
    uint32_t own_cnt = 0, own_cap = 0, pool_cnt = 0, pool_cap = 0;
    HuffDec rdec[3]; /* 0,1 = runs, 2 = fallback when P <= 256 */
    int rc = NHF3_OK;

    if (!kind || !ctx_val || !tmp_syms || !tmp_lens) { rc = NHF3_ERR_MEM; goto done; }

    BitReader r;
    br_init(&r, in + 3 + pal_bytes, rem);
    uint32_t first = br_get(&r, symbits);
    if (first >= P) { rc = NHF3_ERR_CORRUPT; goto done; }
    int rle = (int)br_get(&r, 1);
    int rsingle[2] = { 1, 1 }, rcls[2] = { 0, 0 };
    if (rle) {
        for (int t = 0; t < 2; t++)
            if (!read_run_table(&r, &rsingle[t], &rcls[t], &rdec[t])) { rc = NHF3_ERR_CORRUPT; goto done; }
    }
    uint32_t fb_mode = br_get(&r, 2);
    if (fb_mode == 3 || (fb_mode == 2 && P > 256)) { rc = NHF3_ERR_CORRUPT; goto done; }
    if (fb_mode == 2) {
        uint8_t gl[256];
        if (!get_table(&r, gl)) { rc = NHF3_ERR_CORRUPT; goto done; }
        for (uint32_t s = P; s < 256; s++) if (gl[s]) { rc = NHF3_ERR_CORRUPT; goto done; }
        if (!huff_build_decoder(gl, (int)P, &rdec[2])) { rc = NHF3_ERR_CORRUPT; goto done; }
    }

    for (uint32_t c = 0; c < P; c++) {
        if (br_overrun(&r)) { rc = NHF3_ERR_TRUNCATED; goto done; }
        if (!br_get(&r, 1)) continue;
        uint32_t knd = br_get(&r, 2);
        if (knd == 0) {
            uint32_t s = br_get(&r, symbits);
            if (s >= P) { rc = NHF3_ERR_CORRUPT; goto done; }
            kind[c] = K_SINGLE;
            ctx_val[c] = s;
        } else if (knd == 1) {
            uint32_t k = 0;
            if (!get_table_gen(&r, tmp_syms, tmp_lens, P, symbits, &k)) { rc = NHF3_ERR_CORRUPT; goto done; }
            if (own_cnt == own_cap) {
                uint32_t ncap = own_cap ? own_cap * 2 : 64;
                HuffRankDec *nd = (HuffRankDec *)realloc(own_dec, (size_t)ncap * sizeof(HuffRankDec));
                if (!nd) { rc = NHF3_ERR_MEM; goto done; }
                own_dec = nd;
                own_cap = ncap;
            }
            if (pool_cnt + k > pool_cap) {
                uint32_t ncap = pool_cap ? pool_cap * 2 : 1024;
                while (ncap < pool_cnt + k) ncap *= 2;
                uint32_t *np = (uint32_t *)realloc(sym_pool, (size_t)ncap * sizeof(uint32_t));
                if (!np) { rc = NHF3_ERR_MEM; goto done; }
                sym_pool = np;
                pool_cap = ncap;
            }
            if (!huff_build_rank_decoder(tmp_lens, tmp_syms, (int)k, &own_dec[own_cnt], sym_pool, pool_cnt)) {
                rc = NHF3_ERR_CORRUPT;
                goto done;
            }
            kind[c] = K_OWN;
            ctx_val[c] = own_cnt++;
            pool_cnt += k;
        } else if (knd == 2) {
            if (!fb_mode) { rc = NHF3_ERR_CORRUPT; goto done; }
            kind[c] = K_FALLBACK;
        } else {
            rc = NHF3_ERR_CORRUPT;
            goto done;
        }
    }
    if (br_overrun(&r)) { rc = NHF3_ERR_TRUNCATED; goto done; }

#define GET_PIX_RUN(sym_, outv_)                                                \
    do {                                                                        \
        int t_ = ((sym_) != 0);                                                 \
        int k_ = rsingle[t_] ? rcls[t_] : huff_decode(&r, &rdec[t_]);           \
        if (k_ < 0 || k_ >= RUN_CLASSES) { rc = NHF3_ERR_CORRUPT; goto done; }  \
        uint32_t ex_ = k_ ? br_get(&r, k_) : 0;                                 \
        outv_ = (uint32_t)(1u << k_) | ex_;                                     \
    } while (0)

#define EMIT_PIXELS(sym_, count_)                                               \
    do {                                                                        \
        const uint8_t *sc_ = pal + (size_t)(sym_) * ch;                         \
        uint32_t end_ = pos + (count_);                                         \
        if (pf) {                                                               \
            for (; pos < end_; pos++) {                                         \
                uint8_t *d_ = dst + (size_t)pos * stride;                       \
                for (int c_ = 0; c_ < ch; c_++) d_[c_] = (uint8_t)(d_[c_] + sc_[c_]); \
            }                                                                   \
        } else {                                                                \
            for (; pos < end_; pos++) {                                         \
                uint8_t *d_ = dst + (size_t)pos * stride;                       \
                for (int c_ = 0; c_ < ch; c_++) d_[c_] = sc_[c_];               \
            }                                                                   \
        }                                                                       \
    } while (0)

    {
        uint32_t prev = first, pos = 0;
        if (rle) {
            uint32_t run;
            GET_PIX_RUN(first, run);
            if (run > n) { rc = NHF3_ERR_CORRUPT; goto done; }
            EMIT_PIXELS(first, run);
        } else {
            EMIT_PIXELS(first, 1);
        }
        while (pos < n) {
            uint8_t knd = kind[prev];
            uint32_t s;
            if (knd == K_SINGLE) {
                s = ctx_val[prev];
            } else if (knd == K_OWN) {
                int dec_s = huff_decode_rank(&r, &own_dec[ctx_val[prev]], sym_pool);
                if (dec_s < 0 || (uint32_t)dec_s >= P) { rc = NHF3_ERR_CORRUPT; goto done; }
                s = (uint32_t)dec_s;
            } else if (knd == K_FALLBACK) {
                if (fb_mode == 1) {
                    s = br_get(&r, symbits);
                    if (s >= P) { rc = NHF3_ERR_CORRUPT; goto done; }
                } else {
                    int dec_s = huff_decode(&r, &rdec[2]);
                    if (dec_s < 0 || (uint32_t)dec_s >= P) { rc = NHF3_ERR_CORRUPT; goto done; }
                    s = (uint32_t)dec_s;
                }
            } else {
                rc = NHF3_ERR_CORRUPT;
                goto done;
            }
            if (rle) {
                uint32_t run;
                GET_PIX_RUN(s, run);
                if (run > n - pos) { rc = NHF3_ERR_CORRUPT; goto done; }
                EMIT_PIXELS(s, run);
            } else {
                EMIT_PIXELS(s, 1);
            }
            prev = s;
        }
        if (br_overrun(&r)) rc = NHF3_ERR_TRUNCATED;
    }
#undef GET_PIX_RUN
#undef EMIT_PIXELS

done:
    free(kind); free(ctx_val); free(tmp_syms); free(tmp_lens);
    free(own_dec); free(sym_pool);
    return rc;
}

/* ------------------------------------------------------------------------- */
/* Container                                                                 */
/* ------------------------------------------------------------------------- */
static void put_u16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put_u32(uint8_t *p, uint32_t v) { put_u16(p, v >> 16); put_u16(p + 2, v & 0xFFFF); }
static uint32_t get_u16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static uint32_t get_u32(const uint8_t *p) { return (get_u16(p) << 16) | get_u16(p + 2); }

static uint8_t *encode_pixel_frame(const uint8_t *src, int w, int h, int ch, int stride,
                                   const uint8_t *prev, uint8_t *recon_out,
                                   size_t *out_size, NHF3PlaneStats *stats) {
    uint32_t n = (uint32_t)w * (uint32_t)h;
    int pframe = prev != NULL;

    uint32_t ht_cap = 1024;
    while (ht_cap < n * 2) ht_cap <<= 1;
    uint32_t *ht_key = (uint32_t *)malloc((size_t)ht_cap * sizeof(uint32_t));
    int32_t *ht_val = (int32_t *)malloc((size_t)ht_cap * sizeof(int32_t));
    uint8_t *pal = (uint8_t *)malloc((size_t)n * (size_t)ch);
    uint32_t *pix = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
    uint8_t *pbuf = NULL, *result = NULL;
    if (!ht_key || !ht_val || !pal || !pix) goto fail;
    memset(ht_val, 0xFF, (size_t)ht_cap * sizeof(int32_t));

    uint32_t P = 0;
    for (uint32_t i = 0; i < n; i++) {
        size_t o = (size_t)i * stride;
        uint8_t col_b[4] = { 0, 0, 0, 0 };
        for (int c = 0; c < ch; c++) {
            col_b[c] = pframe ? (uint8_t)(src[o + c] - prev[o + c]) : src[o + c];
        }
        uint32_t key = (uint32_t)col_b[0] | ((uint32_t)col_b[1] << 8)
                     | ((uint32_t)col_b[2] << 16) | ((uint32_t)col_b[3] << 24);
        uint32_t h_idx = (key * 2654435761u) & (ht_cap - 1);
        while (ht_val[h_idx] >= 0 && ht_key[h_idx] != key) h_idx = (h_idx + 1) & (ht_cap - 1);
        if (ht_val[h_idx] < 0) {
            ht_key[h_idx] = key;
            ht_val[h_idx] = (int32_t)P;
            memcpy(pal + (size_t)P * ch, col_b, (size_t)ch);
            P++;
        }
        pix[i] = (uint32_t)ht_val[h_idx];
    }
    if (recon_out && recon_out != src) memcpy(recon_out, src, (size_t)n * (size_t)stride);

    if (stats) memset(stats, 0, (size_t)ch * sizeof(NHF3PlaneStats));
    size_t plen = 0;
    pbuf = pixel_plane_encode(pal, P, ch, pix, n, &plen, stats ? &stats[0] : NULL);
    if (!pbuf) goto fail;

    size_t total = NHF3_HEADER_BASE + 4 * (size_t)ch + plen;
    result = (uint8_t *)malloc(total);
    if (!result) goto fail;

    memcpy(result, "NHF3", 4);
    result[4] = NHF3_VERSION;
    result[5] = (uint8_t)((pframe ? 1 : 0) | (NHF3_PRED_PIXEL << 1));
    put_u16(result + 6, (uint32_t)w);
    put_u16(result + 8, (uint32_t)h);
    result[10] = (uint8_t)ch;
    size_t off = NHF3_HEADER_BASE;
    put_u32(result + off, (uint32_t)plen); off += 4;
    for (int c = 1; c < ch; c++) { put_u32(result + off, 0); off += 4; }
    memcpy(result + off, pbuf, plen);
    *out_size = total;

fail:
    free(ht_key); free(ht_val); free(pal); free(pix); free(pbuf);
    return result;
}

NHF3_API uint8_t *nhf3_encode(const uint8_t *src, int w, int h, int ch, int stride,
                              const uint8_t *prev, int pred, int decorrelate,
                              size_t *out_size, NHF3PlaneStats *stats) {
    if (!src || !out_size || w <= 0 || h <= 0 || w > 65535 || h > 65535) return NULL;
    if (ch < 1 || ch > 4 || stride < ch || pred < 0 || pred > 3) return NULL;
    if ((uint64_t)w * (uint64_t)h > MAX_PIXELS) return NULL;
    if (pred == NHF3_PRED_PIXEL) {
        return encode_pixel_frame(src, w, h, ch, stride, prev, NULL, out_size, stats);
    }
    uint32_t n = (uint32_t)w * (uint32_t)h;
    int pframe = prev != NULL;
    if (pframe) pred = NHF3_PRED_NONE;
    int dec_ok = decorrelate && ch >= 3;

    uint8_t *planes = (uint8_t *)malloc((size_t)ch * n);
    uint8_t *bufs[4] = { 0 };
    size_t lens[4] = { 0 };
    uint8_t *result = NULL;
    if (!planes) return NULL;

    if (ch == 3 && dec_ok) {
        uint8_t *p0 = planes, *p1 = planes + n, *p2 = planes + 2 * (size_t)n;
        int i;
        if (pframe) {
#ifdef _OPENMP
            #pragma omp parallel for if(n >= 64000)
#endif
            for (i = 0; i < (int)n; i++) {
                size_t o = (size_t)i * stride;
                uint8_t dr = (uint8_t)(src[o + 0] - prev[o + 0]);
                uint8_t dg = (uint8_t)(src[o + 1] - prev[o + 1]);
                uint8_t db = (uint8_t)(src[o + 2] - prev[o + 2]);
                p0[i] = (uint8_t)(dr - dg);
                p1[i] = dg;
                p2[i] = (uint8_t)(db - dg);
            }
        } else {
#ifdef _OPENMP
            #pragma omp parallel for if(n >= 64000)
#endif
            for (i = 0; i < (int)n; i++) {
                size_t o = (size_t)i * stride;
                uint8_t g = src[o + 1];
                p0[i] = (uint8_t)(src[o + 0] - g);
                p1[i] = g;
                p2[i] = (uint8_t)(src[o + 2] - g);
            }
        }
    } else {
        for (int c = 0; c < ch; c++) {
            uint8_t *p = planes + (size_t)c * n;
            for (uint32_t i = 0; i < n; i++) {
                uint8_t v = src[(size_t)i * stride + c];
                if (pframe) v = (uint8_t)(v - prev[(size_t)i * stride + c]);
                p[i] = v;
            }
        }
        if (dec_ok) {
            uint8_t *p0 = planes, *p1 = planes + n, *p2 = planes + 2 * (size_t)n;
            for (uint32_t i = 0; i < n; i++) { p0[i] = (uint8_t)(p0[i] - p1[i]); p2[i] = (uint8_t)(p2[i] - p1[i]); }
        }
    }

    if (pred != NHF3_PRED_NONE) {
        uint8_t *f_planes = (uint8_t *)malloc((size_t)ch * n);
        if (!f_planes) { free(planes); return NULL; }
        int c_sf;
#ifdef _OPENMP
        #pragma omp parallel for if(n >= 64000)
#endif
        for (c_sf = 0; c_sf < ch; c_sf++) {
            spatial_forward(planes + (size_t)c_sf * n, f_planes + (size_t)c_sf * n, w, h, pred);
        }
        free(planes);
        planes = f_planes;
    }

    size_t total = NHF3_HEADER_BASE + 4 * (size_t)ch;
    int enc_ok = 1;
    int c_omp;
#ifdef _OPENMP
    #pragma omp parallel for schedule(dynamic) if(n >= 64000)
#endif
    for (c_omp = 0; c_omp < ch; c_omp++) {
        bufs[c_omp] = nhf3_plane_encode(planes + (size_t)c_omp * n, n, &lens[c_omp], stats ? &stats[c_omp] : NULL);
        if (!bufs[c_omp]) enc_ok = 0;
    }
    if (!enc_ok) goto fail;
    for (int c = 0; c < ch; c++) total += lens[c];
    result = (uint8_t *)malloc(total);
    if (!result) goto fail;

    memcpy(result, "NHF3", 4);
    result[4] = NHF3_VERSION;
    result[5] = (uint8_t)((pframe ? 1 : 0) | (pred << 1) | (dec_ok ? 8 : 0));
    put_u16(result + 6, (uint32_t)w);
    put_u16(result + 8, (uint32_t)h);
    result[10] = (uint8_t)ch;
    size_t off = NHF3_HEADER_BASE;
    for (int c = 0; c < ch; c++) { put_u32(result + off, (uint32_t)lens[c]); off += 4; }
    for (int c = 0; c < ch; c++) { memcpy(result + off, bufs[c], lens[c]); off += lens[c]; }
    *out_size = total;

fail:
    for (int c = 0; c < 4; c++) free(bufs[c]);
    free(planes);
    return result;
}

NHF3_API uint8_t *nhf3_encode_near(const uint8_t *src, int w, int h, int ch, int stride,
                                   const uint8_t *prev, int pred, int decorrelate, int delta,
                                   uint8_t *recon_out, size_t *out_size, NHF3PlaneStats *stats) {
    (void)delta;
    if (recon_out && recon_out != src) memcpy(recon_out, src, (size_t)w * h * stride);
    return nhf3_encode(src, w, h, ch, stride, prev, pred, decorrelate, out_size, stats);
}

NHF3_API uint8_t *nhf3_encode_pixel(const uint8_t *src, int w, int h, int ch, int stride,
                                    const uint8_t *prev, int delta, uint8_t *recon_out,
                                    size_t *out_size, NHF3PlaneStats *stats) {
    (void)delta;
    return encode_pixel_frame(src, w, h, ch, stride, prev, recon_out, out_size, stats);
}

NHF3_API uint8_t *nhf3_encode_auto(const uint8_t *src, int w, int h, int ch, int stride,
                                   const uint8_t *prev, int pred, int decorrelate, int delta,
                                   uint8_t *recon_out, size_t *out_size, NHF3PlaneStats *stats) {
    (void)delta;
    if (!src || w <= 0 || h <= 0 || ch < 1 || ch > 4 || stride < ch || !out_size) return NULL;
    if (recon_out && recon_out != src) memcpy(recon_out, src, (size_t)w * h * stride);
    if (!prev) return nhf3_encode(src, w, h, ch, stride, NULL, pred, decorrelate, out_size, stats);

    int p_pred = (pred == NHF3_PRED_PIXEL) ? NHF3_PRED_PIXEL : NHF3_PRED_NONE;

    /* Single-pass 4x4 scene cut detector */
    if ((uint64_t)w * (uint64_t)h >= 64000 && w >= 8 && h >= 8) {
        uint64_t sad_i = 0, sad_p = 0;
        uint32_t samples = 0, static_p = 0;
        for (int y = 2; y < h; y += 4) {
            for (int x = 2; x < w; x += 4) {
                size_t o = ((size_t)y * (size_t)w + (size_t)x) * (size_t)stride;
                int pix_static = 1;
                for (int c = 0; c < ch; c++) {
                    int cur = src[o + c];
                    int prv = prev[o + c];
                    int dp = cur - prv;
                    int adp = dp < 0 ? -dp : dp;
                    if (adp > 2) pix_static = 0;
                    sad_p += (uint64_t)adp;

                    int a = src[o - stride + c];
                    int b = src[o - (size_t)w * stride + c];
                    int d_pix = src[o - (size_t)w * stride - stride + c];
                    int pr = (pred == NHF3_PRED_PIXEL) ? a : (int)nhf3_med_scalar(a, b, d_pix);
                    int di = cur - pr;
                    sad_i += (uint64_t)(di < 0 ? -di : di);
                }
                samples++;
                if (pix_static) static_p++;
            }
        }
        if (static_p * 4 < samples && sad_p > sad_i * 2) {
            return nhf3_encode(src, w, h, ch, stride, NULL, pred, decorrelate, out_size, stats);
        }
        return nhf3_encode(src, w, h, ch, stride, prev, p_pred, decorrelate, out_size, stats);
    }

    size_t sz_i = 0, sz_p = 0;
    NHF3PlaneStats st_i[4], st_p[4];
    uint8_t *pk_i = nhf3_encode(src, w, h, ch, stride, NULL, pred, decorrelate, &sz_i, st_i);
    uint8_t *pk_p = nhf3_encode(src, w, h, ch, stride, prev, p_pred, decorrelate, &sz_p, st_p);

    if (pk_p && (!pk_i || sz_p < sz_i)) {
        free(pk_i);
        *out_size = sz_p;
        if (stats) memcpy(stats, st_p, (size_t)ch * sizeof(NHF3PlaneStats));
        return pk_p;
    } else if (pk_i) {
        free(pk_p);
        *out_size = sz_i;
        if (stats) memcpy(stats, st_i, (size_t)ch * sizeof(NHF3PlaneStats));
        return pk_i;
    }
    return NULL;
}

NHF3_API int nhf3_probe(const uint8_t *in, size_t n, int *w, int *h, int *ch, int *is_pframe) {
    if (!in || n < NHF3_HEADER_BASE) return NHF3_ERR_TRUNCATED;
    if (memcmp(in, "NHF3", 4) != 0) return NHF3_ERR_MAGIC;
    if (in[4] != NHF3_VERSION) return NHF3_ERR_MAGIC;
    int cw = (int)get_u16(in + 6), chh = (int)get_u16(in + 8), cc = in[10];
    if (cw <= 0 || chh <= 0 || cc < 1 || cc > 4) return NHF3_ERR_CORRUPT;
    if (w) *w = cw;
    if (h) *h = chh;
    if (ch) *ch = cc;
    if (is_pframe) *is_pframe = in[5] & 1;
    return NHF3_OK;
}

NHF3_API int nhf3_decode(const uint8_t *in, size_t n, uint8_t *dst, int w, int h, int stride) {
    int pw, ph, ch, pf;
    int rc = nhf3_probe(in, n, &pw, &ph, &ch, &pf);
    if (rc != NHF3_OK) return rc;
    if (!dst || pw != w || ph != h || stride < ch) return NHF3_ERR_SIZE;
    if ((uint64_t)pw * (uint64_t)ph > MAX_PIXELS) return NHF3_ERR_SIZE;
    uint32_t cnt = (uint32_t)pw * (uint32_t)ph;
    int pred = (in[5] >> 1) & 3, decor = (in[5] >> 3) & 1;
    if (decor && ch < 3) return NHF3_ERR_CORRUPT;
    if (pf && pred && pred != NHF3_PRED_PIXEL) return NHF3_ERR_CORRUPT;

    size_t hdr = NHF3_HEADER_BASE + 4 * (size_t)ch;
    if (n < hdr) return NHF3_ERR_TRUNCATED;
    size_t lens[4], off = hdr;
    for (int c = 0; c < ch; c++) {
        lens[c] = get_u32(in + NHF3_HEADER_BASE + 4 * (size_t)c);
        if (lens[c] > n - off) return NHF3_ERR_TRUNCATED;
        off += lens[c];
    }
    if (off != n) return NHF3_ERR_CORRUPT;

    if (pred == NHF3_PRED_PIXEL) {
        if (decor) return NHF3_ERR_CORRUPT;
        for (int c = 1; c < ch; c++) if (lens[c] != 0) return NHF3_ERR_CORRUPT;
        return pixel_plane_decode(in + hdr, lens[0], dst, cnt, ch, stride, pf);
    }

    size_t need_planes = (size_t)ch * cnt;
    uint8_t *planes = (uint8_t *)malloc(need_planes);
    if (!planes) return NHF3_ERR_MEM;
    size_t offs[4];
    offs[0] = hdr;
    for (int c = 1; c < ch; c++) offs[c] = offs[c - 1] + lens[c - 1];

    int dec_err = NHF3_OK;
    int c_omp;
#ifdef _OPENMP
    #pragma omp parallel for schedule(dynamic) if(cnt >= 64000)
#endif
    for (c_omp = 0; c_omp < ch; c_omp++) {
        int r_c = nhf3_plane_decode(in + offs[c_omp], lens[c_omp], planes + (size_t)c_omp * cnt, cnt);
        if (r_c != NHF3_OK) dec_err = r_c;
    }
    if (dec_err != NHF3_OK) { free(planes); return dec_err; }

    if (pred != NHF3_PRED_NONE) {
#ifdef _OPENMP
        #pragma omp parallel for if(cnt >= 64000)
#endif
        for (c_omp = 0; c_omp < ch; c_omp++) spatial_inverse(planes + (size_t)c_omp * cnt, pw, ph, pred);
    }
    if (ch == 3 && decor) {
        const uint8_t *p0 = planes, *p1 = planes + cnt, *p2 = planes + 2 * (size_t)cnt;
        int i;
        if (pf) {
#ifdef _OPENMP
            #pragma omp parallel for if(cnt >= 64000)
#endif
            for (i = 0; i < (int)cnt; i++) {
                uint8_t *d = dst + (size_t)i * stride;
                uint8_t g = p1[i];
                d[0] = (uint8_t)(d[0] + p0[i] + g);
                d[1] = (uint8_t)(d[1] + g);
                d[2] = (uint8_t)(d[2] + p2[i] + g);
                if (stride >= 4) d[3] = 255;
            }
        } else {
#ifdef _OPENMP
            #pragma omp parallel for if(cnt >= 64000)
#endif
            for (i = 0; i < (int)cnt; i++) {
                uint8_t *d = dst + (size_t)i * stride;
                uint8_t g = p1[i];
                d[0] = (uint8_t)(p0[i] + g);
                d[1] = g;
                d[2] = (uint8_t)(p2[i] + g);
                if (stride >= 4) d[3] = 255;
            }
        }
    } else {
        if (decor) {
            uint8_t *p0 = planes, *p1 = planes + cnt, *p2 = planes + 2 * (size_t)cnt;
            for (uint32_t i = 0; i < cnt; i++) { p0[i] = (uint8_t)(p0[i] + p1[i]); p2[i] = (uint8_t)(p2[i] + p1[i]); }
        }
        for (int c = 0; c < ch; c++) {
            const uint8_t *p = planes + (size_t)c * cnt;
            if (pf) for (uint32_t i = 0; i < cnt; i++) dst[(size_t)i * stride + c] = (uint8_t)(dst[(size_t)i * stride + c] + p[i]);
            else    for (uint32_t i = 0; i < cnt; i++) dst[(size_t)i * stride + c] = p[i];
        }
        if (ch == 3 && stride >= 4) {
            for (uint32_t i = 0; i < cnt; i++) {
                dst[(size_t)i * stride + 3] = 255;
            }
        }
    }
    free(planes);
    return NHF3_OK;
}

NHF3_API void nhf3_free(void *p) { free(p); }
NHF3_API void *nhf3_malloc(size_t n) { return malloc(n); }

/* ========================================================================= */
/* PHF File Container (PHF v1.0) Implementation                              */
/* ========================================================================= */

PHF_API void phf_free(void *p) {
    if (p) free(p);
}

PHF_API int phf_probe_memory(const uint8_t *data, size_t size, phf_info_t *out_info) {
    if (!data || size < 20) return -1;
    if (memcmp(data, "PHF1", 4) != 0) return -2;
    if (data[4] != PHF_VERSION) return -3;
    
    if (out_info) {
        out_info->channels = data[5];
        out_info->flags = data[6];
        out_info->width = (uint32_t)data[8] | ((uint32_t)data[9] << 8) | ((uint32_t)data[10] << 16) | ((uint32_t)data[11] << 24);
        out_info->height = (uint32_t)data[12] | ((uint32_t)data[13] << 8) | ((uint32_t)data[14] << 16) | ((uint32_t)data[15] << 24);
        out_info->payload_size = (size_t)((uint32_t)data[16] | ((uint32_t)data[17] << 8) | ((uint32_t)data[18] << 16) | ((uint32_t)data[19] << 24));
    }
    return 0;
}

PHF_API uint8_t* phf_encode_memory(const uint8_t *pixels, uint32_t width, uint32_t height, uint8_t channels, size_t *out_size) {
    if (!pixels || width == 0 || height == 0 || (channels != 3 && channels != 4) || !out_size) {
        return NULL;
    }
    
    size_t payload_sz = 0;
    int decorrelate = (channels >= 3) ? 1 : 0;
    uint8_t *payload = nhf3_encode(pixels, (int)width, (int)height, (int)channels, (int)channels, NULL, NHF3_PRED_MED, decorrelate, &payload_sz, NULL);
    if (!payload) return NULL;
    
    size_t total_sz = 20 + payload_sz;
    uint8_t *phf_buf = (uint8_t*)malloc(total_sz);
    if (!phf_buf) {
        nhf3_free(payload);
        return NULL;
    }
    
    /* Header (20 bytes) */
    memcpy(phf_buf, "PHF1", 4);
    phf_buf[4] = (uint8_t)PHF_VERSION;
    phf_buf[5] = channels;
    phf_buf[6] = (uint8_t)((channels == 4 ? 1 : 0) | (2 << 1) | (decorrelate << 2));
    phf_buf[7] = 0; /* reserved */
    
    /* Width */
    phf_buf[8]  = (uint8_t)(width & 0xFF);
    phf_buf[9]  = (uint8_t)((width >> 8) & 0xFF);
    phf_buf[10] = (uint8_t)((width >> 16) & 0xFF);
    phf_buf[11] = (uint8_t)((width >> 24) & 0xFF);
    
    /* Height */
    phf_buf[12] = (uint8_t)(height & 0xFF);
    phf_buf[13] = (uint8_t)((height >> 8) & 0xFF);
    phf_buf[14] = (uint8_t)((height >> 16) & 0xFF);
    phf_buf[15] = (uint8_t)((height >> 24) & 0xFF);
    
    /* Payload Size */
    phf_buf[16] = (uint8_t)(payload_sz & 0xFF);
    phf_buf[17] = (uint8_t)((payload_sz >> 8) & 0xFF);
    phf_buf[18] = (uint8_t)((payload_sz >> 16) & 0xFF);
    phf_buf[19] = (uint8_t)((payload_sz >> 24) & 0xFF);
    
    memcpy(phf_buf + 20, payload, payload_sz);
    nhf3_free(payload);
    
    *out_size = total_sz;
    return phf_buf;
}

PHF_API int phf_decode_memory(const uint8_t *phf_data, size_t phf_size, uint8_t *dst_pixels, uint32_t width, uint32_t height, uint8_t stride) {
    phf_info_t info;
    int rc = phf_probe_memory(phf_data, phf_size, &info);
    if (rc != 0) return rc;
    
    if (info.width != width || info.height != height) {
        return -4; /* Mismatch dimensions */
    }
    if (stride == 0) stride = info.channels;
    if (stride < info.channels) {
        return -4; /* Stride cannot be less than source channels */
    }
    if (phf_size < 20 + info.payload_size) {
        return -5; /* Truncated */
    }
    
    return nhf3_decode(phf_data + 20, info.payload_size, dst_pixels, (int)width, (int)height, (int)stride);
}

PHF_API int phf_write_file(const char *filepath, const uint8_t *pixels, uint32_t width, uint32_t height, uint8_t channels) {
    if (!filepath || !pixels) return -1;
    size_t out_sz = 0;
    uint8_t *phf_bytes = phf_encode_memory(pixels, width, height, channels, &out_sz);
    if (!phf_bytes) return -2;
    
    FILE *fp = fopen(filepath, "wb");
    if (!fp) {
        phf_free(phf_bytes);
        return -3;
    }
    size_t written = fwrite(phf_bytes, 1, out_sz, fp);
    fclose(fp);
    phf_free(phf_bytes);
    return (written == out_sz) ? 0 : -4;
}

PHF_API uint8_t* phf_read_file(const char *filepath, phf_info_t *out_info) {
    if (!filepath) return NULL;
    FILE *fp = fopen(filepath, "rb");
    if (!fp) return NULL;
    
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 20) { fclose(fp); return NULL; }
    
    uint8_t *file_data = (uint8_t*)malloc((size_t)sz);
    if (!file_data) { fclose(fp); return NULL; }
    
    if (fread(file_data, 1, (size_t)sz, fp) != (size_t)sz) {
        free(file_data);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    
    phf_info_t info;
    if (phf_probe_memory(file_data, (size_t)sz, &info) != 0) {
        free(file_data);
        return NULL;
    }
    
    size_t img_bytes = (size_t)info.width * (size_t)info.height * (size_t)info.channels;
    uint8_t *dst_rgb = (uint8_t*)malloc(img_bytes);
    if (!dst_rgb) {
        free(file_data);
        return NULL;
    }
    
    int dec_rc = phf_decode_memory(file_data, (size_t)sz, dst_rgb, info.width, info.height, info.channels);
    free(file_data);
    if (dec_rc != 0) {
        free(dst_rgb);
        return NULL;
    }
    
    if (out_info) *out_info = info;
    return dst_rgb;
}

