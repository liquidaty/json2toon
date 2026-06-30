/* json2toon - streaming push parser and public API.
 *
 * Objects and root-level primitives are emitted incrementally by an explicit
 * pushdown state machine, so a deeply/widely nested object document converts in
 * memory bounded by its nesting depth, not its length. Arrays are the one
 * construct TOON cannot emit without buffering (the "[N]" count and tabular
 * column set are only knowable after the whole array is seen); each array's raw
 * text is captured into a backing store (store.c, which spills to a temp file
 * past a RAM threshold), validated and encoded by streaming over that store
 * (encode_array.c), then released. Peak memory is bounded by a fixed lookahead
 * window plus a shallow parse stack -- not by array length.
 */
#include "internal.h"
#include "unicode.h"

#include <stdlib.h>
#include <string.h>

#define DELIM ','

/* Parser states. */
enum {
  ST_VALUE,          /* expecting a value (root or object field) */
  ST_OBJ_OPEN,       /* just after '{': expect key or '}' */
  ST_OBJ_KEY,        /* after ',': expect a key string */
  ST_OBJ_COLON,      /* expect ':' */
  ST_OBJ_NEXT,       /* after a member value: expect ',' or '}' */
  ST_STRING,         /* lexing a string token */
  ST_NUMBER,         /* lexing a number token */
  ST_LITERAL,        /* lexing true/false/null */
  ST_ARR_CAP,        /* capturing raw array bytes */
  ST_DONE,           /* root value complete: only trailing whitespace allowed */
  ST_ERR
};

/* String lexer sub-states. */
enum { SS_NORMAL, SS_ESCAPE, SS_U };

/* Scalar kinds for emit_value_line(). */
enum { K_STR, K_NUM, K_TRUE, K_FALSE, K_NULL };

struct json2toon {
  j2t_out out;
  json2toon_options opt;

  int state;
  int err;                 /* sticky JSON2TOON_* */
  size_t stream_offset;    /* bytes consumed in completed feeds */
  size_t err_offset;

  size_t depth;            /* number of open objects (the parse stack) */

  /* current object key, awaiting its value */
  j2t_buf key;
  int has_key;

  /* generic token buffer (string value / key text / number lexeme) */
  j2t_buf tok;

  /* string lexer sub-state */
  int sstate;
  int str_is_key;
  int u_count; unsigned u_val; unsigned u_high;

  /* literal lexer */
  const char *lit_expect; int lit_pos; int lit_kind;

  /* array capture: raw bytes go to `store` (RAM, spilling to a temp file past a
   * threshold); the rest tracks bracket/string nesting to find the array end. */
  j2t_store store;
  int cap_depth, cap_in_str, cap_escape;
  int cap_has_key;
  unsigned cap_level;
  size_t cap_start_off;
};

/* Convenience alias so the internal code can write `json2toon` for the struct
 * (the public header only exposes the `json2toon_t` typedef). */
typedef struct json2toon json2toon;

/* --------------------------------------------------------------- utilities */

static void set_err(json2toon *j, int code, size_t off) {
  if (j->err == JSON2TOON_OK) {
    j->err = code;
    j->err_offset = off;
  }
  j->state = ST_ERR;
}

static int utf8_emit(json2toon *j, unsigned cp) {
  char b[4];
  int n = j2t_utf8_encode(cp, b);
  return j2t_buf_append(&j->tok, b, (size_t)n);
}

/* Return to the appropriate state after a value has been fully emitted. */
static void finish_value(json2toon *j) {
  j->has_key = 0;
  j->state = (j->depth > 0) ? ST_OBJ_NEXT : ST_DONE;
}

static void emit_value_line(json2toon *j, int kind) {
  unsigned level = j->has_key ? (unsigned)(j->depth - 1) : 0;
  j2t_indent(&j->out, level);
  if (j->has_key) {
    j2t_emit_key(&j->out, j->key.p, j->key.len);
    j2t_puts(&j->out, ": ");
  }
  switch (kind) {
    case K_STR: j2t_emit_string(&j->out, j->tok.p, j->tok.len, DELIM); break;
    case K_NUM: j2t_write(&j->out, j->tok.p, j->tok.len); break;
    case K_TRUE: j2t_puts(&j->out, "true"); break;
    case K_FALSE: j2t_puts(&j->out, "false"); break;
    case K_NULL: j2t_puts(&j->out, "null"); break;
  }
  j2t_putc(&j->out, '\n');
  finish_value(j);
}

