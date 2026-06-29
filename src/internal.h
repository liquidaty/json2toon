/* json2toon - internal shared declarations. Not part of the public API. */
#ifndef JSON2TOON_INTERNAL_H
#define JSON2TOON_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include "json2toon.h"

/* ------------------------------------------------------------------ output */

/* Buffered output emitter. All TOON bytes pass through here; the buffer is
 * flushed to the user sink when full or on finish, which keeps output memory
 * bounded regardless of document size. */
#define J2T_OUT_BUFSZ 8192

typedef struct {
  json2toon_sink sink;
  void *ctx;
  unsigned indent_width;     /* spaces per level */
  int err;                   /* sticky: JSON2TOON_OK or an error code */
  size_t len;
  char buf[J2T_OUT_BUFSZ];
} j2t_out;

void j2t_out_init(j2t_out *o, json2toon_sink sink, void *ctx, unsigned indent_width);
int j2t_out_flush(j2t_out *o);
void j2t_write(j2t_out *o, const char *p, size_t n);
void j2t_putc(j2t_out *o, char c);
void j2t_puts(j2t_out *o, const char *s);            /* NUL-terminated */
void j2t_indent(j2t_out *o, unsigned level);          /* level*indent_width spaces */

/* ---------------------------------------------------------------- scalars */

/* True if the string value must be quoted in TOON output (given the active
 * delimiter byte, normally ','). */
int j2t_str_needs_quote(const char *s, size_t n, char delim);

/* True if an object key / tabular field name may be emitted unquoted. */
int j2t_key_is_bare(const char *s, size_t n);

/* Emit a string value, quoting and escaping as required by `delim`. */
void j2t_emit_string(j2t_out *o, const char *s, size_t n, char delim);

/* Emit an object key or field name (quoted+escaped if not bare). */
void j2t_emit_key(j2t_out *o, const char *s, size_t n);

/* Emit a raw quoted+escaped string (used for keys and forced-quote values). */
void j2t_emit_quoted(j2t_out *o, const char *s, size_t n);

/* True if [s,n) matches the JSON number grammar exactly. */
int j2t_looks_like_number(const char *s, size_t n);

/* --------------------------------------------------------------- scanners */

/* SIMD/scalar scanners, selected at compile time and bound at static-init time
 * (see src/simd.c). Read-only after startup, so converter creation needs no
 * init handshake and is safe to use concurrently from multiple threads. */
typedef const char *(*j2t_scan_fn)(const char *p, const char *end);

/* Return first byte in [p,end) that is NOT JSON insignificant whitespace. */
extern j2t_scan_fn j2t_skip_ws;
/* Return first byte in [p,end) that ends a "clean" string run: a '"', a '\\',
 * or a control byte (< 0x20). */
extern j2t_scan_fn j2t_scan_string;
const char *j2t_simd_backend(void);

/* -------------------------------------------------------------------- DOM */

/* In-memory value tree, used to encode arrays (which require their element
 * count up front and so cannot be emitted without buffering). */
typedef enum {
  J2T_NULL, J2T_BOOL, J2T_NUM, J2T_STR, J2T_ARR, J2T_OBJ
} j2t_ntype;

typedef struct j2t_node j2t_node;
struct j2t_node {
  j2t_ntype t;
  union {
    int b;                                   /* J2T_BOOL */
    struct { char *p; size_t n; } s;         /* J2T_STR (decoded) / J2T_NUM (raw) */
    struct { j2t_node **v; size_t n; } arr;  /* J2T_ARR */
    struct { char **k; size_t *klen; j2t_node **v; size_t n; } obj; /* J2T_OBJ */
  } u;
};

/* Bump-allocating arena; freed in one shot after each array is emitted. */
typedef struct j2t_arena j2t_arena;
j2t_arena *j2t_arena_new(void);
void *j2t_arena_alloc(j2t_arena *a, size_t n);
void j2t_arena_free(j2t_arena *a);

/* Parse a complete in-memory JSON document [buf,len) into a node tree.
 * Returns NULL and sets *errpos on malformed input or OOM (*oom set). */
j2t_node *j2t_dom_parse(j2t_arena *a, const char *buf, size_t len,
                        size_t *errpos, int *oom);

/* Encode an array node as TOON. `level` is the indentation level of the array
 * header line; `keytext`/`keylen` is the already-quoted key to place before the
 * '[' (empty for a root array). The caller has NOT yet emitted indentation. */
void j2t_encode_array(j2t_out *o, const j2t_node *arr, unsigned level,
                      const char *keytext, size_t keylen);

/* Encode an object's members at the given level (used for objects nested
 * inside arrays). */
void j2t_encode_object_members(j2t_out *o, const j2t_node *obj, unsigned level);

#endif /* JSON2TOON_INTERNAL_H */
