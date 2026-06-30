/* json2toon - bounded-memory array encoder.
 *
 * A TOON array header ("[N]", "[N]{cols}") needs the element count / columns,
 * known only after the whole array is seen. So each array's raw bytes are
 * captured into a store (store.c, which may spill to disk) and encoded by
 * reading it back -- no O(N) value tree. YAJL is the single tokenizer for both
 * steps, so classification and emission can never disagree about where tokens
 * or whitespace end (the bug class a second hand-rolled walker invited):
 *   1. shape:  one YAJL parse classifies the level (inline/tabular/list) and,
 *              for tabular, records the column template. The top-level shape
 *              over the whole store also validates + bounds depth.
 *   2. emit:   a second YAJL parse streams TOON. Strings/keys arrive decoded;
 *              numbers arrive as their raw lexeme (lossless). A container child
 *              is re-emitted by recursing on its byte span (yajl_get_bytes_-
 *              consumed), so large values are seeked rather than buffered.
 * Peak memory = column template + one tabular row + O(max_depth) parse handles,
 * regardless of array length. Output is identical whether bytes stayed in RAM
 * or spilled.
 */
#include "internal.h"

#include <yajl/yajl_parse.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DELIM ','

/* Array classification (precedence: EMPTY > INLINE > TABULAR > LIST). */
enum { ARR_EMPTY, ARR_INLINE, ARR_TABULAR, ARR_LIST };

/* A decoded key or one tabular cell, by offset/length into a bytes buffer. */
typedef struct { size_t off, len; } j2t_col;
typedef struct { size_t koff, klen, voff, vlen; int vstr; } j2t_cell;

/* Encoder state: the output sink, the captured store, recursion depth, and
 * reusable scratch (template + one row), grown to the widest seen. One captured
 * array allocates a fixed handful of buffers regardless of nesting/length. */
typedef struct {
  j2t_out *out;
  j2t_store *s;
  unsigned max_depth;
  unsigned depth;                          /* structural-emit recursion depth */
  int err;                                 /* sticky JSON2TOON_* */

  char *tpl_buf; size_t tpl_buflen, tpl_bufcap;   /* tabular column key bytes */
  j2t_col *tpl; size_t tpl_n, tpl_cap;            /* tabular columns */

  char *kl_buf; size_t kl_buflen, kl_bufcap;      /* one object's key bytes */
  j2t_col *kl; size_t kl_n, kl_cap;               /* one object's key offsets */

  char *rv_buf; size_t rv_buflen, rv_bufcap;      /* one row's value bytes */
  j2t_cell *cells; size_t ncells, cellcap;        /* one row's cells */
} j2t_enc;

