/*
 * Copyright (c) 2009-2014 Kazuho Oku, Tokuhiro Matsuno, Daisuke Murase,
 *                         Shigeo Mitsunari
 *
 * The software is licensed under either the MIT License (below) or the Perl
 * license.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * AVX2 optimizations based on the Cloudflare patch by Vlad Krasnov.
 * See: https://blog.cloudflare.com/improving-picohttpparser-further-with-avx2/
 */

#include <assert.h>
#include <stddef.h>
#include <string.h>
#ifdef __AVX2__
#include <immintrin.h>
#elif defined(__SSE4_2__)
#ifdef _MSC_VER
#include <nmmintrin.h>
#else
#include <x86intrin.h>
#endif
#endif
#include "picohttpparser.h"

#if __GNUC__ >= 3
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#else
#define likely(x) (x)
#define unlikely(x) (x)
#endif

#ifdef _MSC_VER
#define ALIGNED(n) _declspec(align(n))
#else
#define ALIGNED(n) __attribute__((aligned(n)))
#endif

#define IS_PRINTABLE_ASCII(c) ((unsigned char)(c)-040u < 0137u)

#define CHECK_EOF()                                                                                                                \
    if (buf == buf_end) {                                                                                                          \
        *ret = -2;                                                                                                                 \
        return NULL;                                                                                                               \
    }

#define EXPECT_CHAR_NO_CHECK(ch)                                                                                                   \
    if (*buf++ != ch) {                                                                                                            \
        *ret = -1;                                                                                                                 \
        return NULL;                                                                                                               \
    }

#define EXPECT_CHAR(ch)                                                                                                            \
    CHECK_EOF();                                                                                                                   \
    EXPECT_CHAR_NO_CHECK(ch);

#define ADVANCE_TOKEN(tok, toklen)                                                                                                 \
    do {                                                                                                                           \
        const char *tok_start = buf;                                                                                               \
        static const char ALIGNED(16) ranges2[16] = "\000\040\177\177";                                                            \
        int found2;                                                                                                                \
        buf = findchar_fast(buf, buf_end, ranges2, 4, &found2);                                                                    \
        if (!found2) {                                                                                                             \
            CHECK_EOF();                                                                                                           \
        }                                                                                                                          \
        while (1) {                                                                                                                \
            if (*buf == ' ') {                                                                                                     \
                break;                                                                                                             \
            } else if (unlikely(!IS_PRINTABLE_ASCII(*buf))) {                                                                      \
                if ((unsigned char)*buf < '\040' || *buf == '\177') {                                                              \
                    *ret = -1;                                                                                                     \
                    return NULL;                                                                                                   \
                }                                                                                                                  \
            }                                                                                                                      \
            ++buf;                                                                                                                 \
            CHECK_EOF();                                                                                                           \
        }                                                                                                                          \
        tok = tok_start;                                                                                                           \
        toklen = buf - tok_start;                                                                                                  \
    } while (0)

static const char *token_char_map = "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
                                    "\0\1\0\1\1\1\1\1\0\0\1\1\0\1\1\0\1\1\1\1\1\1\1\1\1\1\0\0\0\0\0\0"
                                    "\0\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\0\0\0\1\1"
                                    "\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\0\1\0\1\0"
                                    "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
                                    "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
                                    "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
                                    "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0";

static const char *findchar_fast(const char *buf, const char *buf_end, const char *ranges, size_t ranges_size, int *found)
{
    *found = 0;
#if defined(__SSE4_2__)
    if (likely(buf_end - buf >= 16)) {
        __m128i ranges16 = _mm_loadu_si128((const __m128i *)ranges);

        size_t left = (buf_end - buf) & ~15;
        do {
            __m128i b16 = _mm_loadu_si128((const __m128i *)buf);
            int r = _mm_cmpestri(ranges16, ranges_size, b16, 16, _SIDD_LEAST_SIGNIFICANT | _SIDD_CMP_RANGES | _SIDD_UBYTE_OPS);
            if (unlikely(r != 16)) {
                buf += r;
                *found = 1;
                break;
            }
            buf += 16;
            left -= 16;
        } while (likely(left != 0));
    }
#else
    /* suppress unused parameter warning */
    (void)buf_end;
    (void)ranges;
    (void)ranges_size;
#endif
    return buf;
}

/* ============================================================================
 * AVX2 bitmap-based header parser (Cloudflare patch by Vlad Krasnov)
 *
 * Strategy: instead of scanning 16 bytes at a time with PCMPESTRI to find
 * ONE delimiter, we build bitmaps of ALL interesting positions across 128
 * bytes (4 x YMM registers), then use TZCNT (BMI2) to jump between them.
 *
 * Two bitmaps are computed simultaneously:
 *   rr0 — control chars (0x00-0x1f) OR colon (0x3a): finds end of header name
 *   rr1 — control chars except TAB (0x00-0x08, 0x0a-0x1f) OR DEL (0x7f):
 *          finds end of header value (CR/LF)
 *
 * Safety notes:
 *   - All loads use _mm256_loadu_si256 (unaligned) — no alignment requirements
 *   - Tail bytes (< 128) are copied into a zeroed tmpbuf to prevent OOB reads
 *   - Bitmap bits beyond buf_end are masked off via the zeroed padding
 *   - TZCNT returns 64 when input is 0 (no match), guarding against false hits
 *   - All positions found via TZCNT are validated against buf_end before access
 * ============================================================================ */

