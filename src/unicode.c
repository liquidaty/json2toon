/* json2toon - internal Unicode primitives (see unicode.h). */
#include "unicode.h"

int j2t_utf8_encode(unsigned cp, char out[4]) {
  if (cp <= 0x7f) {
    out[0] = (char)cp;
    return 1;
  } else if (cp <= 0x7ff) {
    out[0] = (char)(0xc0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3f));
    return 2;
  } else if (cp <= 0xffff) {
    out[0] = (char)(0xe0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[2] = (char)(0x80 | (cp & 0x3f));
    return 3;
  } else {
    out[0] = (char)(0xf0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
  }
}

int j2t_hex4(const char *s, unsigned *out) {
  unsigned v = 0;
  int i;
  for (i = 0; i < 4; i++) {
    char c = s[i];
    v <<= 4;
    if (c >= '0' && c <= '9')
      v |= (unsigned)(c - '0');
    else if (c >= 'a' && c <= 'f')
      v |= (unsigned)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')
      v |= (unsigned)(c - 'A' + 10);
    else
      return -1;
  }
  *out = v;
  return 0;
}
