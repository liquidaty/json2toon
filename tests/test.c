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

/* Direction vtable: forward and reverse drivers differ only in which push
 * functions they call (void* converter handles let one driver serve both). */
typedef struct {
  int (*feed)(void *c, const char *d, size_t n);
  int (*finish)(void *c);
  void (*destroy)(void *c);
} codec;
static int  j2t_feed(void *c, const char *d, size_t n) { return json2toon_feed(c, d, n); }
static int  j2t_fin(void *c) { return json2toon_finish(c); }
static void j2t_del(void *c) { json2toon_delete(c); }
static int  t2j_feed(void *c, const char *d, size_t n) { return toon2json_feed(c, d, n); }
static int  t2j_fin(void *c) { return toon2json_finish(c); }
static void t2j_del(void *c) { toon2json_delete(c); }
static const codec FWD = { j2t_feed, j2t_fin, j2t_del };
static const codec REV = { t2j_feed, t2j_fin, t2j_del };

/* Drive a freshly created converter to completion: feed `in` whole (chunk==0)
 * or in chunk-byte pieces, finish, destroy. *out gets a malloc'd NUL-terminated
 * copy of the output (caller frees). Returns the status code. */
static int drive(void *conv, const codec *c, sbuf *b, const char *in, size_t n,
                 size_t chunk, char **out) {
  int rc = JSON2TOON_OK;
  size_t i;
  if (!conv) { *out = NULL; return JSON2TOON_ERR_MEMORY; }
  if (chunk == 0)
    rc = c->feed(conv, in, n);
  else
    for (i = 0; i < n && rc == JSON2TOON_OK; i += chunk)
      rc = c->feed(conv, in + i, chunk < n - i ? chunk : n - i);
  if (rc == JSON2TOON_OK)
    rc = c->finish(conv);
  c->destroy(conv);
  if (!b->p) b->p = (char *)calloc(1, 1);
  *out = b->p;
  return rc;
}

/* JSON -> TOON, whole input and one byte at a time (to cross every boundary). */
static int convert(const char *json, size_t n, unsigned indent, char **out) {
  sbuf b = {0, 0, 0, 0};
  json2toon_options o; memset(&o, 0, sizeof o); o.indent = indent;
  return drive(json2toon_new(sbuf_sink, &b, &o), &FWD, &b, json, n, 0, out);
}
static int convert_streamed(const char *json, size_t n, unsigned indent, char **out) {
  sbuf b = {0, 0, 0, 0};
  json2toon_options o; memset(&o, 0, sizeof o); o.indent = indent;
  return drive(json2toon_new(sbuf_sink, &b, &o), &FWD, &b, json, n, 1, out);
}

