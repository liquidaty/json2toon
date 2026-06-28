/* json2toon - streaming TOON -> JSON converter (the reverse direction).
 *
 * TOON encodes structure with indentation rather than brackets, so the reverse
 * converter is a line-oriented pushdown machine: input is buffered only up to
 * the current newline, each completed line is reconciled against a stack of
 * open JSON containers (open/close braces are driven by indentation changes),
 * and compact JSON is emitted as soon as each line is understood. Peak memory
 * is a function of nesting depth and the widest single line (e.g. one tabular
 * row), never of total input or output length.
 *
 * The output buffering (j2t_out), JSON-compatible string escaping
 * (j2t_emit_quoted) and the number-grammar test (j2t_looks_like_number) are
 * shared with the forward path; everything specific to parsing TOON lives here.
 */
#include "internal.h"

#include <stdlib.h>
#include <string.h>

#define T2J_DEFAULT_DEPTH 256
#define T2J_DEFAULT_LINE_BYTES (64u * 1024u * 1024u)
#define U_UNSET ((unsigned)-1)

/* Container kinds on the parse stack. */
enum { FR_OBJ, FR_ARR_LIST, FR_ARR_TAB };

/* Document-level state. */
enum { ST_START, ST_BODY, ST_DOC_DONE, ST_FAIL };

typedef struct {
  int kind;
  int opened;              /* whether the '{' / '[' has been emitted */
  unsigned header_indent;  /* indent column of the line that opened this frame */
  unsigned child_indent;   /* indent column of children; U_UNSET until seen */
  size_t emitted;          /* number of children emitted so far */
  size_t expected;         /* declared element count (arrays); 0 if unknown */
  int has_expected;        /* whether `expected` was given */
  /* tabular only: column names (decoded) */
  char **cols;
  size_t *collen;
  size_t ncols;
} frame;

struct toon2json {
  j2t_out out;
  toon2json_options opt;

  int state;
  int err;                 /* sticky JSON2TOON_* */
  size_t stream_pos;       /* total input bytes consumed */
  size_t err_offset;

  /* current line accumulator (without its terminating newline) */
  char *line;
  size_t linelen, linecap;
  size_t line_off;         /* stream offset of the first byte of `line` */

  /* scratch buffer for decoded string/key bytes */
  char *scratch;
  size_t scratchlen, scratchcap;

  frame *frames;
  size_t nframes, framecap;
};

typedef struct toon2json toon2json;

/* --------------------------------------------------------------- utilities */

static int buf_reserve(char **buf, size_t *cap, size_t need) {
  if (need <= *cap)
    return 0;
  {
    size_t nc = *cap ? *cap : 64;
    char *nb;
    while (nc < need)
      nc *= 2;
    nb = (char *)realloc(*buf, nc);
    if (!nb)
      return -1;
    *buf = nb;
    *cap = nc;
  }
  return 0;
}

static void set_err(toon2json *t, int code, size_t off) {
  if (t->err == JSON2TOON_OK) {
    t->err = code;
    t->err_offset = off;
  }
  t->state = ST_FAIL;
}

static int is_key_char(unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '_';
}

/* Skip a TOON-quoted string token. `p` must point at the opening '"'. Returns
 * the byte just past the closing '"', or NULL if unterminated. */
static const char *skip_quoted(const char *p, const char *end) {
  p++;                                   /* opening quote */
  while (p < end) {
    char c = *p;
    if (c == '\\') {
      p++;
      if (p < end)
        p++;
      continue;
    }
    if (c == '"')
      return p + 1;
    p++;
  }
  return NULL;                           /* unterminated */
}

/* ------------------------------------------------------------- decode quoted */

