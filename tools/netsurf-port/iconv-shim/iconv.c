/* Minimal iconv(3) implementation for the InstantOS NetSurf port.
 *
 * See iconv.h for rationale. Supports conversions among:
 *   - UTF-8
 *   - ASCII (US-ASCII / ANSI_X3.4-1968)
 *   - ISO-8859-1 (Latin-1)
 *   - WINDOWS-1252 (CP1252)
 *
 * Semantics follow iconv(3): on success returns the number of irreversible
 * conversions (always 0 here); on failure returns (size_t)-1 with errno set to
 * EILSEQ (invalid input), EINVAL (incomplete multibyte at end of input), or
 * E2BIG (output buffer full). The inbuf/outbuf pointers and byte counts are
 * advanced as conversion proceeds.
 */
#include "iconv.h"
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>

enum enc {
    ENC_UTF8 = 0,
    ENC_ASCII,
    ENC_LATIN1,
    ENC_CP1252,
    ENC_UNKNOWN
};

/* CP1252 differs from Latin-1 only in the 0x80..0x9F range. This table maps
 * those bytes to their Unicode code points (0 means "undefined" in CP1252). */
static const uint16_t cp1252_high[32] = {
    0x20AC, 0x0000, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x0000, 0x017D, 0x0000,
    0x0000, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x0000, 0x017E, 0x0178
};

static int enc_eq(const char *a, const char *b)
{
    return strcasecmp(a, b) == 0;
}

static enum enc parse_enc(const char *name)
{
    /* Strip a trailing "//TRANSLIT" / "//IGNORE" suffix if present. */
    char buf[64];
    size_t i = 0;
    while (name[i] && name[i] != '/' && i < sizeof(buf) - 1) {
        buf[i] = name[i];
        i++;
    }
    buf[i] = '\0';

    if (enc_eq(buf, "UTF-8") || enc_eq(buf, "UTF8"))
        return ENC_UTF8;
    if (enc_eq(buf, "ASCII") || enc_eq(buf, "US-ASCII") ||
        enc_eq(buf, "ANSI_X3.4-1968") || enc_eq(buf, "ANSI_X3.4-1986"))
        return ENC_ASCII;
    if (enc_eq(buf, "ISO-8859-1") || enc_eq(buf, "ISO8859-1") ||
        enc_eq(buf, "LATIN1") || enc_eq(buf, "L1") || enc_eq(buf, "ISO_8859-1"))
        return ENC_LATIN1;
    if (enc_eq(buf, "WINDOWS-1252") || enc_eq(buf, "CP1252") ||
        enc_eq(buf, "MS-ANSI"))
        return ENC_CP1252;
    return ENC_UNKNOWN;
}

/* Conversion descriptor: low bit pair = from, next pair = to. */
struct shim_cd {
    enum enc from;
    enum enc to;
};

/* We avoid heap allocation: encode the (from,to) pair into the pointer value.
 * iconv_t is opaque; callers must only pass it back to iconv()/iconv_close(). */
#define CD_MAGIC      0x49435600u /* "ICV\0" */
#define CD_ENCODE(f, t) ((iconv_t)(uintptr_t)(CD_MAGIC | ((unsigned)(f) << 4) | (unsigned)(t)))
#define CD_VALID(p)   ((((uintptr_t)(p)) & 0xFFFFFF00u) == CD_MAGIC)
#define CD_FROM(p)    ((enum enc)((((uintptr_t)(p)) >> 4) & 0xF))
#define CD_TO(p)      ((enum enc)(((uintptr_t)(p)) & 0xF))

iconv_t iconv_open(const char *tocode, const char *fromcode)
{
    enum enc to = parse_enc(tocode);
    enum enc from = parse_enc(fromcode);
    if (to == ENC_UNKNOWN || from == ENC_UNKNOWN) {
        errno = EINVAL;
        return (iconv_t)-1;
    }
    return CD_ENCODE(from, to);
}

int iconv_close(iconv_t cd)
{
    if (!CD_VALID(cd) && cd != (iconv_t)-1) {
        errno = EBADF;
        return -1;
    }
    return 0;
}

/* Decode one code point from the source encoding. Returns the number of input
 * bytes consumed, or 0 on incomplete input (need more), or -1 on invalid. */
