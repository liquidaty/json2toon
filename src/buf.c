/* json2toon - shared growable byte buffer (see internal.h). */
#include "internal.h"

#include <stdlib.h>
#include <string.h>

int j2t_buf_reserve(j2t_buf *b, size_t need) {
  if (need <= b->cap)
    return 0;
  {
    size_t nc = b->cap ? b->cap : 64;
    char *nb;
    while (nc < need)
      nc *= 2;
    nb = (char *)realloc(b->p, nc);
    if (!nb)
      return -1;
    b->p = nb;
    b->cap = nc;
  }
  return 0;
}

int j2t_buf_append(j2t_buf *b, const char *s, size_t n) {
  if (j2t_buf_reserve(b, b->len + n) != 0)
    return -1;
  memcpy(b->p + b->len, s, n);
  b->len += n;
  return 0;
}

int j2t_buf_putc(j2t_buf *b, char c) {
  if (j2t_buf_reserve(b, b->len + 1) != 0)
    return -1;
  b->p[b->len++] = c;
  return 0;
}

void j2t_buf_free(j2t_buf *b) {
  free(b->p);
  b->p = NULL;
  b->len = 0;
  b->cap = 0;
}
