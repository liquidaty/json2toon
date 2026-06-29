/* json2toon - bounded-memory array encoder.
 *
 * A TOON array header ("[N]", "[N]{cols}") needs the count/columns, known only
 * after the whole array is seen. So each array's raw bytes are captured into a
 * store (store.c, which may spill to disk) and encoded by reading it back -- no
 * O(N) value tree. Two passes per array:
 *   1. Validate + bound depth with one YAJL parse (the only real JSON parse).
 *   2. Emit: a bracket/quote/escape walker over the now-valid bytes finds spans,
 *      classifies inline/tabular/list, and streams TOON (strings/keys decoded,
 *      numbers verbatim).
 * Peak memory = lookahead window + shallow stack + one tabular row, regardless
 * of array length. Output is identical whether bytes stayed in RAM or spilled.
 */
#include "internal.h"

#include <yajl/yajl_parse.h>

#include <stdlib.h>
#include <string.h>

#define DELIM ','

/* Array classification (precedence: EMPTY > INLINE > TABULAR > LIST). */
enum { ARR_EMPTY, ARR_INLINE, ARR_TABULAR, ARR_LIST };

/* One tabular column (a decoded key, by offset into a key-bytes buffer). */
typedef struct { size_t off, len; } j2t_col;
/* One object member captured while scanning a row: decoded key + value span. */
typedef struct { size_t koff, klen; uint64_t vstart, vend; int vfirst; } j2t_cell;

/* Encoder state threaded through the recursive emit. Owns reusable scratch
 * (grown to the widest key/scalar/row), so one captured array allocates a fixed
 * handful of buffers regardless of nesting. */
typedef struct {
  j2t_out *out;
  j2t_reader rd;
  unsigned max_depth;
  unsigned depth;            /* current structural-emit recursion depth */
  int err;                   /* sticky JSON2TOON_* (memory / io / parse) */

  char *dec; size_t deccap;          /* one decoded string or key */
  char *raw; size_t rawcap;          /* one raw token read from the store */

  char *tpl_buf; size_t tpl_buflen, tpl_bufcap;   /* tabular column key bytes */
  j2t_col *tpl; size_t tpl_n, tpl_cap;            /* tabular columns */

  char *row_buf; size_t row_buflen, row_bufcap;   /* current row's key bytes */
  j2t_cell *row; size_t row_n, row_cap;           /* current row's members */
} j2t_enc;

static void enc_free(j2t_enc *e) {
  free(e->dec);
  free(e->raw);
  free(e->tpl_buf);
  free(e->tpl);
  free(e->row_buf);
  free(e->row);
}

static int grow(char **b, size_t *cap, size_t need) {
  if (need <= *cap)
    return 0;
  {
    size_t nc = *cap ? *cap : 64;
    char *nb;
    while (nc < need)
      nc *= 2;
    nb = (char *)realloc(*b, nc);
    if (!nb)
      return -1;
    *b = nb;
    *cap = nc;
  }
  return 0;
}

/* ----------------------------------------------------- pass 1: validation */

/* yajl allocator flagging OOM, so it surfaces as ERR_MEMORY not ERR_PARSE. */
typedef struct { int oom; } yajl_oomf;
static void *y_malloc(void *c, size_t n) {
  void *p = malloc(n);
  if (!p) ((yajl_oomf *)c)->oom = 1;
  return p;
}
static void *y_realloc(void *c, void *p, size_t n) {
  void *q = realloc(p, n);
  if (!q) ((yajl_oomf *)c)->oom = 1;
  return q;
}
static void y_free(void *c, void *p) { (void)c; free(p); }

/* Depth guard: the parser has no hard cap, so count open containers and cancel
 * past max_depth (-> ERR_DEPTH). */
typedef struct { unsigned open, max; int exceeded; } depth_ctx;
static int v_open(void *c) {
  depth_ctx *d = (depth_ctx *)c;
  if (++d->open > d->max) { d->exceeded = 1; return 0; }
  return 1;
}
static int v_close(void *c) { ((depth_ctx *)c)->open--; return 1; }

