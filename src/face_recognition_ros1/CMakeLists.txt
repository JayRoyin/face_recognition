cmake_minimum_required(VERSION 3.10)
project(face_recognition_ros1)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(catkin REQUIRED COMPONENTS
  roscpp
  sensor_msgs
  image_transport
  cv_bridge
  std_msgs
  std_srvs
  face_recognition_ros1_interfaces
)

find_package(OpenCV 4.5 REQUIRED)

# Core library: link the one ./build.sh CORE installed.
#
# NOTE: this package previously linked NO core library at all — no
# add_subdirectory(), no find_package(), and no core entry in package.xml — even
# though node.hpp includes face_recognition_core/*.hpp. It therefore could not
# compile. build.sh passes -DCMAKE_PREFIX_PATH=<repo>/install.
find_package(face_recognition_core 1.0 REQUIRED)

catkin_package(
  CATKIN_DEPENDS
    roscpp
    sensor_msgs
    image_transport
    cv_bridge
    std_msgs
    std_srvs
    face_recognition_ros1_interfaces
)

include_directories(
  include
  ${catkin_INCLUDE_DIRS}
  ${OpenCV_INCLUDE_DIRS}
)

add_executable(face_recognition_node
  src/main.cpp
  src/node.cpp
)

add_dependencies(face_recognition_node
  ${${PROJECT_NAME}_EXPORTED_TARGETS}
  ${catkin_EXPORTED_TARGETS}
  face_recognition_ros1_interfaces_generate_message_cpp
)

target_link_libraries(face_recognition_node
  face_recognition_core::face_recognition_core
  ${catkin_LIBRARIES}
  ${OpenCV_LIBS}
)

install(TARGETS face_recognition_node
  RUNTIME DESTINATION ${CATKIN_PACKAGE_BIN_DESTINATION}
)

install(DIRECTORY launch config
  DESTINATION ${CATKIN_PACKAGE_SHARE_DESTINATION}
)
