# third_party/spatialite — OPTIONAL vendored SpatiaLite

This is **opt-in**. The face_recognition project itself only depends on
SQLite3, not SpatiaLite. SpatiaLite is here for users who want a fully
self-contained GIS toolchain (so e.g. QGIS or GDAL can `dlopen()` a
`libspatialite.so.7` that is guaranteed to be ABI-compatible with our
`libsqlite3.so`).

## Enable

```bash
cmake -B build -DBUILD_VENDORED_SPATIALITE=ON ...
```

Requires the **system** to provide (these are *not* vendored):

- `libgeos-dev`
- `libproj-dev`

## Fetching the source

```bash
./download.sh          # default 5.1.0
SPATIALITE_VERSION=5.0.1 ./download.sh
```

Then build via the normal CMake flow.

## Cross-compile notes

- SpatiaLite is pure C/C++ + GEOS/PROJ C API; cross-compiles to aarch64/Jetson
  cleanly.
- It must link against the **vendored** SQLite (`-DSQLITE_ENABLE_RTREE=1`
  + `-DSQLITE_ENABLE_GEOPOLY=1`); otherwise the same
  `undefined symbol: sqlite3_rtree_query_callback` error recurs.