/* Parse the whole store once for validation + depth bounding. UTF-8 is not
 * validated, matching the streaming object path (both pass bytes through). Sets
 * *errpos (array-relative) on parse/depth errors. */
static int run_validate(j2t_enc *e, uint64_t *errpos) {
  j2t_store *s = e->rd.s;
  depth_ctx d; yajl_oomf oom;
  yajl_alloc_funcs af;
  yajl_callbacks cb;
  yajl_handle h;
  yajl_status st = yajl_status_ok;
  uint64_t lastchunk = 0, total = s->total;
  int rc;

  d.open = 0; d.max = e->max_depth; d.exceeded = 0;
  oom.oom = 0;
  af.malloc = y_malloc; af.realloc = y_realloc; af.free = y_free; af.ctx = &oom;
  memset(&cb, 0, sizeof cb);
  cb.yajl_start_array = v_open; cb.yajl_end_array = v_close;
  cb.yajl_start_map = v_open; cb.yajl_end_map = v_close;

  h = yajl_alloc(&cb, &af, &d);
  if (!h)
    return JSON2TOON_ERR_MEMORY;
  yajl_config(h, yajl_dont_validate_strings, 1);

  if (!s->spilled) {
    st = yajl_parse(h, (const unsigned char *)s->ram, (size_t)total);
  } else {
    char chunk[4096];
    uint64_t off = 0;
    j2t_reader_seek(&e->rd, 0);
    while (off < total && st == yajl_status_ok) {
      size_t want = (total - off) < sizeof chunk ? (size_t)(total - off)
                                                 : sizeof chunk;
      size_t got = j2t_reader_read(&e->rd, chunk, want);
      if (got != want) { yajl_free(h); return JSON2TOON_ERR_IO; }
      lastchunk = off;
      st = yajl_parse(h, (const unsigned char *)chunk, got);
      off += got;
    }
  }
  if (st == yajl_status_ok)
    st = yajl_complete_parse(h);

  if (st == yajl_status_ok)
    rc = JSON2TOON_OK;
  else if (oom.oom)
    rc = JSON2TOON_ERR_MEMORY;
  else if (d.exceeded)
    rc = JSON2TOON_ERR_DEPTH;
  else
    rc = JSON2TOON_ERR_PARSE;
  if ((rc == JSON2TOON_ERR_PARSE || rc == JSON2TOON_ERR_DEPTH) && errpos)
    *errpos = lastchunk + (uint64_t)yajl_get_bytes_consumed(h);
  yajl_free(h);
  return rc;
}

/* ------------------------------------------------ string / key decoding */

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

static int hex4(const char *s, unsigned *out) {
  unsigned v = 0;
  int i;
  for (i = 0; i < 4; i++) {
    char c = s[i];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
    else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
    else return -1;
  }
  *out = v;
  return 0;
}

/* Decode JSON string content [s,s+n) (between the quotes) into *buf. Mirrors the
 * old DOM decoder byte-for-byte; input is pre-validated, so errors are defensive. */
static int decode_into(const char *s, size_t n, char **buf, size_t *cap,
                       size_t *outlen) {
  const char *end = s + n;
  char *w;
  if (grow(buf, cap, n ? n : 1) != 0)
    return -1;
  w = *buf;
  while (s < end) {
    unsigned char c = (unsigned char)*s;
    if (c == '\\') {
      s++;
      if (s >= end) return -1;
      switch (*s) {
        case '"': *w++ = '"'; s++; break;
        case '\\': *w++ = '\\'; s++; break;
        case '/': *w++ = '/'; s++; break;
        case 'b': *w++ = '\b'; s++; break;
        case 'f': *w++ = '\f'; s++; break;
        case 'n': *w++ = '\n'; s++; break;
        case 'r': *w++ = '\r'; s++; break;
        case 't': *w++ = '\t'; s++; break;
        case 'u': {
          unsigned cp; char enc[4]; int el;
          if (s + 5 > end || hex4(s + 1, &cp) != 0) return -1;
          s += 5;
          if (cp >= 0xd800 && cp <= 0xdbff) {
            unsigned lo;
            if (s + 6 > end || s[0] != '\\' || s[1] != 'u' ||
                hex4(s + 2, &lo) != 0 || lo < 0xdc00 || lo > 0xdfff)
              return -1;
            cp = 0x10000 + (((cp - 0xd800) << 10) | (lo - 0xdc00));
            s += 6;
          } else if (cp >= 0xdc00 && cp <= 0xdfff) {
            return -1;
          }
          el = utf8_encode(cp, enc);
          memcpy(w, enc, (size_t)el);
          w += el;
          break;
        }
        default: return -1;
      }
    } else {
      *w++ = (char)c;
      s++;
    }
  }
  *outlen = (size_t)(w - *buf);
  return 0;
}

