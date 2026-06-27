/* json2toon - streaming push parser and public API.
 *
 * Objects and root-level primitives are emitted incrementally by an explicit
 * pushdown state machine, so a deeply/widely nested object document converts in
 * memory bounded by its nesting depth, not its length. Arrays are the one
 * construct TOON cannot emit without buffering (the "[N]" count and tabular
 * column set are only knowable after the whole array is seen); each array's raw
 * text is captured, parsed into a value tree (dom.c), encoded, and freed, so
 * peak memory is bounded by the largest single array, not the whole stream.
 */
#include "internal.h"

#include <stdlib.h>
#include <string.h>

#define J2T_VERSION "1.0.0"
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
  char *key; size_t keylen, keycap;
  int has_key;

  /* generic token buffer (string value / key text / number lexeme) */
  char *tok; size_t toklen, tokcap;

  /* string lexer sub-state */
  int sstate;
  int str_is_key;
  int u_count; unsigned u_val; unsigned u_high;

  /* literal lexer */
  const char *lit_expect; int lit_pos; int lit_kind;

  /* array capture */
  char *cap; size_t caplen, capcap;
  int cap_depth, cap_in_str, cap_escape;
  int cap_has_key;
  unsigned cap_level;
  size_t cap_start_off;
};

/* Convenience alias so the internal code can write `json2toon` for the struct
 * (the public header only exposes the `json2toon_t` typedef). */
typedef struct json2toon json2toon;

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

static int buf_append(char **buf, size_t *len, size_t *cap, const char *p,
                      size_t n) {
  if (buf_reserve(buf, cap, *len + n) != 0)
    return -1;
  memcpy(*buf + *len, p, n);
  *len += n;
  return 0;
}

static int buf_putc(char **buf, size_t *len, size_t *cap, char c) {
  if (buf_reserve(buf, cap, *len + 1) != 0)
    return -1;
  (*buf)[(*len)++] = c;
  return 0;
}

static void set_err(json2toon *j, int code, size_t off) {
  if (j->err == JSON2TOON_OK) {
    j->err = code;
    j->err_offset = off;
  }
  j->state = ST_ERR;
}

static int utf8_emit(json2toon *j, unsigned cp) {
  char b[4];
  int n;
  if (cp <= 0x7f) { b[0] = (char)cp; n = 1; }
  else if (cp <= 0x7ff) {
    b[0] = (char)(0xc0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 0x3f)); n = 2;
  } else if (cp <= 0xffff) {
    b[0] = (char)(0xe0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
    b[2] = (char)(0x80 | (cp & 0x3f)); n = 3;
  } else {
    b[0] = (char)(0xf0 | (cp >> 18)); b[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    b[2] = (char)(0x80 | ((cp >> 6) & 0x3f)); b[3] = (char)(0x80 | (cp & 0x3f));
    n = 4;
  }
  return buf_append(&j->tok, &j->toklen, &j->tokcap, b, (size_t)n);
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
    j2t_emit_key(&j->out, j->key, j->keylen);
    j2t_puts(&j->out, ": ");
  }
  switch (kind) {
    case K_STR: j2t_emit_string(&j->out, j->tok, j->toklen, DELIM); break;
    case K_NUM: j2t_write(&j->out, j->tok, j->toklen); break;
    case K_TRUE: j2t_puts(&j->out, "true"); break;
    case K_FALSE: j2t_puts(&j->out, "false"); break;
    case K_NULL: j2t_puts(&j->out, "null"); break;
  }
  j2t_putc(&j->out, '\n');
  finish_value(j);
}

/* ----------------------------------------------------------- array capture */

