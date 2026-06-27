# Build Protocol

This directory is synchronized with a remote build machine that polls for
changes every 2 seconds.

## Build profiles

Pick a profile by writing `@profile=<name>` as the first line of
`changed-files.txt` (before the file paths). Available profiles:

- `DESC`

  ```
  Build host environment (MacOS ARM64)
  ```

- `asan`

  ```
  make build ASAN=1
  ```

- `asan_DESC`

  ```
  Build host environment (MacOS ARM64) with ASAN=1 flag
  ```

- `asan_clean`

  ```
  make clean ASAN=1
  ```

- `asan_configure`

  ```
  ASAN=1 ./configure
  ```

- `asan_test`

  ```
  make test ASAN=1
  ```

- `asan_test_DESC`

  ```
  Run tests on host with ASAN=1
  ```

- `clean`

  ```
  make clean
  ```

- `configure`

  ```
  ./configure
  ```

- `configure_i686`

  ```
  ~/i686_env ./configure
  ```

- `configure_sbx`

  ```
  ~/sbx_env ./configure
  ```

- `debug`

  ```
  make build DEBUG=1
  ```

- `debug_DESC`

  ```
  Build host environment (MacOS ARM64) with DEBUG=1 flag
  ```

- `debug_clean`

  ```
  make clean DEBUG=1
  ```

- `debug_configure`

  ```
  DEBUG=1 ./configure
  ```

- `debug_test`

  ```
  make test DEBUG=1
  ```

- `debug_test_DESC`

  ```
  Run tests on host with DEBUG=1
  ```

- `gcc`

  ```
  ~/gcc_env make build
  ```

- `gcc_DESC`

  ```
  Build host environment (MacOS ARM64)
  ```

- `gcc_clean`

  ```
  ~/gcc_env make clean
  ```

- `gcc_configure`

  ```
  ~/gcc_env ./configure
  ```

- `gcc_test`

  ```
  ~/gcc_env make test
  ```

- `gcc_test_DESC`

  ```
  Run tests on host
  ```

- `i686`

  ```
  ~/i686_env make build
  ```

- `i686_DESC`

  ```
  Build for i686 environment (cannot run on host)
  ```

- `i686_clean`

  ```
  ~/i686_env make clean
  ```

- `leaks`

  ```
  make test-leaks
  ```

- `sbx`

  ```
  ~/sbx_env make build
  ```

- `sbx_DESC`

  ```
  Build for sandbox environment (cannot run on host)
  ```

- `sbx_clean`

  ```
  ~/sbx_env make build clean
  ```

- `test`

  ```
  make test
  ```

- `test_DESC`

  ```
  Run test-leaks on host
  ```


If `@profile=` is omitted, the build machine falls back to its default
build command:

```
make build
```

## After editing source files

1. Write the relative path of every changed file (one per line) to
   `changed-files.txt`:

   ```
   printf '%s\n' path/to/file1.c path/to/file2.h > changed-files.txt
   ```

   To select a build profile, prepend a `@profile=<name>` line:

   ```
   printf '%s\n' '@profile=<name>' path/to/file1.c path/to/file2.h > changed-files.txt
   ```

2. Wait for the build to finish — the request file is deleted when the cycle
   completes:

   ```
   while [ -f changed-files.txt ]; do sleep 1; done
   ```

3. Read the result:

   ```
   cat result.txt
   ```

   Possible outcomes:
   - `success` — build succeeded; artifacts (if any) have been deployed.
   - `BUILD FAILED: <output>` — build error with compiler/tool output.
   - `FETCH FAILED: ...` — the build machine could not retrieve your changed files.
   - `REQUEST FAILED: ...` — unknown directive or unknown profile in `changed-files.txt`.
   - `APPLY FAILED: ...` — a file path was rejected or could not be placed.
   - `UPLOAD FAILED: ...` — built artifacts could not be sent back.

## Rules

- Paths in `changed-files.txt` must be relative to this directory.
- No absolute paths (`/...`). No `..` path components.
- Wait for each build cycle to complete before starting another.
- Build output in `result.txt` longer than 33554432 bytes is
  truncated: the first 16777216 bytes and the last 16777216
  bytes are kept, with an omission marker between them.
