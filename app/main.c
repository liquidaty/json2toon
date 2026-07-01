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
    "      --lenient       (with -r) accept loosely-formatted TOON: treat any\n"
    "                      unquoted value as a string instead of rejecting it\n"
    "      --indent N      spaces per indentation level for TOON (default 2)\n"
    "  -h, --help          show this help and exit\n"
    "  -V, --version       show version and exit\n",
    prog);
}

/* Run the JSON -> TOON direction via the library's whole-FILE converter. */
static int run_forward(FILE *in, FILE *out, unsigned indent, size_t *err_off) {
  return json2toon_convert_file(in, out,
                                &(json2toon_options){ .indent = indent }, err_off);
}

/* Run the TOON -> JSON direction via the library's whole-FILE converter. */
static int run_reverse(FILE *in, FILE *out, int lenient, size_t *err_off) {
  return toon2json_convert_file(in, out,
                                &(toon2json_options){ .lenient = lenient }, err_off);
}

int main(int argc, char **argv) {
  const char *in_path = NULL, *out_path = NULL;
  unsigned indent = 0;
  int reverse = 0;
  int lenient = 0;
  FILE *in = stdin, *out = stdout;
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
    } else if (!strcmp(a, "--lenient")) {
      lenient = 1;
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

  if (reverse)
    rc = run_reverse(in, out, lenient, &err_off);
  else
    rc = run_forward(in, out, indent, &err_off);

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

  if (in != stdin) fclose(in);
  if (out != stdout) { if (fclose(out) != 0 && status == 0) status = 1; }
  else fflush(out);
  return status;
}
