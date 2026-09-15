cmake_minimum_required(VERSION 3.10)
project(face_recognition_ros1_interfaces)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(catkin REQUIRED COMPONENTS
  message_generation
  std_msgs
)

add_message_files(
  DIRECTORY msg
  FILES
    FaceInfo.msg
    FaceResult.msg
)

add_service_files(
  DIRECTORY srv
  FILES
    AddFace.srv
    RemoveFace.srv
    ListFaces.srv
    ClearFaces.srv
)

generate_messages(
  DEPENDENCIES
    std_msgs
)

catkin_package(
  CATKIN_DEPENDS
    message_runtime
    std_msgs
)
