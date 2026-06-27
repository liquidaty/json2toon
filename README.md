# json2toon

A fast, portable C library and command-line tool that converts **JSON** into
**TOON** (Token-Oriented Object Notation).

`json2toon` is built for the same job a Unix filter is built for: read from a
stream, write to a stream, and never let memory grow with the size of the data.
It is written in portable C, ships as both a standalone library and a standalone
application, processes input incrementally, runs in bounded memory regardless of
how large the input or output is, and uses SIMD where the host CPU provides it —
with a portable scalar fallback everywhere else.

## What is TOON?

TOON (Token-Oriented Object Notation) is a compact, human-readable alternative
to JSON designed to minimize the number of tokens a large language model spends
representing the same data. It keeps JSON's data model — objects, arrays,
strings, numbers, booleans, and null — but drops the redundant punctuation and
repeated keys that dominate JSON's token cost, using indentation and a tabular
layout for arrays of like-shaped objects instead.

The result typically encodes the same structured data in noticeably fewer tokens
while remaining readable to a person. `json2toon` exists to produce TOON quickly
and cheaply from JSON you already have.

## Features

- **Pure C, maximum portability.** Written to a pinned C standard with no
  third-party runtime dependencies. Compiles cleanly under clang, gcc, mingw64
  (cross-compiling to Windows), and emscripten (compiling to WebAssembly).
- **Library *and* application.** Use the `json2toon` command as a Unix filter,
  or link `libjson2toon` and drive the converter from your own program through a
  small, stable C API.
- **Streaming / incremental.** Input is consumed in chunks and output is emitted
  as soon as it is ready. You can feed it a stream of any length — including data
  that never ends — without buffering the whole document.
- **Bounded memory.** Peak memory is a function of the *structure* you choose to
  support (nesting depth and the width of a tabular row), never of total input or
  output size. A multi-gigabyte document converts in the same footprint as a
  small one.
- **SIMD where available.** Hot loops (scanning, whitespace skipping, string and
  escape handling) use SSE2/AVX2 on x86-64, NEON on ARM64, and WASM SIMD under
  emscripten, selected at runtime, with a correct scalar fallback when no vector
  unit is present.

## Building and installing

`json2toon` uses a traditional Unix build. From a release tarball:

```sh
./configure
make
make check        # optional: run the test suite
sudo make install
```

Building from a git checkout (regenerates the `configure` script first):

```sh
./autogen.sh      # requires autoconf/automake; produces ./configure
./configure
make
sudo make install
```

By default this installs the `json2toon` binary, the `libjson2toon` library
(static and shared), the public header, and a `json2toon.pc` pkg-config file
under `/usr/local`. Override the prefix the usual way:

```sh
./configure --prefix="$HOME/.local"
```

Useful `configure` options:

| Option                 | Effect                                                       |
| ---------------------- | ----------------------------------------------------------- |
| `--prefix=DIR`         | Installation root (default `/usr/local`).                   |
| `--disable-simd`       | Build the scalar code path only (useful for debugging).     |
| `--enable-debug`       | Build with debug info and assertions.                       |
| `--disable-shared`     | Build the static library only.                              |

### Dependencies

A C compiler and `make` are the only requirements to build a release tarball.
Building from a git checkout additionally needs autoconf and automake. The test
suite and sanitizer targets are optional.

### Cross-compiling

`json2toon` is regularly built for targets other than the host.

To Windows with mingw64:

```sh
./configure --host=x86_64-w64-mingw32
make
```

To WebAssembly with emscripten:

```sh
emconfigure ./configure
emmake make
```

SIMD is selected per target: the x86 paths are gated behind x86 feature
detection, ARM64 uses NEON, the emscripten build uses WASM SIMD when the
toolchain enables it, and every target retains the scalar fallback.

## Command-line usage

`json2toon` reads JSON from standard input and writes TOON to standard output,
so it composes with the rest of your toolbox:

```sh
# Convert a file
json2toon < data.json > data.toon

# Use it in a pipeline; nothing is fully buffered
curl -s https://example.com/api/records.json | json2toon | less

# Explicit input/output files
json2toon -i data.json -o data.toon
```

Common options:

