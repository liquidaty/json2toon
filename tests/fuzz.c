/* json2toon - fuzz harness.
 *
 * One libFuzzer entry point drives BOTH converters over arbitrary bytes, fed in
 * input-derived chunk sizes so chunk-boundary handling is exercised (the same
 * property the one-byte-streamed unit tests check, but over random data). The
 * output sink discards everything; we are fuzzing the parsers, not the writers.
 *
 * Two build modes (see the Makefile):
 *   make fuzz             libFuzzer build (clang, -fsanitize=fuzzer,address,...)
 *   make fuzz-standalone  portable driver: replays files named on argv (or
 *                         stdin) through the same entry point once each, so the
 *                         harness can be smoke-built and run with plain gcc.
 */
#include "json2toon.h"

#include <stddef.h>
#include <stdint.h>

/* Discard sink: 0 == keep going. */
static int null_sink(const char *data, size_t len, void *ctx) {
  (void)data; (void)len; (void)ctx;
  return 0;
}

/* Feed [data,len) to a freshly created converter of the given direction in
 * chunks of 1..17 bytes (size derived from the data), then finish + destroy.
 * `dir` 0 == json2toon, non-zero == toon2json. */
static void run_one(int dir, const uint8_t *data, size_t len) {
  size_t off = 0;
  if (dir == 0) {
    json2toon_t *j = json2toon_new(null_sink, NULL, NULL);
    if (!j)
      return;
    while (off < len) {
      size_t chunk = (size_t)(data[off] % 17) + 1;
      if (chunk > len - off)
        chunk = len - off;
      if (json2toon_feed(j, (const char *)data + off, chunk) != JSON2TOON_OK)
        break;
      off += chunk;
    }
    json2toon_finish(j);
    json2toon_delete(j);
  } else {
    toon2json_t *t = toon2json_new(null_sink, NULL, NULL);
    if (!t)
      return;
    while (off < len) {
      size_t chunk = (size_t)(data[off] % 17) + 1;
      if (chunk > len - off)
        chunk = len - off;
      if (toon2json_feed(t, (const char *)data + off, chunk) != JSON2TOON_OK)
        break;
      off += chunk;
    }
    toon2json_finish(t);
    toon2json_delete(t);
  }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t len) {
  run_one(0, data, len);   /* as JSON  */
  run_one(1, data, len);   /* as TOON  */
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