/* ----------------------------------------------------------- array capture */

/* Append a run of captured bytes to the store, enforcing max_array_bytes and
 * mapping store errors. Returns 0, or -1 and sets the sticky error at `off`. */
static int cap_append(json2toon *j, const char *p, size_t n, size_t off) {
  if (n == 0)
    return 0;
  if (j->opt.max_array_bytes &&
      j2t_store_size(&j->store) + n > j->opt.max_array_bytes) {
    set_err(j, JSON2TOON_ERR_LIMIT, off);
    return -1;
  }
  if (j2t_store_append(&j->store, p, n) != 0) {
    set_err(j, j->store.err, off);
    return -1;
  }
  return 0;
}

static void capture_done(json2toon *j) {
  uint64_t errpos = 0;
  int rc = j2t_encode_captured(&j->out, &j->store, j->cap_level,
                               j->cap_has_key ? j->key.p : NULL,
                               j->cap_has_key ? j->key.len : 0,
                               j->opt.max_depth, &errpos);
  /* Free the captured bytes (and any temp file) on every path. */
  j2t_store_reset(&j->store);
  if (rc != JSON2TOON_OK) {
    size_t off = (rc == JSON2TOON_ERR_PARSE || rc == JSON2TOON_ERR_DEPTH)
                     ? j->cap_start_off + (size_t)errpos
                     : j->cap_start_off;
    set_err(j, rc, off);
    return;
  }
  finish_value(j);
}

/* ------------------------------------------------------------------- string */

/* Process string bytes starting at *pp (< end). Advances *pp. May complete the
 * string token, transitioning state and emitting output. */
