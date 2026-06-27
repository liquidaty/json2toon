/* json2toon - convert JSON to TOON (Token-Oriented Object Notation).
 *
 * Public C API. The converter is a streaming push parser: input is supplied in
 * arbitrary chunks via json2toon_feed(), and TOON output is delivered through a
 * sink callback in bounded-size pieces. Neither the whole input nor the whole
 * output is ever required to be resident in memory.
 *
 * Copyright (c) 2026. MIT License.
 */
#ifndef JSON2TOON_H
#define JSON2TOON_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Status / error codes. JSON2TOON_OK is zero; all errors are negative. */
enum {
  JSON2TOON_OK = 0,
  JSON2TOON_ERR_PARSE = -1,  /* malformed JSON input */
  JSON2TOON_ERR_IO = -2,     /* the output sink returned non-zero */
  JSON2TOON_ERR_MEMORY = -3, /* allocation failed */
  JSON2TOON_ERR_DEPTH = -4,  /* nesting deeper than the configured limit */
  JSON2TOON_ERR_LIMIT = -5,  /* a configured size limit was exceeded */
  JSON2TOON_ERR_USAGE = -6   /* API used incorrectly (e.g. feed after error) */
};

/* Sink callback. Receives a chunk of UTF-8 TOON output. Return 0 to continue,
 * non-zero to abort the conversion with JSON2TOON_ERR_IO. The bytes are only
 * valid for the duration of the call. */
typedef int (*json2toon_sink)(const char *data, size_t len, void *ctx);

/* Configuration. Pass NULL to json2toon_new() to accept all defaults. A zero
 * field also selects the default for that field. */
typedef struct {
  unsigned indent;          /* spaces per indentation level (default 2) */
  unsigned max_depth;       /* maximum nesting depth (default 256) */
  size_t max_array_bytes;   /* cap on the raw size of a single buffered array
                             * (default 0 == a large built-in limit) */
} json2toon_options;

typedef struct json2toon json2toon_t;

/* Create a converter. Returns NULL on allocation failure. */
json2toon_t *json2toon_new(json2toon_sink sink, void *ctx,
                           const json2toon_options *opts);

/* Feed a chunk of JSON input. May be called repeatedly. Returns JSON2TOON_OK
 * or a negative error code; once an error is returned the converter is poisoned
 * and further feed/finish calls return the same error. */
int json2toon_feed(json2toon_t *j2t, const char *data, size_t len);

/* Signal end of input and flush any pending output. Returns JSON2TOON_OK on a
 * complete, well-formed document, otherwise a negative error code. */
int json2toon_finish(json2toon_t *j2t);

/* Destroy a converter created by json2toon_new(). Safe to call with NULL. */
void json2toon_delete(json2toon_t *j2t);

/* Byte offset within the input stream at which the most recent error was
 * detected. Meaningful only after a call returned a negative code. */
size_t json2toon_error_offset(const json2toon_t *j2t);

/* Human-readable description of a status code. */
const char *json2toon_strerror(int rc);

/* Library version string, e.g. "1.0.0". */
const char *json2toon_version(void);

#ifdef __cplusplus
}
#endif

#endif /* JSON2TOON_H */
