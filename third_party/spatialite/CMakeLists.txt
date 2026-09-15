# -----------------------------------------------------------------------------
# CMakeLists.txt — OPTIONAL vendored SpatiaLite.
#
# Why opt-in:
#   - SpatiaLite is only needed by external tools (QGIS, GDAL, custom GIS
#     pipelines). The face_recognition project itself does not link against
#     libspatialite — it only uses plain SQLite3.
#   - Building SpatiaLite pulls in GEOS + PROJ, which are heavier dependencies
#     and are typically *not* vendored (they are not the source of the
#     undefined-symbol error; the error is purely a sqlite3 ABI mismatch).
#
# Enable with:  cmake -DBUILD_VENDORED_SPATIALITE=ON
# -----------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.10)
project(spatialite_vendored LANGUAGES C CXX)

option(BUILD_VENDORED_SPATIALITE
    "Build the vendored libspatialite (requires system GEOS + PROJ headers)"
    OFF)

if(NOT BUILD_VENDORED_SPATIALITE)
    message(STATUS "[spatialite] disabled (set -DBUILD_VENDORED_SPATIALITE=ON to enable)")
    return()
endif()

# ---------------------------------------------------------------------------
# SpatiaLite source must be fetched ahead of time:
#     ./download.sh
# ---------------------------------------------------------------------------
set(_src_dir "${CMAKE_CURRENT_SOURCE_DIR}/src/libspatialite")
if(NOT EXISTS "${_src_dir}/sqlite3.h")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/download.sh")
        message(STATUS "[spatialite] fetching via download.sh …")
        execute_process(
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/download.sh"
            RESULT_VARIABLE _rc
        )
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR "[spatialite] download.sh failed")
        endif()
    else()
        message(FATAL_ERROR
            "[spatialite] source missing and download.sh not found.\n"
            "Run third_party/spatialite/download.sh first.")
    endif()
endif()

# ---------------------------------------------------------------------------
# System deps for SpatiaLite (GEOS, PROJ). We require them from the system
# because vendoring them is outside the scope of this project.
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(GEOS    REQUIRED geos)
pkg_check_modules(PROJ    REQUIRED proj)
pkg_check_modules(SQLITE3 REQUIRED sqlite3)

# Force the project-local sqlite3 over the system one when available.
set(_local_sqlite "${CMAKE_SOURCE_DIR}/third_party/sqlite3")
if(EXISTS "${_local_sqlite}/libsqlite3.so" OR EXISTS "${_local_sqlite}/CMakeLists.txt")
    # When wired through add_subdirectory, prefer the in-tree sqlite3
    if(TARGET sqlite3_shared)
        set(_sqlite_target sqlite3_shared)
        set(_sqlite_inc    "${_local_sqlite}")
    endif()
endif()

# ---------------------------------------------------------------------------
# Compile libspatialite as a shared object.
# ---------------------------------------------------------------------------
file(GLOB SPATIALITE_SOURCES
    "${_src_dir}/*.c"
    "${_src_dir}/*.cpp"
)

# SpatiaLite does not ship its own CMakeLists; we replicate the minimum.
add_library(spatialite SHARED ${SPATIALITE_SOURCES})
target_include_directories(spatialite PUBLIC
    ${_src_dir}
    ${GEOS_INCLUDE_DIRS}
    ${PROJ_INCLUDE_DIRS}
    ${SQLITE3_INCLUDE_DIRS}
)
target_compile_options(spatialite PRIVATE
    -Wno-unused-parameter
    -DSQLITE_ENABLE_RTREE=1
    -DSQLITE_ENABLE_GEOPOLY=1
)
target_link_libraries(spatialite
    ${GEOS_LIBRARIES}
    ${PROJ_LIBRARIES}
    ${SQLITE3_LIBRARIES}
    m
)

set_target_properties(spatialite PROPERTIES
    OUTPUT_NAME spatialite
    VERSION   7.1.0
    SOVERSION 7
)

install(TARGETS spatialite
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

message(STATUS "[spatialite] built against ${SQLITE3_LIBRARIES}")
message(STATUS "[spatialite] note: it expects sqlite3_rtree_query_callback; "
               "ensure the linked libsqlite3 was compiled with SQLITE_ENABLE_RTREE")