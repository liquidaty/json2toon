# json2toon — improvement specification

Scope: four changes, ordered by priority. Each lists problem, change, and
acceptance criteria. Keep every fix warning-clean under
`-std=c11 -pedantic -Wall -Wextra -Werror` and pass the suite under
`-fsanitize=address,undefined` on all CI targets.

---

## 1. Single-tokenizer forward array path (correctness; deletes a bug class)

**Problem.** The forward array encoder validates each captured array with yajl,
then re-walks the same bytes with a second, hand-rolled tokenizer
(`shape_array` / `sk_value` / `sk_string` / `is_ws`) to classify and emit. The
two tokenizers do not agree: `is_ws` (src/encode_array.c) accepts only
`SP \t \n \r`, while yajl's lexer also skips `0x0b`/`0x0c`. yajl-valid input can
therefore desync the walker, producing **invalid TOON that does not round-trip,
while the process still exits 0**.

Repro (current build):
```
printf '[1,\x0c2]' | json2toon            # -> "[2]: 1,<0x0c>2", exit 0
printf '[1,\x0c2]' | json2toon | json2toon -r   # -> malformed TOON, exit 1
```
The in-tree "bound array emit walkers" patch only stops the resulting hang/OOM;
the silent-miscompile remains.

**Change.** Drive classification and emission from the single yajl parse;
remove the second tokenizer.

- In `src/encode_array.c`, replace the structural walkers with yajl callbacks
  registered on the existing validate pass:
  - `yajl_map_key`, `yajl_string` → decoded UTF-8 bytes (no manual unescaping).
  - `yajl_number` → raw lexeme (keep numbers verbatim/lossless; do **not** use
    `yajl_integer`/`yajl_double`).
  - `yajl_start_array/map`, `yajl_end_array/map` → structure + element count.
  - Record value byte-spans with `yajl_get_bytes_consumed()` so large values are
    re-emitted by seeking the store rather than buffered.
- Classification (EMPTY / INLINE / TABULAR / LIST), the column template, and the
  one-row value-span table move into the callback layer. `j2t_encode_captured`
  keeps its signature.
