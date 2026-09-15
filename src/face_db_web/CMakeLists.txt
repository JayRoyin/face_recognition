cmake_minimum_required(VERSION 3.10)
project(face_db_web)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(OpenCV 4.5 REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(MICROHTTPD REQUIRED libmicrohttpd)

# ---------------------------------------------------------------------------
# Resolve the face_recognition_core directory and prefer in-tree subdirectory
# build (so the same project-local SQLite3 vendored by build.sh is used).
# ---------------------------------------------------------------------------
set(_core_src ${CMAKE_CURRENT_SOURCE_DIR}/../face_recognition_core)
set(FACE_RECOGNITION_CORE_DIR ${_core_src})

if(EXISTS ${_core_src}/CMakeLists.txt)
    add_subdirectory(${_core_src} face_recognition_core_build)
    set(_core_target face_recognition_core)
else()
    find_package(face_recognition_core REQUIRED)
    set(_core_target face_recognition_core::face_recognition_core)
endif()

set(SOURCES
    src/main.cpp
    src/http_server.cpp
    src/face_handler.cpp
)

add_executable(face_db_web ${SOURCES})

target_include_directories(face_db_web PRIVATE
  include
  ${FACE_RECOGNITION_CORE_DIR}/include
  ${OpenCV_INCLUDE_DIRS}
  ${MICROHTTPD_INCLUDE_DIRS}
)

target_link_libraries(face_db_web
  ${_core_target}
  ${OpenCV_LIBS}
  ${MICROHTTPD_LIBRARIES}
  dl
)

# Make the executable find libface_recognition_core.so.1 from the install tree
# even when launched directly without LD_LIBRARY_PATH.
set_target_properties(face_db_web PROPERTIES
  INSTALL_RPATH "$ORIGIN/../lib"
  BUILD_RPATH   "${CMAKE_BINARY_DIR}/face_recognition_core_build"
)

install(TARGETS face_db_web
  RUNTIME DESTINATION bin
)

install(DIRECTORY templates
  DESTINATION share/face_db_web
)