#ifdef __AVX2__

static unsigned long TZCNT(unsigned long long in)
{
    unsigned long res;
    asm("tzcnt %1, %0\n\t" : "=r"(res) : "r"(in));
    return res;
}

static void find_ranges32(__m256i b0, unsigned long *range0, unsigned long *range1)
{
    const __m256i rr0 = _mm256_set1_epi8(0x00 - 1);
    const __m256i rr1 = _mm256_set1_epi8(0x1f + 1);
    const __m256i rr2 = _mm256_set1_epi8(0x3a);       /* ':' */
    const __m256i rr4 = _mm256_set1_epi8(0x7f);       /* DEL */
    const __m256i rr7 = _mm256_set1_epi8(0x09);       /* TAB */

    /* 0 <= x (signed comparison: x > -1) */
    __m256i gz0 = _mm256_cmpgt_epi8(b0, rr0);
    /* 0 <= x <= 0x1f */
    __m256i z_1f_0 = _mm256_and_si256(_mm256_cmpgt_epi8(rr1, b0), gz0);
    /* range0: (0<=x<=0x1f) || (x==0x3a) — control chars OR colon */
    __m256i range0_0 = _mm256_or_si256(_mm256_cmpeq_epi8(rr2, b0), z_1f_0);
    /* range1: control chars except TAB, OR DEL */
    __m256i range1_0 = _mm256_or_si256(
        _mm256_cmpeq_epi8(rr4, b0),
        _mm256_andnot_si256(_mm256_cmpeq_epi8(b0, rr7), z_1f_0));

    *range0 = (unsigned int)_mm256_movemask_epi8(range0_0);
    *range1 = (unsigned int)_mm256_movemask_epi8(range1_0);
}

static void find_ranges64(__m256i b0, __m256i b1,
                          unsigned long *range0, unsigned long *range1)
{
    const __m256i rr0 = _mm256_set1_epi8(0x00 - 1);
    const __m256i rr1 = _mm256_set1_epi8(0x1f + 1);
    const __m256i rr2 = _mm256_set1_epi8(0x3a);
    const __m256i rr4 = _mm256_set1_epi8(0x7f);
    const __m256i rr7 = _mm256_set1_epi8(0x09);

    __m256i gz0 = _mm256_cmpgt_epi8(b0, rr0);
    __m256i gz1 = _mm256_cmpgt_epi8(b1, rr0);

    __m256i z_1f_0 = _mm256_and_si256(_mm256_cmpgt_epi8(rr1, b0), gz0);
    __m256i z_1f_1 = _mm256_and_si256(_mm256_cmpgt_epi8(rr1, b1), gz1);

    __m256i range0_0 = _mm256_or_si256(_mm256_cmpeq_epi8(rr2, b0), z_1f_0);
    __m256i range0_1 = _mm256_or_si256(_mm256_cmpeq_epi8(rr2, b1), z_1f_1);

    __m256i range1_0 = _mm256_or_si256(_mm256_cmpeq_epi8(rr4, b0),
                                       _mm256_andnot_si256(_mm256_cmpeq_epi8(b0, rr7), z_1f_0));
    __m256i range1_1 = _mm256_or_si256(_mm256_cmpeq_epi8(rr4, b1),
                                       _mm256_andnot_si256(_mm256_cmpeq_epi8(b1, rr7), z_1f_1));

    unsigned int r0 = _mm256_movemask_epi8(range0_0);
    unsigned int r1 = _mm256_movemask_epi8(range0_1);
    *range0 = r0 ^ ((unsigned long)r1 << 32);

    r0 = _mm256_movemask_epi8(range1_0);
    r1 = _mm256_movemask_epi8(range1_1);
    *range1 = r0 ^ ((unsigned long)r1 << 32);
}

/* Scan up to 128 bytes and produce two 128-bit bitmaps (as pairs of unsigned long).
 *
 * Safety: when remaining data < 128 bytes, tail bytes are copied into a
 * zero-initialized tmpbuf[32]. Zeroed positions in the bitmap correspond
 * to NUL bytes (0x00) which are control characters — they will be flagged
 * in both bitmaps, but the caller will hit buf_end before reaching them,
 * so they never cause false-positive matches or OOB reads. */