- Delete once unused: `shape_array`, `sk_value`, `sk_string`, `sk_ws`, `is_ws`,
  `array_is_empty`, `object_is_empty`, `decode_into`, `decode_quoted`, local
  `hex4`/`utf8_encode` (string/key decoding now comes from yajl; remaining
  encode/decode needs are covered by change #2).
- Set yajl strictly: keep `yajl_dont_validate_strings` consistent with the
  object path's documented byte-transparency, but ensure no leniency lets yajl
  accept input the rest of the pipeline cannot represent (no comments, no
  trailing garbage, single top-level value).

**Memory/CPU.** Peak memory stays bounded (column template + one row of decoded
keys/spans). Disk I/O for spilled arrays drops from ~3 full passes to ~2 (one
yajl pass that also classifies, one span re-read for emit).

**Acceptance.**
- New fuzz oracle (property test): for every input yajl accepts as a complete
  JSON document, `toon2json(json2toon(x))` parses cleanly and is structurally
  equal to `x`. Add to `tests/fuzz.c`; run in the libFuzzer CI job.
- The two repro inputs above now either convert to valid round-trippable TOON or
  return `ERR_PARSE` — never exit 0 with non-round-trippable output.
- Existing 153 tests still pass (release + ASan).
- `is_ws` and the skip-walkers are gone from the source.

---

## 2. Centralize Unicode + growable-buffer helpers (DRY)

**Problem.** Duplicated across translation units:
- UTF-8 code-point encoder: `json2toon.c:utf8_emit`, `encode_array.c:utf8_encode`,
  `toon2json.c:utf8_encode` (byte-identical).
- `\uXXXX` hex parse: `encode_array.c:hex4`, `toon2json.c:hex4`.
- `\uXXXX` quoted-string decode incl. surrogate pairing: `encode_array.c` and
  `toon2json.c`.
- Growable byte buffer: `buf_reserve` (×2), `grow`, `ram_reserve`.

Surrogate-pair logic living in three places is a bug-multiplier.

**Decision: do NOT add libutf8proc.** Overkill (normalization/case/grapheme +
large data table); harms the WASM/footprint and self-contained-build goals. The
needed primitives are ~30 lines total.

**Change.** Add one internal module (no public API change):

`src/unicode.h` / `src/unicode.c`:
```c
/* Encode a Unicode scalar value to UTF-8. Returns byte count 1..4. */
int  j2t_utf8_encode(unsigned cp, char out[4]);
/* Parse 4 hex digits at [s, s+4) (caller guarantees 4 bytes available).
 * Returns 0 and *out on success, -1 on a non-hex digit. */
int  j2t_hex4(const char *s, unsigned *out);
```

`src/internal.h` — promote the growable buffer to one shared helper:
```c
typedef struct { char *p; size_t len, cap; } j2t_buf;
int  j2t_buf_reserve(j2t_buf *b, size_t need);   /* 0 / -1 on OOM */
int  j2t_buf_append(j2t_buf *b, const char *s, size_t n);
int  j2t_buf_putc(j2t_buf *b, char c);
void j2t_buf_free(j2t_buf *b);
```
Replace `buf_reserve`/`grow`/`ram_reserve` and their `char*,len,cap` triplets in
json2toon.c, toon2json.c, store.c with `j2t_buf`. (`encode_array.c`'s copies are
removed by change #1.)

The `\uXXXX`-decode loop: after #1 only `toon2json.c` still decodes TOON-quoted
strings; keep that single decoder but have it call `j2t_utf8_encode`/`j2t_hex4`.

**Acceptance.**
- Exactly one definition each of UTF-8 encode, hex4, and the growable buffer.
- No behavior change; suite green (release + ASan); WASM build size unchanged or
  smaller.

---

## 3. Build hygiene, feature-test centralization, perf note

**3a. Strict-warnings CI lane.** Current flags: `-Wall -Wextra -Wno-unused-parameter`,
no `-Werror`/`-pedantic`. Add a CI job (not necessarily the default build)
compiling the library TUs with
`-std=c11 -pedantic -Wall -Wextra -Werror -Wconversion -Wshadow -Wstrict-prototypes -Wvla`
and fix what it surfaces (expect implicit narrowing conversions). Vendored yajl
stays under `-w`.

**3b. Centralize feature-test macros.** `_FILE_OFFSET_BITS` / `_POSIX_C_SOURCE`
are defined ad hoc at the top of store.c. Move them to one internal config
header (e.g. `src/j2t_config.h`) included first by every library TU, alongside
the existing `J2T_FSEEK`/`J2T_FTELL` block — same rationale as the centralized
SIMD/compiler constructs in simd.c. Any future POSIX-touching TU then inherits
the correct macros instead of re-tripping glibc `__STRICT_ANSI__`.

**3c. Perf note (no code change required).** Document in the encoder header that
a spilled array is re-read per pass; #1 reduces this. No action beyond #1.

**Acceptance.** New strict CI lane passes on gcc + clang. Removing the per-file
feature-test defines from store.c still cross-compiles (i686, mingw64, wasm).

---

## 4. Smaller robustness fixes

**4a. Duplicate object keys (forward).** `{"a":1,"a":2}` currently emits two
`a:` lines (lossy/ambiguous TOON). Decide policy — recommend **reject**
(`ERR_PARSE`) for the streaming object path and for tabular rows, matching
TOON's unique-key assumption; document it. Implement via a key-seen check in the
object state machine (bounded memory: only flag duplicates within the current
object's already-emitted keys is non-trivial in streaming mode — if full
dedup is too costly, document the pass-through behavior explicitly instead).

**4b. `parse_count` overflow (toon2json.c).** `v = v*10 + digit` in `size_t`
has no overflow guard; a long digit run wraps and corrupts the declared-count
check. Saturate or `ERR_PARSE` on overflow before multiply.

**4c. Temp-file hardening (store.c).** Custom `get_temp_filename` paths are
opened `fopen(name, "w+b")` — predictable-name TOCTOU/symlink risk in shared
dirs. Either open with `O_EXCL` (`open` + `fdopen`) internally, or document that
callbacks must return unguessable, exclusively-created paths. Default
`tmpfile()` is unaffected.

**Acceptance.** Targeted unit tests for 4a/4b that fail before and pass after.
4c: doc update at minimum; `O_EXCL` path preferred where the platform allows.

---

### Sequencing
1 → 2 (2 partly falls out of 1) → 3 → 4. Land the #1 fuzz oracle *first* so the
invariant is pinned before the encoder is refactored.