/* TOON -> JSON: strict bulk, lenient bulk, and one-byte-streamed. */
static int convert_rev(const char *toon, size_t n, char **out) {
  sbuf b = {0, 0, 0, 0};
  return drive(toon2json_new(sbuf_sink, &b, NULL), &REV, &b, toon, n, 0, out);
}
static int convert_rev_lenient(const char *toon, size_t n, char **out) {
  sbuf b = {0, 0, 0, 0};
  toon2json_options o; memset(&o, 0, sizeof o); o.lenient = 1;
  return drive(toon2json_new(sbuf_sink, &b, &o), &REV, &b, toon, n, 0, out);
}
static int convert_rev_streamed(const char *toon, size_t n, char **out) {
  sbuf b = {0, 0, 0, 0};
  return drive(toon2json_new(sbuf_sink, &b, NULL), &REV, &b, toon, n, 1, out);
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

/* TOON -> JSON, checked in both bulk and one-byte-streamed modes. */
static void check_rev_ok(const char *name, const char *toon,
                         const char *expect) {
  char *bulk = NULL, *strm = NULL;
  int rc1, rc2;
  size_t n = strlen(toon);

  rc1 = convert_rev(toon, n, &bulk);
  if (rc1 != JSON2TOON_OK || strcmp(bulk, expect) != 0) {
    g_fail++;
    printf("FAIL rev %s\n  input:    %s\n  expected: %s\n  got(%d):  %s\n",
           name, toon, expect, rc1, bulk ? bulk : "(null)");
    free(bulk);
    return;
  }
  rc2 = convert_rev_streamed(toon, n, &strm);
  if (rc2 != JSON2TOON_OK || strcmp(strm, bulk) != 0) {
    g_fail++;
    printf("FAIL rev %s (streamed mismatch)\n  bulk:     %s\n  streamed(%d): %s\n",
           name, bulk, rc2, strm ? strm : "(null)");
  } else {
    g_pass++;
  }
  free(bulk);
  free(strm);
}

/* TOON -> JSON in lenient mode; expects success and an exact match. */
static void check_rev_lenient_ok(const char *name, const char *toon,
                                 const char *expect) {
  char *out = NULL;
  int rc = convert_rev_lenient(toon, strlen(toon), &out);
  if (rc != JSON2TOON_OK || strcmp(out, expect) != 0) {
    g_fail++;
    printf("FAIL rev-lenient %s\n  input:    %s\n  expected: %s\n  got(%d):  %s\n",
           name, toon, expect, rc, out ? out : "(null)");
  } else {
    g_pass++;
  }
  free(out);
}

static void check_rev_err(const char *name, const char *toon) {
  char *bulk = NULL, *strm = NULL;
  int rc1, rc2;
  size_t n = strlen(toon);
  rc1 = convert_rev(toon, n, &bulk);
  rc2 = convert_rev_streamed(toon, n, &strm);
  if (rc1 == JSON2TOON_OK || rc2 == JSON2TOON_OK) {
    g_fail++;
    printf("FAIL rev %s (expected error)\n  input: %s\n  bulk rc=%d streamed rc=%d\n",
           name, toon, rc1, rc2);
  } else {
    g_pass++;
  }
  free(bulk);
  free(strm);
}

/* Round trip: JSON -> TOON -> JSON must reproduce the original JSON exactly
 * (true for compact inputs without redundant unicode escapes). */
static void check_roundtrip(const char *name, const char *json) {
  char *toon = NULL, *back = NULL;
  int rc;
  rc = convert(json, strlen(json), 2, &toon);
  if (rc != JSON2TOON_OK) {
    g_fail++;
    printf("FAIL roundtrip %s (forward rc=%d)\n", name, rc);
    free(toon);
    return;
  }
  rc = convert_rev(toon, strlen(toon), &back);
  if (rc != JSON2TOON_OK || strcmp(back, json) != 0) {
    g_fail++;
    printf("FAIL roundtrip %s\n  json: %s\n  toon: %s\n  back(%d): %s\n",
           name, json, toon, rc, back ? back : "(null)");
  } else {
    g_pass++;
  }
  free(toon);
  free(back);
}

/* ------------------------------------------ bounded-memory array options */

/* Defined further below; forward-declared so this section can use them. */
static void cv_bool(const char *name, int ok);
static void check_fwd_limit(const char *name, const char *json,
                            const json2toon_options *opt, int want);

/* Forward conversion with explicit options, in both bulk and 1-byte-streamed
 * modes; asserts both succeed and equal `expect`. Used to exercise the spill /
 * temp-file paths and order-insensitive tabular detection. */
static void check_opts_ok(const char *name, const char *json,
                          const json2toon_options *o, const char *expect) {
  sbuf b1 = {0, 0, 0, 0}, b2 = {0, 0, 0, 0};
  char *o1 = NULL, *o2 = NULL;
  size_t n = strlen(json);
  int r1 = drive(json2toon_new(sbuf_sink, &b1, o), &FWD, &b1, json, n, 0, &o1);
  int r2 = drive(json2toon_new(sbuf_sink, &b2, o), &FWD, &b2, json, n, 1, &o2);
  int ok = r1 == JSON2TOON_OK && r2 == JSON2TOON_OK && o1 && o2 &&
           strcmp(o1, expect) == 0 && strcmp(o2, expect) == 0;
  if (!ok) {
    g_fail++;
    printf("FAIL %s\n  expected: %s\n  bulk(%d): %s\n  strm(%d): %s\n", name,
           expect, r1, o1 ? o1 : "(null)", r2, o2 ? o2 : "(null)");
  } else {
    g_pass++;
  }
  free(o1);
  free(o2);
}

/* The output must not depend on lookahead_buffer_size: a large (RAM-only)
 * window and a tiny (spilling) one must produce byte-identical TOON. The small
 * run also streams a byte at a time, crossing the spill boundary repeatedly. */
static void check_determinism(const char *name, const char *json) {
  json2toon_options big, small;
  sbuf bb = {0, 0, 0, 0}, bs = {0, 0, 0, 0};
  char *ob = NULL, *os = NULL;
  size_t n = strlen(json);
  int rb, rs;
  memset(&big, 0, sizeof big);
  memset(&small, 0, sizeof small);
  big.lookahead_buffer_size = 1u << 20;     /* all in RAM */
  small.lookahead_buffer_size = 8;          /* forces spill to a temp file */
  rb = drive(json2toon_new(sbuf_sink, &bb, &big), &FWD, &bb, json, n, 0, &ob);
  rs = drive(json2toon_new(sbuf_sink, &bs, &small), &FWD, &bs, json, n, 1, &os);
  cv_bool(name, rb == JSON2TOON_OK && rs == JSON2TOON_OK && ob && os &&
                    strcmp(ob, os) == 0);
  free(ob);
  free(os);
}

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
static char *dupstr(const char *s) {
  size_t n = strlen(s) + 1;
  char *p = (char *)malloc(n);
  if (p) memcpy(p, s, n);
  return p;
}
static int g_tmp_calls = 0;
static char g_tmp_last[256];
static char *test_tmpname(const char *prefix) {
  (void)prefix;
  snprintf(g_tmp_last, sizeof g_tmp_last, "/tmp/j2t_test_spill_%d.tmp",
           g_tmp_calls++);
  return dupstr(g_tmp_last);
}
static char *unwritable_tmpname(const char *prefix) {
  (void)prefix;
  return dupstr("/json2toon_no_such_dir/spill.tmp");
}
static char *existing_tmpname(const char *prefix) {
  (void)prefix;
  return dupstr("/tmp/j2t_test_existing_spill.tmp");
}
#endif

static void run_array_store_tests(void) {
  json2toon_options o;

  /* Determinism across the spill boundary for every array form. */
  check_determinism("spill-inline",
                    "{\"tags\":[\"alpha\",\"beta\",\"gamma\",\"delta\"]}");
  check_determinism("spill-tabular",
                    "[{\"id\":1,\"name\":\"Alice\"},{\"id\":2,\"name\":\"Bob\"}]");
  check_determinism("spill-list", "[1,{\"a\":1},\"a-fairly-long-scalar\"]");
  check_determinism("spill-nested",
                    "[[1,2,3,4,5,6],[7,8,9,10,11,12],[13,14,15,16,17,18]]");

  /* tmpfile() fallback (NULL hook) with a tiny window: still succeeds. */
  memset(&o, 0, sizeof o);
  o.lookahead_buffer_size = 4;
  check_opts_ok("spill-tmpfile-fallback", "[1,2,3,4,5,6,7,8,9,10]", &o,
                "[10]: 1,2,3,4,5,6,7,8,9,10\n");

  /* Order-insensitive tabular (TOON spec §9.3: same key SET, order may vary). */
  check_ok("tabular-reordered-keys", "[{\"a\":1,\"b\":2},{\"b\":4,\"a\":3}]",
           "[2]{a,b}:\n  1,2\n  3,4\n");
  /* Reordered keys round-trip to the canonical (template-order) JSON. */
  {
    const char *src = "[{\"a\":1,\"b\":2},{\"b\":4,\"a\":3}]";
    char *toon = NULL, *back = NULL;
    int rc = convert(src, strlen(src), 2, &toon);
    if (rc == JSON2TOON_OK)
      rc = convert_rev(toon, strlen(toon), &back);
    cv_bool("tabular-reordered-roundtrip",
            rc == JSON2TOON_OK && back &&
                strcmp(back, "[{\"a\":1,\"b\":2},{\"a\":3,\"b\":4}]") == 0);
    free(toon);
    free(back);
  }

  /* Set inequality declines tabular and falls back to a list. */
  check_ok("not-tabular-extra-key", "[{\"a\":1},{\"a\":1,\"b\":2}]",
           "[2]:\n  - a: 1\n  - a: 1\n    b: 2\n");
  check_ok("not-tabular-missing-key", "[{\"a\":1,\"b\":2},{\"a\":3}]",
           "[2]:\n  - a: 1\n    b: 2\n  - a: 3\n");
  /* Duplicate keys violate TOON's unique-key rule: reject on the bounded
   * array-capture path (4a) -- lone object, list element, row, nested array. */
  check_err("dup-key-single", "[{\"a\":1,\"a\":2}]");
  check_err("dup-key-list-element", "[1,{\"a\":1,\"a\":2}]");
  check_err("dup-key-tabular-row", "[{\"a\":1},{\"a\":2,\"a\":3}]");
  check_err("dup-key-nested-array", "[[{\"a\":1,\"a\":2}]]");

  /* Edge sweep: empty objects/arrays as list items. */
  check_ok("list-empty-object-item", "[{}]", "[1]:\n  -\n");
  check_ok("list-empty-array-item", "[[]]", "[1]:\n  - []\n");
  check_ok("list-two-empty-objects", "[{},{}]", "[2]:\n  -\n  -\n");

  /* Array nesting past max_depth -> ERR_DEPTH (now bounded like objects). */
  memset(&o, 0, sizeof o);
  o.max_depth = 3;
  check_fwd_limit("limit-fwd-array-depth", "[[[[1]]]]", &o,
                  JSON2TOON_ERR_DEPTH);
  memset(&o, 0, sizeof o);
  o.max_depth = 64;
  check_fwd_limit("limit-fwd-array-depth-ok", "[[[[1]]]]", &o, JSON2TOON_OK);

#if defined(__unix__) || defined(__APPLE__)
  /* get_temp_filename hook: it is called for a spilling array, and the temp
   * file it named is removed by the time conversion completes. */
  {
    char *out = NULL;
    struct stat st;
    sbuf b = {0, 0, 0, 0};
    int rc;
    memset(&o, 0, sizeof o);
    o.lookahead_buffer_size = 4;
    o.get_temp_filename = test_tmpname;
    g_tmp_calls = 0;
    rc = drive(json2toon_new(sbuf_sink, &b, &o), &FWD, &b,
               "[1,2,3,4,5,6,7,8,9,10]", 22, 0, &out);
    cv_bool("spill-hook-called", rc == JSON2TOON_OK && g_tmp_calls > 0);
    cv_bool("spill-hook-file-removed", stat(g_tmp_last, &st) != 0);
    free(out);
  }

  /* get_temp_filename pointing at an unwritable location -> ERR_IO. */
  {
    char *out = NULL;
    sbuf b = {0, 0, 0, 0};
    int rc;
    memset(&o, 0, sizeof o);
    o.lookahead_buffer_size = 4;
    o.get_temp_filename = unwritable_tmpname;
    rc = drive(json2toon_new(sbuf_sink, &b, &o), &FWD, &b,
               "[1,2,3,4,5,6,7,8,9,10]", 22, 0, &out);
    cv_bool("spill-io-error", rc == JSON2TOON_ERR_IO);
    free(out);
  }

  /* 4c: a temp name that already exists must be refused (O_EXCL), not silently
   * truncated as fopen(name,"w+b") would -- the predictable-name TOCTOU defense. */
  {
    char *out = NULL;
    sbuf b = {0, 0, 0, 0};
    int rc;
    FILE *pre = fopen("/tmp/j2t_test_existing_spill.tmp", "wb");
    if (pre) fclose(pre);
    memset(&o, 0, sizeof o);
    o.lookahead_buffer_size = 4;
    o.get_temp_filename = existing_tmpname;
    rc = drive(json2toon_new(sbuf_sink, &b, &o), &FWD, &b,
               "[1,2,3,4,5,6,7,8,9,10]", 22, 0, &out);
    cv_bool("spill-excl-existing", rc == JSON2TOON_ERR_IO);
    remove("/tmp/j2t_test_existing_spill.tmp");
    free(out);
  }
#endif
}

/* --------------------------------------------- stdio / convenience layer */

/* Read all of `f` (rewound to the start) into a malloc'd NUL-terminated
 * buffer. Returns NULL only on allocation failure. */
static char *slurp(FILE *f) {
  size_t cap = 256, len = 0;
  char *p = (char *)malloc(cap);
  if (!p)
    return NULL;
  rewind(f);
  for (;;) {
    size_t got;
    if (len + 1 >= cap) {
      char *np = (char *)realloc(p, cap * 2);
      if (!np) { free(p); return NULL; }
      p = np;
      cap *= 2;
    }
    got = fread(p + len, 1, cap - len - 1, f);
    len += got;
    if (got == 0)
      break;
  }
  p[len] = '\0';
  return p;
}

/* Compare a converter result; want==NULL means "don't check the output". */
static void cv_eq(const char *name, int rc, int want_rc,
                  const char *got, const char *want) {
  if (rc != want_rc || (want && (!got || strcmp(got, want) != 0))) {
    g_fail++;
    printf("FAIL %s\n  rc=%d (want %d)\n  got:  %s\n  want: %s\n",
           name, rc, want_rc, got ? got : "(null)", want ? want : "(any)");
  } else {
    g_pass++;
  }
}

static void cv_bool(const char *name, int ok) {
  if (ok) g_pass++;
  else { g_fail++; printf("FAIL %s\n", name); }
}

static void run_convenience_tests(void) {
  FILE *in, *out;
  char *got;
  size_t eoff, r;
  int rc;

  /* json2toon_file_sink: writes its bytes; len==0 is success, not a short write */
  out = tmpfile();
  if (out) {
    rc = json2toon_file_sink("hello", 5, out);
    got = slurp(out);
    cv_eq("sink-basic", rc, 0, got, "hello");
    free(got);
    cv_bool("sink-empty", json2toon_file_sink("", 0, out) == 0);
    fclose(out);
  } else cv_bool("sink-basic(tmpfile)", 0);

  /* convert_mem forward + reverse */
  out = tmpfile();
  if (out) {
    eoff = 12345;
    rc = json2toon_convert_mem("{\"a\":1}", 7, out, NULL, &eoff);
    got = slurp(out);
    cv_eq("mem-fwd", rc, JSON2TOON_OK, got, "a: 1\n");
    free(got); fclose(out);
  }
  out = tmpfile();
  if (out) {
    rc = toon2json_convert_mem("a: 1\n", 5, out, NULL, NULL);
    got = slurp(out);
    cv_eq("mem-rev", rc, JSON2TOON_OK, got, "{\"a\":1}");
    free(got); fclose(out);
  }

  /* convert_file forward via temp in/out */
  in = tmpfile(); out = tmpfile();
  if (in && out) {
    cv_bool("file-fwd(write)", fwrite("{\"a\":1}", 1, 7, in) == 7);
    rewind(in);
    rc = json2toon_convert_file(in, out, NULL, NULL);
    got = slurp(out);
    cv_eq("file-fwd", rc, JSON2TOON_OK, got, "a: 1\n");
    free(got);
  }
  if (in) fclose(in);
  if (out) fclose(out);

  /* empty input: forward -> ERR_PARSE @0 ; reverse -> OK "{}" (codec-faithful) */
  out = tmpfile();
  if (out) {
    eoff = 999;
    rc = json2toon_convert_mem("", 0, out, NULL, &eoff);
    cv_eq("mem-empty-fwd", rc, JSON2TOON_ERR_PARSE, NULL, NULL);
    cv_bool("mem-empty-fwd-offset", eoff == 0);
    fclose(out);
  }
  out = tmpfile();
  if (out) {
    rc = toon2json_convert_mem("", 0, out, NULL, NULL);
    got = slurp(out);
    cv_eq("mem-empty-rev", rc, JSON2TOON_OK, got, "{}");
    free(got); fclose(out);
  }
  in = tmpfile(); out = tmpfile();
  if (in && out) {                                  /* in left empty */
    rc = json2toon_convert_file(in, out, NULL, NULL);
    cv_eq("file-empty-fwd", rc, JSON2TOON_ERR_PARSE, NULL, NULL);
  }
  if (in) fclose(in);
  if (out) fclose(out);

  /* convert_mem NULL handling: NULL+len -> USAGE; NULL+0 -> empty doc */
  out = tmpfile();
  if (out) {
    rc = json2toon_convert_mem(NULL, 5, out, NULL, NULL);
    cv_eq("mem-null-usage", rc, JSON2TOON_ERR_USAGE, NULL, NULL);
    rc = json2toon_convert_mem(NULL, 0, out, NULL, NULL);
    cv_eq("mem-null-empty", rc, JSON2TOON_ERR_PARSE, NULL, NULL);
    fclose(out);
  }

  /* feed_fwrite drives a converter in chunks, then finish */
  out = tmpfile();
  if (out) {
    json2toon_t *j = json2toon_new(json2toon_file_sink, out, NULL);
    if (j) {
      cv_bool("fwrite-chunk1", json2toon_feed_fwrite("{\"a\"", 1, 4, j) == 4);
      cv_bool("fwrite-chunk2", json2toon_feed_fwrite(":1}", 1, 3, j) == 3);
      rc = json2toon_finish(j);
      got = slurp(out);
      cv_eq("fwrite-output", rc, JSON2TOON_OK, got, "a: 1\n");
      free(got);
      json2toon_delete(j);
    } else cv_bool("fwrite-new", 0);
    fclose(out);
  }

  /* feed_fwrite edge cases: overflow, nmemb==0, size==0, poisoned converter */
  out = tmpfile();
  if (out) {
    json2toon_t *j = json2toon_new(json2toon_file_sink, out, NULL);
    if (j) {
      r = json2toon_feed_fwrite("x", 2, ((size_t)-1) / 2 + 1, j);
      cv_bool("fwrite-overflow", r == 0);
      cv_bool("fwrite-nmemb0", json2toon_feed_fwrite("x", 1, 0, j) == 0);
      cv_bool("fwrite-size0", json2toon_feed_fwrite("x", 0, 7, j) == 7);
      json2toon_delete(j);
    }
    fclose(out);
  }
  out = tmpfile();
  if (out) {
    json2toon_t *j = json2toon_new(json2toon_file_sink, out, NULL);
    if (j) {                                        /* bare '}' is a parse error */
      cv_bool("fwrite-poison", json2toon_feed_fwrite("}", 1, 1, j) == 0);
      json2toon_delete(j);
    }
    fclose(out);
  }

#if defined(__unix__) || defined(__APPLE__)
  /* short write -> ERR_IO: a read-only stream cannot be written. Unbuffered so
   * the failure is reported at the sink call regardless of output size. */
  {
    FILE *ro = fopen("/dev/null", "r");
    if (ro) {
      setvbuf(ro, NULL, _IONBF, 0);
      rc = json2toon_convert_mem("{\"a\":1}", 7, ro, NULL, NULL);
      cv_eq("mem-io-error", rc, JSON2TOON_ERR_IO, NULL, NULL);
      fclose(ro);
    }
  }
#endif
}

/* ----------------------------------------------------- limit / DoS guards */

static void check_fwd_limit(const char *name, const char *json,
                            const json2toon_options *opt, int want) {
  sbuf b = {0, 0, 0, 0}; char *out = NULL;
  int rc = drive(json2toon_new(sbuf_sink, &b, opt), &FWD, &b, json, strlen(json), 0, &out);
  free(out);
  cv_bool(name, rc == want);
}

static void check_rev_limit(const char *name, const char *toon,
                            const toon2json_options *opt, int want) {
  sbuf b = {0, 0, 0, 0}; char *out = NULL;
  int rc = drive(toon2json_new(sbuf_sink, &b, opt), &REV, &b, toon, strlen(toon), 0, &out);
  free(out);
  cv_bool(name, rc == want);
}

/* The DEPTH/LIMIT guards exist for untrusted input; exercise both directions so
 * a future refactor cannot silently weaken them. */
static void run_limit_tests(void) {
  json2toon_options fo;
  toon2json_options ro;

  /* forward: object nesting deeper than max_depth -> ERR_DEPTH; the same input
   * within a generous limit converts cleanly (control). Array nesting is bounded
   * the same way now (see limit-fwd-array-depth in run_array_store_tests). */
  memset(&fo, 0, sizeof fo);
  fo.max_depth = 2;
  check_fwd_limit("limit-fwd-depth", "{\"a\":{\"b\":{\"c\":{\"d\":1}}}}", &fo,
                  JSON2TOON_ERR_DEPTH);
  memset(&fo, 0, sizeof fo);
  fo.max_depth = 64;
  check_fwd_limit("limit-fwd-depth-ok", "{\"a\":{\"b\":{\"c\":{\"d\":1}}}}", &fo,
                  JSON2TOON_OK);

  /* forward: a buffered primitive array larger than max_array_bytes -> LIMIT */
  memset(&fo, 0, sizeof fo);
  fo.max_array_bytes = 4;
  check_fwd_limit("limit-fwd-array", "[1,2,3,4,5,6,7,8,9,10,11,12]", &fo,
                  JSON2TOON_ERR_LIMIT);

  /* reverse: indentation deeper than max_depth -> ERR_DEPTH */
  memset(&ro, 0, sizeof ro);
  ro.max_depth = 2;
  check_rev_limit("limit-rev-depth", "a:\n  b:\n    c:\n      d: 1\n", &ro,
                  JSON2TOON_ERR_DEPTH);

  /* reverse: a single line longer than max_line_bytes -> ERR_LIMIT */
  memset(&ro, 0, sizeof ro);
  ro.max_line_bytes = 4;
  check_rev_limit("limit-rev-line",
                  "key: a fairly long scalar value exceeding the cap\n", &ro,
                  JSON2TOON_ERR_LIMIT);
}

/* Nest deeper than jsonwriter's initial 256-frame stack to exercise its
 * growth; expect success and one '{'/'}' pair per level. */
static void run_deep_nesting_test(void) {
  enum { DEPTH = 400 };
  toon2json_options o;
  sbuf b = {0, 0, 0, 0};
  char *toon, *out = NULL;
  size_t i, pos = 0, nopen = 0, nclose = 0;
  size_t cap = (size_t)DEPTH * (DEPTH + 8) + 32;
  int rc, ok = 1;

  toon = (char *)malloc(cap);
  if (!toon) { cv_bool("deep-nesting(alloc)", 0); return; }
  /* k:\n  k:\n ... <2*(DEPTH-1) spaces>k: 1\n  (DEPTH nested objects) */
  for (i = 0; i < (size_t)DEPTH; i++) {
    size_t j;
    for (j = 0; j < i * 2; j++) toon[pos++] = ' ';
    toon[pos++] = 'k';
    toon[pos++] = ':';
    if (i + 1 == (size_t)DEPTH) { toon[pos++] = ' '; toon[pos++] = '1'; }
    toon[pos++] = '\n';
  }
  memset(&o, 0, sizeof o);
  o.max_depth = DEPTH + 8;
  rc = drive(toon2json_new(sbuf_sink, &b, &o), &REV, &b, toon, pos, 0, &out);
  if (rc != JSON2TOON_OK || !out) {
    ok = 0;
  } else {
    for (i = 0; out[i]; i++) {
      if (out[i] == '{') nopen++;
      else if (out[i] == '}') nclose++;
    }
    ok = nopen == (size_t)DEPTH && nclose == (size_t)DEPTH;
  }
  cv_bool("deep-nesting-rev-beyond-256", ok);
  free(out);
  free(toon);
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

  /* empty-STRING key + array value: the captured-array path must emit the key
   * (`""`) -- it once used `key != NULL` to detect a member, but an empty key
   * has a NULL buffer pointer, so the key (and round-trip) was lost. */
  check_ok("empty-key-empty-array", "{\"\":[]}", "\"\": []\n");
  check_ok("empty-key-inline-array", "{\"\":[1,2]}", "\"\"[2]: 1,2\n");
  check_ok("empty-key-tabular-array", "{\"\":[{\"x\":1}]}", "\"\"[1]{x}:\n  1\n");
  check_ok("empty-key-array-then-field", "{\"\":[],\"b\":2}", "\"\": []\nb: 2\n");
  check_roundtrip("empty-key-array-roundtrip", "{\"\":[],\"tag$s\":[\"a\"]}");

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
  /* TOON spec §7.1 escape table is \" \\ \n \r \t; every other control byte
   * below 0x20 (backspace, form feed, ...) uses the \u00XX form */
  check_ok("string-backspace-escape", "{\"s\":\"a\\bb\"}", "s: \"a\\u0008b\"\n");
  check_ok("string-formfeed-escape", "{\"s\":\"a\\fb\"}", "s: \"a\\u000cb\"\n");
  check_ok("string-ctrl-hex", "{\"s\":\"\\u0001\"}", "s: \"\\u0001\"\n");
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

  /* ===================== TOON -> JSON (reverse) ===================== */

  /* root primitives */
  check_rev_ok("rev-number", "42\n", "42");
  check_rev_ok("rev-negative", "-3.14\n", "-3.14");
  check_rev_ok("rev-string", "hello\n", "\"hello\"");
  check_rev_ok("rev-string-spaces", "hello world\n", "\"hello world\"");
  check_rev_ok("rev-true", "true\n", "true");
  check_rev_ok("rev-false", "false\n", "false");
  check_rev_ok("rev-null", "null\n", "null");
  check_rev_ok("rev-no-trailing-newline", "42", "42");

  /* objects */
  check_rev_ok("rev-flat-object", "name: Alice\nage: 30\n",
               "{\"name\":\"Alice\",\"age\":30}");
  check_rev_ok("rev-nested-object", "user:\n  name: Alice\n",
               "{\"user\":{\"name\":\"Alice\"}}");
  check_rev_ok("rev-empty-input", "", "{}");
  check_rev_ok("rev-empty-object-field", "meta:\n", "{\"meta\":{}}");
  check_rev_ok("rev-empty-object-then-sibling", "meta:\nx: 1\n",
               "{\"meta\":{},\"x\":1}");
  check_rev_ok("rev-bool-null-fields", "a: true\nb: null\n",
               "{\"a\":true,\"b\":null}");
  check_rev_ok("rev-number-passthrough", "x: 1.50\nz: 1e3\n",
               "{\"x\":1.50,\"z\":1e3}");
  check_rev_ok("rev-deep-object", "a:\n  b:\n    c: 1\n",
               "{\"a\":{\"b\":{\"c\":1}}}");

  /* inline arrays */
  check_rev_ok("rev-inline-array", "tags[3]: a,b,c\n",
               "{\"tags\":[\"a\",\"b\",\"c\"]}");
  check_rev_ok("rev-inline-numbers", "n[3]: 1,2,3\n", "{\"n\":[1,2,3]}");
  check_rev_ok("rev-root-inline-array", "[3]: 1,2,3\n", "[1,2,3]");
  check_rev_ok("rev-empty-array-field", "tags: []\n", "{\"tags\":[]}");
  check_rev_ok("rev-empty-array-root", "[]\n", "[]");

  /* tabular arrays */
  check_rev_ok("rev-tabular",
               "users[2]{id,name,role}:\n  1,Alice,admin\n  2,Bob,user\n",
               "{\"users\":[{\"id\":1,\"name\":\"Alice\",\"role\":\"admin\"},"
               "{\"id\":2,\"name\":\"Bob\",\"role\":\"user\"}]}");
  check_rev_ok("rev-tabular-quoted-cell",
               "[1]{id,url}:\n  1,\"http://a:b\"\n",
               "[{\"id\":1,\"url\":\"http://a:b\"}]");
  check_rev_ok("rev-not-tabular-different-keys",
               "[2]:\n  - a: 1\n  - b: 2\n",
               "[{\"a\":1},{\"b\":2}]");

  /* list (expanded) arrays */
  check_rev_ok("rev-list-mixed", "[3]:\n  - 1\n  - a: 1\n  - x\n",
               "[1,{\"a\":1},\"x\"]");
  check_rev_ok("rev-array-of-arrays", "[2]:\n  - [2]: 1,2\n  - [2]: 3,4\n",
               "[[1,2],[3,4]]");
  check_rev_ok("rev-list-nested-object",
               "[1]:\n  - status: active\n    details:\n"
               "      level: high\n      count: 5\n",
               "[{\"status\":\"active\",\"details\":"
               "{\"level\":\"high\",\"count\":5}}]");

  /* string quoting / escapes */
  check_rev_ok("rev-empty-string", "msg: \"\"\n", "{\"msg\":\"\"}");
  check_rev_ok("rev-numeric-string", "v: \"42\"\n", "{\"v\":\"42\"}");
  check_rev_ok("rev-string-comma", "v: \"a,b\"\n", "{\"v\":\"a,b\"}");
  check_rev_ok("rev-string-colon", "v: \"a:b\"\n", "{\"v\":\"a:b\"}");
  check_rev_ok("rev-string-literal-collide", "v: \"true\"\n",
               "{\"v\":\"true\"}");
  check_rev_ok("rev-string-newline-escape", "s: \"a\\nb\"\n",
               "{\"s\":\"a\\nb\"}");
  check_rev_ok("rev-string-quote-escape", "s: \"a\\\"b\"\n",
               "{\"s\":\"a\\\"b\"}");
  check_rev_ok("rev-string-backspace-escape", "s: \"a\\u0008b\"\n",
               "{\"s\":\"a\\bb\"}");
  check_rev_ok("rev-string-formfeed-escape", "s: \"a\\u000cb\"\n",
               "{\"s\":\"a\\fb\"}");
  /* spec §7.1: \b, \f and \/ are NOT in the escape table -> MUST be rejected */
  check_rev_err("rev-err-nonspec-escape-b", "s: \"a\\bb\"\n");
  check_rev_err("rev-err-nonspec-escape-f", "s: \"a\\fb\"\n");
  check_rev_err("rev-err-nonspec-escape-slash", "s: \"a\\/b\"\n");
  check_rev_ok("rev-string-ctrl-hex", "s: \"\\u0001\"\n",
               "{\"s\":\"\\u0001\"}");
  check_rev_ok("rev-unicode-multibyte", "s: \xc3\xa9\n", "{\"s\":\"\xc3\xa9\"}");

  /* quoted keys */
  check_rev_ok("rev-key-dash", "\"a-b\": 1\n", "{\"a-b\":1}");
  check_rev_ok("rev-key-space", "\"full name\": Ada\n",
               "{\"full name\":\"Ada\"}");

  /* CRLF tolerance */
  check_rev_ok("rev-crlf", "a: 1\r\nb: 2\r\n", "{\"a\":1,\"b\":2}");

  /* empty object round-trips standalone (Fix 1) */
  check_rev_ok("rev-whitespace-only", "   \n\n  \n", "{}");
  check_roundtrip("rt-empty-object", "{}");

  /* malformed TOON */
  check_rev_err("rev-err-unterminated-quote", "v: \"abc\n");
  check_rev_err("rev-err-junk-colons", ":::garbage:::\n");
  check_rev_err("rev-err-junk-value", "a: b:c\n");
  check_rev_err("rev-err-junk-bracket", "x: a]b\n");

  /* the same junk is accepted as a bare string under --lenient */
  check_rev_lenient_ok("rev-lenient-junk-colons", ":::garbage:::\n",
                       "\":::garbage:::\"");
  check_rev_lenient_ok("rev-lenient-junk-value", "a: b:c\n",
                       "{\"a\":\"b:c\"}");
  /* well-formed bare strings still convert identically in either mode */
  check_rev_lenient_ok("rev-lenient-plain", "hello world\n",
                       "\"hello world\"");
  check_rev_err("rev-err-trailing-after-scalar", "42\nx\n");
  check_rev_err("rev-err-bad-tabular-arity", "[2]{a,b}:\n  1\n");
  check_rev_err("rev-err-count-mismatch-inline", "n[3]: 1,2\n");
  /* 4b: 2^64+1 wraps to 1 in size_t; without the saturating guard it would
   * spuriously match the single element and be accepted. */
  check_rev_err("rev-err-count-overflow", "[18446744073709551617]: 5\n");
  check_rev_err("rev-err-item-in-object", "a: 1\n- 2\n");

  /* round trips through both directions */
  check_roundtrip("rt-flat", "{\"name\":\"Alice\",\"age\":30}");
  check_roundtrip("rt-nested", "{\"a\":{\"b\":{\"c\":1}}}");
  check_roundtrip("rt-tabular",
                  "{\"users\":[{\"id\":1,\"name\":\"Alice\"},"
                  "{\"id\":2,\"name\":\"Bob\"}]}");
  check_roundtrip("rt-inline", "{\"n\":[1,2,3],\"t\":[\"x\",\"y\"]}");
  check_roundtrip("rt-list-mixed", "[1,{\"a\":1},\"x\"]");
  check_roundtrip("rt-array-of-arrays", "[[1,2],[3,4]]");
  check_roundtrip("rt-strings",
                  "{\"a\":\"a,b\",\"b\":\"a:b\",\"c\":\"true\",\"d\":\"\"}");
  check_roundtrip("rt-control-escapes",
                  "{\"a\":\"x\\by\\fz\",\"b\":\"\\t\\n\\r\\u0001\"}");
  check_roundtrip("rt-quoted-keys", "{\"a-b\":1,\"full name\":\"Ada\"}");
  check_roundtrip("rt-empty-array", "{\"tags\":[]}");
  check_roundtrip("rt-deep-list",
                  "[{\"status\":\"active\",\"details\":"
                  "{\"level\":\"high\",\"count\":5}}]");

  /* Invalid UTF-8 is rejected on the forward (input) side, so every string
   * json2toon emits re-parses as TOON (the fuzz round-trip oracle). These are
   * the sequences YAJL's classic lexer check let through but yatl (the reverse
   * parser) rejects: overlong forms, raw surrogate bytes and code points above
   * U+10FFFF. Rejection is checked in bulk and 1-byte-streamed modes. */
  check_err("utf8-overlong-2", "\"\xc0\x88\"");         /* overlong U+0008 */
  check_err("utf8-lead-c1", "\"\xc1\x92\"");            /* 0xc1 lead (overlong) */
  check_err("utf8-overlong-3", "\"\xe0\x80\x80\"");     /* overlong 3-byte */
  check_err("utf8-overlong-4", "\"\xf0\x80\x80\x80\""); /* overlong 4-byte */
  check_err("utf8-raw-surrogate", "\"\xed\xa0\x80\""); /* U+D800 as raw bytes */
  check_err("utf8-gt-10ffff", "\"\xf5\x80\x80\x80\""); /* > U+10FFFF */
  check_err("utf8-lone-continuation", "\"\x80\"");     /* stray 0x80 */

  /* Escaped unpaired surrogates decode to '?' (lossy, YAJL-idiomatic) rather
   * than an invalid surrogate byte sequence, so they round-trip as valid UTF-8.
   * A valid pair decodes to its astral code point (emoji U+1F600). */
  check_ok("utf8-esc-lone-high", "\"\\uD800\"", "?\n");
  check_ok("utf8-esc-lone-low", "\"\\uDC00\"", "?\n");
  check_ok("utf8-esc-pair", "\"\\uD83D\\uDE00\"", "\xf0\x9f\x98\x80\n");

  /* An embedded NUL is data, not a terminator: it survives the forward path
   * (escaped to a hex escape in TOON) and, crucially, the reverse path must not
   * truncate the string at it -- so the full value round-trips. */
  check_roundtrip("rt-embedded-nul", "\"a\\u0000b\"");
  check_roundtrip("rt-embedded-nul-key", "{\"k\\u0000y\":\"v\\u0000w\"}");

  run_array_store_tests();
  run_convenience_tests();
  run_limit_tests();
  run_deep_nesting_test();

  printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