static void find_ranges(const char *buf, const char *buf_end,
                        unsigned long *range0, unsigned long *range1)
{
    const __m256i rr0_v = _mm256_set1_epi8(0x00 - 1);
    const __m256i rr1_v = _mm256_set1_epi8(0x1f + 1);
    const __m256i rr2_v = _mm256_set1_epi8(0x3a);
    const __m256i rr4_v = _mm256_set1_epi8(0x7f);
    const __m256i rr7_v = _mm256_set1_epi8(0x09);

    __m256i b0, b1, b2, b3;
    unsigned char tmpbuf[32];
    int i;
    int dist = (int)(buf_end - buf);

    if (dist < 128) {
        /* Zero-fill tmpbuf to avoid OOB reads on tail bytes.
         * NUL bytes (0x00) are control chars and will be flagged in bitmaps,
         * but the caller always checks buf < buf_end before dereferencing. */
        memset(tmpbuf, 0, sizeof(tmpbuf));
        for (i = 0; i < (dist & 31); i++)
            tmpbuf[i] = buf[(dist & (-32)) + i];

        if (dist >= 96) {
            b0 = _mm256_loadu_si256((const __m256i *)(buf + 32 * 0));
            b1 = _mm256_loadu_si256((const __m256i *)(buf + 32 * 1));
            b2 = _mm256_loadu_si256((const __m256i *)(buf + 32 * 2));
            b3 = _mm256_loadu_si256((const __m256i *)tmpbuf);
        } else if (dist >= 64) {
            b0 = _mm256_loadu_si256((const __m256i *)(buf + 32 * 0));
            b1 = _mm256_loadu_si256((const __m256i *)(buf + 32 * 1));
            b2 = _mm256_loadu_si256((const __m256i *)tmpbuf);
            b3 = _mm256_setzero_si256();
        } else {
            if (dist < 32) {
                b0 = _mm256_loadu_si256((const __m256i *)tmpbuf);
                return find_ranges32(b0, range0, range1);
            } else {
                b0 = _mm256_loadu_si256((const __m256i *)(buf + 32 * 0));
                b1 = _mm256_loadu_si256((const __m256i *)tmpbuf);
                return find_ranges64(b0, b1, range0, range1);
            }
        }
    } else {
        /* Full 128 bytes available — safe to load all four registers */
        b0 = _mm256_loadu_si256((const __m256i *)(buf + 32 * 0));
        b1 = _mm256_loadu_si256((const __m256i *)(buf + 32 * 1));
        b2 = _mm256_loadu_si256((const __m256i *)(buf + 32 * 2));
        b3 = _mm256_loadu_si256((const __m256i *)(buf + 32 * 3));
    }

    /* Apply range checks across all 4 registers */
    __m256i gz0 = _mm256_cmpgt_epi8(b0, rr0_v);
    __m256i gz1 = _mm256_cmpgt_epi8(b1, rr0_v);
    __m256i gz2 = _mm256_cmpgt_epi8(b2, rr0_v);
    __m256i gz3 = _mm256_cmpgt_epi8(b3, rr0_v);

    __m256i z_1f_0 = _mm256_and_si256(_mm256_cmpgt_epi8(rr1_v, b0), gz0);
    __m256i z_1f_1 = _mm256_and_si256(_mm256_cmpgt_epi8(rr1_v, b1), gz1);
    __m256i z_1f_2 = _mm256_and_si256(_mm256_cmpgt_epi8(rr1_v, b2), gz2);
    __m256i z_1f_3 = _mm256_and_si256(_mm256_cmpgt_epi8(rr1_v, b3), gz3);

    __m256i range0_0 = _mm256_or_si256(_mm256_cmpeq_epi8(rr2_v, b0), z_1f_0);
    __m256i range0_1 = _mm256_or_si256(_mm256_cmpeq_epi8(rr2_v, b1), z_1f_1);
    __m256i range0_2 = _mm256_or_si256(_mm256_cmpeq_epi8(rr2_v, b2), z_1f_2);
    __m256i range0_3 = _mm256_or_si256(_mm256_cmpeq_epi8(rr2_v, b3), z_1f_3);

    __m256i range1_0 = _mm256_or_si256(_mm256_cmpeq_epi8(rr4_v, b0),
                                       _mm256_andnot_si256(_mm256_cmpeq_epi8(b0, rr7_v), z_1f_0));
    __m256i range1_1 = _mm256_or_si256(_mm256_cmpeq_epi8(rr4_v, b1),
                                       _mm256_andnot_si256(_mm256_cmpeq_epi8(b1, rr7_v), z_1f_1));
    __m256i range1_2 = _mm256_or_si256(_mm256_cmpeq_epi8(rr4_v, b2),
                                       _mm256_andnot_si256(_mm256_cmpeq_epi8(b2, rr7_v), z_1f_2));
    __m256i range1_3 = _mm256_or_si256(_mm256_cmpeq_epi8(rr4_v, b3),
                                       _mm256_andnot_si256(_mm256_cmpeq_epi8(b3, rr7_v), z_1f_3));

    /* Combine 32-bit masks into 128-bit bitmaps (as pairs of 64-bit words) */
    unsigned int r0, r1;
    r0 = _mm256_movemask_epi8(range0_0);
    r1 = _mm256_movemask_epi8(range0_1);
    range0[0] = r0 ^ ((unsigned long)r1 << 32);
    r0 = _mm256_movemask_epi8(range0_2);
    r1 = _mm256_movemask_epi8(range0_3);
    range0[1] = r0 ^ ((unsigned long)r1 << 32);

    r0 = _mm256_movemask_epi8(range1_0);
    r1 = _mm256_movemask_epi8(range1_1);
    range1[0] = r0 ^ ((unsigned long)r1 << 32);
    r0 = _mm256_movemask_epi8(range1_2);
    r1 = _mm256_movemask_epi8(range1_3);
    range1[1] = r0 ^ ((unsigned long)r1 << 32);
}

