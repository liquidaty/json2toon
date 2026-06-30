/* json2toon - append-then-read backing store for one buffered array.
 *
 * An array's raw bytes are captured here, then read back (validate, then emit).
 * To bound peak memory by a configurable window rather than array length, bytes
 * stay in `ram` up to a threshold and the overflow spills to a temp file. RAM vs
 * spill is invisible to the output: the reader yields the same bytes either way.
 */
#include "internal.h"      /* includes j2t_config.h first (feature-test macros) */

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------- store */

void j2t_store_init(j2t_store *s, size_t threshold,
                    char *(*get_temp_filename)(const char *prefix)) {
  s->ram.p = NULL;
  s->ram.len = 0;
  s->ram.cap = 0;
  s->threshold = threshold ? threshold : (1u << 20);
  s->spilled = 0;
  s->fp = NULL;
  s->tmpname = NULL;
  s->total = 0;
  s->get_temp_filename = get_temp_filename;
  s->err = JSON2TOON_OK;
}

/* Move to spilled mode: obtain the temp file and flush the resident bytes into
 * it. After this, `ram` no longer mirrors the data and is free for reuse. */
static int spill_open(j2t_store *s) {
  if (s->get_temp_filename) {
    s->tmpname = s->get_temp_filename("json2toon");
    if (!s->tmpname) {
      s->err = JSON2TOON_ERR_IO;
      return -1;
    }
    /* Exclusive-create: a caller-supplied name in a shared dir is otherwise a
     * predictable-name symlink/TOCTOU target (4c). tmpfile() is already safe. */
    s->fp = j2t_fopen_excl(s->tmpname);
  } else {
    s->fp = tmpfile();
  }
  if (!s->fp) {
    s->err = JSON2TOON_ERR_IO;
    return -1;
  }
  if (s->ram.len && fwrite(s->ram.p, 1, s->ram.len, s->fp) != s->ram.len) {
    s->err = JSON2TOON_ERR_IO;
    return -1;
  }
  s->spilled = 1;
  s->ram.len = 0;
  return 0;
}

int j2t_store_append(j2t_store *s, const char *p, size_t n) {
  if (s->err != JSON2TOON_OK)
    return s->err;
  if (n == 0)
    return JSON2TOON_OK;

  if (!s->spilled && s->ram.len + n > s->threshold) {
    if (spill_open(s) != 0)
      return s->err;
  }

  if (s->spilled) {
    if (fwrite(p, 1, n, s->fp) != n) {
      s->err = JSON2TOON_ERR_IO;
      return s->err;
    }
  } else {
    if (j2t_buf_append(&s->ram, p, n) != 0) {
      s->err = JSON2TOON_ERR_MEMORY;
      return s->err;
    }
  }
  s->total += n;
  return JSON2TOON_OK;
}

uint64_t j2t_store_size(const j2t_store *s) { return s->total; }

void j2t_store_reset(j2t_store *s) {
  if (s->fp) {
    fclose(s->fp);
    s->fp = NULL;
  }
  if (s->tmpname) {
    remove(s->tmpname);
    free(s->tmpname);
    s->tmpname = NULL;
  }
  s->spilled = 0;
  s->ram.len = 0;
  s->total = 0;
  s->err = JSON2TOON_OK;
}

void j2t_store_free(j2t_store *s) {
  j2t_store_reset(s);
  j2t_buf_free(&s->ram);
}

/* ------------------------------------------------------------------ reader */

void j2t_reader_init(j2t_reader *r, j2t_store *s) {
  r->s = s;
  r->pos = 0;
  r->buf_off = 0;
  r->buf_len = 0;
}

void j2t_reader_seek(j2t_reader *r, uint64_t off) { r->pos = off; }

uint64_t j2t_reader_tell(const j2t_reader *r) { return r->pos; }

/* Make buf[] cover r->pos (spilled stores only). Returns 0 on success, -1 at
 * end-of-data or on an I/O error (which it records in the store). */
static int reader_fill(j2t_reader *r) {
  j2t_store *s = r->s;
  size_t got;
  if (r->pos >= s->total)
    return -1;
  if (r->pos >= r->buf_off && r->pos < r->buf_off + r->buf_len)
    return 0;
  if (J2T_FSEEK(s->fp, r->pos) != 0) {
    s->err = JSON2TOON_ERR_IO;
    return -1;
  }
  got = fread(r->buf, 1, sizeof r->buf, s->fp);
  if (got == 0) {
    s->err = JSON2TOON_ERR_IO;
    return -1;
  }
  r->buf_off = r->pos;
  r->buf_len = got;
  return 0;
}

int j2t_reader_getc(j2t_reader *r) {
  j2t_store *s = r->s;
  int c;
  if (r->pos >= s->total)
    return -1;
  if (!s->spilled) {
    c = (unsigned char)s->ram.p[r->pos];
  } else {
    if (reader_fill(r) != 0)
      return -1;
    c = (unsigned char)r->buf[r->pos - r->buf_off];
  }
  r->pos++;
  return c;
}

int j2t_reader_peek(j2t_reader *r) {
  j2t_store *s = r->s;
  if (r->pos >= s->total)
    return -1;
  if (!s->spilled)
    return (unsigned char)s->ram.p[r->pos];
  if (reader_fill(r) != 0)
    return -1;
  return (unsigned char)r->buf[r->pos - r->buf_off];
}

size_t j2t_reader_read(j2t_reader *r, char *dst, size_t n) {
  j2t_store *s = r->s;
  uint64_t avail = s->total - r->pos;
  size_t got;
  if (r->pos >= s->total)
    return 0;
  if ((uint64_t)n > avail)
    n = (size_t)avail;
  if (!s->spilled) {
    memcpy(dst, s->ram.p + r->pos, n);
    r->pos += n;
    return n;
  }
  if (J2T_FSEEK(s->fp, r->pos) != 0) {
    s->err = JSON2TOON_ERR_IO;
    return 0;
  }
  got = fread(dst, 1, n, s->fp);
  if (got != n)
    s->err = JSON2TOON_ERR_IO;
  r->pos += got;
  return got;
}