| Option              | Description                                              |
| ------------------- | ------------------------------------------------------- |
| `-i, --input FILE`  | Read from `FILE` instead of standard input.             |
| `-o, --output FILE` | Write to `FILE` instead of standard output.             |
| `--indent N`        | Indent width in spaces (default 2).                     |
| `-h, --help`        | Print usage and exit.                                   |
| `-V, --version`     | Print version and exit.                                 |

Exit status is `0` on success and non-zero on malformed input or I/O error; a
diagnostic identifying the byte offset of the problem is written to standard
error.

## Library usage

Link against `libjson2toon` and discover flags with pkg-config:

```sh
cc myprog.c $(pkg-config --cflags --libs json2toon) -o myprog
```

The API is a push parser: you hand it input as it arrives and supply a sink
callback that receives TOON output in bounded-size pieces. Neither the input nor
the output is ever fully held in memory.

```c
#include <json2toon.h>
#include <stdio.h>

/* Called repeatedly with chunks of TOON output. Return 0 to continue. */
static int write_sink(const char *data, size_t len, void *ctx) {
    FILE *out = ctx;
    return fwrite(data, 1, len, out) == len ? 0 : -1;
}

int main(void) {
    json2toon_t *j2t = json2toon_new(write_sink, stdout, NULL);
    if (!j2t)
        return 1;

    char buf[65536];
    size_t n;
    int rc = 0;
    while ((n = fread(buf, 1, sizeof buf, stdin)) > 0) {
        if ((rc = json2toon_feed(j2t, buf, n)) != JSON2TOON_OK)
            break;
    }
    if (rc == JSON2TOON_OK)
        rc = json2toon_finish(j2t);   /* flush any pending output */

    if (rc != JSON2TOON_OK)
        fprintf(stderr, "json2toon: %s\n", json2toon_strerror(rc));

    json2toon_delete(j2t);
    return rc == JSON2TOON_OK ? 0 : 1;
}
```

The shape of the API — a converter object you `feed()` incrementally and then
`finish()`, with output delivered through a callback — is what keeps memory
bounded, and it is also the surface through which the planned reverse direction
(TOON → JSON) will be exposed, so code written against it today will not need to
change.

## Design notes

**Streaming and bounded memory.** The converter is a state machine over the JSON
grammar. It never materializes the document tree; it emits TOON for each value as
soon as it has parsed enough to do so unambiguously. The only structures that
grow are a nesting stack (one small frame per level of object/array depth) and,
for the tabular array encoding, a header describing the current row's columns.
Both are bounded by the *shape* of the data you accept, not its length, and both
have configurable limits so adversarial input cannot force unbounded growth.

**SIMD.** Vectorization is applied to the character-classification work that
dominates a converter's time: skipping insignificant whitespace, finding the end
of unescaped string runs, and locating bytes that need escaping. Each vectorized
routine is paired with a scalar implementation of identical semantics; the
fastest variant available on the running CPU is chosen at startup. All
compiler- and architecture-specific constructs (intrinsics, feature macros) are
centralized rather than scattered through the code, so adding or removing an
instruction set touches one place.

**UTF-8.** All string handling is UTF-8 aware; multi-byte sequences are passed
through correctly and validated on the input side.

## Portability

`json2toon` targets, and is tested against, the following toolchains:

- **clang** and **gcc** on Linux and macOS (x86-64 and ARM64)
- **mingw64** cross-compiled to Windows (x86-64)
- **emscripten** compiled to WebAssembly

The code makes no assumptions about endianness, struct packing, or
pointer/`int` width, and uses fixed-width integer types where width matters.

## Testing

```sh
make check                 # run the unit and round-trip test suite
make check-asan            # build and test under AddressSanitizer + UBSan
make check-valgrind        # run the suite under Valgrind (leak check)
```

The suite includes conformance cases comparing `json2toon` output against
reference TOON, fuzz/streaming tests that feed input one byte at a time to prove
the incremental path matches the bulk path, and memory-bound tests that confirm
peak allocation does not scale with input size. Releases are gated on a clean
sanitizer and leak report.

## Roadmap

- **TOON → JSON** lossless round-trip, exposed through the same incremental API.
- Additional SIMD back-ends as targets warrant.

## License

MIT. See [LICENSE](LICENSE).
