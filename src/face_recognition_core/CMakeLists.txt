cmake_minimum_required(VERSION 3.10)
project(face_recognition_core)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -----------------------------------------------------------------------------
# Resolve project layout so vendored deps can be located.
# When consumed via add_subdirectory(), CMAKE_SOURCE_DIR is the *top-level*
# project. When built standalone from src/face_recognition_core, we have to
# walk up the directory tree until we find third_party/.
# -----------------------------------------------------------------------------
set(_PROJECT_ROOT "${CMAKE_SOURCE_DIR}")

# Walk upwards until we find third_party/ — max 6 levels.
set(_cursor "${CMAKE_SOURCE_DIR}")
set(_i 0)
while(_i LESS 6)
    if(EXISTS "${_cursor}/third_party/sqlite3/CMakeLists.txt")
        set(_PROJECT_ROOT "${_cursor}")
        break()
    endif()
    get_filename_component(_cursor "${_cursor}" DIRECTORY)
    math(EXPR _i "${_i} + 1")
endwhile()

# Allow callers to override (e.g. colcon passes its own prefix)
if(NOT DEFINED FACE_RECOGNITION_VENDORED_PREFIX)
    set(FACE_RECOGNITION_VENDORED_PREFIX
        "${_PROJECT_ROOT}/install/vendored"
        CACHE PATH "Install prefix of vendored third-party libs")
endif()

# -----------------------------------------------------------------------------
# OpenCV, Threads, UUID — system deps (not vendored).
# -----------------------------------------------------------------------------
find_package(OpenCV 4.5 REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(UUID REQUIRED uuid)

# -----------------------------------------------------------------------------
# ONNX Runtime — required for RetinaFace detection (det_10g.onnx).
# OpenCV 4.5 dnn cannot run models with dynamic Reshape outputs.
# -----------------------------------------------------------------------------
set(_onnxruntime_inc_candidates
    /home/qstdc/.local/onnxruntime/include
    /home/qstdc/anaconda3/pkgs/onnxruntime-cpp-1.16.3-h7b6976b_4_cuda/include/onnxruntime
    /opt/onnxruntime/include
    /usr/local/include
    /usr/include)

set(_onnxruntime_lib_candidates
    /home/qstdc/.local/onnxruntime/lib
    /home/qstdc/anaconda3/pkgs/onnxruntime-cpp-1.16.3-h7b6976b_4_cuda/lib
    /opt/onnxruntime/lib
    /usr/local/lib
    /usr/lib
    /usr/lib/x86_64-linux-gnu)

set(ONNXRUNTIME_INCLUDE_DIR "")
foreach(inc_dir IN LISTS _onnxruntime_inc_candidates)
    if(EXISTS "${inc_dir}/core/session/onnxruntime_cxx_api.h")
        # Path includes the trailing "onnxruntime" already.
        # Strip it so #include <onnxruntime/...> resolves correctly.
        get_filename_component(_inc_parent "${inc_dir}" DIRECTORY)
        if(EXISTS "${_inc_parent}/onnxruntime/core/session/onnxruntime_cxx_api.h")
            set(ONNXRUNTIME_INCLUDE_DIR "${_inc_parent}")
        else()
            set(ONNXRUNTIME_INCLUDE_DIR "${inc_dir}")
        endif()
        break()
    endif()
endforeach()

if(NOT ONNXRUNTIME_INCLUDE_DIR)
    find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h
        PATH_SUFFIXES onnxruntime/core/session core/session)
endif()

set(ONNXRUNTIME_LIBRARY "")
foreach(lib_dir IN LISTS _onnxruntime_lib_candidates)
    if(EXISTS "${lib_dir}/libonnxruntime.so")
        set(ONNXRUNTIME_LIBRARY "${lib_dir}/libonnxruntime.so")
        break()
    endif()
endforeach()

if(NOT ONNXRUNTIME_LIBRARY)
    find_library(ONNXRUNTIME_LIBRARY NAMES onnxruntime)
endif()

if(NOT ONNXRUNTIME_INCLUDE_DIR OR NOT ONNXRUNTIME_LIBRARY)
    message(FATAL_ERROR "ONNX Runtime not found. Please install ONNX Runtime C++.")
