/* json2toon - internal shared declarations. Not part of the public API. */
#ifndef JSON2TOON_INTERNAL_H
#define JSON2TOON_INTERNAL_H

#include "j2t_config.h"      /* first: feature-test macros + J2T_FSEEK/FTELL */

#include <stddef.h>
#include <stdint.h>
#include "json2toon.h"

/* --------------------------------------------------------- growable buffer */

/* A heap byte buffer that grows geometrically. One shared implementation
 * (buf.c) backs every "char*, len, cap" triplet in the converters. */
typedef struct { char *p; size_t len, cap; } j2t_buf;

/* Ensure capacity for at least `need` bytes total. 0 on success, -1 on OOM. */
int  j2t_buf_reserve(j2t_buf *b, size_t need);
/* Append n bytes / one byte. 0 on success, -1 on OOM. */
int  j2t_buf_append(j2t_buf *b, const char *s, size_t n);
int  j2t_buf_putc(j2t_buf *b, char c);
/* Release storage and reset to empty. */
void j2t_buf_free(j2t_buf *b);

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

/* Emit a non-negative count as decimal (the "N" in a TOON "[N]" header). */
void j2t_emit_count(j2t_out *o, size_t n);

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

/* ------------------------------------------------------------- backing store */

/* Append-then-read store for one buffered array's raw bytes. Bytes live in
 * `ram` up to `threshold`; the overflow spills to a temp file so peak RAM stays
 * bounded regardless of array length. `ram` is reused across stored arrays. */
typedef struct {
  j2t_buf ram;               /* resident bytes (pre-spill); .len meaningful pre-spill */
  size_t threshold;          /* spill once a store would exceed this many bytes */
  int spilled;               /* non-zero once bytes have moved to fp */
  FILE *fp;                  /* spill file (NULL until spilled) */
  char *tmpname;             /* get_temp_filename() result; NULL for tmpfile() */
  uint64_t total;            /* total bytes appended */
  char *(*get_temp_filename)(const char *prefix);
  int err;                   /* sticky JSON2TOON_ERR_MEMORY / _IO, or OK */
} j2t_store;

void j2t_store_init(j2t_store *s, size_t threshold,
                    char *(*get_temp_filename)(const char *prefix));
/* Append n bytes; returns JSON2TOON_OK or a sticky error (also left in s->err). */
int j2t_store_append(j2t_store *s, const char *p, size_t n);
/* Bytes appended so far (for the max_array_bytes check). */
uint64_t j2t_store_size(const j2t_store *s);
/* Close+remove any temp file and ready the store for the next array (keeps ram). */
void j2t_store_reset(j2t_store *s);
/* Reset, then release ram. */
void j2t_store_free(j2t_store *s);

/* Sequential reader with seek over a store, sourcing from ram or the spill
 * file transparently. */
typedef struct {
  j2t_store *s;
  uint64_t pos;              /* logical read position in [0, total) */
  char buf[4096];            /* file read window (spilled stores only) */
  uint64_t buf_off;          /* logical offset of buf[0] */
  size_t buf_len;            /* valid bytes in buf */
} j2t_reader;

void j2t_reader_init(j2t_reader *r, j2t_store *s);
void j2t_reader_seek(j2t_reader *r, uint64_t off);
uint64_t j2t_reader_tell(const j2t_reader *r);
int j2t_reader_getc(j2t_reader *r);   /* byte at pos, advance; -1 at end/error */
int j2t_reader_peek(j2t_reader *r);   /* byte at pos, no advance; -1 at end/error */
size_t j2t_reader_read(j2t_reader *r, char *dst, size_t n);  /* bulk; advances */

/* ----------------------------------------------------------- array encoder */

/* Encode the array filling `s` (a complete "[...]") as TOON. `level` is the
 * header line's indent level; `key`/`keylen` is the decoded key before '[' (NULL
 * for a root array); `max_depth` bounds nesting (ERR_DEPTH past it). On a parse
 * or depth error, *errpos gets the array-relative byte offset. */
int j2t_encode_captured(j2t_out *o, j2t_store *s, unsigned level,
                        const char *key, size_t keylen,
                        unsigned max_depth, uint64_t *errpos);

#endif /* JSON2TOON_INTERNAL_H */
