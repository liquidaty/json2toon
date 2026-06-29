# Vendored YAJL (parser subset)

This directory contains a vendored copy of [YAJL](https://github.com/lloyd/yajl)
**2.1.1**, used by json2toon's forward path to validate captured array bytes
before they are streamed back out as TOON. Vendoring (rather than linking a
system yajl) keeps cross targets (i686, sbx, mingw, emscripten) self-contained.

## What is included

Only the **stream parser** is vendored — the JSON *generator* and the DOM *tree*
builder are not used by json2toon and are omitted:

- Compiled: `yajl.c`, `yajl_alloc.c`, `yajl_buf.c`, `yajl_encode.c`, `yajl_lex.c`,
  `yajl_parser.c`, `yajl_version.c`
- Internal headers: `yajl_alloc.h`, `yajl_buf.h`, `yajl_bytestack.h`,
  `yajl_encode.h`, `yajl_lex.h`, `yajl_parser.h`
- Public headers (`api/`): `yajl_common.h`, `yajl_parse.h`, `yajl_gen.h`
  (`yajl_gen.h` supplies only the `yajl_print_t` typedef that `yajl_encode.h`
  references; no generator code is compiled.)
- **Omitted**: `yajl_gen.c`, `yajl_tree.c`, `yajl_tree.h`, `api/yajl_gen` impl,
  `api/yajl_tree.h`.

All files above are verbatim from upstream 2.1.1.

## Include layout

Upstream sources reference public headers two ways: `#include "api/..."`
(relative) and `#include <yajl/...>` (angle). To satisfy both without editing
upstream source, the verbatim public headers live under `api/`, and the `yajl/`
directory holds thin shims (`#include "../api/..."`) plus the generated
`yajl_version.h` (materialized for 2.1.1). Build with `-Ithird_party/yajl` so the
angle includes resolve through `yajl/`.

## License

YAJL is ISC-licensed; see `COPYING`.