static int utf8_encode(unsigned cp, char out[4]) {
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

static int hex4(const char *s, const char *end, unsigned *out) {
  unsigned v = 0;
  int i;
  if (s + 4 > end)
    return -1;
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

static int scratch_putc(toon2json *t, char c) {
  if (buf_reserve(&t->scratch, &t->scratchcap, t->scratchlen + 1) != 0)
    return -1;
  t->scratch[t->scratchlen++] = c;
  return 0;
}

static int scratch_put(toon2json *t, const char *p, size_t n) {
  if (buf_reserve(&t->scratch, &t->scratchcap, t->scratchlen + n) != 0)
    return -1;
  memcpy(t->scratch + t->scratchlen, p, n);
  t->scratchlen += n;
  return 0;
}

/* Decode a TOON-quoted token [s,e) (including surrounding quotes) into
 * t->scratch. Returns 0 on success, -1 on malformed escape, -2 on OOM. */
static int decode_quoted(toon2json *t, const char *s, const char *e) {
  const char *p = s + 1;                 /* after opening quote */
  const char *q = e - 1;                 /* the closing quote */
  t->scratchlen = 0;
  while (p < q) {
    unsigned char c = (unsigned char)*p;
    if (c == '\\') {
      char out = 0;
      p++;
      if (p >= q)
        return -1;
      switch (*p) {
        case '"': out = '"'; break;
        case '\\': out = '\\'; break;
        case '/': out = '/'; break;
        case 'b': out = '\b'; break;
        case 'f': out = '\f'; break;
        case 'n': out = '\n'; break;
        case 'r': out = '\r'; break;
        case 't': out = '\t'; break;
        case 'u': {
          unsigned cp;
          char enc[4];
          int el;
          if (hex4(p + 1, q, &cp) != 0)
            return -1;
          p += 5;
          if (cp >= 0xd800 && cp <= 0xdbff) {
            unsigned lo;
            if (p + 6 > q || p[0] != '\\' || p[1] != 'u' ||
                hex4(p + 2, q, &lo) != 0 || lo < 0xdc00 || lo > 0xdfff)
              return -1;
            cp = 0x10000u + (((cp - 0xd800u) << 10) | (lo - 0xdc00u));
            p += 6;
          } else if (cp >= 0xdc00 && cp <= 0xdfff) {
            return -1;                   /* lone low surrogate */
          }
          el = utf8_encode(cp, enc);
          if (scratch_put(t, enc, (size_t)el) != 0)
            return -2;
          continue;
        }
        default:
          return -1;
      }
      if (scratch_putc(t, out) != 0)
        return -2;
      p++;
    } else if (c < 0x20) {
      return -1;                         /* unescaped control byte */
    } else {
      if (scratch_putc(t, (char)c) != 0)
        return -2;
      p++;
    }
  }
  return 0;
}

/* ------------------------------------------------------------- JSON emitters */

/* Emit a JSON string from already-decoded raw bytes. */
static void emit_json_string(toon2json *t, const char *s, size_t n) {
  j2t_emit_quoted(&t->out, s, n);
}

/* Emit a JSON object key from a key token [s,e) (quoted or bare in TOON). */
static int emit_key(toon2json *t, const char *s, const char *e) {
  if (s < e && *s == '"') {
    int rc = decode_quoted(t, s, e);
    if (rc == -2) { set_err(t, JSON2TOON_ERR_MEMORY, t->line_off); return -1; }
    if (rc != 0) { set_err(t, JSON2TOON_ERR_PARSE, t->line_off); return -1; }
    emit_json_string(t, t->scratch, t->scratchlen);
  } else {
    emit_json_string(t, s, (size_t)(e - s));
  }
  return 0;
}

static int lit_eq(const char *s, size_t n, const char *lit) {
  return strlen(lit) == n && memcmp(s, lit, n) == 0;
}

/* Emit a JSON scalar from a TOON scalar token [s,s+n). The token is either a
 * quoted string, a bare literal (true/false/null), a bare number, or a bare
 * (unquoted) string. */
static int emit_scalar(toon2json *t, const char *s, size_t n) {
  if (n >= 1 && s[0] == '"') {
    /* A bare TOON scalar never contains '"', so a leading quote must be a
     * properly terminated quoted token. */
    int rc;
    if (n < 2 || s[n - 1] != '"' || skip_quoted(s, s + n) != s + n) {
      set_err(t, JSON2TOON_ERR_PARSE, t->line_off);
      return -1;
    }
    rc = decode_quoted(t, s, s + n);
    if (rc == -2) { set_err(t, JSON2TOON_ERR_MEMORY, t->line_off); return -1; }
    if (rc != 0) { set_err(t, JSON2TOON_ERR_PARSE, t->line_off); return -1; }
    emit_json_string(t, t->scratch, t->scratchlen);
    return 0;
  }
  if (n == 0) {                          /* empty bare token -> empty string */
    j2t_puts(&t->out, "\"\"");
    return 0;
  }
  if (lit_eq(s, n, "true") || lit_eq(s, n, "false") || lit_eq(s, n, "null")) {
    j2t_write(&t->out, s, n);
    return 0;
  }
  if (j2t_looks_like_number(s, n)) {
    j2t_write(&t->out, s, n);
    return 0;
  }
  emit_json_string(t, s, n);             /* bare unquoted string */
  return 0;
}

/* ----------------------------------------------------------------- frames */

static frame *push_frame(toon2json *t, int kind, unsigned header_indent) {
  frame *f;
  if (t->nframes >= t->opt.max_depth) {
    set_err(t, JSON2TOON_ERR_DEPTH, t->line_off);
    return NULL;
  }
  if (t->nframes == t->framecap) {
    size_t nc = t->framecap ? t->framecap * 2 : 16;
    frame *nf = (frame *)realloc(t->frames, nc * sizeof *nf);
    if (!nf) {
      set_err(t, JSON2TOON_ERR_MEMORY, t->line_off);
      return NULL;
    }
    t->frames = nf;
    t->framecap = nc;
  }
  f = &t->frames[t->nframes++];
  memset(f, 0, sizeof *f);
  f->kind = kind;
  f->header_indent = header_indent;
  f->child_indent = U_UNSET;
  return f;
}

static void frame_free_cols(frame *f) {
  size_t i;
  if (f->cols) {
    for (i = 0; i < f->ncols; i++)
      free(f->cols[i]);
    free(f->cols);
    free(f->collen);
  }
  f->cols = NULL;
  f->collen = NULL;
  f->ncols = 0;
}

/* Emit the closer for a frame being popped, validating declared counts. */
static void close_frame(toon2json *t, frame *f) {
  if (f->kind == FR_OBJ) {
    if (f->opened)
      j2t_putc(&t->out, '}');
    else
      j2t_puts(&t->out, "{}");
  } else {
    if (f->has_expected && f->emitted != f->expected)
      set_err(t, JSON2TOON_ERR_PARSE, t->line_off);
    j2t_putc(&t->out, ']');             /* '[' was already emitted */
    frame_free_cols(f);
  }
}

/* ---------------------------------------------------------- array headers */

/* Split TOON comma-separated scalar elements in [p,end) and emit them as the
 * body of a JSON array (without the surrounding brackets). Returns element
 * count, or (size_t)-1 on error. */
static size_t emit_inline_elements(toon2json *t, const char *p,
                                   const char *end) {
  size_t count = 0;
  while (p < end) {
    const char *elem = p;
    const char *q;
    if (count)
      j2t_putc(&t->out, ',');
    if (*p == '"') {
      q = skip_quoted(p, end);
      if (!q) { set_err(t, JSON2TOON_ERR_PARSE, t->line_off); return (size_t)-1; }
      if (emit_scalar(t, elem, (size_t)(q - elem)) != 0)
        return (size_t)-1;
      p = q;
      if (p < end) {
        if (*p != ',') { set_err(t, JSON2TOON_ERR_PARSE, t->line_off); return (size_t)-1; }
        p++;
      }
    } else {
      q = p;
      while (q < end && *q != ',')
        q++;
      if (emit_scalar(t, elem, (size_t)(q - elem)) != 0)
        return (size_t)-1;
      p = q;
      if (p < end)
        p++;                             /* skip comma */
    }
    count++;
  }
  return count;
}

/* Duplicate a key token [ks,ke) (quoted or bare) into a freshly malloc'd
 * buffer, decoding TOON escapes for quoted tokens. Returns the buffer and sets
 * *outlen. On failure returns NULL and sets *bad to 1 for a malformed token or
 * 0 for an allocation failure. */
static char *dup_token(toon2json *t, const char *ks, const char *ke,
                       size_t *outlen, int *bad) {
  char *r;
  *bad = 0;
  if (ks < ke && *ks == '"') {
    int rc = decode_quoted(t, ks, ke);
    if (rc == -2)
      return NULL;                       /* OOM */
    if (rc != 0) { *bad = 1; return NULL; }
    r = (char *)malloc(t->scratchlen ? t->scratchlen : 1);
    if (!r)
      return NULL;
    memcpy(r, t->scratch, t->scratchlen);
    *outlen = t->scratchlen;
  } else {
    size_t n = (size_t)(ke - ks);
    r = (char *)malloc(n ? n : 1);
    if (!r)
      return NULL;
    memcpy(r, ks, n);
    *outlen = n;
  }
  return r;
}

/* Parse a count after '['. *pp points just past '['. On success advances *pp
 * past the digits and stores the value; missing digits is allowed (returns
 * has=0). Returns 0 on success. */
static void parse_count(const char **pp, const char *end, size_t *val,
                        int *has) {
  const char *p = *pp;
  size_t v = 0;
  int any = 0;
  while (p < end && *p >= '0' && *p <= '9') {
    v = v * 10 + (size_t)(*p - '0');
    p++;
    any = 1;
  }
  *val = v;
  *has = any;
  *pp = p;
}

/* Parse and emit a TOON array beginning at [p,end) where *p == '['. The header
 * may be empty ("[]"), inline ("[N]: a,b"), tabular ("[N]{cols}:") or a list
 * body ("[N]:"); tabular/list push a frame opened at `header_indent`. Returns 0
 * on success (errors set sticky state). */
static int emit_array_header(toon2json *t, const char *p, const char *end,
                             unsigned header_indent) {
  size_t expected;
  int has_count;
  p++;                                   /* '[' */
  if (p < end && *p == ']') {            /* empty array */
    p++;
    if (p != end) { set_err(t, JSON2TOON_ERR_PARSE, t->line_off); return -1; }
    j2t_puts(&t->out, "[]");
    return 0;
  }
  parse_count(&p, end, &expected, &has_count);
  if (p >= end || *p != ']') { set_err(t, JSON2TOON_ERR_PARSE, t->line_off); return -1; }
  p++;                                   /* ']' */

  if (p < end && *p == '{') {            /* tabular header */
    frame *f;
    char **cols;
    size_t *collen;
    size_t ncols = 0, idx;
    const char *scan;
    p++;                                 /* '{' */

    /* pass 1: count columns and validate the header terminates with ":" EOL */
    scan = p;
    for (;;) {
      const char *ks = scan, *ke;
      if (scan < end && *scan == '"') {
        ke = skip_quoted(scan, end);
        if (!ke) { set_err(t, JSON2TOON_ERR_PARSE, t->line_off); return -1; }
      } else {
        while (scan < end && is_key_char((unsigned char)*scan))
          scan++;
        ke = scan;
        if (ke == ks) { set_err(t, JSON2TOON_ERR_PARSE, t->line_off); return -1; }
      }
      ncols++;
      scan = ke;
      if (scan < end && *scan == ',') { scan++; continue; }
      if (scan < end && *scan == '}') { scan++; break; }
      set_err(t, JSON2TOON_ERR_PARSE, t->line_off);
      return -1;
    }
    if (scan >= end || *scan != ':' || scan + 1 != end) {
      set_err(t, JSON2TOON_ERR_PARSE, t->line_off);
      return -1;
    }

    /* pass 2: decode each column name into owned storage */
    cols = (char **)malloc(ncols * sizeof *cols);
    collen = (size_t *)malloc(ncols * sizeof *collen);
    if (!cols || !collen) {
      free(cols);
      free(collen);
      set_err(t, JSON2TOON_ERR_MEMORY, t->line_off);
      return -1;
    }
    scan = p;
    for (idx = 0; idx < ncols; idx++) {
      const char *ks = scan, *ke;
      int bad;
      char *col;
      if (*scan == '"')
        ke = skip_quoted(scan, end);     /* validated in pass 1 */
      else {
        while (scan < end && is_key_char((unsigned char)*scan))
          scan++;
        ke = scan;
      }
      col = dup_token(t, ks, ke, &collen[idx], &bad);
      if (!col) {
        size_t k;
        for (k = 0; k < idx; k++)
          free(cols[k]);
        free(cols);
        free(collen);
        set_err(t, bad ? JSON2TOON_ERR_PARSE : JSON2TOON_ERR_MEMORY,
                t->line_off);
        return -1;
      }
      cols[idx] = col;
      scan = ke;
      if (scan < end && *scan == ',')
        scan++;
    }

    f = push_frame(t, FR_ARR_TAB, header_indent);
    if (!f) {                            /* depth/OOM already recorded */
      for (idx = 0; idx < ncols; idx++)
        free(cols[idx]);
      free(cols);
      free(collen);
      return -1;
    }
    f->opened = 1;
    f->expected = expected;
    f->has_expected = has_count;
    f->cols = cols;
    f->collen = collen;
    f->ncols = ncols;
    j2t_putc(&t->out, '[');
    return 0;
  }

  if (p < end && *p == ':') {            /* list body or inline */
    p++;
    if (p == end) {                      /* list array body */
      frame *f = push_frame(t, FR_ARR_LIST, header_indent);
      if (!f)
        return -1;
      f->opened = 1;
      f->expected = expected;
      f->has_expected = has_count;
      j2t_putc(&t->out, '[');
      return 0;
    }
    if (*p == ' ')
      p++;
    /* inline scalar array */
    j2t_putc(&t->out, '[');
    {
      size_t n = emit_inline_elements(t, p, end);
      if (n == (size_t)-1)
        return -1;
      j2t_putc(&t->out, ']');
      if (has_count && n != expected) {
        set_err(t, JSON2TOON_ERR_PARSE, t->line_off);
        return -1;
      }
    }
    return 0;
  }

  set_err(t, JSON2TOON_ERR_PARSE, t->line_off);
  return -1;
}

/* ----------------------------------------------------- members / items / rows */

/* Handle an object member. The frame at index `fi` is an FR_OBJ. The line
 * content is [p,end) beginning at the key; `member_indent` is the column where
 * the key sits (recorded as the object's child indent). */
static int handle_member(toon2json *t, size_t fi, const char *p,
                         const char *end, unsigned member_indent) {
  frame *f = &t->frames[fi];
  const char *ks = p, *ke;
  char c;

  if (f->kind != FR_OBJ) { set_err(t, JSON2TOON_ERR_PARSE, t->line_off); return -1; }

  if (!f->opened) {
    j2t_putc(&t->out, '{');
    f->opened = 1;
  } else {
    j2t_putc(&t->out, ',');
  }
  f->emitted++;
  f->child_indent = member_indent;

  /* parse key */
  if (p < end && *p == '"') {
    ke = skip_quoted(p, end);
    if (!ke) { set_err(t, JSON2TOON_ERR_PARSE, t->line_off); return -1; }
  } else {
    while (p < end && is_key_char((unsigned char)*p))
      p++;
    ke = p;
    if (ke == ks) { set_err(t, JSON2TOON_ERR_PARSE, t->line_off); return -1; }
  }
  if (emit_key(t, ks, ke) != 0)
    return -1;
  j2t_putc(&t->out, ':');
  p = ke;

  if (p >= end) { set_err(t, JSON2TOON_ERR_PARSE, t->line_off); return -1; }
  c = *p;
  if (c == '[')
    return emit_array_header(t, p, end, member_indent);

  if (c == ':') {
    p++;
    if (p == end) {                      /* nested object */
      if (!push_frame(t, FR_OBJ, member_indent))
        return -1;
      return 0;
    }
    if (*p == ' ')
      p++;
    if (p == end) { set_err(t, JSON2TOON_ERR_PARSE, t->line_off); return -1; }
    if ((size_t)(end - p) == 2 && p[0] == '[' && p[1] == ']') {
      j2t_puts(&t->out, "[]");           /* empty array value */
      return 0;
    }
    return emit_scalar(t, p, (size_t)(end - p));
  }

  set_err(t, JSON2TOON_ERR_PARSE, t->line_off);
  return -1;
}

/* Handle a list-array element. Frame `fi` is an FR_ARR_LIST. The value content
 * is [p,end) (the text after "- "); `dash_indent` is the indent of the '-'. */
static int handle_item(toon2json *t, size_t fi, const char *p, const char *end,
                       unsigned dash_indent) {
  frame *f = &t->frames[fi];
  const char *ks, *ke;

  if (f->emitted)
    j2t_putc(&t->out, ',');
  f->emitted++;
  f->child_indent = dash_indent;

  if (p == end) {                        /* "-" alone -> empty object */
    j2t_puts(&t->out, "{}");
    return 0;
  }
  if (*p == '[')
    return emit_array_header(t, p, end, dash_indent);

  /* object item vs scalar item: an item is an object iff its first token is a
   * key (immediately followed by ':' or '['). */
  ks = p;
  if (*p == '"') {
    ke = skip_quoted(p, end);
    if (!ke) { set_err(t, JSON2TOON_ERR_PARSE, t->line_off); return -1; }
  } else {
    const char *q = p;
    while (q < end && is_key_char((unsigned char)*q))
      q++;
    ke = q;
  }
  if (ke > ks && ke < end && (*ke == ':' || *ke == '[')) {
    /* object item: push a frame and emit its first member inline */
    size_t oi;
    if (!push_frame(t, FR_OBJ, dash_indent))
      return -1;
    oi = t->nframes - 1;
    if (handle_member(t, oi, p, end, dash_indent + 2) != 0)
      return -1;
    t->frames[oi].child_indent = dash_indent + 2;
    return 0;
  }
  return emit_scalar(t, p, (size_t)(end - p));   /* scalar item */
}

/* Handle a tabular row. Frame `fi` is an FR_ARR_TAB. [p,end) is the full row;
 * `row_indent` is its indent column. */
static int handle_row(toon2json *t, size_t fi, const char *p, const char *end,
                      unsigned row_indent) {
  frame *f = &t->frames[fi];
  size_t col = 0;

  if (f->emitted)
    j2t_putc(&t->out, ',');
  f->emitted++;
  f->child_indent = row_indent;

  j2t_putc(&t->out, '{');
  while (col < f->ncols) {
    const char *cs = p;
    const char *q;
    j2t_emit_quoted(&t->out, f->cols[col], f->collen[col]);
    j2t_putc(&t->out, ':');
    if (p < end && *p == '"') {
      q = skip_quoted(p, end);
      if (!q) { set_err(t, JSON2TOON_ERR_PARSE, t->line_off); return -1; }
      if (emit_scalar(t, cs, (size_t)(q - cs)) != 0)
        return -1;
      p = q;
    } else {
      q = p;
      while (q < end && *q != ',')
        q++;
      if (emit_scalar(t, cs, (size_t)(q - cs)) != 0)
        return -1;
      p = q;
    }
    col++;
    if (col < f->ncols) {
      if (p >= end || *p != ',') { set_err(t, JSON2TOON_ERR_PARSE, t->line_off); return -1; }
      p++;
      j2t_putc(&t->out, ',');
    }
  }
  if (p != end) { set_err(t, JSON2TOON_ERR_PARSE, t->line_off); return -1; }
  j2t_putc(&t->out, '}');
  return 0;
}

/* ----------------------------------------------------------- line processing */

/* Pop and close frames that the current line (at column `indent`) has dedented
 * out of. */
static void reconcile(toon2json *t, unsigned indent) {
  while (t->nframes > 0) {
    frame *f = &t->frames[t->nframes - 1];
    if (f->child_indent == U_UNSET) {
      if (indent <= f->header_indent) {
        close_frame(t, f);
        t->nframes--;
        if (t->err != JSON2TOON_OK)
          return;
        continue;
      }
      break;                             /* this line is the first child */
    } else {
      if (indent < f->child_indent) {
        close_frame(t, f);
        t->nframes--;
        if (t->err != JSON2TOON_OK)
          return;
        continue;
      }
      break;
    }
  }
}

static void process_line(toon2json *t) {
  const char *content = t->line;
  size_t clen = t->linelen;
  unsigned indent = 0;
  int is_item = 0;
  const char *val;
  size_t vlen;

  /* strip a trailing CR (CRLF inputs) */
  if (clen > 0 && content[clen - 1] == '\r')
    clen--;

  /* count leading spaces */
  while (indent < clen && content[indent] == ' ')
    indent++;
  content += indent;
  clen -= indent;

  if (clen == 0)
    return;                              /* blank line: ignore */

  /* detect a list-item marker: '-' followed by space or end of line */
  if (content[0] == '-' && (clen == 1 || content[1] == ' ')) {
    is_item = 1;
    if (clen == 1) {
      val = content + 1;
      vlen = 0;
    } else {
      val = content + 2;
      vlen = clen - 2;
    }
  } else {
    val = content;
    vlen = clen;
  }

  if (t->state == ST_DOC_DONE) {
    set_err(t, JSON2TOON_ERR_PARSE, t->line_off);
    return;
  }

  if (t->state == ST_START) {
    /* The first content line determines the document's root shape. */
    if (is_item) {                       /* a bare list item with no header */
      set_err(t, JSON2TOON_ERR_PARSE, t->line_off);
      return;
    }
    if (content[0] == '[') {             /* root array */
      if (emit_array_header(t, content, content + clen, indent) != 0)
        return;
      /* inline / empty arrays leave no frame -> document complete */
      if (t->nframes == 0)
        t->state = ST_DOC_DONE;
      else
        t->state = ST_BODY;
      return;
    }
    /* classify: object (key followed by ':' or '[') vs root scalar */
    {
      const char *ks = content, *ke;
      if (content[0] == '"') {
        ke = skip_quoted(content, content + clen);
        if (!ke) { set_err(t, JSON2TOON_ERR_PARSE, t->line_off); return; }
      } else {
        const char *q = content;
        while (q < content + clen && is_key_char((unsigned char)*q))
          q++;
        ke = q;
      }
      if (ke > ks && ke < content + clen && (*ke == ':' || *ke == '[')) {
        /* root object */
        size_t oi;
        if (!push_frame(t, FR_OBJ, indent))
          return;
        oi = t->nframes - 1;
        if (handle_member(t, oi, content, content + clen, indent) != 0)
          return;
        t->state = ST_BODY;
        return;
      }
      /* root scalar */
      if (emit_scalar(t, content, clen) != 0)
        return;
      t->state = ST_DOC_DONE;
      return;
    }
  }

  /* ST_BODY: reconcile against open containers, then attach. */
  reconcile(t, indent);
  if (t->err != JSON2TOON_OK)
    return;
  if (t->nframes == 0) {                 /* content after the root closed */
    set_err(t, JSON2TOON_ERR_PARSE, t->line_off);
    return;
  }
  {
    size_t ti = t->nframes - 1;
    frame *f = &t->frames[ti];
    if (f->child_indent != U_UNSET && indent > f->child_indent) {
      set_err(t, JSON2TOON_ERR_PARSE, t->line_off);   /* unexpected over-indent */
      return;
    }
    switch (f->kind) {
      case FR_OBJ:
        if (is_item) { set_err(t, JSON2TOON_ERR_PARSE, t->line_off); return; }
        handle_member(t, ti, content, content + clen, indent);
        break;
      case FR_ARR_LIST:
        if (!is_item) { set_err(t, JSON2TOON_ERR_PARSE, t->line_off); return; }
        handle_item(t, ti, val, val + vlen, indent);
        break;
      case FR_ARR_TAB:
        if (is_item) { set_err(t, JSON2TOON_ERR_PARSE, t->line_off); return; }
        handle_row(t, ti, content, content + clen, indent);
        break;
    }
  }
}

/* ------------------------------------------------------------------- feed */

int toon2json_feed(toon2json *t, const char *data, size_t len) {
  size_t i;
  if (t->err != JSON2TOON_OK)
    return t->err;

  for (i = 0; i < len; i++) {
    char c = data[i];
    if (c == '\n') {
      process_line(t);
      t->linelen = 0;
      t->stream_pos++;
      t->line_off = t->stream_pos;
      if (t->err != JSON2TOON_OK)
        return t->err;
      if (t->out.err != JSON2TOON_OK) {
        set_err(t, t->out.err, t->stream_pos);
        return t->err;
      }
      continue;
    }
    if (t->opt.max_line_bytes && t->linelen + 1 > t->opt.max_line_bytes) {
      set_err(t, JSON2TOON_ERR_LIMIT, t->stream_pos);
      return t->err;
    }
    if (buf_reserve(&t->line, &t->linecap, t->linelen + 1) != 0) {
      set_err(t, JSON2TOON_ERR_MEMORY, t->stream_pos);
      return t->err;
    }
    t->line[t->linelen++] = c;
    t->stream_pos++;
  }
  return JSON2TOON_OK;
}

/* ----------------------------------------------------------------- finish */

int toon2json_finish(toon2json *t) {
  if (t->err != JSON2TOON_OK)
    return t->err;

  /* process any final line not terminated by a newline */
  if (t->linelen > 0) {
    process_line(t);
    t->linelen = 0;
    if (t->err != JSON2TOON_OK)
      return t->err;
  }

  /* empty / whitespace-only input round-trips the empty object */
  if (t->state == ST_START) {
    j2t_puts(&t->out, "{}");
    t->state = ST_DOC_DONE;
  }

  /* close any still-open containers */
  while (t->nframes > 0) {
    close_frame(t, &t->frames[t->nframes - 1]);
    t->nframes--;
    if (t->err != JSON2TOON_OK)
      return t->err;
  }

  if (t->out.err != JSON2TOON_OK) {
    set_err(t, t->out.err, t->stream_pos);
    return t->err;
  }
  if (j2t_out_flush(&t->out) != JSON2TOON_OK) {
    set_err(t, JSON2TOON_ERR_IO, t->stream_pos);
    return t->err;
  }
  return JSON2TOON_OK;
}

/* -------------------------------------------------------------- lifecycle */

toon2json *toon2json_new(json2toon_sink sink, void *ctx,
                         const toon2json_options *opts) {
  toon2json *t;
  if (!sink)
    return NULL;
  t = (toon2json *)calloc(1, sizeof *t);
  if (!t)
    return NULL;

  t->opt.max_depth = T2J_DEFAULT_DEPTH;
  t->opt.max_line_bytes = T2J_DEFAULT_LINE_BYTES;
  if (opts) {
    if (opts->max_depth)
      t->opt.max_depth = opts->max_depth;
    if (opts->max_line_bytes)
      t->opt.max_line_bytes = opts->max_line_bytes;
  }

  j2t_simd_init();
  /* indent width is irrelevant for JSON output; pass 0 */
  j2t_out_init(&t->out, sink, ctx, 0);
  t->state = ST_START;
  t->err = JSON2TOON_OK;
  return t;
}

void toon2json_delete(toon2json *t) {
  size_t i;
  if (!t)
    return;
  for (i = 0; i < t->nframes; i++)
    frame_free_cols(&t->frames[i]);
  free(t->frames);
  free(t->line);
  free(t->scratch);
  free(t);
}

size_t toon2json_error_offset(const toon2json *t) {
  return t ? t->err_offset : 0;
}

const char *toon2json_strerror(int rc) {
  switch (rc) {
    case JSON2TOON_OK: return "success";
    case JSON2TOON_ERR_PARSE: return "malformed TOON input";
    case JSON2TOON_ERR_IO: return "output write error";
    case JSON2TOON_ERR_MEMORY: return "out of memory";
    case JSON2TOON_ERR_DEPTH: return "maximum nesting depth exceeded";
    case JSON2TOON_ERR_LIMIT: return "configured size limit exceeded";
    case JSON2TOON_ERR_USAGE: return "API misuse";
    default: return "unknown error";
  }
}
