cmake_minimum_required(VERSION 3.10)
project(face_recognition_ros2)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(image_transport REQUIRED)
find_package(cv_bridge REQUIRED)
find_package(std_msgs REQUIRED)
find_package(builtin_interfaces REQUIRED)
find_package(face_recognition_ros2_interfaces REQUIRED)
find_package(OpenCV 4.5 REQUIRED)

# Core library: link the one ./build.sh CORE installed into install/lib.
#
# This used to add_subdirectory() the core sources, which (a) compiled a second
# copy of the same library on every build — colcon already built the core as a
# package — and (b) let this node and face_recognition_app drift onto different
# builds of the core. build.sh passes -DCMAKE_PREFIX_PATH=<repo>/install so the
# package resolves without the core being a colcon package.
find_package(face_recognition_core 1.0 REQUIRED)

include_directories(
  include
  ${OpenCV_INCLUDE_DIRS}
)

set(HEADERS
  include/face_recognition_ros2/node.hpp
  include/face_recognition_ros2/face_viewer.hpp
  include/face_recognition_ros2/face_stream_server.hpp
)

set(SOURCES
  src/main.cpp
  src/node.cpp
)

add_executable(face_recognition_node ${SOURCES} ${HEADERS})

ament_target_dependencies(face_recognition_node
  rclcpp
  sensor_msgs
  image_transport
  cv_bridge
  std_msgs
  builtin_interfaces
  face_recognition_ros2_interfaces
)

target_link_libraries(face_recognition_node
  face_recognition_core::face_recognition_core
  ${OpenCV_LIBS}
)

# The node is installed to install/ros2/<pkg>/lib/<pkg>/, while the shared core
# library lives in the plain-CMake install root, four levels up.
set_target_properties(face_recognition_node PROPERTIES
  INSTALL_RPATH "$ORIGIN/../../../../lib"
)

# ---------------------------------------------------------------------------
# Face viewer (UI): draws bounding boxes + labels on incoming frames.
#
# Note: deliberately NOT linking libopencv_highgui here. On hosts with
# snap GTK3 modules (vscode/etc.), highgui -> libgtk-3 -> snap libpthread
# -> GLIBC_PRIVATE symbol clash at startup. We only draw (imgproc) and
# publish; the user views /face/annotated via rqt_image_view.
# ---------------------------------------------------------------------------
add_executable(face_viewer_node
  src/face_viewer_main.cpp
  src/face_viewer.cpp
  include/face_recognition_ros2/face_viewer.hpp
)

ament_target_dependencies(face_viewer_node
  rclcpp
  sensor_msgs
  image_transport
  cv_bridge
  std_msgs
  builtin_interfaces
  face_recognition_ros2_interfaces
)

target_link_libraries(face_viewer_node
  opencv_core
  opencv_imgproc
)

# ---------------------------------------------------------------------------
# Face stream server: HTTP MJPEG publisher for an image topic.
#
# This is a Qt/GTK-free way to view annotated images — useful on hosts where
# rqt_image_view crashes (e.g. snap GTK3 modules pulling snap-core20
# libpthread). Runs a small libmicrohttpd daemon that exposes /stream as a
# multipart/x-mixed-replace MJPEG stream. Open http://localhost:8090/ in
# any browser to view.
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(MICROHTTPD REQUIRED libmicrohttpd)

add_executable(face_stream_server
  src/face_stream_server_main.cpp
  src/face_stream_server.cpp
  include/face_recognition_ros2/face_stream_server.hpp
)

ament_target_dependencies(face_stream_server
  rclcpp
  sensor_msgs
  image_transport
  cv_bridge
  std_msgs
)

target_include_directories(face_stream_server PRIVATE
  ${MICROHTTPD_INCLUDE_DIRS}
)

target_link_libraries(face_stream_server
  ${MICROHTTPD_LIBRARIES}
  opencv_core
  opencv_imgcodecs
  opencv_imgproc
)

install(TARGETS face_recognition_node face_viewer_node face_stream_server
  DESTINATION lib/${PROJECT_NAME}
)

install(DIRECTORY launch config
  DESTINATION share/${PROJECT_NAME}/
)

if(BUILD_TESTING)
  find_package(ament_lint_auto REQUIRED)
  ament_lint_auto_find_test_dependencies()
endif()

ament_package()
