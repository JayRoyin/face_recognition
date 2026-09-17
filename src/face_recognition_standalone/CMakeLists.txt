cmake_minimum_required(VERSION 3.10)
project(face_recognition_standalone LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# -----------------------------------------------------------------------------
# Core library.
#
# find_package (NOT add_subdirectory): this app links the SAME installed
# libface_recognition_core.so as face_db_web and the ROS nodes, so a fix in the
# core can never end up in one binary but not another.
#
# Configure with -DCMAKE_PREFIX_PATH=<repo>/install (build.sh passes it), or run
# ./build.sh CORE first so the package exists.
# -----------------------------------------------------------------------------
find_package(face_recognition_core 1.0 REQUIRED)

# The core target propagates its own headers transitively, but this target also
# uses OpenCV directly (highgui / videoio / imgproc / imgcodecs).
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
    face_recognition_core::face_recognition_core
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
    # Let the INSTALLED binary resolve libface_recognition_core.so from
    # <prefix>/lib without any LD_LIBRARY_PATH setup, exactly like face_db_web.
    INSTALL_RPATH "$ORIGIN/../lib"
)

# -----------------------------------------------------------------------------
# Install into the project-wide install root (install/bin) so there is exactly
# one place users pick binaries from, instead of a build/ dir inside the package.
# -----------------------------------------------------------------------------
install(TARGETS face_recognition_app
    RUNTIME DESTINATION bin
)

message(STATUS "face_recognition_standalone: built face_recognition_app")