endif()

message(STATUS "face_recognition_core: ONNX Runtime at ${ONNXRUNTIME_LIBRARY}")
message(STATUS "face_recognition_core: ONNX Runtime headers at ${ONNXRUNTIME_INCLUDE_DIR}")

# -----------------------------------------------------------------------------
# SQLite3 — prefer vendored, fall back to system pkg-config.
# The vendored copy always has SQLITE_ENABLE_RTREE + GEOPOLY, so external
# tools (spatialite, GDAL rtree) load correctly regardless of the host.
# -----------------------------------------------------------------------------
set(_vendored_sqlite_lib_shared
    "${FACE_RECOGNITION_VENDORED_PREFIX}/lib/libsqlite3.so")
set(_vendored_sqlite_lib_static
    "${FACE_RECOGNITION_VENDORED_PREFIX}/lib/libsqlite3.a")
set(_vendored_sqlite_inc
    "${FACE_RECOGNITION_VENDORED_PREFIX}/include")

set(USE_VENDORED_SQLITE OFF
    CACHE BOOL "Link against vendored SQLite3 instead of the system one")

if(EXISTS "${_vendored_sqlite_inc}/sqlite3.h" AND
   (EXISTS "${_vendored_sqlite_lib_shared}" OR
    EXISTS "${_vendored_sqlite_lib_static}"))
    set(USE_VENDORED_SQLITE ON)
    message(STATUS "face_recognition_core: using vendored SQLite3 at "
                   "${FACE_RECOGNITION_VENDORED_PREFIX}")
endif()

if(USE_VENDORED_SQLITE)
    if(EXISTS "${_vendored_sqlite_lib_static}")
        set(SQLITE3_LIBRARIES "${_vendored_sqlite_lib_static}")
    else()
        set(SQLITE3_LIBRARIES "${_vendored_sqlite_lib_shared}")
    endif()
    set(SQLITE3_INCLUDE_DIRS "${_vendored_sqlite_inc}")
else()
    message(STATUS "face_recognition_core: vendored SQLite3 not found, "
                   "falling back to pkg-config sqlite3")
    pkg_check_modules(SQLITE3 REQUIRED sqlite3)
endif()

# -----------------------------------------------------------------------------
# Sources
# -----------------------------------------------------------------------------
set(SOURCES
    src/face_detector.cpp
    src/face_recognizer.cpp
    src/face_database.cpp
)

add_library(face_recognition_core SHARED ${SOURCES})

target_include_directories(face_recognition_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
    ${SQLITE3_INCLUDE_DIRS}
    ${ONNXRUNTIME_INCLUDE_DIR}
)

target_link_libraries(face_recognition_core
    PUBLIC
    ${OpenCV_LIBS}
    Threads::Threads
    ${SQLITE3_LIBRARIES}
    ${UUID_LIBRARIES}
    ${ONNXRUNTIME_LIBRARY}
    dl
)

# RPATH so the .so loads the vendored libsqlite3.so first, regardless of
# the host system. We point at both install/vendored/lib (when vendored is
# installed) and the in-tree third_party/sqlite3/build dir (during dev).
set(_rpath_dirs "")
if(EXISTS "${FACE_RECOGNITION_VENDORED_PREFIX}/lib")
    list(APPEND _rpath_dirs "${FACE_RECOGNITION_VENDORED_PREFIX}/lib")
endif()
if(TARGET sqlite3_shared)
    # In-tree development: the shared sqlite3 lives in third_party/sqlite3/build
    list(APPEND _rpath_dirs "${CMAKE_SOURCE_DIR}/third_party/sqlite3/build")
endif()
if(_rpath_dirs)
    list(REMOVE_ITEM _rpath_dirs "")
    set_target_properties(face_recognition_core PROPERTIES
        INSTALL_RPATH "${_rpath_dirs}"
        BUILD_RPATH    "${_rpath_dirs}"
    )
endif()

set_target_properties(face_recognition_core PROPERTIES VERSION "1.0.0" SOVERSION 1)

install(TARGETS face_recognition_core
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(DIRECTORY include/
    DESTINATION include
)