# third_party/sqlite3 — vendored SQLite

This directory builds SQLite from the official amalgamation source as part of
the project. **No system-wide install required.**

## What it gives you

- `SQLITE_ENABLE_RTREE` — exports `sqlite3_rtree_query_callback` and friends.
  Without this, any extension that links against SQLite R-Tree (e.g.
  `libspatialite`) will fail to load with
  `undefined symbol: sqlite3_rtree_query_callback`.
- `SQLITE_ENABLE_GEOPOLY` — required by SpatiaLite 5.x.
- `SQLITE_ENABLE_FTS5`, `SQLITE_ENABLE_JSON1`, `SQLITE_ENABLE_COLUMN_METADATA`.
- Thread-safe (1), URI filenames enabled, sane foreign-key defaults.
- Builds both **static** (`libsqlite3.a`) and **shared** (`libsqlite3.so`).

## Usage from CMake

```cmake
add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/sqlite3 sqlite3_build)
target_link_libraries(mylib PRIVATE sqlite3)   # static by default
# or, if you need dlopen semantics:
target_link_libraries(mylib PRIVATE sqlite3_shared)
```

The include directory is exported as a `BUILD_INTERFACE`, so consumers get
`#include <sqlite3.h>` for free.

## Updating the version

```bash
SQLITE_VERSION=3470200 SQLITE_YEAR=2024 ./download.sh   # then rebuild
```

## Cross-compile notes

- Tested to compile on `aarch64-linux-gnu` (Jetson) with
  `CMAKE_C_COMPILER=aarch64-linux-gnu-gcc`.
- The amalgamation has zero build-time dependencies beyond libc + pthreads +
  libm + libdl, all of which exist on every supported target.
- The build is hermetic: no autoconf, no `make`, no `pkg-config`.

## Layout after first build

```
third_party/sqlite3/
├── CMakeLists.txt
├── download.sh
├── README.md
├── sqlite3.c          ← downloaded by download.sh
├── sqlite3.h          ← downloaded by download.sh
└── shell.c            ← optional, used if you build the CLI too
```