# third_party — vendored dependencies

Everything in this directory is built as part of the project. Nothing here
writes to `/usr/local` or `/usr/lib`, so the project is **fully reproducible**
on a fresh machine (x86_64, aarch64/Jetson, ARM) with only a C/C++ toolchain.

| Module                | Default | Purpose                                                                                      |
|-----------------------|---------|--------------------------------------------------------------------------------------------|
| [`sqlite3/`](sqlite3/)   | ON      | Vendored SQLite amalgamation. Built with `SQLITE_ENABLE_RTREE`, `SQLITE_ENABLE_GEOPOLY`, etc. — fixes `undefined symbol: sqlite3_rtree_query_callback`. |
| [`spatialite/`](spatialite/) | OFF     | Opt-in vendored SpatiaLite for users who need a self-contained GIS stack.                |
| [`libmicrohttpd/`](libmicrohttpd/) | OFF | Opt-in vendored libmicrohttpd (default: use system `libmicrohttpd-dev`). Needed by `./build.sh WEB`. |

## Why is SQLite vendored?

The Linux distribution on the build host had two coexisting SQLite libraries:

```
/usr/lib/x86_64-linux-gnu/libsqlite3.so.0     # apt, OK
/usr/local/lib/libsqlite3.so                  # hand-built, missing R-Tree
```

R-Tree-using extensions (SpatiaLite, GDAL rtree driver, etc.) get loaded by
external tools and need `sqlite3_rtree_query_callback`. When they link
against the hand-built `/usr/local/lib/libsqlite3.so` (compiled without
`SQLITE_ENABLE_RTREE`), loading fails. Vendoring SQLite makes the project's
ABI stable and platform-independent.

## Adding a new third-party module

1. Create `third_party/<name>/` with `CMakeLists.txt`, optional `download.sh`,
   and `README.md`.
2. Expose `add_subdirectory()`-friendly targets.
3. Wire it up in `build.sh` and the consumer's `CMakeLists.txt`.
4. Document it in this file.