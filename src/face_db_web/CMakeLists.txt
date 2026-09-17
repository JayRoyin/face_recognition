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
# Core library.
#
# find_package (NOT add_subdirectory): this binary must use the SAME shared
# library as face_recognition_app. A stale face_db_web that compiled its own
# copy of the core once wrote embeddings from an old feature space into the
# shared database, and nothing could detect it.
#
# Configure with -DCMAKE_PREFIX_PATH=<repo>/install (build.sh passes it).
# ---------------------------------------------------------------------------
find_package(face_recognition_core 1.0 REQUIRED)

set(SOURCES
    src/main.cpp
    src/http_server.cpp
    src/face_handler.cpp
)

add_executable(face_db_web ${SOURCES})

target_include_directories(face_db_web PRIVATE
  include
  ${OpenCV_INCLUDE_DIRS}
  ${MICROHTTPD_INCLUDE_DIRS}
)

target_link_libraries(face_db_web
  face_recognition_core::face_recognition_core
  ${OpenCV_LIBS}
  ${MICROHTTPD_LIBRARIES}
  dl
)

# Make the executable find libface_recognition_core.so.1 from the install tree
# even when launched directly without LD_LIBRARY_PATH.
set_target_properties(face_db_web PROPERTIES
  INSTALL_RPATH "$ORIGIN/../lib"
)

install(TARGETS face_db_web
  RUNTIME DESTINATION bin
)

install(DIRECTORY templates
  DESTINATION share/face_db_web
)