static int decode_one(enum enc enc, const unsigned char *in, size_t avail,
                      uint32_t *cp)
{
    if (avail == 0)
        return 0;

    switch (enc) {
    case ENC_ASCII:
        if (in[0] >= 0x80)
            return -1;
        *cp = in[0];
        return 1;
    case ENC_LATIN1:
        *cp = in[0];
        return 1;
    case ENC_CP1252:
        if (in[0] >= 0x80 && in[0] <= 0x9F) {
            uint16_t u = cp1252_high[in[0] - 0x80];
            if (u == 0)
                return -1; /* undefined */
            *cp = u;
        } else {
            *cp = in[0];
        }
        return 1;
    case ENC_UTF8: {
        unsigned char c = in[0];
        if (c < 0x80) { *cp = c; return 1; }
        int n;
        uint32_t v;
        if ((c & 0xE0) == 0xC0) { n = 2; v = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { n = 3; v = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { n = 4; v = c & 0x07; }
        else return -1;
        if (avail < (size_t)n)
            return 0; /* incomplete */
        for (int i = 1; i < n; i++) {
            if ((in[i] & 0xC0) != 0x80)
                return -1;
            v = (v << 6) | (in[i] & 0x3F);
        }
        *cp = v;
        return n;
    }
    default:
        return -1;
    }
}

/* Encode one code point into the destination encoding. Returns bytes written,
 * 0 if the output buffer is too small, or -1 if the code point is unmappable. */
static int encode_one(enum enc enc, uint32_t cp, unsigned char *out,
                      size_t room)
{
    switch (enc) {
    case ENC_ASCII:
        if (cp > 0x7F) return -1;
        if (room < 1) return 0;
        out[0] = (unsigned char)cp;
        return 1;
    case ENC_LATIN1:
        if (cp > 0xFF) return -1;
        if (room < 1) return 0;
        out[0] = (unsigned char)cp;
        return 1;
    case ENC_CP1252:
        if (cp <= 0x7F || (cp >= 0xA0 && cp <= 0xFF)) {
            if (room < 1) return 0;
            out[0] = (unsigned char)cp;
            return 1;
        }
        for (int i = 0; i < 32; i++) {
            if (cp1252_high[i] == cp) {
                if (room < 1) return 0;
                out[0] = (unsigned char)(0x80 + i);
                return 1;
            }
        }
        return -1;
    case ENC_UTF8:
        if (cp < 0x80) {
            if (room < 1) return 0;
            out[0] = (unsigned char)cp;
            return 1;
        } else if (cp < 0x800) {
            if (room < 2) return 0;
            out[0] = (unsigned char)(0xC0 | (cp >> 6));
            out[1] = (unsigned char)(0x80 | (cp & 0x3F));
            return 2;
        } else if (cp < 0x10000) {
            if (room < 3) return 0;
            out[0] = (unsigned char)(0xE0 | (cp >> 12));
            out[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
            out[2] = (unsigned char)(0x80 | (cp & 0x3F));
            return 3;
        } else {
            if (room < 4) return 0;
            out[0] = (unsigned char)(0xF0 | (cp >> 18));
            out[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
            out[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
            out[3] = (unsigned char)(0x80 | (cp & 0x3F));
            return 4;
        }
    default:
        return -1;
    }
}

size_t iconv(iconv_t cd, char **inbuf, size_t *inbytesleft,
             char **outbuf, size_t *outbytesleft)
{
    if (!CD_VALID(cd)) {
        errno = EBADF;
        return (size_t)-1;
    }

    /* A NULL inbuf (or NULL *inbuf) means "reset state". Stateless here. */
    if (inbuf == NULL || *inbuf == NULL)
        return 0;

    enum enc from = CD_FROM(cd);
    enum enc to = CD_TO(cd);

    const unsigned char *in = (const unsigned char *)*inbuf;
    unsigned char *out = (unsigned char *)*outbuf;
    size_t in_left = *inbytesleft;
    size_t out_left = *outbytesleft;

    while (in_left > 0) {
        uint32_t cp;
        int consumed = decode_one(from, in, in_left, &cp);
        if (consumed == 0) {
            errno = EINVAL; /* incomplete multibyte sequence */
            break;
        }
        if (consumed < 0) {
            errno = EILSEQ;
            break;
        }
        int produced = encode_one(to, cp, out, out_left);
        if (produced == 0) {
            errno = E2BIG;
            break;
        }
        if (produced < 0) {
            /* Unmappable character. iconv(3) treats this as EILSEQ. */
            errno = EILSEQ;
            break;
        }
        in += consumed;
        in_left -= consumed;
        out += produced;
        out_left -= produced;
    }

    *inbuf = (char *)in;
    *outbuf = (char *)out;
    *inbytesleft = in_left;
    *outbytesleft = out_left;

    if (in_left > 0)
        return (size_t)-1;
    return 0;
}