/* Advance buf through bitmap rr[], searching for the first set bit.
 * Returns 1 if found (buf updated), 0 if exhausted (buf set past window).
 *
 * Safety: TZCNT returns 64 when the input is 0 (no bits set), so the
 * `find < 64` check correctly rejects empty bitmaps without UB. The shift
 * `rr[0] >> distance` is only performed when distance < 64 (guarded by
 * the `if (distance >= 64)` branch), avoiding UB from over-shifting. */
static int advance_bitmap(const char **buf_p, const char *prep_start,
                          unsigned long rr[2])
{
    const char *buf = *buf_p;
    unsigned long distance = (unsigned long)(buf - prep_start);

    if (distance >= 64) {
        /* Second half of 128-bit window */
        unsigned long index = rr[1] >> (distance - 64);
        unsigned long find = TZCNT(index);
        if (find < 64) {
            *buf_p = buf + find;
            return 1;
        }
        /* Window exhausted */
        *buf_p = prep_start + 128;
        return 0;
    }

    /* First half */
    unsigned long index = rr[0] >> distance;
    unsigned long find = TZCNT(index);
    if (find < 64) {
        *buf_p = buf + find;
        return 1;
    }
    /* Try second half */
    find = TZCNT(rr[1]);
    if (find < 64) {
        *buf_p = buf + (64 - distance) + find;
        return 1;
    }
    /* Window exhausted */
    *buf_p = prep_start + 128;
    return 0;
}

static const char *parse_headers_avx2(const char *buf, const char *buf_end,
                                      struct phr_header *headers, size_t *num_headers,
                                      size_t max_headers, int *ret)
{
    unsigned long rr0[2] = {0, 0}; /* bitmap: control chars + colon */
    unsigned long rr1[2] = {0, 0}; /* bitmap: control chars (except TAB) + DEL */
    const char *prep_start = NULL; /* start of current 128-byte scanned window */

    for (;; ++*num_headers) {
        CHECK_EOF();
        if (*buf == '\015') {
            ++buf;
            EXPECT_CHAR('\012');
            break;
        } else if (*buf == '\012') {
            ++buf;
            break;
        }
        if (*num_headers == max_headers) {
            *ret = -1;
            return NULL;
        }
        if (!(*num_headers != 0 && (*buf == ' ' || *buf == '\t'))) {
            /* Not a continuation line — parse header name up to ':' */
            if (!token_char_map[(unsigned char)*buf]) {
                *ret = -1;
                return NULL;
            }
            headers[*num_headers].name = buf;

            /* Find ':' using rr0 bitmap */
            {
                int found = 0;
                do {
                    unsigned long distance = (unsigned long)(buf - prep_start);
                    if (unlikely(distance >= 128) || prep_start == NULL) {
                        prep_start = buf;
                        find_ranges(buf, buf_end, rr0, rr1);
                    }
                    found = advance_bitmap(&buf, prep_start, rr0);
                    if (found)
                        break;
                } while (buf < buf_end);

                if (!found) {
                    *ret = -2;
                    return NULL;
                }
            }

            /* Verify the character at buf is actually a colon, not a control char.
             * Safety: buf was found via bitmap, but rr0 flags both ':' and control
             * chars. If it's a control char, the header name is invalid. */
            if (unlikely(buf >= buf_end)) {
                *ret = -2;
                return NULL;
            }
            if (*buf != ':') {
                /* Control char in header name — invalid request */
                *ret = -1;
                return NULL;
            }

            headers[*num_headers].name_len = buf - headers[*num_headers].name;
            if (headers[*num_headers].name_len == 0) {
                *ret = -1;
                return NULL;
            }
            ++buf;
            /* Skip OWS (optional whitespace) after colon */
            for (;; ++buf) {
                CHECK_EOF();
                if (!(*buf == ' ' || *buf == '\t'))
                    break;
            }
        } else {
            headers[*num_headers].name = NULL;
            headers[*num_headers].name_len = 0;
        }

        /* Parse header value — find end-of-line using rr1 bitmap */
        {
            const char *token_start = buf;
            int found = 0;

            do {
                unsigned long distance = (unsigned long)(buf - prep_start);
                if (unlikely(distance >= 128) || prep_start == NULL) {
                    prep_start = buf;
                    find_ranges(buf, buf_end, rr0, rr1);
                }
                found = advance_bitmap(&buf, prep_start, rr1);
                if (found)
                    break;
            } while (buf < buf_end);

            if (!found) {
                *ret = -2;
                return NULL;
            }

            /* Safety: verify buf is within bounds before dereferencing */
            if (unlikely(buf >= buf_end)) {
                *ret = -2;
                return NULL;
            }

            /* Check for CRLF or bare LF.
             * Safety: we check buf + 1 < buf_end before accessing buf[1]
             * to prevent the OOB read described in Bug #1. If \r is at
             * the very end of available data, we return -2 (incomplete). */
            if (likely(*buf == '\015')) {
                if (unlikely(buf + 1 >= buf_end)) {
                    *ret = -2; /* \r at end of buffer — need more data */
                    return NULL;
                }
                if (unlikely(*(buf + 1) != '\012')) {
                    *ret = -1; /* \r not followed by \n — invalid */
                    return NULL;
                }
                headers[*num_headers].value_len = buf - token_start;
                buf += 2;
            } else if (*buf == '\012') {
                headers[*num_headers].value_len = buf - token_start;
                ++buf;
            } else {
                /* DEL or other invalid control char in value */
                *ret = -1;
                return NULL;
            }

            headers[*num_headers].value = token_start;

            /* Remove trailing OWS from value */
            {
                const char *value_end = token_start + headers[*num_headers].value_len;
                for (; value_end != token_start; --value_end) {
                    const char c = *(value_end - 1);
                    if (!(c == ' ' || c == '\t'))
                        break;
                }
                headers[*num_headers].value_len = value_end - token_start;
            }
        }
    }
    return buf;
}

