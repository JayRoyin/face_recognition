# -----------------------------------------------------------------------------
# CMakeLists.txt — OPTIONAL vendored libmicrohttpd.
#
# libmicrohttpd is only needed by the face_db_web target. By default we rely
# on the system apt package (`libmicrohttpd-dev`) because it has no ABI
# surprises. On Jetson / air-gapped builds, enable vendoring:
#
#     ./download.sh            # fetches the upstream tarball
#     cmake -B build -DBUILD_VENDORED_LIBMICROHTTPD=ON
# -----------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.10)
project(libmicrohttpd_vendored LANGUAGES C)

option(BUILD_VENDORED_LIBMICROHTTPD
    "Build vendored libmicrohttpd instead of using the system one"
    OFF)

if(NOT BUILD_VENDORED_LIBMICROHTTPD)
    message(STATUS "[libmicrohttpd] using system package (set -DBUILD_VENDORED_LIBMICROHTTPD=ON to override)")
    return()
endif()

set(_src "${CMAKE_CURRENT_SOURCE_DIR}/src/libmicrohttpd")
if(NOT EXISTS "${_src}/src/microhttpd/microhttpd.h")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/download.sh")
        message(STATUS "[libmicrohttpd] fetching source via download.sh")
        execute_process(COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/download.sh"
            RESULT_VARIABLE _rc)
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR "[libmicrohttpd] download.sh failed")
        endif()
    else()
        message(FATAL_ERROR "[libmicrohttpd] source missing; run download.sh first")
    endif()
endif()

# libmicrohttpd ships an autoconf build, not CMake. We invoke configure.
include(ExternalProject)
ExternalProject_Add(libmicrohttpd_ep
    SOURCE_DIR    "${_src}"
    CONFIGURE_COMMAND "${_src}/configure"
                    --prefix=${CMAKE_INSTALL_PREFIX}
                    --disable-shared
                    --enable-static
                    --disable-doc
                    --disable-curl
                    --disable-examples
    BUILD_COMMAND   make -j
    INSTALL_COMMAND make install
    BUILD_IN_SOURCE 1
    BUILD_ALWAYS     1
)

# Expose an INTERFACE target so consumers can `target_link_libraries(... libmicrohttpd)`
add_library(libmicrohttpd INTERFACE)
target_include_directories(libmicrohttpd INTERFACE
    $<BUILD_INTERFACE:${CMAKE_INSTALL_PREFIX}/include>
)
target_link_libraries(libmicrohttpd INTERFACE
    ${CMAKE_INSTALL_PREFIX}/lib/libmicrohttpd.a
    pthread
)
add_dependencies(libmicrohttpd libmicrohttpd_ep)

message(STATUS "[libmicrohttpd] vendored build configured")