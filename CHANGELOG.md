# Changelog

All notable changes to libjson2toon are recorded here. The version is defined
once in `include/json2toon.h` (`JSON2TOON_VERSION`); `json2toon_version()` and
the generated `json2toon.pc` both derive from it.

## 1.1.0

### Added
- stdio / convenience layer over the push API (all additive; no existing symbol
  changes):
  - `json2toon_file_sink` — ready-made sink writing each chunk to a `FILE *`
    (serves both directions).
  - `json2toon_feed_fwrite` / `toon2json_feed_fwrite` — `fwrite(3)`-shaped feed
    adapters for driving a converter straight from an fwrite-style producer with
    no intermediate buffer.
  - `json2toon_convert_file` / `toon2json_convert_file` — whole-`FILE`
    converters.
  - `json2toon_convert_mem` / `toon2json_convert_mem` — whole-buffer converters.
- `JSON2TOON_VERSION`, `JSON2TOON_VERSION_{MAJOR,MINOR,PATCH}` and
  `JSON2TOON_VERSION_NUMBER` macros so consumers can require a minimum floor at
  compile time.
- Fuzz harness (`tests/fuzz.c`) driving both converter directions, with a
  `fuzz` (libFuzzer) and `fuzz-standalone` (portable replay) make target and a
  CI job.

### Changed
- SIMD scanner selection is now bound at static-initialization time instead of
  via a per-`*_new()` `j2t_simd_init()` call. The backend was already chosen at
  compile time, so this removes a data race on the shared scanner pointers and
  makes converter creation reentrant.
- `app/json2toon` (the CLI) now uses the new whole-`FILE` converters instead of
  a private copy of the sink + feed loop.

## 1.0.0

- Initial release: streaming `json2toon` (JSON → TOON) and `toon2json`
  (TOON → JSON) push converters, the `json2toon` CLI, SSE2/NEON SIMD scanners
  with a scalar fallback, and the autoconf-style build.