static void capture_done(json2toon *j) {
  j2t_arena *a = j2t_arena_new();
  j2t_node *node;
  size_t errpos = 0;
  int oom = 0;
  if (!a) {
    set_err(j, JSON2TOON_ERR_MEMORY, j->cap_start_off);
    return;
  }
  node = j2t_dom_parse(a, j->cap, j->caplen, &errpos, &oom);
  if (!node) {
    j2t_arena_free(a);
    if (oom)
      set_err(j, JSON2TOON_ERR_MEMORY, j->cap_start_off);
    else
      set_err(j, JSON2TOON_ERR_PARSE, j->cap_start_off + errpos);
    return;
  }
  j2t_encode_array(&j->out, node, j->cap_level,
                   j->cap_has_key ? j->key : NULL,
                   j->cap_has_key ? j->keylen : 0);
  j2t_arena_free(a);
  j->caplen = 0;
  if (j->out.err != JSON2TOON_OK) {
    set_err(j, j->out.err, j->stream_offset);
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
        if (buf_append(&j->tok, &j->toklen, &j->tokcap, p, (size_t)(run - p))
            != 0) {
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
            if (buf_reserve(&j->key, &j->keycap, j->toklen ? j->toklen : 1)
                != 0) {
              set_err(j, JSON2TOON_ERR_MEMORY, base); *pp = p; return;
            }
            memcpy(j->key, j->tok, j->toklen);
            j->keylen = j->toklen;
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
      if (buf_putc(&j->tok, &j->toklen, &j->tokcap, out) != 0) {
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
  j->toklen = 0;
}

static void begin_object(json2toon *j) {
  if (j->has_key) {
    j2t_indent(&j->out, (unsigned)(j->depth - 1));
    j2t_emit_key(&j->out, j->key, j->keylen);
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
  j->caplen = 0;
  j->cap_depth = 1;
  j->cap_in_str = 0;
  j->cap_escape = 0;
  if (buf_putc(&j->cap, &j->caplen, &j->capcap, '[') != 0) {
    set_err(j, JSON2TOON_ERR_MEMORY, off);
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
        j->toklen = 0;
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
            buf_append(&j->tok, &j->toklen, &j->tokcap, s, (size_t)(p - s))
                != 0) {
          set_err(j, JSON2TOON_ERR_MEMORY, base + (size_t)(s - data));
          break;
        }
        if (p < end) {
          /* a non-number byte terminates the number */
          if (!j2t_looks_like_number(j->tok, j->toklen))
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
        while (p < end) {
          char c = *p++;
          if (j->opt.max_array_bytes &&
              j->caplen + 1 > j->opt.max_array_bytes) {
            set_err(j, JSON2TOON_ERR_LIMIT, base + (size_t)(p - 1 - data));
            break;
          }
          if (buf_putc(&j->cap, &j->caplen, &j->capcap, c) != 0) {
            set_err(j, JSON2TOON_ERR_MEMORY, base + (size_t)(p - 1 - data));
            break;
          }
          if (j->cap_in_str) {
            if (j->cap_escape) j->cap_escape = 0;
            else if (c == '\\') j->cap_escape = 1;
            else if (c == '"') j->cap_in_str = 0;
          } else {
            if (c == '"') j->cap_in_str = 1;
            else if (c == '[' || c == '{') j->cap_depth++;
            else if (c == ']' || c == '}') {
              if (--j->cap_depth == 0) {
                capture_done(j);
                break;
              }
            }
          }
        }
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
    if (!j2t_looks_like_number(j->tok, j->toklen))
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
  j->opt.max_depth = 256;
  j->opt.max_array_bytes = 0;            /* 0 == unlimited */
  if (opts) {
    if (opts->indent) j->opt.indent = opts->indent;
    if (opts->max_depth) j->opt.max_depth = opts->max_depth;
    j->opt.max_array_bytes = opts->max_array_bytes;
  }

  j2t_simd_init();
  j2t_out_init(&j->out, sink, ctx, j->opt.indent);
  j->state = ST_VALUE;
  j->err = JSON2TOON_OK;
  return j;
}

void json2toon_delete(json2toon *j) {
  if (!j)
    return;
  free(j->key);
  free(j->tok);
  free(j->cap);
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

const char *json2toon_version(void) { return J2T_VERSION; }