#endif /* __AVX2__ */

static const char *get_token_to_eol(const char *buf, const char *buf_end, const char **token, size_t *token_len, int *ret)
{
    const char *token_start = buf;

#ifdef __SSE4_2__
    static const char ALIGNED(16) ranges1[16] = "\0\010"    /* allow HT */
                                                "\012\037"  /* allow SP and up to but not including DEL */
                                                "\177\177"; /* allow chars w. MSB set */
    int found;
    buf = findchar_fast(buf, buf_end, ranges1, 6, &found);
    if (found)
        goto FOUND_CTL;
#else
    /* find non-printable char within the next 8 bytes, this is the hottest code; manually inlined */
    while (likely(buf_end - buf >= 8)) {
#define DOIT()                                                                                                                     \
    do {                                                                                                                           \
        if (unlikely(!IS_PRINTABLE_ASCII(*buf)))                                                                                   \
            goto NonPrintable;                                                                                                     \
        ++buf;                                                                                                                     \
    } while (0)
        DOIT();
        DOIT();
        DOIT();
        DOIT();
        DOIT();
        DOIT();
        DOIT();
        DOIT();
#undef DOIT
        continue;
    NonPrintable:
        if ((likely((unsigned char)*buf < '\040') && likely(*buf != '\011')) || unlikely(*buf == '\177')) {
            goto FOUND_CTL;
        }
        ++buf;
    }
#endif
    for (;; ++buf) {
        CHECK_EOF();
        if (unlikely(!IS_PRINTABLE_ASCII(*buf))) {
            if ((likely((unsigned char)*buf < '\040') && likely(*buf != '\011')) || unlikely(*buf == '\177')) {
                goto FOUND_CTL;
            }
        }
    }
FOUND_CTL:
    if (likely(*buf == '\015')) {
        ++buf;
        EXPECT_CHAR('\012');
        *token_len = buf - 2 - token_start;
    } else if (*buf == '\012') {
        *token_len = buf - token_start;
        ++buf;
    } else {
        *ret = -1;
        return NULL;
    }
    *token = token_start;

    return buf;
}

static const char *is_complete(const char *buf, const char *buf_end, size_t last_len, int *ret)
{
    int ret_cnt = 0;
    buf = last_len < 3 ? buf : buf + last_len - 3;

    while (1) {
        CHECK_EOF();
        if (*buf == '\015') {
            ++buf;
            CHECK_EOF();
            EXPECT_CHAR('\012');
            ++ret_cnt;
        } else if (*buf == '\012') {
            ++buf;
            ++ret_cnt;
        } else {
            ++buf;
            ret_cnt = 0;
        }
        if (ret_cnt == 2) {
            return buf;
        }
    }

    *ret = -2;
    return NULL;
}

#define PARSE_INT(valp_, mul_)                                                                                                     \
    if (*buf < '0' || '9' < *buf) {                                                                                                \
        buf++;                                                                                                                     \
        *ret = -1;                                                                                                                 \
        return NULL;                                                                                                               \
    }                                                                                                                              \
    *(valp_) = (mul_) * (*buf++ - '0');

#define PARSE_INT_3(valp_)                                                                                                         \
    do {                                                                                                                           \
        int res_ = 0;                                                                                                              \
        PARSE_INT(&res_, 100)                                                                                                      \
        *valp_ = res_;                                                                                                             \
        PARSE_INT(&res_, 10)                                                                                                       \
        *valp_ += res_;                                                                                                            \
        PARSE_INT(&res_, 1)                                                                                                        \
        *valp_ += res_;                                                                                                            \
    } while (0)

