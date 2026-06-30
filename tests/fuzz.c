/* json2toon - fuzz harness.
 *
 * One libFuzzer entry point drives BOTH converters over arbitrary bytes in
 * input-derived chunk sizes (to exercise chunk boundaries); the sink discards
 * output -- we fuzz the parsers, not the writers.
 *
 * Build (see the Makefile): `make fuzz` (libFuzzer/clang), or `make
 * fuzz-standalone` for a portable driver that replays argv files / stdin once
 * each through the same entry point (smoke-buildable with any compiler).
 */
#include "json2toon.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Discard sink: 0 == keep going. */
static int null_sink(const char *data, size_t len, void *ctx) {
  (void)data; (void)len; (void)ctx;
  return 0;
}

/* Growable capture sink for the round-trip oracle. Returns -1 (and flags oom)
 * on allocation failure, which the converter surfaces as a sticky error. */
typedef struct { char *p; size_t len, cap; int oom; } cap_sink;
static int cap_write(const char *data, size_t len, void *ctx) {
  cap_sink *c = (cap_sink *)ctx;
  if (c->len + len > c->cap) {
    size_t nc = c->cap ? c->cap : 256;
    char *nb;
    while (nc < c->len + len)
      nc *= 2;
    nb = (char *)realloc(c->p, nc);
    if (!nb) { c->oom = 1; return -1; }
    c->p = nb;
    c->cap = nc;
  }
  memcpy(c->p + c->len, data, len);
  c->len += len;
  return 0;
}

/* Per-direction vtable so one driver fuzzes both converters. */
typedef struct {
  void *(*create)(void);
  int   (*feed)(void *c, const char *d, size_t n);
  int   (*finish)(void *c);
  void  (*destroy)(void *c);
} codec;
static void *j_new(void) { return json2toon_new(null_sink, NULL, NULL); }
static int   j_feed(void *c, const char *d, size_t n) { return json2toon_feed(c, d, n); }
static int   j_fin(void *c) { return json2toon_finish(c); }
static void  j_del(void *c) { json2toon_delete(c); }
static void *t_new(void) { return toon2json_new(null_sink, NULL, NULL); }
static int   t_feed(void *c, const char *d, size_t n) { return toon2json_feed(c, d, n); }
static int   t_fin(void *c) { return toon2json_finish(c); }
static void  t_del(void *c) { toon2json_delete(c); }
static const codec J2T = { j_new, j_feed, j_fin, j_del };
static const codec T2J = { t_new, t_feed, t_fin, t_del };

/* Feed [data,len) through one converter in input-derived 1..17 byte chunks,
 * then finish + destroy. */
static void run_one(const codec *c, const uint8_t *data, size_t len) {
  void *conv = c->create();
  size_t off = 0;
  if (!conv)
    return;
  while (off < len) {
    size_t chunk = (size_t)(data[off] % 17) + 1;
    if (chunk > len - off)
      chunk = len - off;
    if (c->feed(conv, (const char *)data + off, chunk) != JSON2TOON_OK)
      break;
    off += chunk;
  }
  c->finish(conv);
  c->destroy(conv);
}

/* Feed [in,n) whole through one converter into `out`; returns the converter's
 * final status. (Chunk-boundary coverage is run_one's job; the round-trip
 * property is chunk-independent.) */
static int run_fwd(const char *in, size_t n, cap_sink *out) {
  json2toon_t *j = json2toon_new(cap_write, out, NULL);
  int rc;
  if (!j)
    return JSON2TOON_ERR_MEMORY;
  rc = json2toon_feed(j, in, n);
  if (rc == JSON2TOON_OK)
    rc = json2toon_finish(j);
  json2toon_delete(j);
  return rc;
}
static int run_rev(const char *in, size_t n, cap_sink *out) {
  toon2json_t *t = toon2json_new(cap_write, out, NULL);
  int rc;
  if (!t)
    return JSON2TOON_ERR_MEMORY;
  rc = toon2json_feed(t, in, n);
  if (rc == JSON2TOON_OK)
    rc = toon2json_finish(t);
  toon2json_delete(t);
  return rc;
}

/* Round-trip oracle (acceptance #1). For any input json2toon accepts as a
 * complete JSON document, the TOON it emits MUST round-trip: it must re-parse
 * as valid JSON, and re-encoding that JSON must reproduce byte-identical TOON
 * (TOON is the canonical form). A failure means the converter reported success
 * while emitting non-round-trippable output -- the silent-miscompile class this
 * test exists to pin. Any abort() is a real find. */
static void run_roundtrip(const uint8_t *data, size_t len) {
  cap_sink toon = {0}, json = {0}, toon2 = {0};
  if (run_fwd((const char *)data, len, &toon) != JSON2TOON_OK || toon.oom)
    goto done;                       /* x is not JSON we accept: nothing to check */
  if (run_rev(toon.p, toon.len, &json) != JSON2TOON_OK || json.oom)
    abort();                         /* emitted TOON does not parse: miscompile */
  if (run_fwd(json.p, json.len, &toon2) != JSON2TOON_OK || toon2.oom)
    abort();                         /* emitted JSON is itself invalid: miscompile */
  if (toon.len != toon2.len || memcmp(toon.p, toon2.p, toon.len) != 0)
    abort();                         /* round-trip changed structure: miscompile */
done:
  free(toon.p);
  free(json.p);
  free(toon2.p);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t len) {
  run_one(&J2T, data, len);   /* as JSON */
  run_one(&T2J, data, len);   /* as TOON */
  run_roundtrip(data, len);   /* json2toon(x) must round-trip */
  return 0;
}

#ifdef J2T_FUZZ_STANDALONE
/* Minimal libFuzzer-compatible standalone driver: run each argv file (or stdin
 * if none) through LLVMFuzzerTestOneInput exactly once. Lets the harness be
 * built and exercised with any compiler, no libFuzzer required. */
#include <stdio.h>
#include <stdlib.h>

static int run_stream(FILE *f) {
  size_t cap = 4096, len = 0;
  unsigned char *buf = (unsigned char *)malloc(cap);
  if (!buf)
    return -1;
  for (;;) {
    size_t got;
    if (len == cap) {
      unsigned char *nb = (unsigned char *)realloc(buf, cap * 2);
      if (!nb) { free(buf); return -1; }
      buf = nb;
      cap *= 2;
    }
    got = fread(buf + len, 1, cap - len, f);
    len += got;
    if (got == 0)
      break;
  }
  LLVMFuzzerTestOneInput(buf, len);
  free(buf);
  return 0;
}

int main(int argc, char **argv) {
  int i;
  if (argc < 2)
    return run_stream(stdin) == 0 ? 0 : 1;
  for (i = 1; i < argc; i++) {
    FILE *f = fopen(argv[i], "rb");
    if (!f) {
      fprintf(stderr, "fuzz: cannot open '%s'\n", argv[i]);
      return 1;
    }
    if (run_stream(f) != 0) { fclose(f); return 1; }
    fclose(f);
  }
  return 0;
}
#endif /* J2T_FUZZ_STANDALONE */
