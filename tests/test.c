/* json2toon - unit, conformance, and streaming-equivalence tests. */
#include "json2toon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ sink buffer */

typedef struct {
  char *p;
  size_t len, cap;
  int oom;
} sbuf;

static int sbuf_sink(const char *data, size_t n, void *ctx) {
  sbuf *b = (sbuf *)ctx;
  if (b->len + n + 1 > b->cap) {
    size_t nc = b->cap ? b->cap : 256;
    char *np;
    while (nc < b->len + n + 1)
      nc *= 2;
    np = (char *)realloc(b->p, nc);
    if (!np) { b->oom = 1; return -1; }
    b->p = np;
    b->cap = nc;
  }
  memcpy(b->p + b->len, data, n);
  b->len += n;
  b->p[b->len] = '\0';
  return 0;
}

/* Convert in one shot. *out is a malloc'd NUL-terminated string (caller frees).
 * Returns the json2toon status code. */
static int convert(const char *json, size_t n, unsigned indent, char **out) {
  sbuf b = {0, 0, 0, 0};
  json2toon_options opt;
  json2toon_t *j;
  int rc;
  memset(&opt, 0, sizeof opt);
  opt.indent = indent;
  j = json2toon_new(sbuf_sink, &b, &opt);
  if (!j) { *out = NULL; return JSON2TOON_ERR_MEMORY; }
  rc = json2toon_feed(j, json, n);
  if (rc == JSON2TOON_OK)
    rc = json2toon_finish(j);
  json2toon_delete(j);
  if (!b.p) { b.p = (char *)calloc(1, 1); }
  *out = b.p;
  return rc;
}

/* Convert feeding one byte at a time, to exercise every chunk boundary. */
static int convert_streamed(const char *json, size_t n, unsigned indent,
                            char **out) {
  sbuf b = {0, 0, 0, 0};
  json2toon_options opt;
  json2toon_t *j;
  int rc = JSON2TOON_OK;
  size_t i;
  memset(&opt, 0, sizeof opt);
  opt.indent = indent;
  j = json2toon_new(sbuf_sink, &b, &opt);
  if (!j) { *out = NULL; return JSON2TOON_ERR_MEMORY; }
  for (i = 0; i < n && rc == JSON2TOON_OK; i++)
    rc = json2toon_feed(j, json + i, 1);
  if (rc == JSON2TOON_OK)
    rc = json2toon_finish(j);
  json2toon_delete(j);
  if (!b.p) { b.p = (char *)calloc(1, 1); }
  *out = b.p;
  return rc;
}

/* ----------------------------------------------------------------- harness */

static int g_pass = 0, g_fail = 0;

static void check_ok(const char *name, const char *json, const char *expect) {
  char *bulk = NULL, *strm = NULL;
  int rc1, rc2;
  size_t n = strlen(json);

  rc1 = convert(json, n, 2, &bulk);
  if (rc1 != JSON2TOON_OK || strcmp(bulk, expect) != 0) {
    g_fail++;
    printf("FAIL %s\n  input:    %s\n  expected: %s\n  got(%d):  %s\n",
           name, json, expect, rc1, bulk ? bulk : "(null)");
    free(bulk);
    return;
  }
  rc2 = convert_streamed(json, n, 2, &strm);
  if (rc2 != JSON2TOON_OK || strcmp(strm, bulk) != 0) {
    g_fail++;
    printf("FAIL %s (streamed mismatch)\n  bulk:     %s\n  streamed(%d): %s\n",
           name, bulk, rc2, strm ? strm : "(null)");
  } else {
    g_pass++;
  }
  free(bulk);
  free(strm);
}

static void check_err(const char *name, const char *json) {
  char *bulk = NULL, *strm = NULL;
  int rc1, rc2;
  size_t n = strlen(json);
  rc1 = convert(json, n, 2, &bulk);
  rc2 = convert_streamed(json, n, 2, &strm);
  if (rc1 == JSON2TOON_OK || rc2 == JSON2TOON_OK) {
    g_fail++;
    printf("FAIL %s (expected error)\n  input: %s\n  bulk rc=%d streamed rc=%d\n",
           name, json, rc1, rc2);
  } else {
    g_pass++;
  }
  free(bulk);
  free(strm);
}

static void check_indent(const char *name, const char *json, unsigned indent,
                         const char *expect) {
  char *bulk = NULL;
  int rc = convert(json, strlen(json), indent, &bulk);
  if (rc != JSON2TOON_OK || strcmp(bulk, expect) != 0) {
    g_fail++;
    printf("FAIL %s\n  expected: %s\n  got(%d):  %s\n", name, expect, rc,
           bulk ? bulk : "(null)");
  } else {
    g_pass++;
  }
  free(bulk);
}

