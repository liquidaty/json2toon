# Vendored toonwriter

This is a vendored snapshot of the `toonwriter` library (a small, bounded-memory
TOON writer with a streaming push API), used by json2toon's forward path
(JSON → TOON): the JSON SAX parser (YAJL) drives the toonwriter push API, and
toonwriter owns all TOON concerns (array buffering, inline/tabular/list shaping,
quoting, and the bounded spill-to-disk store).

- Source: the `toon_writer` repo, commit `f8b312b` (TOON §7.1: backspace and
  form feed emit `\uXXXX`, not `\b`/`\f`).
- Files: `toonwriter.h`, `toonwriter.c`, `toon_numeric.h`, `toon_numeric.c`.
  (`toonwriter.c` `#include`s `toon_numeric.c`; both are compiled as one TU.)
- Build: compiled into `libjson2toon` from the Makefile; add
  `-Ithird_party/toonwriter` so `#include <toonwriter.h>` resolves.

To update: re-copy the four files from the toon_writer repo and bump the commit
hash above. Do not edit them here — changes belong upstream in toon_writer.
