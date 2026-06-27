/* json2toon - command-line filter: JSON on stdin/-i, TOON on stdout/-o. */
#include "json2toon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *prog = "json2toon";

/* ---------------------------------------------------------------- help model
 *
 * One source of truth for the program's self-description. The plain-text
 * usage(), the JSON help (--help-json) and the TOON help (--help-toon) are all
 * rendered from this table, so they can never drift apart. */

static const char *const help_summary =
  "Convert JSON (stdin) to TOON (stdout).";
static const char *const help_usage = "json2toon [OPTIONS]";

typedef struct {
  const char *shortf;   /* "-i", or NULL when there is no short form */
  const char *longf;    /* "--input" (always present) */
  const char *arg;      /* metavar, e.g. "FILE"/"N", or NULL if the option
                         * takes no argument */
  const char *def;      /* default value as text, or NULL when not applicable */
  const char *desc;     /* one-line description */
} opt_info;

static const opt_info options[] = {
  {"-i", "--input",     "FILE", NULL, "read JSON from FILE instead of stdin"},
  {"-o", "--output",    "FILE", NULL, "write TOON to FILE instead of stdout"},
  {NULL, "--indent",    "N",    "2",  "spaces per indentation level"},
  {NULL, "--help-json", NULL,   NULL, "show this help as JSON and exit"},
  {NULL, "--help-toon", NULL,   NULL, "show this help as TOON and exit"},
  {"-h", "--help",      NULL,   NULL, "show this help and exit"},
  {"-V", "--version",   NULL,   NULL, "show version and exit"}
};
static const size_t n_options = sizeof options / sizeof options[0];

/* ------------------------------------------------------------ plain-text help */

/* Build the left "synopsis" column for one option, e.g. "  -i, --input FILE"
 * or "      --indent N". Truncation-safe via snprintf. */
static void opt_synopsis(char *buf, size_t cap, const opt_info *o) {
  char head[16];
  if (o->shortf)
    snprintf(head, sizeof head, "  %s, ", o->shortf);
  else
    snprintf(head, sizeof head, "      ");
  snprintf(buf, cap, "%s%s%s%s", head, o->longf,
           o->arg ? " " : "", o->arg ? o->arg : "");
}

static void usage(FILE *f) {
  size_t i;
  fprintf(f, "Usage: %s\n%s\n\n", help_usage, help_summary);
  for (i = 0; i < n_options; i++) {
    char syn[80];
    opt_synopsis(syn, sizeof syn, &options[i]);
    if (options[i].def)
      fprintf(f, "%-20s%s (default %s)\n", syn, options[i].desc, options[i].def);
    else
      fprintf(f, "%-20s%s\n", syn, options[i].desc);
  }
}

/* ------------------------------------------------------------------- sinks */

static int write_sink(const char *data, size_t len, void *ctx) {
  FILE *out = (FILE *)ctx;
  return fwrite(data, 1, len, out) == len ? 0 : -1;
}

/* Sink adapter that pushes bytes into a json2toon converter, so the JSON help
 * emitter below can drive a live JSON->TOON conversion without buffering. */
static int feed_sink(const char *data, size_t len, void *ctx) {
  return json2toon_feed((json2toon_t *)ctx, data, len) == JSON2TOON_OK ? 0 : -1;
}

/* ------------------------------------------------------------- JSON help out
 *
 * Emits the help model as a JSON document through a json2toon_sink. Errors are
 * latched in the emitter and checked once at the end, keeping the per-token
 * call sites readable while still honouring every sink return value. */

typedef struct {
  json2toon_sink sink;
  void *ctx;
  int err;
} json_emitter;

static void emit(json_emitter *e, const char *s) {
  if (e->err)
    return;
  if (e->sink(s, strlen(s), e->ctx) != 0)
    e->err = -1;
}

/* Emit s as a quoted, JSON-escaped string; a NULL pointer emits the literal
 * null. UTF-8 continuation/lead bytes (>= 0x80) are valid inside JSON strings
 * and pass through unchanged. */
static void emit_qstr(json_emitter *e, const char *s) {
  char esc[8];
  if (e->err)
    return;
  if (!s) {
    emit(e, "null");
    return;
  }
  emit(e, "\"");
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    switch (c) {
    case '"':  emit(e, "\\\""); break;
    case '\\': emit(e, "\\\\"); break;
    case '\b': emit(e, "\\b");  break;
    case '\f': emit(e, "\\f");  break;
    case '\n': emit(e, "\\n");  break;
    case '\r': emit(e, "\\r");  break;
    case '\t': emit(e, "\\t");  break;
    default:
      if (c < 0x20) {
        snprintf(esc, sizeof esc, "\\u%04x", c);
        emit(e, esc);
      } else {
        char one[2];
        one[0] = (char)c;
        one[1] = '\0';
        emit(e, one);
      }
    }
  }
  emit(e, "\"");
}

/* Emit one "key": value pair (string value), with the leading comma handled by
 * the caller-supplied separator. */
static void emit_field(json_emitter *e, const char *key, const char *val) {
  emit(e, "\"");
  emit(e, key);
  emit(e, "\": ");
  emit_qstr(e, val);
}

/* Render the help model as JSON. Returns 0 on success, non-zero if the sink
 * ever reported failure. */
static int emit_help_json(json2toon_sink sink, void *ctx) {
  json_emitter e;
  size_t i;
  e.sink = sink;
  e.ctx = ctx;
  e.err = 0;

  emit(&e, "{\n  ");
  emit_field(&e, "program", prog);
  emit(&e, ",\n  ");
  emit_field(&e, "version", json2toon_version());
  emit(&e, ",\n  ");
  emit_field(&e, "summary", help_summary);
  emit(&e, ",\n  ");
  emit_field(&e, "usage", help_usage);
  emit(&e, ",\n  \"options\": [");
  for (i = 0; i < n_options; i++) {
    const opt_info *o = &options[i];
    emit(&e, i ? ",\n    {" : "\n    {");
    emit(&e, "\"short\": ");
    emit_qstr(&e, o->shortf);
    emit(&e, ", \"long\": ");
    emit_qstr(&e, o->longf);
    emit(&e, ", \"arg\": ");
    emit_qstr(&e, o->arg);
    emit(&e, ", \"default\": ");
    emit_qstr(&e, o->def);
    emit(&e, ", \"description\": ");
    emit_qstr(&e, o->desc);
    emit(&e, "}");
  }
  emit(&e, "\n  ]\n}\n");
  return e.err;
}

/* Print the help model as JSON to stdout. */
static int print_help_json(void) {
  return emit_help_json(write_sink, stdout) == 0 ? 0 : 1;
}

/* Print the help model as TOON to stdout, by routing the JSON help through the
 * json2toon converter itself. The TOON help is therefore, by construction,
 * exactly what json2toon would produce for the JSON help. */
static int print_help_toon(void) {
  json2toon_t *j2t;
  int rc;
  j2t = json2toon_new(write_sink, stdout, NULL);
  if (!j2t) {
    fprintf(stderr, "%s: out of memory\n", prog);
    return 1;
  }
  rc = emit_help_json(feed_sink, j2t);
  if (rc == 0)
    rc = json2toon_finish(j2t);
  json2toon_delete(j2t);
  if (rc != 0) {
    fprintf(stderr, "%s: failed to render TOON help\n", prog);
    return 1;
  }
  return 0;
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
    } else if (!strcmp(a, "--help-json")) {
      return print_help_json();
    } else if (!strcmp(a, "--help-toon")) {
      return print_help_toon();
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
