# -----------------------------------------------------------------------------
# CMakeLists.txt — build the vendored amalgamation as a CMake target.
#
# Produces:
#     sqlite3_static  — static library (recommended for embed / RPATH-free use)
#     sqlite3_shared  — shared library  (use when an external tool dlopens it)
#     sqlite3         — alias for sqlite3_shared for convenience
#
# Adds the INTERFACE include directory so consumers can do:
#     add_subdirectory(third_party/sqlite3 sqlite3_build)
#     target_link_libraries(mylib PRIVATE sqlite3)
#
# To force a re-download of the amalgamation:
#     rm -f sqlite3.c sqlite3.h && ./download.sh
# -----------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.10)
project(sqlite3_vendored LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Locate amalgamation source (auto-fetch if missing).
# ---------------------------------------------------------------------------
set(_amalg_c "${CMAKE_CURRENT_SOURCE_DIR}/sqlite3.c")
set(_amalg_h "${CMAKE_CURRENT_SOURCE_DIR}/sqlite3.h")

if(NOT EXISTS "${_amalg_c}" OR NOT EXISTS "${_amalg_h}")
    message(STATUS "[sqlite3] amalgamation missing — attempting to fetch via download.sh")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/download.sh")
        execute_process(
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/download.sh"
            RESULT_VARIABLE _dl_rc
            OUTPUT_VARIABLE _dl_out
            ERROR_VARIABLE  _dl_err
        )
        if(NOT _dl_rc EQUAL 0)
            message(FATAL_ERROR
                "[sqlite3] download.sh failed (rc=${_dl_rc}).\n"
                "stderr:\n${_dl_err}\nstdout:\n${_dl_out}")
        endif()
    else()
        message(FATAL_ERROR
            "[sqlite3] Expected ${_amalg_c} but it does not exist and "
            "download.sh was not found. Please run download.sh manually.")
    endif()
endif()

# ---------------------------------------------------------------------------
# Compile flags required by SpatiaLite / modern SQLite consumers.
# SQLITE_ENABLE_RTREE      — required by libspatialite
# SQLITE_ENABLE_GEOPOLY    — required by SpatiaLite 5.x
# SQLITE_ENABLE_FTS5       — full-text search (useful, almost free)
# SQLITE_ENABLE_JSON1      — JSON functions (default-on upstream, explicit for safety)
# SQLITE_ENABLE_COLUMN_METADATA — used by some ORMs / GDAL
# SQLITE_THREADSAFE=1      — multi-thread safe
# SQLITE_TEMP_STORE=2      — temp tables in memory (faster)
# SQLITE_DEFAULT_FOREIGN_KEYS=1 — sane defaults
# SQLITE_USE_URI=1         — accept URI filenames (used by our DB layer)
# SQLITE_DQS=0             — disable double-quoted string literals (ANSI-friendly)
# ---------------------------------------------------------------------------
set(SQLITE3_ENABLE_DEFINES
    -DSQLITE_ENABLE_RTREE=1
    -DSQLITE_ENABLE_GEOPOLY=1
    -DSQLITE_ENABLE_FTS5=1
    -DSQLITE_ENABLE_JSON1=1
    -DSQLITE_ENABLE_COLUMN_METADATA=1
    -DSQLITE_THREADSAFE=1
    -DSQLITE_TEMP_STORE=2
    -DSQLITE_DEFAULT_FOREIGN_KEYS=1
    -DSQLITE_USE_URI=1
    -DSQLITE_DQS=0
)

set(SQLITE3_SOURCES "${_amalg_c}")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/shell.c")
    list(APPEND SQLITE3_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/shell.c")
endif()

# ---------------------------------------------------------------------------
# Shared library (needed when an external tool such as spatialite, GDAL,
# or the sqlite3 CLI dlopen()s it).
# ---------------------------------------------------------------------------
add_library(sqlite3_shared SHARED ${SQLITE3_SOURCES})
target_compile_options(sqlite3_shared PRIVATE ${SQLITE3_ENABLE_DEFINES})
target_include_directories(sqlite3_shared PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
set_target_properties(sqlite3_shared PROPERTIES
    OUTPUT_NAME sqlite3
    VERSION   3.46.1
    SOVERSION 0
    POSITION_INDEPENDENT_CODE ON
)
target_link_libraries(sqlite3_shared PUBLIC Threads::Threads dl m)

# Make sure pthread / dl / m are discoverable on every platform (incl. Jetson)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Static library — recommended for our own C++ targets; no RPATH juggling.
# ---------------------------------------------------------------------------
add_library(sqlite3_static STATIC ${SQLITE3_SOURCES})
target_compile_options(sqlite3_static PRIVATE ${SQLITE3_ENABLE_DEFINES})
target_include_directories(sqlite3_static PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
set_target_properties(sqlite3_static PROPERTIES
    OUTPUT_NAME sqlite3
    POSITION_INDEPENDENT_CODE ON
)
target_link_libraries(sqlite3_static PUBLIC Threads::Threads dl m)

# ---------------------------------------------------------------------------
# Convenience alias — most consumers want "sqlite3" regardless of linkage.
# Default to static (no RPATH issues); override with -DUSE_SQLITE_SHARED=ON.
# ---------------------------------------------------------------------------
option(USE_SQLITE_SHARED "Link sqlite3 as shared library" OFF)
if(USE_SQLITE_SHARED)
    add_library(sqlite3 ALIAS sqlite3_shared)
else()
    add_library(sqlite3 ALIAS sqlite3_static)
endif()

# ---------------------------------------------------------------------------
# Installs — both variants go to lib/ under CMAKE_INSTALL_PREFIX.
# ---------------------------------------------------------------------------
install(TARGETS sqlite3_shared sqlite3_static
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(FILES "${_amalg_h}"
    DESTINATION include
)

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "[sqlite3] amalgamation : ${_amalg_c}")
message(STATUS "[sqlite3] defines        : ${SQLITE3_ENABLE_DEFINES}")
message(STATUS "[sqlite3] linkage        : $<IF:$<BOOL:${USE_SQLITE_SHARED}>,shared,static>")