static void process_string(json2toon *j, const char **pp, const char *end,
                           size_t base) {
  const char *p = *pp;
  while (p < end) {
    if (j->sstate == SS_NORMAL && j->u_high == 0) {
      const char *run = j2t_scan_string(p, end);
      if (run > p) {
        if (j2t_buf_append(&j->tok, p, (size_t)(run - p)) != 0) {
          set_err(j, JSON2TOON_ERR_MEMORY, base + (size_t)(p - *pp));
          *pp = p; return;
        }
        p = run;
        if (p == end)
          break;
      }
      {
        unsigned char c = (unsigned char)*p;
        if (c == '"') {
          p++;
          /* string complete */
          if (j->str_is_key) {
            j->key.len = 0;
            if (j2t_buf_append(&j->key, j->tok.p, j->tok.len) != 0) {
              set_err(j, JSON2TOON_ERR_MEMORY, base); *pp = p; return;
            }
            j->has_key = 1;
            j->state = ST_OBJ_COLON;
          } else {
            emit_value_line(j, K_STR);
          }
          *pp = p;
          return;
        } else if (c == '\\') {
          p++;
          j->sstate = SS_ESCAPE;
        } else { /* control byte */
          set_err(j, JSON2TOON_ERR_PARSE, base + (size_t)(p - *pp));
          *pp = p; return;
        }
      }
    } else if (j->sstate == SS_NORMAL && j->u_high != 0) {
      /* a high surrogate must be followed immediately by \u */
      unsigned char c = (unsigned char)*p++;
      if (c != '\\') {
        set_err(j, JSON2TOON_ERR_PARSE, base + (size_t)(p - 1 - *pp));
        *pp = p; return;
      }
      j->sstate = SS_ESCAPE;
    } else if (j->sstate == SS_ESCAPE) {
      unsigned char c = (unsigned char)*p++;
      char out = 0;
      int ok = 1;
      if (c == 'u') {
        j->sstate = SS_U; j->u_count = 0; j->u_val = 0;
        continue;
      }
      if (j->u_high != 0) { ok = 0; }       /* expected \u after high surrogate */
      else switch (c) {
        case '"': out = '"'; break;
        case '\\': out = '\\'; break;
        case '/': out = '/'; break;
        case 'b': out = '\b'; break;
        case 'f': out = '\f'; break;
        case 'n': out = '\n'; break;
        case 'r': out = '\r'; break;
        case 't': out = '\t'; break;
        default: ok = 0; break;
      }
      if (!ok) {
        set_err(j, JSON2TOON_ERR_PARSE, base + (size_t)(p - 1 - *pp));
        *pp = p; return;
      }
      if (j2t_buf_putc(&j->tok, out) != 0) {
        set_err(j, JSON2TOON_ERR_MEMORY, base); *pp = p; return;
      }
      j->sstate = SS_NORMAL;
    } else { /* SS_U: collecting 4 hex digits */
      unsigned char c = (unsigned char)*p++;
      unsigned hv;
      if (c >= '0' && c <= '9') hv = (unsigned)(c - '0');
      else if (c >= 'a' && c <= 'f') hv = (unsigned)(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') hv = (unsigned)(c - 'A' + 10);
      else {
        set_err(j, JSON2TOON_ERR_PARSE, base + (size_t)(p - 1 - *pp));
        *pp = p; return;
      }
      j->u_val = (j->u_val << 4) | hv;
      if (++j->u_count == 4) {
        unsigned cp = j->u_val;
        j->sstate = SS_NORMAL;
        if (j->u_high != 0) {
          if (cp >= 0xdc00 && cp <= 0xdfff) {
            unsigned comb = 0x10000u +
                (((j->u_high - 0xd800u) << 10) | (cp - 0xdc00u));
            j->u_high = 0;
            if (utf8_emit(j, comb) != 0) {
              set_err(j, JSON2TOON_ERR_MEMORY, base); *pp = p; return;
            }
          } else {
            set_err(j, JSON2TOON_ERR_PARSE, base + (size_t)(p - *pp));
            *pp = p; return;
          }
        } else if (cp >= 0xd800 && cp <= 0xdbff) {
          j->u_high = cp;                   /* await low surrogate */
        } else if (cp >= 0xdc00 && cp <= 0xdfff) {
          set_err(j, JSON2TOON_ERR_PARSE, base + (size_t)(p - *pp));
          *pp = p; return;
        } else {
          if (utf8_emit(j, cp) != 0) {
            set_err(j, JSON2TOON_ERR_MEMORY, base); *pp = p; return;
          }
        }
      }
    }
  }
  *pp = p;
}

/* ------------------------------------------------------------- value start */

static void start_string(json2toon *j, int is_key) {
  j->state = ST_STRING;
  j->sstate = SS_NORMAL;
  j->str_is_key = is_key;
  j->u_count = 0; j->u_val = 0; j->u_high = 0;
  j->tok.len = 0;
}

static void begin_object(json2toon *j) {
  if (j->has_key) {
    j2t_indent(&j->out, (unsigned)(j->depth - 1));
    j2t_emit_key(&j->out, j->key.p, j->key.len);
    j2t_putc(&j->out, ':');
    j2t_putc(&j->out, '\n');
  }
  if (j->depth + 1 > j->opt.max_depth) {
    set_err(j, JSON2TOON_ERR_DEPTH, j->stream_offset);
    return;
  }
  j->depth++;
  j->has_key = 0;
  j->state = ST_OBJ_OPEN;
}

static void begin_array(json2toon *j, size_t off) {
  j->cap_has_key = j->has_key;
  j->cap_level = j->has_key ? (unsigned)(j->depth - 1) : 0;
  j->cap_start_off = off;
  j->cap_depth = 1;
  j->cap_in_str = 0;
  j->cap_escape = 0;
  j2t_store_reset(&j->store);
  if (j2t_store_append(&j->store, "[", 1) != 0) {
    set_err(j, j->store.err, off);
    return;
  }
  j->state = ST_ARR_CAP;
}

/* Dispatch on the first byte of a value (already located, not yet consumed). */
static void dispatch_value(json2toon *j, const char **pp, const char *end,
                           size_t base) {
  char c = **pp;
  size_t off = base;             /* offset of this value's first byte */
  (void)end;
  switch (c) {
    case '{': (*pp)++; begin_object(j); break;
    case '[': (*pp)++; begin_array(j, base); break;
    case '"': (*pp)++; start_string(j, 0); break;
    case 't': j->lit_expect = "true"; j->lit_kind = K_TRUE;
              j->lit_pos = 0; j->state = ST_LITERAL; break;
    case 'f': j->lit_expect = "false"; j->lit_kind = K_FALSE;
              j->lit_pos = 0; j->state = ST_LITERAL; break;
    case 'n': j->lit_expect = "null"; j->lit_kind = K_NULL;
              j->lit_pos = 0; j->state = ST_LITERAL; break;
    default:
      if (c == '-' || (c >= '0' && c <= '9')) {
        j->tok.len = 0;
        j->state = ST_NUMBER;
      } else {
        set_err(j, JSON2TOON_ERR_PARSE, off);
      }
      break;
  }
}

/* ------------------------------------------------------------------- feed */

int json2toon_feed(json2toon *j, const char *data, size_t len) {
  const char *p = data;
  const char *end = data + len;
  size_t base = j->stream_offset;

  if (j->err != JSON2TOON_OK)
    return j->err;

  while (p < end) {
    if (j->out.err != JSON2TOON_OK) {
      set_err(j, j->out.err, base + (size_t)(p - data));
      break;
    }
    switch (j->state) {
      case ST_VALUE: {
        p = j2t_skip_ws(p, end);
        if (p == end) break;
        {
          const char *q = p;
          dispatch_value(j, &q, end, base + (size_t)(p - data));
          p = q;
        }
        break;
      }
      case ST_OBJ_OPEN: {
        p = j2t_skip_ws(p, end);
        if (p == end) break;
        if (*p == '}') { p++; j->depth--; finish_value(j); }
        else if (*p == '"') { p++; start_string(j, 1); }
        else set_err(j, JSON2TOON_ERR_PARSE, base + (size_t)(p - data));
        break;
      }
      case ST_OBJ_KEY: {
        p = j2t_skip_ws(p, end);
        if (p == end) break;
        if (*p == '"') { p++; start_string(j, 1); }
        else set_err(j, JSON2TOON_ERR_PARSE, base + (size_t)(p - data));
        break;
      }
      case ST_OBJ_COLON: {
        p = j2t_skip_ws(p, end);
        if (p == end) break;
        if (*p == ':') { p++; j->state = ST_VALUE; }
        else set_err(j, JSON2TOON_ERR_PARSE, base + (size_t)(p - data));
        break;
      }
      case ST_OBJ_NEXT: {
        p = j2t_skip_ws(p, end);
        if (p == end) break;
        if (*p == ',') { p++; j->state = ST_OBJ_KEY; }
        else if (*p == '}') { p++; j->depth--; finish_value(j); }
        else set_err(j, JSON2TOON_ERR_PARSE, base + (size_t)(p - data));
        break;
      }
      case ST_STRING: {
        const char *q = p;
        process_string(j, &q, end, base + (size_t)(p - data));
        p = q;
        break;
      }
      case ST_NUMBER: {
        const char *s = p;
        while (p < end) {
          char c = *p;
          if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' ||
              c == 'e' || c == 'E')
            p++;
          else
            break;
        }
        if (p > s &&
            j2t_buf_append(&j->tok, s, (size_t)(p - s))
                != 0) {
          set_err(j, JSON2TOON_ERR_MEMORY, base + (size_t)(s - data));
          break;
        }
        if (p < end) {
          /* a non-number byte terminates the number */
          if (!j2t_looks_like_number(j->tok.p, j->tok.len))
            set_err(j, JSON2TOON_ERR_PARSE, base + (size_t)(p - data));
          else
            emit_value_line(j, K_NUM);
        }
        break;
      }
      case ST_LITERAL: {
        while (p < end && j->state == ST_LITERAL) {
          if (*p == j->lit_expect[j->lit_pos]) {
            p++; j->lit_pos++;
            if (j->lit_expect[j->lit_pos] == '\0')
              emit_value_line(j, j->lit_kind);
          } else {
            set_err(j, JSON2TOON_ERR_PARSE, base + (size_t)(p - data));
          }
        }
        break;
      }
      case ST_ARR_CAP: {
        /* Scan for the array's closing bracket, tracking string/escape and
         * nesting, but append captured bytes in bulk runs (so a spilled store
         * is not written one byte at a time). */
        const char *seg = p;
        while (p < end) {
          char c = *p++;
          if (j->cap_in_str) {
            if (j->cap_escape) j->cap_escape = 0;
            else if (c == '\\') j->cap_escape = 1;
            else if (c == '"') j->cap_in_str = 0;
          } else {
            if (c == '"') j->cap_in_str = 1;
            else if (c == '[' || c == '{') j->cap_depth++;
            else if (c == ']' || c == '}') {
              if (--j->cap_depth == 0) {
                /* the closing byte is part of the array */
                if (cap_append(j, seg, (size_t)(p - seg),
                               base + (size_t)(seg - data)) == 0)
                  capture_done(j);
                seg = p;
                break;
              }
            }
          }
        }
        if (j->state == ST_ARR_CAP && p > seg)
          cap_append(j, seg, (size_t)(p - seg), base + (size_t)(seg - data));
        break;
      }
      case ST_DONE: {
        p = j2t_skip_ws(p, end);
        if (p == end) break;
        set_err(j, JSON2TOON_ERR_PARSE, base + (size_t)(p - data));
        break;
      }
      default:
        /* ST_ERR */
        j->stream_offset = base + (size_t)(p - data);
        return j->err;
    }
    if (j->err != JSON2TOON_OK) {
      j->stream_offset = j->err_offset;
      return j->err;
    }
  }

  j->stream_offset = base + len;
  return JSON2TOON_OK;
}

