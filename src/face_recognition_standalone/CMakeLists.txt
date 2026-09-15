cmake_minimum_required(VERSION 3.10)
project(face_recognition_standalone LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# -----------------------------------------------------------------------------
# Locate project root so we can reuse the existing face_recognition_core and
# third_party/* exactly the same way the ROS versions do.
# -----------------------------------------------------------------------------
set(_cursor "${CMAKE_CURRENT_SOURCE_DIR}")
set(_i 0)
while(_i LESS 8)
    if(EXISTS "${_cursor}/src/face_recognition_core/CMakeLists.txt" AND
       EXISTS "${_cursor}/third_party/sqlite3/CMakeLists.txt")
        set(_PROJECT_ROOT "${_cursor}")
        break()
    endif()
    get_filename_component(_cursor "${_cursor}" DIRECTORY)
    math(EXPR _i "${_i} + 1")
endwhile()

if(NOT _PROJECT_ROOT)
    message(FATAL_ERROR
        "face_recognition_standalone: could not locate project root. "
        "Place this directory under <repo>/src/.")
endif()

message(STATUS "face_recognition_standalone: project root = ${_PROJECT_ROOT}")

if(NOT DEFINED FACE_RECOGNITION_VENDORED_PREFIX)
    set(FACE_RECOGNITION_VENDORED_PREFIX
        "${_PROJECT_ROOT}/install/vendored"
        CACHE PATH "Install prefix of vendored third-party libs")
endif()

# Reuse the core library (detector + recognizer + database) via add_subdirectory.
# This is the same pattern used by the ROS1/ROS2 packages.
add_subdirectory(${_PROJECT_ROOT}/src/face_recognition_core
                 ${CMAKE_BINARY_DIR}/face_recognition_core_build)

# The core library exports its own headers transitively, but we also need
# OpenCV headers in this target (for highgui/videoio/imgproc/imgcodecs).
# face_recognition_core only links against ${OpenCV_LIBS}, so the OpenCV
# include directory does not propagate. Pull it in explicitly.
find_package(OpenCV 4.5 REQUIRED)
find_package(Threads REQUIRED)

# -----------------------------------------------------------------------------
# Sources
# -----------------------------------------------------------------------------
set(STANDALONE_SOURCES
    src/cli.cpp
    src/video_source.cpp
    src/recognition_pipeline.cpp
    src/main.cpp
)

add_executable(face_recognition_app ${STANDALONE_SOURCES})

target_include_directories(face_recognition_app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(face_recognition_app PRIVATE
    face_recognition_core
    ${OpenCV_LIBS}
    Threads::Threads
    dl
)

# -----------------------------------------------------------------------------
# Copy default config files alongside the binary so `./face_recognition_app help`
# works out-of-the-box without users reading the README.
# -----------------------------------------------------------------------------
set(_runtime_files
    README.md
)
foreach(f IN LISTS _runtime_files)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${f}")
        configure_file("${CMAKE_CURRENT_SOURCE_DIR}/${f}"
                       "${CMAKE_BINARY_DIR}/${f}" COPYONLY)
    endif()
endforeach()

# Helpful alias for IDEs / multi-config tools.
set_target_properties(face_recognition_app PROPERTIES
    OUTPUT_NAME "face_recognition_app"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

message(STATUS "face_recognition_standalone: built face_recognition_app")
