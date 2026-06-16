/* Minimal iconv replacement for the InstantOS NetSurf port.
 *
 * mlibc does not provide iconv(3). NetSurf (via libparserutils, when built
 * WITHOUT_ICONV_FILTER it mostly avoids iconv, but a few code paths and the
 * core build still reference the iconv symbols) needs the three iconv entry
 * points to link. This shim implements just enough: conversions between
 * UTF-8 and the single-byte encodings the browser actually encounters
 * (ASCII, ISO-8859-1 / Latin-1, and Windows-1252), plus UTF-8 -> UTF-8
 * (validation/passthrough).
 *
 * It is intentionally small and allocation-free. Unknown conversions return
 * (iconv_t)-1 from iconv_open with errno = EINVAL.
 */
#ifndef INSTANTOS_ICONV_SHIM_H
#define INSTANTOS_ICONV_SHIM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *iconv_t;

iconv_t iconv_open(const char *tocode, const char *fromcode);
size_t  iconv(iconv_t cd, char **inbuf, size_t *inbytesleft,
              char **outbuf, size_t *outbytesleft);
int     iconv_close(iconv_t cd);

#ifdef __cplusplus
}
#endif

#endif /* INSTANTOS_ICONV_SHIM_H */
