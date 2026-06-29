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

/* Discard sink: 0 == keep going. */
static int null_sink(const char *data, size_t len, void *ctx) {
  (void)data; (void)len; (void)ctx;
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

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t len) {
  run_one(&J2T, data, len);   /* as JSON */
  run_one(&T2J, data, len);   /* as TOON */
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