/* returned pointer is always within [buf, buf_end), or null */
static const char *parse_token(const char *buf, const char *buf_end, const char **token, size_t *token_len, char next_char,
                               int *ret)
{
    /* We use pcmpestri to detect non-token characters. This instruction can take no more than eight character ranges (8*2*8=128
     * bits that is the size of a SSE register). Due to this restriction, characters `|` and `~` are handled in the slow loop. */
    static const char ALIGNED(16) ranges[] = "\x00 "  /* control chars and up to SP */
                                             "\"\""   /* 0x22 */
                                             "()"     /* 0x28,0x29 */
                                             ",,"     /* 0x2c */
                                             "//"     /* 0x2f */
                                             ":@"     /* 0x3a-0x40 */
                                             "[]"     /* 0x5b-0x5d */
                                             "{\xff"; /* 0x7b-0xff */
    const char *buf_start = buf;
    int found;
    buf = findchar_fast(buf, buf_end, ranges, sizeof(ranges) - 1, &found);
    if (!found) {
        CHECK_EOF();
    }
    while (1) {
        if (*buf == next_char) {
            break;
        } else if (!token_char_map[(unsigned char)*buf]) {
            *ret = -1;
            return NULL;
        }
        ++buf;
        CHECK_EOF();
    }
    *token = buf_start;
    *token_len = buf - buf_start;
    return buf;
}

/* returned pointer is always within [buf, buf_end), or null */
static const char *parse_http_version(const char *buf, const char *buf_end, int *minor_version, int *ret)
{
    /* we want at least [HTTP/1.<two chars>] to try to parse */
    if (buf_end - buf < 9) {
        *ret = -2;
        return NULL;
    }
    EXPECT_CHAR_NO_CHECK('H');
    EXPECT_CHAR_NO_CHECK('T');
    EXPECT_CHAR_NO_CHECK('T');
    EXPECT_CHAR_NO_CHECK('P');
    EXPECT_CHAR_NO_CHECK('/');
    EXPECT_CHAR_NO_CHECK('1');
    EXPECT_CHAR_NO_CHECK('.');
    PARSE_INT(minor_version, 1);
    return buf;
}

static const char *parse_headers(const char *buf, const char *buf_end, struct phr_header *headers, size_t *num_headers,
                                 size_t max_headers, int *ret)
{
#ifdef __AVX2__
    return parse_headers_avx2(buf, buf_end, headers, num_headers, max_headers, ret);
#else
    for (;; ++*num_headers) {
        CHECK_EOF();
        if (*buf == '\015') {
            ++buf;
            EXPECT_CHAR('\012');
            break;
        } else if (*buf == '\012') {
            ++buf;
            break;
        }
        if (*num_headers == max_headers) {
            *ret = -1;
            return NULL;
        }
        if (!(*num_headers != 0 && (*buf == ' ' || *buf == '\t'))) {
            /* parsing name, but do not discard SP before colon, see
             * http://www.mozilla.org/security/announce/2006/mfsa2006-33.html */
            if ((buf = parse_token(buf, buf_end, &headers[*num_headers].name, &headers[*num_headers].name_len, ':', ret)) == NULL) {
                return NULL;
            }
            if (headers[*num_headers].name_len == 0) {
                *ret = -1;
                return NULL;
            }
            ++buf;
            for (;; ++buf) {
                CHECK_EOF();
                if (!(*buf == ' ' || *buf == '\t')) {
                    break;
                }
            }
        } else {
            headers[*num_headers].name = NULL;
            headers[*num_headers].name_len = 0;
        }
        const char *value;
        size_t value_len;
        if ((buf = get_token_to_eol(buf, buf_end, &value, &value_len, ret)) == NULL) {
            return NULL;
        }
        /* remove trailing SPs and HTABs */
        const char *value_end = value + value_len;
        for (; value_end != value; --value_end) {
            const char c = *(value_end - 1);
            if (!(c == ' ' || c == '\t')) {
                break;
            }
        }
        headers[*num_headers].value = value;
        headers[*num_headers].value_len = value_end - value;
    }
    return buf;
#endif
}

static const char *parse_request(const char *buf, const char *buf_end, const char **method, size_t *method_len, const char **path,
                                 size_t *path_len, int *minor_version, struct phr_header *headers, size_t *num_headers,
                                 size_t max_headers, int *ret)
{
    /* skip first empty line (some clients add CRLF after POST content) */
    CHECK_EOF();
    if (*buf == '\015') {
        ++buf;
        EXPECT_CHAR('\012');
    } else if (*buf == '\012') {
        ++buf;
    }

    /* parse request line */
    if ((buf = parse_token(buf, buf_end, method, method_len, ' ', ret)) == NULL) {
        return NULL;
    }
    do {
        ++buf;
        CHECK_EOF();
    } while (*buf == ' ');
    ADVANCE_TOKEN(*path, *path_len);
    do {
        ++buf;
        CHECK_EOF();
    } while (*buf == ' ');
    if (*method_len == 0 || *path_len == 0) {
        *ret = -1;
        return NULL;
    }
    if ((buf = parse_http_version(buf, buf_end, minor_version, ret)) == NULL) {
        return NULL;
    }
    if (*buf == '\015') {
        ++buf;
        EXPECT_CHAR('\012');
    } else if (*buf == '\012') {
        ++buf;
    } else {
        *ret = -1;
        return NULL;
    }

    return parse_headers(buf, buf_end, headers, num_headers, max_headers, ret);
}

