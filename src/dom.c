/* json2toon - in-memory value tree (DOM) used to encode arrays.
 *
 * TOON array headers carry the element count and, for tabular arrays, the
 * column set and a uniformity guarantee. None of these can be emitted before
 * the whole array has been seen, so arrays are buffered into this tree, encoded,
 * then freed. Objects and primitives outside arrays are emitted by the
 * streaming machine in json2toon.c and never touch this code.
 */
#include "internal.h"

#include <stdlib.h>
#include <string.h>

#define DELIM ','
#define DOM_MAX_DEPTH 512

/* ------------------------------------------------------------------- arena */

typedef struct arena_block {
  struct arena_block *next;
  size_t used, cap;
  char data[];
} arena_block;

struct j2t_arena {
  arena_block *head;
};

j2t_arena *j2t_arena_new(void) {
  j2t_arena *a = (j2t_arena *)calloc(1, sizeof *a);
  return a;
}

void j2t_arena_free(j2t_arena *a) {
  arena_block *b;
  if (!a)
    return;
  b = a->head;
  while (b) {
    arena_block *n = b->next;
    free(b);
    b = n;
  }
  free(a);
}

void *j2t_arena_alloc(j2t_arena *a, size_t n) {
  arena_block *b = a->head;
  n = (n + 15u) & ~(size_t)15u;            /* 16-byte align */
  if (!b || b->used + n > b->cap) {
    size_t cap = 65536;
    if (cap < n)
      cap = n;
    b = (arena_block *)malloc(sizeof *b + cap);
    if (!b)
      return NULL;
    b->next = a->head;
    b->used = 0;
    b->cap = cap;
    a->head = b;
  }
  void *p = b->data + b->used;
  b->used += n;
  return p;
}

/* -------------------------------------------------------------- temp vector */

/* A heap vector of pointers, used while building objects/arrays of unknown
 * length, then snapshotted into the arena and freed. */
typedef struct {
  void **p;
  size_t n, cap;
} vec;

static int vec_push(vec *v, void *x) {
  if (v->n == v->cap) {
    size_t nc = v->cap ? v->cap * 2 : 8;
    void **np = (void **)realloc(v->p, nc * sizeof *np);
    if (!np)
      return -1;
    v->p = np;
    v->cap = nc;
  }
  v->p[v->n++] = x;
  return 0;
}

/* ------------------------------------------------------------------- parser */

typedef struct {
  const char *p, *end, *base;
  j2t_arena *a;
  int oom;
} P;

static void skipws(P *p) {
  while (p->p < p->end) {
    char c = *p->p;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
      p->p++;
    else
      break;
  }
}

static j2t_node *node_new(P *p, j2t_ntype t) {
  j2t_node *n = (j2t_node *)j2t_arena_alloc(p->a, sizeof *n);
  if (!n) {
    p->oom = 1;
    return NULL;
  }
  memset(n, 0, sizeof *n);
  n->t = t;
  return n;
}

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

/* Parse a JSON string starting at the opening '"'. On success advances past the
 * closing quote and returns 0, storing decoded bytes in *out / *outlen. */
static int parse_string(P *p, char **out, size_t *outlen) {
  const char *s = p->p + 1;             /* after opening quote */
  char *buf, *w;
  /* Decoded length never exceeds the raw span between the quotes. */
  const char *scan = s;
  size_t raw = 0;
  while (scan < p->end && *scan != '"') {
    if (*scan == '\\')
      scan++;
    if (scan < p->end)
      scan++;
    raw++;
  }
  if (scan >= p->end)
    return -1;                          /* unterminated */
  buf = (char *)j2t_arena_alloc(p->a, raw ? raw : 1);
  if (!buf) {
    p->oom = 1;
    return -1;
  }
  w = buf;
  while (s < p->end && *s != '"') {
    unsigned char c = (unsigned char)*s;
    if (c == '\\') {
      s++;
      if (s >= p->end)
        return -1;
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
          unsigned cp;
          char enc[4];
          int el;
          if (s + 5 > p->end || hex4(s + 1, &cp) != 0)
            return -1;
          s += 5;
          if (cp >= 0xd800 && cp <= 0xdbff) {
            unsigned lo;
            if (s + 6 > p->end || s[0] != '\\' || s[1] != 'u' ||
                hex4(s + 2, &lo) != 0 || lo < 0xdc00 || lo > 0xdfff)
              return -1;
            cp = 0x10000 + (((cp - 0xd800) << 10) | (lo - 0xdc00));
            s += 6;
          } else if (cp >= 0xdc00 && cp <= 0xdfff) {
            return -1;                  /* lone low surrogate */
          }
          el = utf8_encode(cp, enc);
          memcpy(w, enc, (size_t)el);
          w += el;
          break;
        }
        default:
          return -1;
      }
    } else if (c < 0x20) {
      return -1;                        /* unescaped control char */
    } else {
      *w++ = (char)c;
      s++;
    }
  }
  if (s >= p->end)
    return -1;
  p->p = s + 1;                         /* past closing quote */
  *out = buf;
  *outlen = (size_t)(w - buf);
  return 0;
}

