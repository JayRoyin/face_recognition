cmake_minimum_required(VERSION 3.10)
project(face_recognition_ros2_interfaces)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(ament_cmake REQUIRED)
find_package(rosidl_default_generators REQUIRED)
find_package(std_msgs REQUIRED)
find_package(builtin_interfaces REQUIRED)

rosidl_generate_interfaces(${PROJECT_NAME}
  "msg/FaceInfo.msg"
  "msg/FaceResult.msg"
  "srv/AddFace.srv"
  "srv/RemoveFace.srv"
  "srv/ListFaces.srv"
  "srv/ClearFaces.srv"
  "srv/AddTemplate.srv"
  "srv/Verify.srv"
  DEPENDENCIES builtin_interfaces std_msgs
)

ament_package()
