/* json2toon - command-line filter.
 *
 * Default direction: JSON on stdin/-i -> TOON on stdout/-o.
 * With -r/--reverse:  TOON on stdin/-i -> JSON on stdout/-o.
 */
#include "json2toon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *prog = "json2toon";

static void usage(FILE *f) {
  fprintf(f,
    "Usage: %s [OPTIONS]\n"
    "Convert JSON (stdin) to TOON (stdout), or the reverse with -r.\n\n"
    "  -i, --input FILE    read input from FILE instead of stdin\n"
    "  -o, --output FILE   write output to FILE instead of stdout\n"
    "  -r, --reverse       convert TOON to JSON (default is JSON to TOON)\n"
    "      --indent N      spaces per indentation level for TOON (default 2)\n"
    "  -h, --help          show this help and exit\n"
    "  -V, --version       show version and exit\n",
    prog);
}

static int write_sink(const char *data, size_t len, void *ctx) {
  FILE *out = (FILE *)ctx;
  return fwrite(data, 1, len, out) == len ? 0 : -1;
}

/* Run the JSON -> TOON direction. Returns a json2toon status code. */
static int run_forward(FILE *in, FILE *out, unsigned indent, char *buf,
                       size_t bufsz, size_t *err_off) {
  json2toon_options opt;
  json2toon_t *j2t;
  size_t n;
  int rc = JSON2TOON_OK;

  memset(&opt, 0, sizeof opt);
  opt.indent = indent;
  j2t = json2toon_new(write_sink, out, &opt);
  if (!j2t)
    return JSON2TOON_ERR_MEMORY;

  while ((n = fread(buf, 1, bufsz, in)) > 0) {
    rc = json2toon_feed(j2t, buf, n);
    if (rc != JSON2TOON_OK)
      break;
  }
  if (rc == JSON2TOON_OK && ferror(in))
    rc = JSON2TOON_ERR_IO;
  else if (rc == JSON2TOON_OK)
    rc = json2toon_finish(j2t);

  if (rc != JSON2TOON_OK)
    *err_off = json2toon_error_offset(j2t);
  json2toon_delete(j2t);
  return rc;
}

/* Run the TOON -> JSON direction. Returns a json2toon status code. */
static int run_reverse(FILE *in, FILE *out, char *buf, size_t bufsz,
                       size_t *err_off) {
  toon2json_t *t2j;
  size_t n;
  int rc = JSON2TOON_OK;

  t2j = toon2json_new(write_sink, out, NULL);
  if (!t2j)
    return JSON2TOON_ERR_MEMORY;

  while ((n = fread(buf, 1, bufsz, in)) > 0) {
    rc = toon2json_feed(t2j, buf, n);
    if (rc != JSON2TOON_OK)
      break;
  }
  if (rc == JSON2TOON_OK && ferror(in))
    rc = JSON2TOON_ERR_IO;
  else if (rc == JSON2TOON_OK)
    rc = toon2json_finish(t2j);

  if (rc != JSON2TOON_OK)
    *err_off = toon2json_error_offset(t2j);
  toon2json_delete(t2j);
  return rc;
}

int main(int argc, char **argv) {
  const char *in_path = NULL, *out_path = NULL;
  unsigned indent = 0;
  int reverse = 0;
  FILE *in = stdin, *out = stdout;
  char *buf;
  size_t err_off = 0;
  int rc, i, status = 0;

  for (i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (!strcmp(a, "-i") || !strcmp(a, "--input")) {
      if (++i >= argc) { fprintf(stderr, "%s: %s needs an argument\n", prog, a); return 2; }
      in_path = argv[i];
    } else if (!strcmp(a, "-o") || !strcmp(a, "--output")) {
      if (++i >= argc) { fprintf(stderr, "%s: %s needs an argument\n", prog, a); return 2; }
      out_path = argv[i];
    } else if (!strcmp(a, "-r") || !strcmp(a, "--reverse")) {
      reverse = 1;
    } else if (!strcmp(a, "--indent")) {
      if (++i >= argc) { fprintf(stderr, "%s: --indent needs an argument\n", prog); return 2; }
      indent = (unsigned)strtoul(argv[i], NULL, 10);
    } else if (!strncmp(a, "--indent=", 9)) {
      indent = (unsigned)strtoul(a + 9, NULL, 10);
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

  buf = (char *)malloc(65536);
  if (!buf) {
    fprintf(stderr, "%s: out of memory\n", prog);
    status = 1;
    goto done;
  }

  if (reverse)
    rc = run_reverse(in, out, buf, 65536, &err_off);
  else
    rc = run_forward(in, out, indent, buf, 65536, &err_off);

  if (rc != JSON2TOON_OK) {
    if (rc == JSON2TOON_ERR_IO && ferror(in)) {
      fprintf(stderr, "%s: read error\n", prog);
    } else {
      const char *msg = reverse ? toon2json_strerror(rc)
                                : json2toon_strerror(rc);
      fprintf(stderr, "%s: %s at byte offset %lu\n", prog, msg,
              (unsigned long)err_off);
    }
    status = 1;
  }

  free(buf);

done:
  if (in != stdin) fclose(in);
  if (out != stdout) { if (fclose(out) != 0 && status == 0) status = 1; }
  else fflush(out);
  return status;
}
