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
 * The list/object emitters share one generic level-walker (walk_level) that
 * surfaces each direct child as a typed ch_info; inline/tabular/shape keep
 * bespoke single-pass callbacks (the hot, structural paths).
 * Peak memory = column template + one tabular row + O(max_depth) parse handles,
 * regardless of array length. Output is identical whether bytes stayed in RAM
 * or spilled.
 *
 * Perf note (3c): a spilled array's bytes are re-read once per pass -- the
 * top-level shape (which also validates) and the emit pass -- and each nested
 * array/object level is re-parsed when it is shaped and emitted. Folding the
 * standalone validate pass into the top-level shape (#1) removed one full re-read.
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

  j2t_buf tpl_buf;                          /* tabular column key bytes */
  j2t_col *tpl; size_t tpl_n, tpl_cap;      /* tabular columns */

  j2t_buf kl_buf;                           /* shape: an object's keys; tabular: row keys */
  j2t_col *kl; size_t kl_n, kl_cap;         /* shape: that object's key offsets */

  j2t_buf rv_buf;                           /* tabular: one row's value bytes */
  j2t_cell *cells; size_t ncells, cellcap;  /* tabular: one row's cells */

  j2t_buf wk_buf;                           /* level-walker: the current member key */
} j2t_enc;

static void enc_free(j2t_enc *e) {
  j2t_buf_free(&e->tpl_buf);
  free(e->tpl);
  j2t_buf_free(&e->kl_buf);
  free(e->kl);
  j2t_buf_free(&e->rv_buf);
  free(e->cells);
  j2t_buf_free(&e->wk_buf);
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
    st = yajl_parse(h, (const unsigned char *)s->ram.p + start,
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

/* Emit one scalar child (from a ch_info/cell): strings get TOON quoting, every
 * other scalar is written as its already-emit-ready bytes (raw number / literal). */
static void emit_scalar(j2t_out *o, int is_string, const char *v, size_t n) {
  if (is_string) j2t_emit_string(o, v, n, DELIM);
  else j2t_write(o, v, n);
}

/* ----------------------------------------------- template / row scratch */

static int col_find_tpl(const j2t_enc *e, const char *k, size_t kl) {
  size_t i;
  for (i = 0; i < e->tpl_n; i++)
    if (e->tpl[i].len == kl &&
        memcmp(e->tpl_buf.p + e->tpl[i].off, k, kl) == 0)
      return (int)i;
  return -1;
}

/* Append a decoded key to the current-object key list (used during shape). */
static int kl_push(j2t_enc *e, const unsigned char *k, size_t kl) {
  size_t off = e->kl_buf.len;
  if (j2t_buf_append(&e->kl_buf, (const char *)k, kl) != 0)
    return -1;
  if (e->kl_n == e->kl_cap) {
    size_t nc = e->kl_cap ? e->kl_cap * 2 : 8;
    j2t_col *np = (j2t_col *)realloc(e->kl, nc * sizeof *np);
    if (!np) return -1;
    e->kl = np;
    e->kl_cap = nc;
  }
  e->kl[e->kl_n].off = off;
  e->kl[e->kl_n].len = kl;
  e->kl_n++;
  return 0;
}

/* Snapshot the current key list as the column template; flags a duplicate key. */
static int build_tpl(j2t_enc *e, int *dup) {
  size_t i;
  e->tpl_n = 0;
  e->tpl_buf.len = 0;
  *dup = 0;
  for (i = 0; i < e->kl_n; i++) {
    const char *k = e->kl_buf.p + e->kl[i].off;
    size_t kl = e->kl[i].len;
    size_t off = e->tpl_buf.len;
    if (col_find_tpl(e, k, kl) >= 0)
      *dup = 1;
    if (j2t_buf_append(&e->tpl_buf, k, kl) != 0)
      return -1;
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

/* True if the current key list is exactly the template's key set (same count,
 * no duplicates, every key present) -- order may differ, per TOON spec 9.3. */
static int kl_matches_tpl(const j2t_enc *e) {
  size_t i, j;
  if (e->kl_n != e->tpl_n)
    return 0;
  for (i = 0; i < e->kl_n; i++) {
    const char *k = e->kl_buf.p + e->kl[i].off;
    size_t kl = e->kl[i].len;
    for (j = 0; j < i; j++)
      if (e->kl[j].len == kl &&
          memcmp(e->kl_buf.p + e->kl[j].off, k, kl) == 0)
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
    x->e->kl_n = 0; x->e->kl_buf.len = 0;      /* collect its keys (dup check + template) */
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
  /* Keys of a direct-child object. TOON keys are unique, so reject a duplicate
   * (4a) -- bounded: only one object's keys are held. Dups in object-VALUED
   * members (depth >= 3 here) and in the streaming top-level object are not seen
   * by this bounded path and pass through; see json2toon.c. */
  if (x->depth == 2) {
    size_t i;
    for (i = 0; i < x->e->kl_n; i++)
      if (x->e->kl[i].len == n &&
          memcmp(x->e->kl_buf.p + x->e->kl[i].off, k, n) == 0) {
        return 0;                              /* duplicate key -> ERR_PARSE */
      }
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

/* Shape then emit an array reached as a child: `empty_str` is what to print in
 * place of the "[N]..." header when the array is empty (list item: "[]";
 * object member: ": []"). */
static void emit_array_at(j2t_enc *e, uint64_t start, uint64_t end,
                          unsigned level, const char *empty_str) {
  int kind;
  size_t count;
  int rc;
  if (e->err || e->out->err) return;
  rc = shape_array(e, start, end, &kind, &count, NULL);
  if (rc != JSON2TOON_OK) { if (!e->err) e->err = rc; return; }
  if (count == 0) {
    j2t_puts(e->out, empty_str);
    j2t_putc(e->out, '\n');
  } else {
    emit_array_dispatch(e, start, end, level, kind, count);
  }
}

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
  voff = e->rv_buf.len;
  if (j2t_buf_append(&e->rv_buf, v, n) != 0) { e->err = JSON2TOON_ERR_MEMORY; return 0; }
  if (e->ncells == e->cellcap) {
    size_t nc = e->cellcap ? e->cellcap * 2 : 8;
    j2t_cell *np = (j2t_cell *)realloc(e->cells, nc * sizeof *np);
    if (!np) { e->err = JSON2TOON_ERR_MEMORY; return 0; }
    e->cells = np; e->cellcap = nc;
  }
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
    x->e->kl_buf.len = 0; x->e->rv_buf.len = 0; x->e->ncells = 0; x->have_key = 0;
    x->depth = 2; return 1;
  }
  x->e->err = JSON2TOON_ERR_PARSE; return 0;       /* defensive: not primitive */
}
static int tb_map_key(void *c, const unsigned char *k, size_t n) {
  tab_ctx *x = (tab_ctx *)c;
  j2t_enc *e = x->e;
  if (e->err || e->out->err) return 0;
  if (x->depth != 2) return 1;
  x->koff = e->kl_buf.len; x->klen = n;
  if (j2t_buf_append(&e->kl_buf, (const char *)k, n) != 0) {
    e->err = JSON2TOON_ERR_MEMORY; return 0;
  }
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
    const char *tk = e->tpl_buf.p + e->tpl[t].off;
    size_t tl = e->tpl[t].len;
    if (t) j2t_putc(e->out, DELIM);
    for (i = 0; i < e->ncells; i++)
      if (e->cells[i].klen == tl &&
          memcmp(e->kl_buf.p + e->cells[i].koff, tk, tl) == 0) {
        emit_scalar(e->out, e->cells[i].vstr, e->rv_buf.p + e->cells[i].voff,
                    e->cells[i].vlen);
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
    j2t_emit_key(e->out, e->tpl_buf.p + e->tpl[t].off, e->tpl[t].len);
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

/* --------------------------------- generic per-level child walker --------- */

/* One direct child of an array/object level, surfaced by walk_level. For a
 * scalar, `val`/`vlen` are emit-ready bytes (decoded string / raw number /
 * literal). For a container, [start,end) is its byte span and `empty` says
 * whether it has any direct child. `key`/`klen` is set for object members. */
typedef enum { CH_NULL, CH_BOOL, CH_NUM, CH_STR, CH_ARR, CH_OBJ } ch_kind;
typedef struct {
  ch_kind kind;
  const char *key; size_t klen;
  const char *val; size_t vlen;
  uint64_t start, end;
  int empty;
} ch_info;
typedef int (*ch_fn)(void *ctx, const ch_info *ci);   /* return 0 to cancel */

typedef struct {
  span_pos pos; j2t_enc *e; int is_object;
  ch_fn visit; void *vctx;
  int depth;
  int have_key;                                /* member key buffered in e->wk_buf */
  uint64_t cstart; ch_kind ckind; int cempty;  /* pending depth-1 container child */
} walk_ctx;

static int wl_scalar(walk_ctx *w, ch_kind k, const char *v, size_t n) {
  ch_info ci;
  if (w->depth == 2) w->cempty = 0;            /* the pending child has content */
  if (w->depth != 1) return 1;                 /* deeper scalar: handled by recursion */
  if (w->e->err || w->e->out->err) return 0;
  ci.kind = k;
  ci.key = w->is_object ? w->e->wk_buf.p : NULL;
  ci.klen = w->is_object ? w->e->wk_buf.len : 0;
  ci.val = v; ci.vlen = n;
  ci.start = ci.end = 0; ci.empty = 0;
  w->have_key = 0;
  return w->visit(w->vctx, &ci);
}
static int wl_null(void *c) { return wl_scalar((walk_ctx *)c, CH_NULL, "null", 4); }
static int wl_bool(void *c, int b) {
  return wl_scalar((walk_ctx *)c, CH_BOOL, b ? "true" : "false", b ? 4u : 5u);
}
static int wl_number(void *c, const char *v, size_t n) {
  return wl_scalar((walk_ctx *)c, CH_NUM, v, n);
}
static int wl_string(void *c, const unsigned char *v, size_t n) {
  return wl_scalar((walk_ctx *)c, CH_STR, (const char *)v, n);
}
static int wl_map_key(void *c, const unsigned char *k, size_t n) {
  walk_ctx *w = (walk_ctx *)c;
  if (w->depth == 2) w->cempty = 0;
  if (w->depth != 1 || !w->is_object) return 1;
  w->e->wk_buf.len = 0;
  if (j2t_buf_append(&w->e->wk_buf, (const char *)k, n) != 0) {
    w->e->err = JSON2TOON_ERR_MEMORY; return 0;
  }
  w->have_key = 1;
  return 1;
}
static int wl_start_map(void *c) {
  walk_ctx *w = (walk_ctx *)c;
  if (w->depth == 2) w->cempty = 0;
  if (w->depth == 0) { w->depth = 1; return 1; }   /* the object level itself */
  if (w->depth == 1) { w->cstart = span_off(&w->pos) - 1; w->ckind = CH_OBJ; w->cempty = 1; }
  w->depth++;
  return 1;
}
static int wl_start_array(void *c) {
  walk_ctx *w = (walk_ctx *)c;
  if (w->depth == 2) w->cempty = 0;
  if (w->depth == 0) { w->depth = 1; return 1; }   /* the array level itself */
  if (w->depth == 1) { w->cstart = span_off(&w->pos) - 1; w->ckind = CH_ARR; w->cempty = 1; }
  w->depth++;
  return 1;
}
static int wl_container(walk_ctx *w) {             /* a depth-1 container child closed */
  ch_info ci;
  if (w->e->err || w->e->out->err) return 0;
  ci.kind = w->ckind;
  ci.key = w->is_object ? w->e->wk_buf.p : NULL;
  ci.klen = w->is_object ? w->e->wk_buf.len : 0;
  ci.val = NULL; ci.vlen = 0;
  ci.start = w->cstart; ci.end = span_off(&w->pos);
  ci.empty = w->cempty;
  w->have_key = 0;
  return w->visit(w->vctx, &ci);
}
static int wl_end_map(void *c) {
  walk_ctx *w = (walk_ctx *)c;
  w->depth--;
  if (w->depth == 1) return wl_container(w);
  return 1;
}
static int wl_end_array(void *c) {
  walk_ctx *w = (walk_ctx *)c;
  w->depth--;
  if (w->depth == 1) return wl_container(w);
  return 1;
}

/* Walk the direct children of the array/object at [start,end), invoking `visit`
 * per child. Containers are surfaced at their close (span known); the visitor
 * recurses on the span. is_object selects whether member keys are reported.
 * Note: ci->key aliases e->wk_buf, which a nested walk reuses -- visitors must
 * consume the key before recursing (all do: they emit it first). */
static void walk_level(j2t_enc *e, uint64_t start, uint64_t end, int is_object,
                       ch_fn visit, void *vctx) {
  walk_ctx w;
  yajl_callbacks cb;
  int rc;
  memset(&w, 0, sizeof w);
  w.e = e; w.is_object = is_object; w.visit = visit; w.vctx = vctx;
  memset(&cb, 0, sizeof cb);
  cb.yajl_null = wl_null; cb.yajl_boolean = wl_bool; cb.yajl_number = wl_number;
  cb.yajl_string = wl_string;
  cb.yajl_start_map = wl_start_map; cb.yajl_map_key = wl_map_key;
  cb.yajl_end_map = wl_end_map;
  cb.yajl_start_array = wl_start_array; cb.yajl_end_array = wl_end_array;
  rc = parse_span(e, start, end, &cb, &w, &w.pos, NULL);
  if (!e->err && !e->out->err && rc != JSON2TOON_OK) e->err = rc;
}

/* ----- list: "[N]:" then "- item" per element at level+1 ----- */
typedef struct { j2t_enc *e; unsigned level; } list_vctx;
static int v_list(void *ctx, const ch_info *ci) {
  list_vctx *x = (list_vctx *)ctx;
  j2t_enc *e = x->e;
  unsigned il = x->level + 1;
  if (e->err || e->out->err) return 0;
  if (ci->kind == CH_OBJ) {
    j2t_indent(e->out, il);
    if (ci->empty) { j2t_putc(e->out, '-'); j2t_putc(e->out, '\n'); }
    else { j2t_puts(e->out, "- "); emit_object_body(e, ci->start, ci->end, il + 1, 1); }
  } else if (ci->kind == CH_ARR) {
    j2t_indent(e->out, il); j2t_puts(e->out, "- ");
    emit_array_at(e, ci->start, ci->end, il + 1, "[]");
  } else {
    j2t_indent(e->out, il); j2t_puts(e->out, "- ");
    emit_scalar(e->out, ci->kind == CH_STR, ci->val, ci->vlen);
    j2t_putc(e->out, '\n');
  }
  return (e->err || e->out->err) ? 0 : 1;
}
static void emit_list(j2t_enc *e, uint64_t start, uint64_t end,
                      unsigned level, size_t count) {
  list_vctx vc;
  j2t_putc(e->out, '[');
  j2t_emit_count(e->out, count);
  j2t_puts(e->out, "]:");
  j2t_putc(e->out, '\n');
  vc.e = e; vc.level = level;
  walk_level(e, start, end, 0, v_list, &vc);
}

/* ----- object: "key: value" / "key:" + nested, one member per line ----- */
typedef struct { j2t_enc *e; unsigned level; int first_inline; size_t idx; } obj_vctx;
static int v_object(void *ctx, const ch_info *ci) {
  obj_vctx *x = (obj_vctx *)ctx;
  j2t_enc *e = x->e;
  if (e->err || e->out->err) return 0;
  if (!(x->idx == 0 && x->first_inline))
    j2t_indent(e->out, x->level);
  j2t_emit_key(e->out, ci->key, ci->klen);
  x->idx++;
  if (ci->kind == CH_OBJ) {
    j2t_putc(e->out, ':'); j2t_putc(e->out, '\n');
    emit_object_body(e, ci->start, ci->end, x->level + 1, 0);
  } else if (ci->kind == CH_ARR) {
    emit_array_at(e, ci->start, ci->end, x->level, ": []");
  } else {
    j2t_puts(e->out, ": ");
    emit_scalar(e->out, ci->kind == CH_STR, ci->val, ci->vlen);
    j2t_putc(e->out, '\n');
  }
  return (e->err || e->out->err) ? 0 : 1;
}
static void emit_object_body(j2t_enc *e, uint64_t start, uint64_t end,
                             unsigned level, int first_inline) {
  obj_vctx vc;
  if (e->err || e->out->err) return;
  if (e->depth >= e->max_depth) { e->err = JSON2TOON_ERR_DEPTH; return; }
  e->depth++;
  vc.e = e; vc.level = level; vc.first_inline = first_inline; vc.idx = 0;
  walk_level(e, start, end, 1, v_object, &vc);
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