static void enc_free(j2t_enc *e) {
  free(e->tpl_buf);
  free(e->tpl);
  free(e->kl_buf);
  free(e->kl);
  free(e->rv_buf);
  free(e->cells);
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

/* ------------------------------------------------------ yajl over a span */

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

/* Maintained by parse_span so callbacks can map a token to its absolute store
 * offset: chunk_abs is the store offset of the current parse buffer's byte 0,
 * and yajl_get_bytes_consumed is reset per yajl_parse call (per-chunk). */
typedef struct { yajl_handle h; uint64_t chunk_abs; } span_pos;
static uint64_t span_off(const span_pos *p) {
  return p->chunk_abs + (uint64_t)yajl_get_bytes_consumed(p->h);
}

/* Parse store bytes [start,end) with `cb`/`ctx`, sourcing from ram or the spill
 * file transparently. Returns OK / ERR_MEMORY (yajl OOM) / ERR_PARSE (any other
 * non-ok status, including a cancelled callback) / ERR_IO. On ERR_PARSE *errpos
 * (if non-NULL) gets the absolute store offset of the failure. The caller refines
 * ERR_PARSE into ERR_DEPTH / ERR_MEMORY from its own context flags. */
static int parse_span(j2t_enc *e, uint64_t start, uint64_t end,
                      const yajl_callbacks *cb, void *ctx,
                      span_pos *pos, uint64_t *errpos) {
  j2t_store *s = e->s;
  yajl_oomf oom;
  yajl_alloc_funcs af;
  yajl_handle h;
  yajl_status st = yajl_status_ok;
  int rc;

  oom.oom = 0;
  af.malloc = y_malloc; af.realloc = y_realloc; af.free = y_free; af.ctx = &oom;
  h = yajl_alloc(cb, &af, ctx);
  if (!h)
    return JSON2TOON_ERR_MEMORY;
  yajl_config(h, yajl_dont_validate_strings, 1);
  pos->h = h;

  if (!s->spilled) {
    pos->chunk_abs = start;
    st = yajl_parse(h, (const unsigned char *)s->ram + start,
                    (size_t)(end - start));
  } else {
    char chunk[4096];
    uint64_t off = start;
    while (off < end && st == yajl_status_ok) {
      size_t want = (end - off) < sizeof chunk ? (size_t)(end - off)
                                               : sizeof chunk;
      size_t got;
      if (J2T_FSEEK(s->fp, off) != 0) { yajl_free(h); return JSON2TOON_ERR_IO; }
      got = fread(chunk, 1, want, s->fp);
      if (got != want) { yajl_free(h); return JSON2TOON_ERR_IO; }
      pos->chunk_abs = off;
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
  else
    rc = JSON2TOON_ERR_PARSE;
  if (rc == JSON2TOON_ERR_PARSE && errpos)
    *errpos = pos->chunk_abs + (uint64_t)yajl_get_bytes_consumed(h);
  yajl_free(h);
  return rc;
}

/* ----------------------------------------------- template / row scratch */

static int col_find_tpl(const j2t_enc *e, const char *k, size_t kl) {
  size_t i;
  for (i = 0; i < e->tpl_n; i++)
    if (e->tpl[i].len == kl &&
        memcmp(e->tpl_buf + e->tpl[i].off, k, kl) == 0)
      return (int)i;
  return -1;
}

/* Append a decoded key to the current-object key list (used during shape). */
static int kl_push(j2t_enc *e, const unsigned char *k, size_t kl) {
  if (grow(&e->kl_buf, &e->kl_bufcap, e->kl_buflen + (kl ? kl : 1)) != 0)
    return -1;
  if (e->kl_n == e->kl_cap) {
    size_t nc = e->kl_cap ? e->kl_cap * 2 : 8;
    j2t_col *np = (j2t_col *)realloc(e->kl, nc * sizeof *np);
    if (!np) return -1;
    e->kl = np;
    e->kl_cap = nc;
  }
  memcpy(e->kl_buf + e->kl_buflen, k, kl);
  e->kl[e->kl_n].off = e->kl_buflen;
  e->kl[e->kl_n].len = kl;
  e->kl_n++;
  e->kl_buflen += kl;
  return 0;
}

/* Snapshot the current key list as the column template; flags a duplicate key. */
static int build_tpl(j2t_enc *e, int *dup) {
  size_t i;
  e->tpl_n = 0;
  e->tpl_buflen = 0;
  *dup = 0;
  for (i = 0; i < e->kl_n; i++) {
    const char *k = e->kl_buf + e->kl[i].off;
    size_t kl = e->kl[i].len;
    if (col_find_tpl(e, k, kl) >= 0)
      *dup = 1;
    if (grow(&e->tpl_buf, &e->tpl_bufcap, e->tpl_buflen + (kl ? kl : 1)) != 0)
      return -1;
    if (e->tpl_n == e->tpl_cap) {
      size_t nc = e->tpl_cap ? e->tpl_cap * 2 : 8;
      j2t_col *np = (j2t_col *)realloc(e->tpl, nc * sizeof *np);
      if (!np) return -1;
      e->tpl = np;
      e->tpl_cap = nc;
    }
    memcpy(e->tpl_buf + e->tpl_buflen, k, kl);
    e->tpl[e->tpl_n].off = e->tpl_buflen;
    e->tpl[e->tpl_n].len = kl;
    e->tpl_n++;
    e->tpl_buflen += kl;
  }
  return 0;
}

/* True if the current key list is exactly the template's key set (same count,
 * no duplicates, every key present) -- order may differ, per TOON spec 9.3. */
static int kl_matches_tpl(const j2t_enc *e) {
  size_t i, j;
  if (e->kl_n != e->tpl_n)
    return 0;
  for (i = 0; i < e->kl_n; i++) {
    const char *k = e->kl_buf + e->kl[i].off;
    size_t kl = e->kl[i].len;
    for (j = 0; j < i; j++)
      if (e->kl[j].len == kl &&
          memcmp(e->kl_buf + e->kl[j].off, k, kl) == 0)
        return 0;                          /* duplicate within the object */
    if (col_find_tpl(e, k, kl) < 0)
      return 0;
  }
  return 1;
}

/* ------------------------------------------------------ pass 1: shape */

/* Classify one array level: kind + element count, and (TABULAR) the template.
 * Direct children open at depth 1->2 or are scalars seen at depth 1; a container
 * at depth >= 2 is a non-primitive member value, which rules out tabular. */
typedef struct {
  span_pos pos;
  j2t_enc *e;
  unsigned max_depth;
  int depth;
  size_t count;
  int all_scalar, all_objects, tab_ok, have_tpl;
  int mem_err, depth_exceeded;
} shape_ctx;

static int sh_enter(shape_ctx *x) {            /* common to start_map/array */
  x->depth++;
  if (x->depth > (int)x->max_depth) { x->depth_exceeded = 1; return 0; }
  return 1;
}
static void sh_scalar(shape_ctx *x) {
  if (x->depth == 1) { x->count++; x->all_objects = 0; }
}
static int sh_null(void *c) { sh_scalar((shape_ctx *)c); return 1; }
static int sh_bool(void *c, int b) { (void)b; sh_scalar((shape_ctx *)c); return 1; }
static int sh_number(void *c, const char *v, size_t n) {
  (void)v; (void)n; sh_scalar((shape_ctx *)c); return 1;
}
static int sh_string(void *c, const unsigned char *v, size_t n) {
  (void)v; (void)n; sh_scalar((shape_ctx *)c); return 1;
}
static int sh_start_map(void *c) {
  shape_ctx *x = (shape_ctx *)c;
  if (x->depth == 1) {                         /* direct-child object */
    x->count++;
    x->all_scalar = 0;
    if (x->tab_ok) { x->e->kl_n = 0; x->e->kl_buflen = 0; }  /* collect its keys */
  } else if (x->depth >= 2) {
    x->tab_ok = 0;                             /* container as a member value */
  }
  return sh_enter(x);
}
static int sh_start_array(void *c) {
  shape_ctx *x = (shape_ctx *)c;
  if (x->depth == 1) {                         /* direct-child array */
    x->count++;
    x->all_scalar = 0; x->all_objects = 0; x->tab_ok = 0;
  } else if (x->depth >= 2) {
    x->tab_ok = 0;
  }
  return sh_enter(x);
}
static int sh_map_key(void *c, const unsigned char *k, size_t n) {
  shape_ctx *x = (shape_ctx *)c;
  if (x->depth == 2 && x->tab_ok) {
    if (kl_push(x->e, k, n) != 0) { x->mem_err = 1; return 0; }
  }
  return 1;
}
static int sh_end_map(void *c) {
  shape_ctx *x = (shape_ctx *)c;
  x->depth--;
  if (x->depth == 1 && x->tab_ok && x->all_objects) {
    if (x->e->kl_n == 0) {
      x->tab_ok = 0;                           /* empty object: not tabular */
    } else if (!x->have_tpl) {
      int dup = 0;
      if (build_tpl(x->e, &dup) != 0) { x->mem_err = 1; return 0; }
      if (dup) x->tab_ok = 0;
      x->have_tpl = 1;
    } else if (!kl_matches_tpl(x->e)) {
      x->tab_ok = 0;
    }
  }
  return 1;
}
static int sh_end_array(void *c) { ((shape_ctx *)c)->depth--; return 1; }

static int shape_array(j2t_enc *e, uint64_t start, uint64_t end,
                       int *kind, size_t *count, uint64_t *errpos) {
  shape_ctx x;
  yajl_callbacks cb;
  int rc;
  memset(&x, 0, sizeof x);
  x.e = e; x.max_depth = e->max_depth;
  x.all_scalar = 1; x.all_objects = 1; x.tab_ok = 1;
  memset(&cb, 0, sizeof cb);
  cb.yajl_null = sh_null; cb.yajl_boolean = sh_bool; cb.yajl_number = sh_number;
  cb.yajl_string = sh_string;
  cb.yajl_start_map = sh_start_map; cb.yajl_map_key = sh_map_key;
  cb.yajl_end_map = sh_end_map;
  cb.yajl_start_array = sh_start_array; cb.yajl_end_array = sh_end_array;

  rc = parse_span(e, start, end, &cb, &x, &x.pos, errpos);
  if (rc == JSON2TOON_ERR_PARSE) {
    if (x.mem_err) rc = JSON2TOON_ERR_MEMORY;
    else if (x.depth_exceeded) rc = JSON2TOON_ERR_DEPTH;
  }
  if (rc != JSON2TOON_OK)
    return rc;

  *count = x.count;
  if (x.count == 0) *kind = ARR_EMPTY;
  else if (x.all_scalar) *kind = ARR_INLINE;
  else if (x.all_objects && x.tab_ok) *kind = ARR_TABULAR;
  else *kind = ARR_LIST;
  return JSON2TOON_OK;
}

/* ------------------------------------------------------ pass 2: emit */

static void emit_array_dispatch(j2t_enc *e, uint64_t start, uint64_t end,
                                unsigned level, int kind, size_t count);
static void emit_object_body(j2t_enc *e, uint64_t start, uint64_t end,
                             unsigned level, int first_inline);

/* ----- inline: "[N]: v1,v2,..." (all elements scalar) ----- */
typedef struct { span_pos pos; j2t_enc *e; size_t idx; } inline_ctx;
static int il_sep(inline_ctx *x) {
  if (x->e->err || x->e->out->err) return 0;
  if (x->idx++) j2t_putc(x->e->out, DELIM);
  return 1;
}
static int il_null(void *c) {
  inline_ctx *x = (inline_ctx *)c;
  if (!il_sep(x)) return 0;
  j2t_puts(x->e->out, "null"); return 1;
}
static int il_bool(void *c, int b) {
  inline_ctx *x = (inline_ctx *)c;
  if (!il_sep(x)) return 0;
  j2t_puts(x->e->out, b ? "true" : "false"); return 1;
}
static int il_number(void *c, const char *v, size_t n) {
  inline_ctx *x = (inline_ctx *)c;
  if (!il_sep(x)) return 0;
  j2t_write(x->e->out, v, n); return 1;
}
static int il_string(void *c, const unsigned char *v, size_t n) {
  inline_ctx *x = (inline_ctx *)c;
  if (!il_sep(x)) return 0;
  j2t_emit_string(x->e->out, (const char *)v, n, DELIM); return 1;
}
static void emit_inline(j2t_enc *e, uint64_t start, uint64_t end, size_t count) {
  inline_ctx x;
  yajl_callbacks cb;
  int rc;
  memset(&x, 0, sizeof x); x.e = e;
  j2t_putc(e->out, '[');
  j2t_emit_count(e->out, count);
  j2t_puts(e->out, "]: ");
  memset(&cb, 0, sizeof cb);
  cb.yajl_null = il_null; cb.yajl_boolean = il_bool;
  cb.yajl_number = il_number; cb.yajl_string = il_string;
  rc = parse_span(e, start, end, &cb, &x, &x.pos, NULL);
  if (!e->err && !e->out->err && rc != JSON2TOON_OK) e->err = rc;
  j2t_putc(e->out, '\n');
}

/* ----- tabular: "[N]{cols}:" then one row per object, in column order ----- */
typedef struct {
  span_pos pos; j2t_enc *e; unsigned level; int depth;
  int have_key; size_t koff, klen;             /* pending key in e->kl_buf */
} tab_ctx;
static int tab_value(tab_ctx *x, const char *v, size_t n, int vstr) {
  j2t_enc *e = x->e;
  size_t voff;
  if (!x->have_key) return 1;                  /* defensive */
  if (grow(&e->rv_buf, &e->rv_bufcap, e->rv_buflen + (n ? n : 1)) != 0) {
    e->err = JSON2TOON_ERR_MEMORY; return 0;
  }
  if (e->ncells == e->cellcap) {
    size_t nc = e->cellcap ? e->cellcap * 2 : 8;
    j2t_cell *np = (j2t_cell *)realloc(e->cells, nc * sizeof *np);
    if (!np) { e->err = JSON2TOON_ERR_MEMORY; return 0; }
    e->cells = np; e->cellcap = nc;
  }
  voff = e->rv_buflen;
  if (n) memcpy(e->rv_buf + voff, v, n);
  e->rv_buflen += n;
  e->cells[e->ncells].koff = x->koff; e->cells[e->ncells].klen = x->klen;
  e->cells[e->ncells].voff = voff; e->cells[e->ncells].vlen = n;
  e->cells[e->ncells].vstr = vstr;
  e->ncells++;
  x->have_key = 0;
  return 1;
}
static int tb_null(void *c) {
  tab_ctx *x = (tab_ctx *)c;
  if (x->e->err || x->e->out->err) return 0;
  return tab_value(x, "null", 4, 0);
}
static int tb_bool(void *c, int b) {
  tab_ctx *x = (tab_ctx *)c;
  if (x->e->err || x->e->out->err) return 0;
  return tab_value(x, b ? "true" : "false", b ? 4u : 5u, 0);
}
static int tb_number(void *c, const char *v, size_t n) {
  tab_ctx *x = (tab_ctx *)c;
  if (x->e->err || x->e->out->err) return 0;
  return tab_value(x, v, n, 0);
}
static int tb_string(void *c, const unsigned char *v, size_t n) {
  tab_ctx *x = (tab_ctx *)c;
  if (x->e->err || x->e->out->err) return 0;
  return tab_value(x, (const char *)v, n, 1);
}
static int tb_start_array(void *c) {
  tab_ctx *x = (tab_ctx *)c;
  if (x->depth == 0) { x->depth = 1; return 1; }   /* the array itself */
  x->e->err = JSON2TOON_ERR_PARSE; return 0;       /* defensive: non-primitive cell */
}
static int tb_end_array(void *c) { ((tab_ctx *)c)->depth--; return 1; }
static int tb_start_map(void *c) {
  tab_ctx *x = (tab_ctx *)c;
  if (x->depth == 1) {                             /* a row begins */
    x->e->kl_buflen = 0; x->e->rv_buflen = 0; x->e->ncells = 0; x->have_key = 0;
    x->depth = 2; return 1;
  }
  x->e->err = JSON2TOON_ERR_PARSE; return 0;       /* defensive: not primitive */
}
static int tb_map_key(void *c, const unsigned char *k, size_t n) {
  tab_ctx *x = (tab_ctx *)c;
  j2t_enc *e = x->e;
  if (e->err || e->out->err) return 0;
  if (x->depth != 2) return 1;
  if (grow(&e->kl_buf, &e->kl_bufcap, e->kl_buflen + (n ? n : 1)) != 0) {
    e->err = JSON2TOON_ERR_MEMORY; return 0;
  }
  x->koff = e->kl_buflen; x->klen = n;
  memcpy(e->kl_buf + e->kl_buflen, k, n);
  e->kl_buflen += n;
  x->have_key = 1;
  return 1;
}
static int tb_end_map(void *c) {
  tab_ctx *x = (tab_ctx *)c;
  j2t_enc *e = x->e;
  size_t t;
  x->depth = 1;                                    /* a row ends */
  if (e->err || e->out->err) return 0;
  j2t_indent(e->out, x->level + 1);
  for (t = 0; t < e->tpl_n; t++) {
    size_t i;
    const char *tk = e->tpl_buf + e->tpl[t].off;
    size_t tl = e->tpl[t].len;
    if (t) j2t_putc(e->out, DELIM);
    for (i = 0; i < e->ncells; i++)
      if (e->cells[i].klen == tl &&
          memcmp(e->kl_buf + e->cells[i].koff, tk, tl) == 0) {
        const char *v = e->rv_buf + e->cells[i].voff;
        if (e->cells[i].vstr) j2t_emit_string(e->out, v, e->cells[i].vlen, DELIM);
        else j2t_write(e->out, v, e->cells[i].vlen);
        break;
      }
  }
  j2t_putc(e->out, '\n');
  return 1;
}
static void emit_tabular(j2t_enc *e, uint64_t start, uint64_t end,
                         unsigned level, size_t count) {
  tab_ctx x;
  yajl_callbacks cb;
  size_t t;
  int rc;
  memset(&x, 0, sizeof x); x.e = e; x.level = level;
  j2t_putc(e->out, '[');
  j2t_emit_count(e->out, count);
  j2t_puts(e->out, "]{");
  for (t = 0; t < e->tpl_n; t++) {
    if (t) j2t_putc(e->out, DELIM);
    j2t_emit_key(e->out, e->tpl_buf + e->tpl[t].off, e->tpl[t].len);
  }
  j2t_puts(e->out, "}:");
  j2t_putc(e->out, '\n');
  memset(&cb, 0, sizeof cb);
  cb.yajl_null = tb_null; cb.yajl_boolean = tb_bool; cb.yajl_number = tb_number;
  cb.yajl_string = tb_string;
  cb.yajl_start_map = tb_start_map; cb.yajl_map_key = tb_map_key;
  cb.yajl_end_map = tb_end_map;
  cb.yajl_start_array = tb_start_array; cb.yajl_end_array = tb_end_array;
  rc = parse_span(e, start, end, &cb, &x, &x.pos, NULL);
  if (!e->err && !e->out->err && rc != JSON2TOON_OK) e->err = rc;
}

/* ----- list: "[N]:" then "- item" per element at level+1 ----- */
typedef struct {
  span_pos pos; j2t_enc *e; unsigned level; int depth;
  uint64_t cstart; int ckind;                  /* pending container child */
  int child_empty;                             /* for object items */
} list_ctx;
static int ls_item_scalar(list_ctx *x) {
  if (x->e->err || x->e->out->err) return 0;
  if (x->depth != 1) return 1;                 /* deeper scalar: emitted by recursion */
  j2t_indent(x->e->out, x->level + 1);
  j2t_puts(x->e->out, "- ");
  return 1;
}
static int ll_null(void *c) {
  list_ctx *x = (list_ctx *)c;
  if (x->depth != 1) return 1;
  if (!ls_item_scalar(x)) return 0;
  j2t_puts(x->e->out, "null"); j2t_putc(x->e->out, '\n'); return 1;
}
static int ll_bool(void *c, int b) {
  list_ctx *x = (list_ctx *)c;
  if (x->depth != 1) return 1;
  if (!ls_item_scalar(x)) return 0;
  j2t_puts(x->e->out, b ? "true" : "false"); j2t_putc(x->e->out, '\n'); return 1;
}
static int ll_number(void *c, const char *v, size_t n) {
  list_ctx *x = (list_ctx *)c;
  if (x->depth != 1) return 1;
  if (!ls_item_scalar(x)) return 0;
  j2t_write(x->e->out, v, n); j2t_putc(x->e->out, '\n'); return 1;
}
static int ll_string(void *c, const unsigned char *v, size_t n) {
  list_ctx *x = (list_ctx *)c;
  if (x->depth != 1) return 1;
  if (!ls_item_scalar(x)) return 0;
  j2t_emit_string(x->e->out, (const char *)v, n, DELIM);
  j2t_putc(x->e->out, '\n'); return 1;
}
static int ll_start_map(void *c) {
  list_ctx *x = (list_ctx *)c;
  if (x->depth == 1) { x->cstart = span_off(&x->pos) - 1; x->ckind = '{'; x->child_empty = 1; }
  x->depth++;
  return 1;
}
static int ll_start_array(void *c) {
  list_ctx *x = (list_ctx *)c;
  if (x->depth == 0) { x->depth = 1; return 1; } /* the array itself */
  if (x->depth == 1) { x->cstart = span_off(&x->pos) - 1; x->ckind = '['; }
  x->depth++;
  return 1;
}
static int ll_map_key(void *c, const unsigned char *k, size_t n) {
  list_ctx *x = (list_ctx *)c;
  (void)k; (void)n;
  if (x->depth == 2) x->child_empty = 0;        /* direct child object has a member */
  return 1;
}
static int ll_end_array(void *c) {
  list_ctx *x = (list_ctx *)c;
  j2t_enc *e = x->e;
  x->depth--;
  if (x->depth == 0) return 1;                  /* end of the array itself */
  if (x->depth == 1) {                          /* a direct-child array closed */
    uint64_t cend = span_off(&x->pos);
    if (e->err || e->out->err) return 0;
    j2t_indent(e->out, x->level + 1);
    j2t_puts(e->out, "- ");
    {
      int kind;
      size_t count;
      int rc = shape_array(e, x->cstart, cend, &kind, &count, NULL);
      if (rc != JSON2TOON_OK) { if (!e->err) e->err = rc; return 0; }
      if (count == 0) { j2t_puts(e->out, "[]"); j2t_putc(e->out, '\n'); }
      else emit_array_dispatch(e, x->cstart, cend, x->level + 2, kind, count);
    }
    if (e->err || e->out->err) return 0;
  }
  return 1;
}
static int ll_end_map(void *c) {
  list_ctx *x = (list_ctx *)c;
  j2t_enc *e = x->e;
  x->depth--;
  if (x->depth == 1) {                          /* a direct-child object closed */
    uint64_t cend = span_off(&x->pos);
    if (e->err || e->out->err) return 0;
    if (x->child_empty) {
      j2t_indent(e->out, x->level + 1);
      j2t_putc(e->out, '-'); j2t_putc(e->out, '\n');
    } else {
      j2t_indent(e->out, x->level + 1);
      j2t_puts(e->out, "- ");
      emit_object_body(e, x->cstart, cend, x->level + 2, 1);
    }
    if (e->err || e->out->err) return 0;
  }
  return 1;
}
static void emit_list(j2t_enc *e, uint64_t start, uint64_t end,
                      unsigned level, size_t count) {
  list_ctx x;
  yajl_callbacks cb;
  int rc;
  memset(&x, 0, sizeof x); x.e = e; x.level = level;
  j2t_putc(e->out, '[');
  j2t_emit_count(e->out, count);
  j2t_puts(e->out, "]:");
  j2t_putc(e->out, '\n');
  memset(&cb, 0, sizeof cb);
  cb.yajl_null = ll_null; cb.yajl_boolean = ll_bool; cb.yajl_number = ll_number;
  cb.yajl_string = ll_string;
  cb.yajl_start_map = ll_start_map; cb.yajl_map_key = ll_map_key;
  cb.yajl_end_map = ll_end_map;
  cb.yajl_start_array = ll_start_array; cb.yajl_end_array = ll_end_array;
  rc = parse_span(e, start, end, &cb, &x, &x.pos, NULL);
  if (!e->err && !e->out->err && rc != JSON2TOON_OK) e->err = rc;
}

/* ----- object: "key: value" / "key:" + nested, one member per line ----- */
typedef struct {
  span_pos pos; j2t_enc *e; unsigned level; int first_inline; int depth;
  size_t idx; int awaiting_value;
  uint64_t cstart;                             /* pending container value */
} obj_ctx;
/* Write a member's line lead + decoded key; the value follows. */
static void ob_key(obj_ctx *x, const unsigned char *k, size_t n) {
  if (!(x->idx == 0 && x->first_inline))
    j2t_indent(x->e->out, x->level);
  j2t_emit_key(x->e->out, (const char *)k, n);
  x->idx++;
  x->awaiting_value = 1;
}
static int ob_map_key(void *c, const unsigned char *k, size_t n) {
  obj_ctx *x = (obj_ctx *)c;
  if (x->e->err || x->e->out->err) return 0;
  if (x->depth == 1) ob_key(x, k, n);          /* a member key of this object */
  return 1;
}
static int ob_scalar_lead(obj_ctx *x) {        /* "key: " for a scalar value */
  if (x->e->err || x->e->out->err) return 0;
  if (x->depth != 1 || !x->awaiting_value) return 1;
  j2t_puts(x->e->out, ": ");
  x->awaiting_value = 0;
  return 2;                                    /* 2 == proceed to emit value */
}
static int ob_null(void *c) {
  obj_ctx *x = (obj_ctx *)c;
  int s = ob_scalar_lead(x);
  if (s == 0) return 0;
  if (s == 2) { j2t_puts(x->e->out, "null"); j2t_putc(x->e->out, '\n'); }
  return 1;
}
static int ob_bool(void *c, int b) {
  obj_ctx *x = (obj_ctx *)c;
  int s = ob_scalar_lead(x);
  if (s == 0) return 0;
  if (s == 2) { j2t_puts(x->e->out, b ? "true" : "false"); j2t_putc(x->e->out, '\n'); }
  return 1;
}
static int ob_number(void *c, const char *v, size_t n) {
  obj_ctx *x = (obj_ctx *)c;
  int s = ob_scalar_lead(x);
  if (s == 0) return 0;
  if (s == 2) { j2t_write(x->e->out, v, n); j2t_putc(x->e->out, '\n'); }
  return 1;
}
static int ob_string(void *c, const unsigned char *v, size_t n) {
  obj_ctx *x = (obj_ctx *)c;
  int s = ob_scalar_lead(x);
  if (s == 0) return 0;
  if (s == 2) { j2t_emit_string(x->e->out, (const char *)v, n, DELIM); j2t_putc(x->e->out, '\n'); }
  return 1;
}
static int ob_start_map(void *c) {
  obj_ctx *x = (obj_ctx *)c;
  if (x->depth == 0) { x->depth = 1; return 1; }   /* the object itself */
  if (x->depth == 1 && x->awaiting_value) {        /* member value: object */
    if (x->e->err || x->e->out->err) return 0;
    j2t_putc(x->e->out, ':'); j2t_putc(x->e->out, '\n');
    x->cstart = span_off(&x->pos) - 1;
    x->awaiting_value = 0;
  }
  x->depth++;
  return 1;
}
static int ob_start_array(void *c) {
  obj_ctx *x = (obj_ctx *)c;
  if (x->depth == 1 && x->awaiting_value) {        /* member value: array */
    x->cstart = span_off(&x->pos) - 1;
    x->awaiting_value = 0;                          /* emitted at end_array */
  }
  x->depth++;
  return 1;
}
static int ob_end_array(void *c) {
  obj_ctx *x = (obj_ctx *)c;
  j2t_enc *e = x->e;
  x->depth--;
  if (x->depth == 1) {                              /* a member array value closed */
    uint64_t cend = span_off(&x->pos);
    int kind;
    size_t count;
    int rc;
    if (e->err || e->out->err) return 0;
    rc = shape_array(e, x->cstart, cend, &kind, &count, NULL);
    if (rc != JSON2TOON_OK) { if (!e->err) e->err = rc; return 0; }
    if (count == 0) { j2t_puts(e->out, ": []"); j2t_putc(e->out, '\n'); }
    else emit_array_dispatch(e, x->cstart, cend, x->level, kind, count);
    if (e->err || e->out->err) return 0;
  }
  return 1;
}
static int ob_end_map(void *c) {
  obj_ctx *x = (obj_ctx *)c;
  j2t_enc *e = x->e;
  x->depth--;
  if (x->depth == 1) {                              /* a member object value closed */
    uint64_t cend = span_off(&x->pos);
    if (e->err || e->out->err) return 0;
    emit_object_body(e, x->cstart, cend, x->level + 1, 0);
    if (e->err || e->out->err) return 0;
  }
  return 1;                                         /* depth==0: object itself done */
}
static void emit_object_body(j2t_enc *e, uint64_t start, uint64_t end,
                             unsigned level, int first_inline) {
  obj_ctx x;
  yajl_callbacks cb;
  int rc;
  if (e->err || e->out->err) return;
  if (e->depth >= e->max_depth) { e->err = JSON2TOON_ERR_DEPTH; return; }
  e->depth++;
  memset(&x, 0, sizeof x); x.e = e; x.level = level; x.first_inline = first_inline;
  memset(&cb, 0, sizeof cb);
  cb.yajl_null = ob_null; cb.yajl_boolean = ob_bool; cb.yajl_number = ob_number;
  cb.yajl_string = ob_string;
  cb.yajl_start_map = ob_start_map; cb.yajl_map_key = ob_map_key;
  cb.yajl_end_map = ob_end_map;
  cb.yajl_start_array = ob_start_array; cb.yajl_end_array = ob_end_array;
  rc = parse_span(e, start, end, &cb, &x, &x.pos, NULL);
  if (!e->err && !e->out->err && rc != JSON2TOON_OK) e->err = rc;
  e->depth--;
}

/* Dispatch a non-empty array level to its kind's emitter (tabular reads the
 * template left by the caller's shape; tabular never recurses, so it stays valid). */
static void emit_array_dispatch(j2t_enc *e, uint64_t start, uint64_t end,
                                unsigned level, int kind, size_t count) {
  if (e->err || e->out->err) return;
  if (e->depth >= e->max_depth) { e->err = JSON2TOON_ERR_DEPTH; return; }
  e->depth++;
  switch (kind) {
    case ARR_INLINE: emit_inline(e, start, end, count); break;
    case ARR_TABULAR: emit_tabular(e, start, end, level, count); break;
    default: emit_list(e, start, end, level, count); break;   /* ARR_LIST */
  }
  e->depth--;
}

/* ------------------------------------------------------------- public API */

int j2t_encode_captured(j2t_out *o, j2t_store *s, unsigned level,
                        const char *key, size_t keylen,
                        unsigned max_depth, uint64_t *errpos) {
  j2t_enc e;
  int kind, rc;
  size_t count;
  memset(&e, 0, sizeof e);
  e.out = o;
  e.s = s;
  e.max_depth = max_depth ? max_depth : 128;

  /* The top-level shape over the whole store validates the document, bounds
   * depth (sets *errpos on failure) and classifies the root array. */
  rc = shape_array(&e, 0, s->total, &kind, &count, errpos);
  if (rc != JSON2TOON_OK) {
    enc_free(&e);
    return rc;
  }

  j2t_indent(o, level);
  if (count == 0) {                          /* empty root array */
    if (key) { j2t_emit_key(o, key, keylen); j2t_puts(o, ": []"); }
    else j2t_puts(o, "[]");
    j2t_putc(o, '\n');
  } else {
    if (key) j2t_emit_key(o, key, keylen);
    emit_array_dispatch(&e, 0, s->total, level, kind, count);
  }

  rc = e.err ? e.err : o->err;
  if (rc == JSON2TOON_OK && s->err != JSON2TOON_OK)
    rc = s->err;
  enc_free(&e);
  return rc;
}