int main(void) {
  /* primitives at root */
  check_ok("root-number", "42", "42\n");
  check_ok("root-negative", "-3.14", "-3.14\n");
  check_ok("root-string", "\"hello\"", "hello\n");
  check_ok("root-true", "true", "true\n");
  check_ok("root-false", "false", "false\n");
  check_ok("root-null", "null", "null\n");

  /* objects */
  check_ok("flat-object", "{\"name\":\"Alice\",\"age\":30}",
           "name: Alice\nage: 30\n");
  check_ok("nested-object", "{\"user\":{\"name\":\"Alice\"}}",
           "user:\n  name: Alice\n");
  check_ok("empty-object-root", "{}", "");
  check_ok("empty-object-field", "{\"meta\":{}}", "meta:\n");
  check_ok("bool-null-fields", "{\"a\":true,\"b\":null}", "a: true\nb: null\n");
  check_ok("number-passthrough", "{\"x\":1.50,\"z\":1e3}", "x: 1.50\nz: 1e3\n");
  check_ok("deep-object",
           "{\"a\":{\"b\":{\"c\":1}}}", "a:\n  b:\n    c: 1\n");

  /* whitespace insensitivity */
  check_ok("whitespace", "  {\n  \"a\" : 1 ,\n  \"b\":2\n}  ",
           "a: 1\nb: 2\n");

  /* primitive (inline) arrays */
  check_ok("inline-array", "{\"tags\":[\"a\",\"b\",\"c\"]}", "tags[3]: a,b,c\n");
  check_ok("inline-numbers", "{\"n\":[1,2,3]}", "n[3]: 1,2,3\n");
  check_ok("root-inline-array", "[1,2,3]", "[3]: 1,2,3\n");
  check_ok("empty-array-field", "{\"tags\":[]}", "tags: []\n");
  check_ok("empty-array-root", "[]", "[]\n");

  /* tabular arrays */
  check_ok("tabular",
           "{\"users\":[{\"id\":1,\"name\":\"Alice\",\"role\":\"admin\"},"
           "{\"id\":2,\"name\":\"Bob\",\"role\":\"user\"}]}",
           "users[2]{id,name,role}:\n  1,Alice,admin\n  2,Bob,user\n");
  check_ok("tabular-quoted-cell",
           "[{\"id\":1,\"url\":\"http://a:b\"}]",
           "[1]{id,url}:\n  1,\"http://a:b\"\n");
  check_ok("not-tabular-different-keys",
           "[{\"a\":1},{\"b\":2}]",
           "[2]:\n  - a: 1\n  - b: 2\n");

  /* list (expanded) arrays */
  check_ok("list-mixed", "[1,{\"a\":1},\"x\"]",
           "[3]:\n  - 1\n  - a: 1\n  - x\n");
  check_ok("array-of-arrays", "[[1,2],[3,4]]",
           "[2]:\n  - [2]: 1,2\n  - [2]: 3,4\n");
  check_ok("list-nested-object",
           "[{\"status\":\"active\",\"details\":{\"level\":\"high\",\"count\":5}}]",
           "[1]:\n  - status: active\n    details:\n"
           "      level: high\n      count: 5\n");

  /* string quoting */
  check_ok("empty-string", "{\"msg\":\"\"}", "msg: \"\"\n");
  check_ok("numeric-string", "{\"v\":\"42\"}", "v: \"42\"\n");
  check_ok("string-comma", "{\"v\":\"a,b\"}", "v: \"a,b\"\n");
  check_ok("string-colon", "{\"v\":\"a:b\"}", "v: \"a:b\"\n");
  check_ok("string-literal-collide", "{\"v\":\"true\"}", "v: \"true\"\n");
  check_ok("string-leading-space", "{\"v\":\" x\"}", "v: \" x\"\n");
  check_ok("string-hyphen", "{\"v\":\"-x\"}", "v: \"-x\"\n");
  check_ok("string-newline-escape", "{\"s\":\"a\\nb\"}", "s: \"a\\nb\"\n");
  check_ok("string-quote-escape", "{\"s\":\"a\\\"b\"}", "s: \"a\\\"b\"\n");
  check_ok("unicode-escape", "{\"s\":\"a\\u0041b\"}", "s: aAb\n");
  check_ok("unicode-multibyte", "{\"s\":\"\\u00e9\"}", "s: \xc3\xa9\n");

  /* keys requiring quoting */
  check_ok("key-dash", "{\"a-b\":1}", "\"a-b\": 1\n");
  check_ok("key-space", "{\"full name\":\"Ada\"}", "\"full name\": Ada\n");

  /* indent option */
  check_indent("indent-4", "{\"user\":{\"name\":\"Alice\"}}", 4,
               "user:\n    name: Alice\n");

  /* malformed input */
  check_err("err-empty", "");
  check_err("err-open-object", "{");
  check_err("err-open-array", "[1,2");
  check_err("err-missing-value", "{\"a\":}");
  check_err("err-trailing", "42 x");
  check_err("err-bad-literal", "nul");
  check_err("err-bare-word", "abc");
  check_err("err-unterminated-string", "\"ab");
  check_err("err-trailing-comma-array", "[1,2,]");
  check_err("err-double-comma", "{\"a\":1,,\"b\":2}");

  printf("\n%d passed, %d failed (SIMD backend reported at runtime)\n",
         g_pass, g_fail);
  return g_fail ? 1 : 0;
}