/* Read the quoted token [qstart,qend) from the store and decode it into *buf. */
static int decode_quoted(j2t_enc *e, uint64_t qstart, uint64_t qend,
                         char **buf, size_t *cap, size_t *outlen) {
  uint64_t a = qstart + 1;                 /* skip opening quote */
  uint64_t b = qend > qstart + 1 ? qend - 1 : qstart + 1;  /* before close */
  size_t n = (size_t)(b - a);
  if (grow(&e->raw, &e->rawcap, n ? n : 1) != 0) {
    e->err = JSON2TOON_ERR_MEMORY;
    return -1;
  }
  if (n) {
    j2t_reader_seek(&e->rd, a);
    if (j2t_reader_read(&e->rd, e->raw, n) != n) {
      e->err = e->rd.s->err ? e->rd.s->err : JSON2TOON_ERR_IO;
      return -1;
    }
  }
  if (decode_into(e->raw, n, buf, cap, outlen) != 0) {
    if (!e->err) e->err = JSON2TOON_ERR_PARSE;
    return -1;
  }
  return 0;
}

/* ----------------------------------------------- structural skip walkers */

static int is_ws(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static void sk_ws(j2t_enc *e) {
  while (is_ws(j2t_reader_peek(&e->rd)))
    j2t_reader_getc(&e->rd);
}

/* Advance past a JSON string starting at the opening quote. */
static void sk_string(j2t_enc *e) {
  j2t_reader_getc(&e->rd);                 /* opening quote */
  for (;;) {
    int c = j2t_reader_getc(&e->rd);
    if (c < 0) break;
    if (c == '\\') j2t_reader_getc(&e->rd);
    else if (c == '"') break;
  }
}

/* Advance past one JSON value (positioned at its first byte); assumes valid. */
static void sk_value(j2t_enc *e) {
  int c = j2t_reader_peek(&e->rd);
  if (c == '"') { sk_string(e); return; }
  if (c == '[' || c == '{') {
    int depth = 0;
    for (;;) {
      int d = j2t_reader_getc(&e->rd);
      if (d < 0) break;
      if (d == '"') {
        for (;;) {
          int x = j2t_reader_getc(&e->rd);
          if (x < 0) break;
          if (x == '\\') j2t_reader_getc(&e->rd);
          else if (x == '"') break;
        }
      } else if (d == '[' || d == '{') {
        depth++;
      } else if (d == ']' || d == '}') {
        if (--depth == 0) break;
      }
    }
    return;
  }
  /* number / literal: run to the next structural byte */
  for (;;) {
    int p = j2t_reader_peek(&e->rd);
    if (p < 0 || p == ',' || p == ']' || p == '}' || is_ws(p)) break;
    j2t_reader_getc(&e->rd);
  }
}

/* True if the array at `astart` is empty. Leaves pos unspecified; callers reseek. */
static int array_is_empty(j2t_enc *e, uint64_t astart) {
  j2t_reader_seek(&e->rd, astart);
  j2t_reader_getc(&e->rd);                 /* '[' */
  sk_ws(e);
  return j2t_reader_peek(&e->rd) == ']';
}

static int object_is_empty(j2t_enc *e, uint64_t ostart) {
  j2t_reader_seek(&e->rd, ostart);
  j2t_reader_getc(&e->rd);                 /* '{' */
  sk_ws(e);
  return j2t_reader_peek(&e->rd) == '}';
}

/* ------------------------------------------------------ scalar emission */

static void emit_scalar_span(j2t_enc *e, uint64_t vstart, uint64_t vend) {
  int c;
  if (e->err || e->out->err) return;
  j2t_reader_seek(&e->rd, vstart);
  c = j2t_reader_peek(&e->rd);
  if (c == '"') {
    size_t len;
    if (decode_quoted(e, vstart, vend, &e->dec, &e->deccap, &len) != 0)
      return;
    j2t_emit_string(e->out, e->dec, len, DELIM);
  } else {
    /* numbers and true/false/null: emit the raw token verbatim */
    uint64_t k;
    for (k = vstart; k < vend; k++) {
      int b = j2t_reader_getc(&e->rd);
      if (b < 0) { e->err = e->rd.s->err ? e->rd.s->err : JSON2TOON_ERR_IO; return; }
      j2t_putc(e->out, (char)b);
    }
  }
}

/* --------------------------------------------------- row / column tables */

static int col_find(j2t_enc *e, const char *k, size_t kl) {
  size_t i;
  for (i = 0; i < e->tpl_n; i++)
    if (e->tpl[i].len == kl &&
        memcmp(e->tpl_buf + e->tpl[i].off, k, kl) == 0)
      return (int)i;
  return -1;
}

static int row_find(j2t_enc *e, const char *k, size_t kl) {
  size_t i;
  for (i = 0; i < e->row_n; i++)
    if (e->row[i].klen == kl &&
        memcmp(e->row_buf + e->row[i].koff, k, kl) == 0)
      return (int)i;
  return -1;
}

static int row_push(j2t_enc *e, const char *k, size_t kl, uint64_t vstart,
                    uint64_t vend, int vfirst) {
  size_t koff;
  if (grow(&e->row_buf, &e->row_bufcap, e->row_buflen + (kl ? kl : 1)) != 0)
    return -1;
  koff = e->row_buflen;
  memcpy(e->row_buf + koff, k, kl);
  e->row_buflen += kl;
  if (e->row_n == e->row_cap) {
    size_t nc = e->row_cap ? e->row_cap * 2 : 8;
    j2t_cell *np = (j2t_cell *)realloc(e->row, nc * sizeof *np);
    if (!np) return -1;
    e->row = np;
    e->row_cap = nc;
  }
  e->row[e->row_n].koff = koff;
  e->row[e->row_n].klen = kl;
  e->row[e->row_n].vstart = vstart;
  e->row[e->row_n].vend = vend;
  e->row[e->row_n].vfirst = vfirst;
  e->row_n++;
  return 0;
}

/* Scan the object at `ostart` ("{") into the row table (decoded key + value span
 * per member). Leaves the reader just past the closing '}'. */
static int collect_row(j2t_enc *e, uint64_t ostart) {
  e->row_n = 0;
  e->row_buflen = 0;
  j2t_reader_seek(&e->rd, ostart);
  j2t_reader_getc(&e->rd);                 /* '{' */
  for (;;) {
    int c;
    uint64_t kstart, kend, vstart, vend;
    size_t klen;
    sk_ws(e);
    c = j2t_reader_peek(&e->rd);
    if (c == '}') { j2t_reader_getc(&e->rd); break; }
    if (c < 0) { e->err = JSON2TOON_ERR_PARSE; return -1; }  /* ran past end */
    if (c == ',') { j2t_reader_getc(&e->rd); sk_ws(e); }
    kstart = j2t_reader_tell(&e->rd);
    sk_string(e);
    kend = j2t_reader_tell(&e->rd);
    sk_ws(e);
    j2t_reader_getc(&e->rd);                /* ':' */
    sk_ws(e);
    vstart = j2t_reader_tell(&e->rd);
    c = j2t_reader_peek(&e->rd);
    sk_value(e);
    vend = j2t_reader_tell(&e->rd);
    if (decode_quoted(e, kstart, kend, &e->dec, &e->deccap, &klen) != 0)
      return -1;
    /* decode_quoted seeked away to read the key; resume after the value. */
    j2t_reader_seek(&e->rd, vend);
    if (row_push(e, e->dec, klen, vstart, vend, c) != 0) {
      e->err = JSON2TOON_ERR_MEMORY;
      return -1;
    }
  }
  return 0;
}

/* Snapshot the current row as the tabular column template; declines (clears
 * *tab_ok) on a duplicate key in the first object. */
static int build_template(j2t_enc *e, int *tab_ok) {
  size_t i;
  e->tpl_n = 0;
  e->tpl_buflen = 0;
  for (i = 0; i < e->row_n; i++) {
    const char *k = e->row_buf + e->row[i].koff;
    size_t kl = e->row[i].klen;
    size_t off;
    if (col_find(e, k, kl) >= 0)
      *tab_ok = 0;                         /* duplicate key: not tabular */
    if (grow(&e->tpl_buf, &e->tpl_bufcap, e->tpl_buflen + (kl ? kl : 1)) != 0)
      return -1;
    off = e->tpl_buflen;
    memcpy(e->tpl_buf + off, k, kl);
    e->tpl_buflen += kl;
    if (e->tpl_n == e->tpl_cap) {
      size_t nc = e->tpl_cap ? e->tpl_cap * 2 : 8;
      j2t_col *np = (j2t_col *)realloc(e->tpl, nc * sizeof *np);
      if (!np) return -1;
      e->tpl = np;
      e->tpl_cap = nc;
    }
    e->tpl[e->tpl_n].off = off;
    e->tpl[e->tpl_n].len = kl;
    e->tpl_n++;
  }
  return 0;
}

/* True if the current row's keys are exactly the template's key set (same count,
 * no duplicates, every key present) -- order may differ, per TOON spec §9.3. */
static int row_matches_template(j2t_enc *e) {
  size_t i, j;
  if (e->row_n != e->tpl_n)
    return 0;
  for (i = 0; i < e->row_n; i++) {
    const char *k = e->row_buf + e->row[i].koff;
    size_t kl = e->row[i].klen;
    for (j = 0; j < i; j++)
      if (e->row[j].klen == kl &&
          memcmp(e->row_buf + e->row[j].koff, k, kl) == 0)
        return 0;                          /* duplicate within the row */
    if (col_find(e, k, kl) < 0)
      return 0;
  }
  return 1;
}

/* --------------------------------------------------- pass 2: classify */

/* Classify the array [astart,...]: kind + element count, and (for TABULAR) the
 * column template in e->tpl. Structural only -- bytes are already validated. */
static void shape_array(j2t_enc *e, uint64_t astart, int *kind, size_t *count) {
  size_t n = 0;
  int all_scalar = 1, all_objects = 1, tab_ok = 1, have_tpl = 0;

  e->tpl_n = 0;
  e->tpl_buflen = 0;
  j2t_reader_seek(&e->rd, astart);
  j2t_reader_getc(&e->rd);                  /* '[' */
  for (;;) {
    int c;
    sk_ws(e);
    c = j2t_reader_peek(&e->rd);
    if (c == ']') { j2t_reader_getc(&e->rd); break; }
    if (c == ',') { j2t_reader_getc(&e->rd); sk_ws(e); c = j2t_reader_peek(&e->rd); }
    if (c < 0) { e->err = JSON2TOON_ERR_PARSE; return; }  /* ran past end */

    if (c == '{') {
      uint64_t ostart = j2t_reader_tell(&e->rd);
      all_scalar = 0;
      if (all_objects && tab_ok) {
        size_t i;
        if (collect_row(e, ostart) != 0) return;
        if (e->row_n == 0) tab_ok = 0;     /* empty object: not tabular */
        for (i = 0; i < e->row_n && tab_ok; i++)
          if (e->row[i].vfirst == '{' || e->row[i].vfirst == '[')
            tab_ok = 0;                     /* non-primitive value */
        if (tab_ok) {
          if (!have_tpl) {
            if (build_template(e, &tab_ok) != 0) { e->err = JSON2TOON_ERR_MEMORY; return; }
            have_tpl = 1;
          } else if (!row_matches_template(e)) {
            tab_ok = 0;
          }
        }
      } else {
        j2t_reader_seek(&e->rd, ostart);
        sk_value(e);
      }
    } else if (c == '[') {
      all_scalar = 0; all_objects = 0; tab_ok = 0;
      sk_value(e);
    } else {
      all_objects = 0; tab_ok = 0;
      sk_value(e);
    }
    n++;
    sk_ws(e);
    c = j2t_reader_getc(&e->rd);
    if (c == ']') break;
    if (c < 0) { e->err = JSON2TOON_ERR_PARSE; return; }  /* unterminated */
    /* otherwise c == ',' -> next element */
  }

  *count = n;
  if (n == 0) *kind = ARR_EMPTY;
  else if (all_scalar) *kind = ARR_INLINE;
  else if (all_objects && tab_ok) *kind = ARR_TABULAR;
  else *kind = ARR_LIST;
}

/* --------------------------------------------------- pass 2: emit */

static void emit_array_tail(j2t_enc *e, uint64_t astart, unsigned level);
static void emit_object_body(j2t_enc *e, uint64_t ostart, unsigned level,
                             int first_inline);

/* Emit "key: value" / "key:" + nested, indentation already written. */
static void emit_member_body(j2t_enc *e, uint64_t kstart, uint64_t kend,
                             uint64_t vstart, unsigned level) {
  size_t klen;
  int c;
  if (e->err || e->out->err) return;
  if (decode_quoted(e, kstart, kend, &e->dec, &e->deccap, &klen) != 0)
    return;
  j2t_emit_key(e->out, e->dec, klen);
  j2t_reader_seek(&e->rd, vstart);
  c = j2t_reader_peek(&e->rd);
  if (c == '{') {
    j2t_putc(e->out, ':');
    j2t_putc(e->out, '\n');
    if (!object_is_empty(e, vstart))
      emit_object_body(e, vstart, level + 1, 0);
  } else if (c == '[') {
    if (array_is_empty(e, vstart)) {
      j2t_puts(e->out, ": []");
      j2t_putc(e->out, '\n');
    } else {
      emit_array_tail(e, vstart, level);
    }
  } else {
    uint64_t vend;
    j2t_reader_seek(&e->rd, vstart);
    sk_value(e);
    vend = j2t_reader_tell(&e->rd);
    j2t_puts(e->out, ": ");
    emit_scalar_span(e, vstart, vend);
    j2t_putc(e->out, '\n');
  }
}

/* Walk the object at `ostart`, emitting each member at `level`. When
 * first_inline, the first member follows an already-written "- " (no indent). */
static void emit_object_body(j2t_enc *e, uint64_t ostart, unsigned level,
                             int first_inline) {
  uint64_t cur;
  size_t idx = 0;
  if (e->err || e->out->err) return;
  if (e->depth >= e->max_depth) { e->err = JSON2TOON_ERR_DEPTH; return; }
  e->depth++;
  j2t_reader_seek(&e->rd, ostart);
  j2t_reader_getc(&e->rd);                  /* '{' */
  cur = j2t_reader_tell(&e->rd);
  for (;;) {
    int c;
    uint64_t kstart, kend, vstart;
    if (e->err || e->out->err) break;
    j2t_reader_seek(&e->rd, cur);
    sk_ws(e);
    c = j2t_reader_peek(&e->rd);
    if (c == '}' || c < 0) break;            /* close, or ran past end */
    if (c == ',') { j2t_reader_getc(&e->rd); sk_ws(e); }
    kstart = j2t_reader_tell(&e->rd);
    sk_string(e);
    kend = j2t_reader_tell(&e->rd);
    sk_ws(e);
    j2t_reader_getc(&e->rd);                 /* ':' */
    sk_ws(e);
    vstart = j2t_reader_tell(&e->rd);
    sk_value(e);
    cur = j2t_reader_tell(&e->rd);
    if (!(idx == 0 && first_inline))
      j2t_indent(e->out, level);
    emit_member_body(e, kstart, kend, vstart, level);
    idx++;
  }
  e->depth--;
}

/* Emit one array element as a list item ("- ...") at `level`. */
static void emit_item(j2t_enc *e, uint64_t cstart, unsigned level) {
  int c;
  if (e->err || e->out->err) return;
  j2t_reader_seek(&e->rd, cstart);
  c = j2t_reader_peek(&e->rd);
  if (c == '[') {
    j2t_indent(e->out, level);
    j2t_puts(e->out, "- ");
    if (array_is_empty(e, cstart)) {
      j2t_puts(e->out, "[]");
      j2t_putc(e->out, '\n');
    } else {
      emit_array_tail(e, cstart, level + 1);
    }
  } else if (c == '{') {
    if (object_is_empty(e, cstart)) {
      j2t_indent(e->out, level);
      j2t_putc(e->out, '-');
      j2t_putc(e->out, '\n');
    } else {
      j2t_indent(e->out, level);
      j2t_puts(e->out, "- ");
      emit_object_body(e, cstart, level + 1, 1);
    }
  } else {
    uint64_t vend;
    sk_value(e);
    vend = j2t_reader_tell(&e->rd);
    j2t_indent(e->out, level);
    j2t_puts(e->out, "- ");
    emit_scalar_span(e, cstart, vend);
    j2t_putc(e->out, '\n');
  }
}

/* "[N]: v1,v2,..." */
static void emit_inline(j2t_enc *e, uint64_t astart, unsigned level,
                        size_t count) {
  uint64_t cur;
  size_t idx = 0;
  (void)level;
  j2t_putc(e->out, '[');
  j2t_emit_count(e->out, count);
  j2t_puts(e->out, "]: ");
  j2t_reader_seek(&e->rd, astart);
  j2t_reader_getc(&e->rd);                  /* '[' */
  cur = j2t_reader_tell(&e->rd);
  for (;;) {
    uint64_t cstart, cend;
    int c;
    if (e->err || e->out->err) return;
    j2t_reader_seek(&e->rd, cur);
    sk_ws(e);
    c = j2t_reader_peek(&e->rd);
    if (c == ']' || c < 0) break;            /* close, or ran past end */
    if (c == ',') { j2t_reader_getc(&e->rd); sk_ws(e); }
    cstart = j2t_reader_tell(&e->rd);
    sk_value(e);
    cend = j2t_reader_tell(&e->rd);
    if (cend <= cstart) { e->err = JSON2TOON_ERR_PARSE; break; }  /* no progress */
    cur = cend;
    if (idx) j2t_putc(e->out, DELIM);
    emit_scalar_span(e, cstart, cend);
    idx++;
  }
  j2t_putc(e->out, '\n');
}

/* "[N]{cols}:" then one row per object, cells in template-column order. */
static void emit_tabular(j2t_enc *e, uint64_t astart, unsigned level,
                         size_t count) {
  uint64_t cur;
  size_t t;
  j2t_putc(e->out, '[');
  j2t_emit_count(e->out, count);
  j2t_puts(e->out, "]{");
  for (t = 0; t < e->tpl_n; t++) {
    if (t) j2t_putc(e->out, DELIM);
    j2t_emit_key(e->out, e->tpl_buf + e->tpl[t].off, e->tpl[t].len);
  }
  j2t_puts(e->out, "}:");
  j2t_putc(e->out, '\n');

  j2t_reader_seek(&e->rd, astart);
  j2t_reader_getc(&e->rd);                  /* '[' */
  cur = j2t_reader_tell(&e->rd);
  for (;;) {
    uint64_t ostart, oend;
    int c;
    if (e->err || e->out->err) return;
    j2t_reader_seek(&e->rd, cur);
    sk_ws(e);
    c = j2t_reader_peek(&e->rd);
    if (c == ']' || c < 0) break;            /* close, or ran past end */
    if (c == ',') { j2t_reader_getc(&e->rd); sk_ws(e); }
    ostart = j2t_reader_tell(&e->rd);
    if (collect_row(e, ostart) != 0) return;
    oend = j2t_reader_tell(&e->rd);
    if (oend <= ostart) { e->err = JSON2TOON_ERR_PARSE; break; }  /* no progress */
    cur = oend;
    j2t_indent(e->out, level + 1);
    for (t = 0; t < e->tpl_n; t++) {
      int ci;
      if (t) j2t_putc(e->out, DELIM);
      ci = row_find(e, e->tpl_buf + e->tpl[t].off, e->tpl[t].len);
      if (ci >= 0)
        emit_scalar_span(e, e->row[ci].vstart, e->row[ci].vend);
    }
    j2t_putc(e->out, '\n');
  }
}

/* "[N]:" then one list item per element at level+1. */
static void emit_list(j2t_enc *e, uint64_t astart, unsigned level,
                      size_t count) {
  uint64_t cur;
  j2t_putc(e->out, '[');
  j2t_emit_count(e->out, count);
  j2t_puts(e->out, "]:");
  j2t_putc(e->out, '\n');
  j2t_reader_seek(&e->rd, astart);
  j2t_reader_getc(&e->rd);                  /* '[' */
  cur = j2t_reader_tell(&e->rd);
  for (;;) {
    uint64_t cstart, cend;
    int c;
    if (e->err || e->out->err) return;
    j2t_reader_seek(&e->rd, cur);
    sk_ws(e);
    c = j2t_reader_peek(&e->rd);
    if (c == ']' || c < 0) break;            /* close, or ran past end */
    if (c == ',') { j2t_reader_getc(&e->rd); sk_ws(e); }
    cstart = j2t_reader_tell(&e->rd);
    sk_value(e);
    cend = j2t_reader_tell(&e->rd);
    if (cend <= cstart) { e->err = JSON2TOON_ERR_PARSE; break; }  /* no progress */
    cur = cend;
    emit_item(e, cstart, level + 1);
  }
}

/* Emit a non-empty array's "[N]..." header and body; the line lead (indent+key
 * or indent+"- ") is already written. Classifies, then dispatches. */
static void emit_array_tail(j2t_enc *e, uint64_t astart, unsigned level) {
  int kind;
  size_t count;
  if (e->err || e->out->err) return;
  if (e->depth >= e->max_depth) { e->err = JSON2TOON_ERR_DEPTH; return; }
  e->depth++;
  shape_array(e, astart, &kind, &count);
  if (!e->err) {
    switch (kind) {
      case ARR_INLINE: emit_inline(e, astart, level, count); break;
      case ARR_TABULAR: emit_tabular(e, astart, level, count); break;
      default: emit_list(e, astart, level, count); break;
    }
  }
  e->depth--;
}

/* Top-level entry: indent, handle the empty array, place the key, then tail. */
static void emit_value_array(j2t_enc *e, uint64_t astart, unsigned level,
                             const char *key, size_t keylen) {
  j2t_indent(e->out, level);
  if (array_is_empty(e, astart)) {
    if (key) {
      j2t_emit_key(e->out, key, keylen);
      j2t_puts(e->out, ": []");
    } else {
      j2t_puts(e->out, "[]");
    }
    j2t_putc(e->out, '\n');
    return;
  }
  if (key)
    j2t_emit_key(e->out, key, keylen);
  emit_array_tail(e, astart, level);
}

/* ------------------------------------------------------------- public API */

int j2t_encode_captured(j2t_out *o, j2t_store *s, unsigned level,
                        const char *key, size_t keylen,
                        unsigned max_depth, uint64_t *errpos) {
  j2t_enc e;
  int rc;
  memset(&e, 0, sizeof e);
  e.out = o;
  e.max_depth = max_depth ? max_depth : 128;
  j2t_reader_init(&e.rd, s);

  rc = run_validate(&e, errpos);
  if (rc != JSON2TOON_OK) {
    enc_free(&e);
    return rc;
  }

  emit_value_array(&e, 0, level, key, keylen);

  rc = e.err ? e.err : o->err;
  if (rc == JSON2TOON_OK && s->err != JSON2TOON_OK)
    rc = s->err;
  enc_free(&e);
  return rc;
}
