/* json2toon - internal Unicode primitives shared by the converters. Not part
 * of the public API. Deliberately tiny (no normalization/case/grapheme tables):
 * the converters only need scalar<->UTF-8 and \uXXXX parsing. */
#ifndef JSON2TOON_UNICODE_H
#define JSON2TOON_UNICODE_H

#include <stddef.h>

/* Encode a Unicode scalar value to UTF-8. Returns the byte count, 1..4. */
int j2t_utf8_encode(unsigned cp, char out[4]);

/* Parse 4 hex digits at [s, s+4) (the caller guarantees 4 bytes are available).
 * Returns 0 and *out on success, -1 on a non-hex digit. */
int j2t_hex4(const char *s, unsigned *out);

#endif /* JSON2TOON_UNICODE_H */