/* ----------------------------------------------------------------- finish */

int json2toon_finish(json2toon *j) {
  if (j->err != JSON2TOON_OK)
    return j->err;

  /* A number can only be terminated by EOF when it is the whole document or
   * the document is otherwise complete; finalize it now. */
  if (j->state == ST_NUMBER) {
    if (!j2t_looks_like_number(j->tok.p, j->tok.len))
      set_err(j, JSON2TOON_ERR_PARSE, j->stream_offset);
    else
      emit_value_line(j, K_NUM);
  }

  if (j->err != JSON2TOON_OK)
    return j->err;

  if (j->state != ST_DONE || j->depth != 0) {
    set_err(j, JSON2TOON_ERR_PARSE, j->stream_offset);
    return j->err;
  }

  if (j2t_out_flush(&j->out) != JSON2TOON_OK) {
    set_err(j, JSON2TOON_ERR_IO, j->stream_offset);
    return j->err;
  }
  return JSON2TOON_OK;
}

/* -------------------------------------------------------------- lifecycle */

json2toon *json2toon_new(json2toon_sink sink, void *ctx,
                         const json2toon_options *opts) {
  json2toon *j;
  if (!sink)
    return NULL;
  j = (json2toon *)calloc(1, sizeof *j);
  if (!j)
    return NULL;

  j->opt.indent = 2;
  j->opt.max_depth = 128;
  j->opt.max_array_bytes = 0;            /* 0 == unlimited */
  j->opt.lookahead_buffer_size = 1u << 20;   /* 1 MiB */
  j->opt.get_temp_filename = NULL;       /* NULL == tmpfile() */
  if (opts) {
    if (opts->indent) j->opt.indent = opts->indent;
    if (opts->max_depth) j->opt.max_depth = opts->max_depth;
    j->opt.max_array_bytes = opts->max_array_bytes;
    if (opts->lookahead_buffer_size)
      j->opt.lookahead_buffer_size = opts->lookahead_buffer_size;
    j->opt.get_temp_filename = opts->get_temp_filename;
  }

  j2t_store_init(&j->store, j->opt.lookahead_buffer_size,
                 j->opt.get_temp_filename);
  j2t_out_init(&j->out, sink, ctx, j->opt.indent);
  j->state = ST_VALUE;
  j->err = JSON2TOON_OK;
  return j;
}

void json2toon_delete(json2toon *j) {
  if (!j)
    return;
  j2t_buf_free(&j->key);
  j2t_buf_free(&j->tok);
  j2t_store_free(&j->store);
  free(j);
}

size_t json2toon_error_offset(const json2toon *j) {
  return j ? j->err_offset : 0;
}

const char *json2toon_strerror(int rc) {
  switch (rc) {
    case JSON2TOON_OK: return "success";
    case JSON2TOON_ERR_PARSE: return "malformed JSON input";
    case JSON2TOON_ERR_IO: return "output write error";
    case JSON2TOON_ERR_MEMORY: return "out of memory";
    case JSON2TOON_ERR_DEPTH: return "maximum nesting depth exceeded";
    case JSON2TOON_ERR_LIMIT: return "configured size limit exceeded";
    case JSON2TOON_ERR_USAGE: return "API misuse";
    default: return "unknown error";
  }
}

const char *json2toon_version(void) { return JSON2TOON_VERSION; }