static j2t_node *parse_value(P *p, int depth);

static j2t_node *parse_string_node(P *p) {
  j2t_node *n = node_new(p, J2T_STR);
  if (!n)
    return NULL;
  if (parse_string(p, &n->u.s.p, &n->u.s.n) != 0)
    return NULL;
  return n;
}

static j2t_node *parse_number(P *p) {
  const char *s = p->p;
  const char *q = s;
  j2t_node *n;
  while (q < p->end) {
    char c = *q;
    if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' ||
        c == 'e' || c == 'E')
      q++;
    else
      break;
  }
  if (!j2t_looks_like_number(s, (size_t)(q - s)))
    return NULL;
  n = node_new(p, J2T_NUM);
  if (!n)
    return NULL;
  n->u.s.p = (char *)s;                 /* points into the source buffer */
  n->u.s.n = (size_t)(q - s);
  p->p = q;
  return n;
}

static j2t_node *parse_literal(P *p, const char *lit, j2t_ntype t, int bval) {
  size_t len = strlen(lit);
  j2t_node *n;
  if ((size_t)(p->end - p->p) < len || memcmp(p->p, lit, len) != 0)
    return NULL;
  p->p += len;
  n = node_new(p, t);
  if (!n)
    return NULL;
  n->u.b = bval;
  return n;
}

static j2t_node *parse_array(P *p, int depth) {
  j2t_node *n;
  vec items = {0, 0, 0};
  p->p++;                               /* '[' */
  skipws(p);
  if (p->p < p->end && *p->p == ']') {
    p->p++;
    n = node_new(p, J2T_ARR);
    return n;                           /* empty array */
  }
  for (;;) {
    j2t_node *v = parse_value(p, depth + 1);
    if (!v) {
      free(items.p);
      return NULL;
    }
    if (vec_push(&items, v) != 0) {
      p->oom = 1;
      free(items.p);
      return NULL;
    }
    skipws(p);
    if (p->p >= p->end) {
      free(items.p);
      return NULL;
    }
    if (*p->p == ',') {
      p->p++;
      continue;
    }
    if (*p->p == ']') {
      p->p++;
      break;
    }
    free(items.p);
    return NULL;
  }
  n = node_new(p, J2T_ARR);
  if (!n) {
    free(items.p);
    return NULL;
  }
  n->u.arr.n = items.n;
  n->u.arr.v = (j2t_node **)j2t_arena_alloc(p->a, items.n * sizeof(j2t_node *));
  if (!n->u.arr.v) {
    p->oom = 1;
    free(items.p);
    return NULL;
  }
  memcpy(n->u.arr.v, items.p, items.n * sizeof(j2t_node *));
  free(items.p);
  return n;
}

static j2t_node *parse_object(P *p, int depth) {
  j2t_node *n;
  vec keys = {0, 0, 0}, klens = {0, 0, 0}, vals = {0, 0, 0};
  p->p++;                               /* '{' */
  skipws(p);
  if (p->p < p->end && *p->p == '}') {
    p->p++;
    return node_new(p, J2T_OBJ);        /* empty object */
  }
  for (;;) {
    char *k;
    size_t klen;
    j2t_node *v;
    skipws(p);
    if (p->p >= p->end || *p->p != '"')
      goto fail;
    if (parse_string(p, &k, &klen) != 0)
      goto fail;
    skipws(p);
    if (p->p >= p->end || *p->p != ':')
      goto fail;
    p->p++;
    v = parse_value(p, depth + 1);
    if (!v)
      goto fail;
    if (vec_push(&keys, k) != 0 || vec_push(&klens, (void *)klen) != 0 ||
        vec_push(&vals, v) != 0) {
      p->oom = 1;
      goto fail;
    }
    skipws(p);
    if (p->p >= p->end)
      goto fail;
    if (*p->p == ',') {
      p->p++;
      continue;
    }
    if (*p->p == '}') {
      p->p++;
      break;
    }
    goto fail;
  }
  n = node_new(p, J2T_OBJ);
  if (!n)
    goto fail;
  n->u.obj.n = keys.n;
  n->u.obj.k = (char **)j2t_arena_alloc(p->a, keys.n * sizeof(char *));
  n->u.obj.klen = (size_t *)j2t_arena_alloc(p->a, keys.n * sizeof(size_t));
  n->u.obj.v = (j2t_node **)j2t_arena_alloc(p->a, keys.n * sizeof(j2t_node *));
  if (!n->u.obj.k || !n->u.obj.klen || !n->u.obj.v) {
    p->oom = 1;
    goto fail;
  }
  memcpy(n->u.obj.k, keys.p, keys.n * sizeof(char *));
  memcpy(n->u.obj.v, vals.p, vals.n * sizeof(j2t_node *));
  {
    size_t i;
    for (i = 0; i < klens.n; i++)
      n->u.obj.klen[i] = (size_t)klens.p[i];
  }
  free(keys.p);
  free(klens.p);
  free(vals.p);
  return n;
fail:
  free(keys.p);
  free(klens.p);
  free(vals.p);
  return NULL;
}