int phr_parse_request(const char *buf_start, size_t len, const char **method, size_t *method_len, const char **path,
                      size_t *path_len, int *minor_version, struct phr_header *headers, size_t *num_headers, size_t last_len)
{
    const char *buf = buf_start, *buf_end = buf_start + len;
    size_t max_headers = *num_headers;
    int r;

    *method = NULL;
    *method_len = 0;
    *path = NULL;
    *path_len = 0;
    *minor_version = -1;
    *num_headers = 0;

    /* if last_len != 0, check if the request is complete (a fast countermeasure
       againt slowloris */
    if (last_len != 0 && is_complete(buf, buf_end, last_len, &r) == NULL) {
        return r;
    }

    if ((buf = parse_request(buf, buf_end, method, method_len, path, path_len, minor_version, headers, num_headers, max_headers,
                             &r)) == NULL) {
        return r;
    }

    return (int)(buf - buf_start);
}

static const char *parse_response(const char *buf, const char *buf_end, int *minor_version, int *status, const char **msg,
                                  size_t *msg_len, struct phr_header *headers, size_t *num_headers, size_t max_headers, int *ret)
{
    /* parse "HTTP/1.x" */
    if ((buf = parse_http_version(buf, buf_end, minor_version, ret)) == NULL) {
        return NULL;
    }
    /* skip space */
    if (*buf != ' ') {
        *ret = -1;
        return NULL;
    }
    do {
        ++buf;
        CHECK_EOF();
    } while (*buf == ' ');
    /* parse status code, we want at least [:digit:][:digit:][:digit:]<other char> to try to parse */
    if (buf_end - buf < 4) {
        *ret = -2;
        return NULL;
    }
    PARSE_INT_3(status);

    /* get message including preceding space */
    if ((buf = get_token_to_eol(buf, buf_end, msg, msg_len, ret)) == NULL) {
        return NULL;
    }
    if (*msg_len == 0) {
        /* ok */
    } else if (**msg == ' ') {
        /* Remove preceding space. Successful return from `get_token_to_eol` guarantees that we would hit something other than SP
         * before running past the end of the given buffer. */
        do {
            ++*msg;
            --*msg_len;
        } while (**msg == ' ');
    } else {
        /* garbage found after status code */
        *ret = -1;
        return NULL;
    }

    return parse_headers(buf, buf_end, headers, num_headers, max_headers, ret);
}

int phr_parse_response(const char *buf_start, size_t len, int *minor_version, int *status, const char **msg, size_t *msg_len,
                       struct phr_header *headers, size_t *num_headers, size_t last_len)
{
    const char *buf = buf_start, *buf_end = buf + len;
    size_t max_headers = *num_headers;
    int r;

    *minor_version = -1;
    *status = 0;
    *msg = NULL;
    *msg_len = 0;
    *num_headers = 0;

    /* if last_len != 0, check if the response is complete (a fast countermeasure
       against slowloris */
    if (last_len != 0 && is_complete(buf, buf_end, last_len, &r) == NULL) {
        return r;
    }

    if ((buf = parse_response(buf, buf_end, minor_version, status, msg, msg_len, headers, num_headers, max_headers, &r)) == NULL) {
        return r;
    }

    return (int)(buf - buf_start);
}

int phr_parse_headers(const char *buf_start, size_t len, struct phr_header *headers, size_t *num_headers, size_t last_len)
{
    const char *buf = buf_start, *buf_end = buf + len;
    size_t max_headers = *num_headers;
    int r;

    *num_headers = 0;

    /* if last_len != 0, check if the response is complete (a fast countermeasure
       against slowloris */
    if (last_len != 0 && is_complete(buf, buf_end, last_len, &r) == NULL) {
        return r;
    }

    if ((buf = parse_headers(buf, buf_end, headers, num_headers, max_headers, &r)) == NULL) {
        return r;
    }

    return (int)(buf - buf_start);
}

enum {
    CHUNKED_IN_CHUNK_SIZE,
    CHUNKED_IN_CHUNK_EXT,
    CHUNKED_IN_CHUNK_HEADER_EXPECT_LF,
    CHUNKED_IN_CHUNK_DATA,
    CHUNKED_IN_CHUNK_DATA_EXPECT_CR,
    CHUNKED_IN_CHUNK_DATA_EXPECT_LF,
    CHUNKED_IN_TRAILERS_LINE_HEAD,
    CHUNKED_IN_TRAILERS_LINE_MIDDLE
};

static int decode_hex(int ch)
{
    if ('0' <= ch && ch <= '9') {
        return ch - '0';
    } else if ('A' <= ch && ch <= 'F') {
        return ch - 'A' + 0xa;
    } else if ('a' <= ch && ch <= 'f') {
        return ch - 'a' + 0xa;
    } else {
        return -1;
    }
}

