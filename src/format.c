/* json2toon - output buffering and scalar (string/number/key) formatting. */
#include "internal.h"

#include <string.h>

/* ------------------------------------------------------------------ output */

void j2t_out_init(j2t_out *o, json2toon_sink sink, void *ctx,
                  unsigned indent_width) {
  o->sink = sink;
  o->ctx = ctx;
  o->indent_width = indent_width;
  o->err = JSON2TOON_OK;
  o->len = 0;
}

int j2t_out_flush(j2t_out *o) {
  if (o->err != JSON2TOON_OK)
    return o->err;
  if (o->len) {
    if (o->sink(o->buf, o->len, o->ctx) != 0)
      o->err = JSON2TOON_ERR_IO;
    o->len = 0;
  }
  return o->err;
}

void j2t_write(j2t_out *o, const char *p, size_t n) {
  if (o->err != JSON2TOON_OK)
    return;
  /* Large writes bypass the buffer (after flushing) to avoid copying. */
  if (n >= J2T_OUT_BUFSZ) {
    if (j2t_out_flush(o) != JSON2TOON_OK)
      return;
    if (o->sink(p, n, o->ctx) != 0)
      o->err = JSON2TOON_ERR_IO;
    return;
  }
  if (o->len + n > J2T_OUT_BUFSZ) {
    if (j2t_out_flush(o) != JSON2TOON_OK)
      return;
  }
  memcpy(o->buf + o->len, p, n);
  o->len += n;
}

void j2t_putc(j2t_out *o, char c) {
  if (o->err != JSON2TOON_OK)
    return;
  if (o->len == J2T_OUT_BUFSZ && j2t_out_flush(o) != JSON2TOON_OK)
    return;
  o->buf[o->len++] = c;
}

void j2t_puts(j2t_out *o, const char *s) { j2t_write(o, s, strlen(s)); }

void j2t_indent(j2t_out *o, unsigned level) {
  unsigned n = level * o->indent_width;
  static const char spaces[16] =
      {' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' '};
  while (n >= sizeof spaces) {
    j2t_write(o, spaces, sizeof spaces);
    n -= sizeof spaces;
  }
  if (n)
    j2t_write(o, spaces, n);
}

/* ---------------------------------------------------------------- numbers */

/* Exact match of the JSON number grammar:
 *   -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?  */
int j2t_looks_like_number(const char *s, size_t n) {
  size_t i = 0;
  if (n == 0)
    return 0;
  if (s[i] == '-') {
    if (++i == n)
      return 0;
  }
  if (s[i] == '0') {
    i++;
  } else if (s[i] >= '1' && s[i] <= '9') {
    while (i < n && s[i] >= '0' && s[i] <= '9')
      i++;
  } else {
    return 0;
  }
  if (i < n && s[i] == '.') {
    i++;
    if (i == n || s[i] < '0' || s[i] > '9')
      return 0;
    while (i < n && s[i] >= '0' && s[i] <= '9')
      i++;
  }
  if (i < n && (s[i] == 'e' || s[i] == 'E')) {
    i++;
    if (i < n && (s[i] == '+' || s[i] == '-'))
      i++;
    if (i == n || s[i] < '0' || s[i] > '9')
      return 0;
    while (i < n && s[i] >= '0' && s[i] <= '9')
      i++;
  }
  return i == n;
}

/* Emit a non-negative count as decimal (the "N" in a TOON "[N]" header). */
void j2t_emit_count(j2t_out *o, size_t n) {
  char b[24];
  int i = (int)sizeof b;
  if (n == 0) {
    j2t_putc(o, '0');
    return;
  }
  while (n) {
    b[--i] = (char)('0' + (n % 10));
    n /= 10;
  }
  j2t_write(o, b + i, (size_t)((int)sizeof b - i));
}

/* ------------------------------------------------------------ quoting test */

static int eq(const char *s, size_t n, const char *lit) {
  return strlen(lit) == n && memcmp(s, lit, n) == 0;
}

int j2t_str_needs_quote(const char *s, size_t n, char delim) {
  size_t i;
  if (n == 0)
    return 1;                                  /* empty string */
  if (eq(s, n, "true") || eq(s, n, "false") || eq(s, n, "null"))
    return 1;                                  /* literal collision */
  if (j2t_looks_like_number(s, n))
    return 1;                                  /* numeric collision */
  if (s[0] == ' ' || s[0] == '\t' || s[n - 1] == ' ' || s[n - 1] == '\t')
    return 1;                                  /* leading/trailing whitespace */
  if (s[0] == '-')
    return 1;                                  /* hyphen prefix (list marker) */
  for (i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x20)
      return 1;                                /* control char */
    if (c == ':' || c == '"' || c == '\\' || c == '[' || c == ']' ||
        c == '{' || c == '}' || (char)c == delim)
      return 1;                                /* structural / delimiter */
  }
  return 0;
}

int j2t_key_is_bare(const char *s, size_t n) {
  size_t i;
  if (n == 0)
    return 0;
  if (!((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z') ||
        s[0] == '_'))
    return 0;
  for (i = 1; i < n; i++) {
    char c = s[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '_'))
      return 0;
  }
  return 1;
}

/* ------------------------------------------------------------- escaping */

static void emit_hex4(j2t_out *o, unsigned v) {
  static const char hex[] = "0123456789abcdef";
  char b[6];
  b[0] = '\\';
  b[1] = 'u';
  b[2] = hex[(v >> 12) & 0xf];
  b[3] = hex[(v >> 8) & 0xf];
  b[4] = hex[(v >> 4) & 0xf];
  b[5] = hex[v & 0xf];
  j2t_write(o, b, 6);
}

void j2t_emit_quoted(j2t_out *o, const char *s, size_t n) {
  size_t i, run = 0;
  j2t_putc(o, '"');
  for (i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    const char *esc = NULL;
    char two[2];
    if (c == '"') { two[0] = '\\'; two[1] = '"'; esc = two; }
    else if (c == '\\') { two[0] = '\\'; two[1] = '\\'; esc = two; }
    else if (c == '\n') { two[0] = '\\'; two[1] = 'n'; esc = two; }
    else if (c == '\r') { two[0] = '\\'; two[1] = 'r'; esc = two; }
    else if (c == '\t') { two[0] = '\\'; two[1] = 't'; esc = two; }
    else if (c < 0x20) {
      if (run) { j2t_write(o, s + i - run, run); run = 0; }
      emit_hex4(o, c);
      continue;
    } else {
      run++;
      continue;
    }
    if (run) { j2t_write(o, s + i - run, run); run = 0; }
    j2t_write(o, esc, 2);
  }
  if (run)
    j2t_write(o, s + n - run, run);
  j2t_putc(o, '"');
}

void j2t_emit_string(j2t_out *o, const char *s, size_t n, char delim) {
  if (j2t_str_needs_quote(s, n, delim))
    j2t_emit_quoted(o, s, n);
  else
    j2t_write(o, s, n);
}

void j2t_emit_key(j2t_out *o, const char *s, size_t n) {
  if (j2t_key_is_bare(s, n))
    j2t_write(o, s, n);
  else
    j2t_emit_quoted(o, s, n);
}