static j2t_node *parse_value(P *p, int depth) {
  if (depth > DOM_MAX_DEPTH)
    return NULL;
  skipws(p);
  if (p->p >= p->end)
    return NULL;
  switch (*p->p) {
    case '{': return parse_object(p, depth);
    case '[': return parse_array(p, depth);
    case '"': return parse_string_node(p);
    case 't': return parse_literal(p, "true", J2T_BOOL, 1);
    case 'f': return parse_literal(p, "false", J2T_BOOL, 0);
    case 'n': return parse_literal(p, "null", J2T_NULL, 0);
    default:
      if (*p->p == '-' || (*p->p >= '0' && *p->p <= '9'))
        return parse_number(p);
      return NULL;
  }
}

j2t_node *j2t_dom_parse(j2t_arena *a, const char *buf, size_t len,
                        size_t *errpos, int *oom) {
  P p;
  j2t_node *root;
  p.p = buf;
  p.end = buf + len;
  p.base = buf;
  p.a = a;
  p.oom = 0;
  root = parse_value(&p, 0);
  if (root) {
    skipws(&p);
    if (p.p != p.end)                   /* trailing garbage */
      root = NULL;
  }
  *oom = p.oom;
  if (!root && errpos)
    *errpos = (size_t)(p.p - p.base);
  return root;
}

/* ------------------------------------------------------------------ encoder */

static int is_scalar(const j2t_node *n) {
  return n->t == J2T_NULL || n->t == J2T_BOOL || n->t == J2T_NUM ||
         n->t == J2T_STR;
}

static void emit_scalar(j2t_out *o, const j2t_node *n) {
  switch (n->t) {
    case J2T_NULL: j2t_puts(o, "null"); break;
    case J2T_BOOL: j2t_puts(o, n->u.b ? "true" : "false"); break;
    case J2T_NUM: j2t_write(o, n->u.s.p, n->u.s.n); break;
    case J2T_STR: j2t_emit_string(o, n->u.s.p, n->u.s.n, DELIM); break;
    default: break;                     /* unreachable */
  }
}