ssize_t phr_decode_chunked(struct phr_chunked_decoder *decoder, char *buf, size_t *_bufsz)
{
    size_t dst = 0, src = 0, bufsz = *_bufsz;
    ssize_t ret = -2; /* incomplete */

    decoder->_total_read += bufsz;

    while (1) {
        switch (decoder->_state) {
        case CHUNKED_IN_CHUNK_SIZE:
            for (;; ++src) {
                int v;
                if (src == bufsz)
                    goto Exit;
                if ((v = decode_hex(buf[src])) == -1) {
                    if (decoder->_hex_count == 0) {
                        ret = -1;
                        goto Exit;
                    }
                    /* the only characters that may appear after the chunk size are BWS, semicolon, or CRLF */
                    switch (buf[src]) {
                    case ' ':
                    case '\011':
                    case ';':
                    case '\012':
                    case '\015':
                        break;
                    default:
                        ret = -1;
                        goto Exit;
                    }
                    break;
                }
                if (decoder->_hex_count == sizeof(size_t) * 2) {
                    ret = -1;
                    goto Exit;
                }
                decoder->bytes_left_in_chunk = decoder->bytes_left_in_chunk * 16 + v;
                ++decoder->_hex_count;
            }
            decoder->_hex_count = 0;
            decoder->_state = CHUNKED_IN_CHUNK_EXT;
        /* fallthru */
        case CHUNKED_IN_CHUNK_EXT:
            /* RFC 7230 A.2 "Line folding in chunk extensions is disallowed" */
            for (;; ++src) {
                if (src == bufsz)
                    goto Exit;
                if (buf[src] == '\015') {
                    break;
                } else if (buf[src] == '\012') {
                    ret = -1;
                    goto Exit;
                }
            }
            ++src;
            decoder->_state = CHUNKED_IN_CHUNK_HEADER_EXPECT_LF;
        /* fallthru */
        case CHUNKED_IN_CHUNK_HEADER_EXPECT_LF:
            if (src == bufsz)
                goto Exit;
            if (buf[src] != '\012') {
                ret = -1;
                goto Exit;
            }
            ++src;
            if (decoder->bytes_left_in_chunk == 0) {
                if (decoder->consume_trailer) {
                    decoder->_state = CHUNKED_IN_TRAILERS_LINE_HEAD;
                    break;
                } else {
                    goto Complete;
                }
            }
            decoder->_state = CHUNKED_IN_CHUNK_DATA;
        /* fallthru */
        case CHUNKED_IN_CHUNK_DATA: {
            size_t avail = bufsz - src;
            if (avail < decoder->bytes_left_in_chunk) {
                if (dst != src)
                    memmove(buf + dst, buf + src, avail);
                src += avail;
                dst += avail;
                decoder->bytes_left_in_chunk -= avail;
                goto Exit;
            }
            if (dst != src)
                memmove(buf + dst, buf + src, decoder->bytes_left_in_chunk);
            src += decoder->bytes_left_in_chunk;
            dst += decoder->bytes_left_in_chunk;
            decoder->bytes_left_in_chunk = 0;
            decoder->_state = CHUNKED_IN_CHUNK_DATA_EXPECT_CR;
        }
        /* fallthru */
        case CHUNKED_IN_CHUNK_DATA_EXPECT_CR:
            if (src == bufsz)
                goto Exit;
            if (buf[src] != '\015') {
                ret = -1;
                goto Exit;
            }
            ++src;
            decoder->_state = CHUNKED_IN_CHUNK_DATA_EXPECT_LF;
        /* fallthru */
        case CHUNKED_IN_CHUNK_DATA_EXPECT_LF:
            if (src == bufsz)
                goto Exit;
            if (buf[src] != '\012') {
                ret = -1;
                goto Exit;
            }
            ++src;
            decoder->_state = CHUNKED_IN_CHUNK_SIZE;
            break;
        case CHUNKED_IN_TRAILERS_LINE_HEAD:
            for (;; ++src) {
                if (src == bufsz)
                    goto Exit;
                if (buf[src] != '\015')
                    break;
            }
            if (buf[src++] == '\012')
                goto Complete;
            decoder->_state = CHUNKED_IN_TRAILERS_LINE_MIDDLE;
        /* fallthru */
        case CHUNKED_IN_TRAILERS_LINE_MIDDLE:
            for (;; ++src) {
                if (src == bufsz)
                    goto Exit;
                if (buf[src] == '\012')
                    break;
            }
            ++src;
            decoder->_state = CHUNKED_IN_TRAILERS_LINE_HEAD;
            break;
        default:
            assert(!"decoder is corrupt");
        }
    }

Complete:
    ret = bufsz - src;
Exit:
    if (dst != src)
        memmove(buf + dst, buf + src, bufsz - src);
    *_bufsz = dst;
    /* if incomplete but the overhead of the chunked encoding is >=100KB and >80%, signal an error */
    if (ret == -2) {
        decoder->_total_overhead += bufsz - dst;
        if (decoder->_total_overhead >= 100 * 1024 && decoder->_total_read - decoder->_total_overhead < decoder->_total_read / 4)
            ret = -1;
    }
    return ret;
}

int phr_decode_chunked_is_in_data(struct phr_chunked_decoder *decoder)
{
    return decoder->_state == CHUNKED_IN_CHUNK_DATA;
}

#undef CHECK_EOF
#undef EXPECT_CHAR
#undef ADVANCE_TOKEN
