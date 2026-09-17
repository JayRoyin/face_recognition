# third_party — vendored dependencies

The vendored modules listed below are built as part of the project. Nothing here
writes to `/usr/local` or `/usr/lib`, so the project is **fully reproducible**
on a fresh machine (x86_64, aarch64/Jetson, ARM) with only a C/C++ toolchain.

> `insightface/` and `opencv_zoo/` are an exception: they are **read-only
> references** and are not built. See "Reference implementations" below.

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

## Reference implementations (NOT built)

Two upstream repositories are kept here purely as a **reference** for the
detection / alignment / embedding front-end in `src/face_recognition_core/`.
Unlike the modules above they have no `CMakeLists.txt`, are not pulled in by
`add_subdirectory()`, and are not linked into any target.

| Module | Source | Purpose |
|---|---|---|
| [`insightface/`](insightface/) | [deepinsight/insightface](https://github.com/deepinsight/insightface) | Origin of `models/det_10g.onnx` (SCRFD-10GF) and `models/w600k_r50.onnx` (ArcFace R50). Authoritative reference for ArcFace input normalisation, the 112x112 alignment template, and the SCRFD decode / letterbox / NMS. |
| [`opencv_zoo/`](opencv_zoo/) | [opencv/opencv_zoo](https://github.com/opencv/opencv_zoo) | Apache-2.0 alternative stack (YuNet + SFace) with C++ demos — the licence-clean option if the project ever ships commercially. |

Fetch / refresh / remove them with:

```bash
./third_party/fetch_reference.sh          # clone, or reuse existing copy
./third_party/fetch_reference.sh --clean  # delete local copies
```

Both are pinned to a fixed commit inside the script, and both are listed in
`.gitignore` (source is ~80 MB; the opencv_zoo LFS weights are ~1.4 GB and are
deliberately skipped). A fresh machine therefore needs to run the fetch script
once before the reference sources are available.

**The file-by-file comparison against this project's implementation lives in
[`REFERENCE.md`](REFERENCE.md)** — that is the document to read when
investigating detection or recognition accuracy.

## Adding a new third-party module

1. Create `third_party/<name>/` with `CMakeLists.txt`, optional `download.sh`,
   and `README.md`.
2. Expose `add_subdirectory()`-friendly targets.
3. Wire it up in `build.sh` and the consumer's `CMakeLists.txt`.
4. Document it in this file.

If the new module is a **read-only reference** rather than a build dependency,
follow the `insightface/` / `opencv_zoo/` pattern instead: add it to
`fetch_reference.sh`, register it here and in a section of `REFERENCE.md`, and
add it to `.gitignore`.