static void emit_count(j2t_out *o, size_t n) {
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

static int array_all_scalar(const j2t_node *arr) {
  size_t i;
  for (i = 0; i < arr->u.arr.n; i++)
    if (!is_scalar(arr->u.arr.v[i]))
      return 0;
  return 1;
}

static int array_is_tabular(const j2t_node *arr) {
  const j2t_node *first;
  size_t m, i, j;
  if (arr->u.arr.n == 0)
    return 0;
  first = arr->u.arr.v[0];
  if (first->t != J2T_OBJ || first->u.obj.n == 0)
    return 0;
  m = first->u.obj.n;
  for (j = 0; j < m; j++)
    if (!is_scalar(first->u.obj.v[j]))
      return 0;
  for (i = 1; i < arr->u.arr.n; i++) {
    const j2t_node *e = arr->u.arr.v[i];
    if (e->t != J2T_OBJ || e->u.obj.n != m)
      return 0;
    for (j = 0; j < m; j++) {
      if (e->u.obj.klen[j] != first->u.obj.klen[j] ||
          memcmp(e->u.obj.k[j], first->u.obj.k[j], first->u.obj.klen[j]) != 0)
        return 0;
      if (!is_scalar(e->u.obj.v[j]))
        return 0;
    }
  }
  return 1;
}

static void encode_array_tail(j2t_out *o, const j2t_node *arr, unsigned level);
static void emit_member_body(j2t_out *o, const char *k, size_t klen,
                             const j2t_node *v, unsigned level);

void j2t_encode_object_members(j2t_out *o, const j2t_node *obj, unsigned level) {
  size_t i;
  for (i = 0; i < obj->u.obj.n; i++) {
    j2t_indent(o, level);
    emit_member_body(o, obj->u.obj.k[i], obj->u.obj.klen[i], obj->u.obj.v[i],
                     level);
  }
}

/* Emit "key: value" (or "key:" + nested) with leading indentation ALREADY
 * written by the caller. `level` is this member's indentation level; nested
 * children go to level+1. */
static void emit_member_body(j2t_out *o, const char *k, size_t klen,
                             const j2t_node *v, unsigned level) {
  j2t_emit_key(o, k, klen);
  if (is_scalar(v)) {
    j2t_puts(o, ": ");
    emit_scalar(o, v);
    j2t_putc(o, '\n');
  } else if (v->t == J2T_OBJ) {
    j2t_putc(o, ':');
    j2t_putc(o, '\n');
    if (v->u.obj.n > 0)
      j2t_encode_object_members(o, v, level + 1);
  } else { /* J2T_ARR */
    if (v->u.arr.n == 0) {
      j2t_puts(o, ": []");
      j2t_putc(o, '\n');
    } else {
      encode_array_tail(o, v, level);
    }
  }
}

/* Emit one array element as a list item at `level` (leading "- " marker). */
static void emit_item(j2t_out *o, const j2t_node *v, unsigned level) {
  if (is_scalar(v)) {
    j2t_indent(o, level);
    j2t_puts(o, "- ");
    emit_scalar(o, v);
    j2t_putc(o, '\n');
  } else if (v->t == J2T_ARR) {
    j2t_indent(o, level);
    j2t_puts(o, "- ");
    if (v->u.arr.n == 0) {
      j2t_puts(o, "[]");
      j2t_putc(o, '\n');
    } else {
      encode_array_tail(o, v, level + 1);
    }
  } else { /* J2T_OBJ */
    size_t i;
    if (v->u.obj.n == 0) {
      j2t_indent(o, level);
      j2t_putc(o, '-');
      j2t_putc(o, '\n');
      return;
    }
    /* First member shares the hyphen line; the rest align one level deeper. */
    j2t_indent(o, level);
    j2t_puts(o, "- ");
    emit_member_body(o, v->u.obj.k[0], v->u.obj.klen[0], v->u.obj.v[0],
                     level + 1);
    for (i = 1; i < v->u.obj.n; i++) {
      j2t_indent(o, level + 1);
      emit_member_body(o, v->u.obj.k[i], v->u.obj.klen[i], v->u.obj.v[i],
                       level + 1);
    }
  }
}

/* Emit the "[N]..." header and body of a non-empty array. The line lead (either
 * indentation+key or indentation+"- ") has ALREADY been written by the caller.
 * `level` is the level of the header line; bodies indent to level+1. */
static void encode_array_tail(j2t_out *o, const j2t_node *arr, unsigned level) {
  size_t n = arr->u.arr.n, i, j;

  if (array_all_scalar(arr)) {
    j2t_putc(o, '[');
    emit_count(o, n);
    j2t_puts(o, "]: ");
    for (i = 0; i < n; i++) {
      if (i)
        j2t_putc(o, DELIM);
      emit_scalar(o, arr->u.arr.v[i]);
    }
    j2t_putc(o, '\n');
    return;
  }

  if (array_is_tabular(arr)) {
    const j2t_node *first = arr->u.arr.v[0];
    size_t m = first->u.obj.n;
    j2t_putc(o, '[');
    emit_count(o, n);
    j2t_puts(o, "]{");
    for (j = 0; j < m; j++) {
      if (j)
        j2t_putc(o, DELIM);
      j2t_emit_key(o, first->u.obj.k[j], first->u.obj.klen[j]);
    }
    j2t_puts(o, "}:");
    j2t_putc(o, '\n');
    for (i = 0; i < n; i++) {
      const j2t_node *row = arr->u.arr.v[i];
      j2t_indent(o, level + 1);
      for (j = 0; j < m; j++) {
        if (j)
          j2t_putc(o, DELIM);
        emit_scalar(o, row->u.obj.v[j]);
      }
      j2t_putc(o, '\n');
    }
    return;
  }

  /* List form. */
  j2t_putc(o, '[');
  emit_count(o, n);
  j2t_puts(o, "]:");
  j2t_putc(o, '\n');
  for (i = 0; i < n; i++)
    emit_item(o, arr->u.arr.v[i], level + 1);
}

void j2t_encode_array(j2t_out *o, const j2t_node *arr, unsigned level,
                      const char *keytext, size_t keylen) {
  j2t_indent(o, level);
  if (arr->u.arr.n == 0) {
    if (keylen) {
      j2t_emit_key(o, keytext, keylen);
      j2t_puts(o, ": []");
    } else {
      j2t_puts(o, "[]");
    }
    j2t_putc(o, '\n');
    return;
  }
  if (keylen)
    j2t_emit_key(o, keytext, keylen);
  encode_array_tail(o, arr, level);
}
