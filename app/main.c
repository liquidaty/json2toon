/* json2toon - command-line filter: JSON on stdin/-i, TOON on stdout/-o. */
#include "json2toon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *prog = "json2toon";

static void usage(FILE *f) {
  fprintf(f,
    "Usage: %s [OPTIONS]\n"
    "Convert JSON (stdin) to TOON (stdout).\n\n"
    "  -i, --input FILE    read JSON from FILE instead of stdin\n"
    "  -o, --output FILE   write TOON to FILE instead of stdout\n"
    "      --indent N      spaces per indentation level (default 2)\n"
    "  -h, --help          show this help and exit\n"
    "  -V, --version       show version and exit\n",
    prog);
}

static int write_sink(const char *data, size_t len, void *ctx) {
  FILE *out = (FILE *)ctx;
  return fwrite(data, 1, len, out) == len ? 0 : -1;
}

int main(int argc, char **argv) {
  const char *in_path = NULL, *out_path = NULL;
  json2toon_options opt;
  json2toon_t *j2t;
  FILE *in = stdin, *out = stdout;
  char *buf;
  size_t n;
  int rc = JSON2TOON_OK, i, status = 0;

  memset(&opt, 0, sizeof opt);

  for (i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (!strcmp(a, "-i") || !strcmp(a, "--input")) {
      if (++i >= argc) { fprintf(stderr, "%s: %s needs an argument\n", prog, a); return 2; }
      in_path = argv[i];
    } else if (!strcmp(a, "-o") || !strcmp(a, "--output")) {
      if (++i >= argc) { fprintf(stderr, "%s: %s needs an argument\n", prog, a); return 2; }
      out_path = argv[i];
    } else if (!strcmp(a, "--indent")) {
      if (++i >= argc) { fprintf(stderr, "%s: --indent needs an argument\n", prog); return 2; }
      opt.indent = (unsigned)strtoul(argv[i], NULL, 10);
    } else if (!strncmp(a, "--indent=", 9)) {
      opt.indent = (unsigned)strtoul(a + 9, NULL, 10);
    } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
      usage(stdout);
      return 0;
    } else if (!strcmp(a, "-V") || !strcmp(a, "--version")) {
      printf("%s %s\n", prog, json2toon_version());
      return 0;
    } else {
      fprintf(stderr, "%s: unknown option '%s'\n", prog, a);
      usage(stderr);
      return 2;
    }
  }

  if (in_path) {
    in = fopen(in_path, "rb");
    if (!in) { fprintf(stderr, "%s: cannot open '%s' for reading\n", prog, in_path); return 2; }
  }
  if (out_path) {
    out = fopen(out_path, "wb");
    if (!out) {
      fprintf(stderr, "%s: cannot open '%s' for writing\n", prog, out_path);
      if (in != stdin) fclose(in);
      return 2;
    }
  }

  j2t = json2toon_new(write_sink, out, &opt);
  if (!j2t) {
    fprintf(stderr, "%s: out of memory\n", prog);
    status = 1;
    goto done;
  }

  buf = (char *)malloc(65536);
  if (!buf) {
    fprintf(stderr, "%s: out of memory\n", prog);
    json2toon_delete(j2t);
    status = 1;
    goto done;
  }

  while ((n = fread(buf, 1, 65536, in)) > 0) {
    rc = json2toon_feed(j2t, buf, n);
    if (rc != JSON2TOON_OK)
      break;
  }
  if (rc == JSON2TOON_OK && ferror(in)) {
    fprintf(stderr, "%s: read error\n", prog);
    status = 1;
  } else if (rc == JSON2TOON_OK) {
    rc = json2toon_finish(j2t);
  }

  if (rc != JSON2TOON_OK) {
    fprintf(stderr, "%s: %s at byte offset %lu\n", prog,
            json2toon_strerror(rc),
            (unsigned long)json2toon_error_offset(j2t));
    status = 1;
  }

  free(buf);
  json2toon_delete(j2t);

done:
  if (in != stdin) fclose(in);
  if (out != stdout) { if (fclose(out) != 0 && status == 0) status = 1; }
  else fflush(out);
  return status;
}
