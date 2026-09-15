#include "face_recognition_ros2/face_stream_server.hpp"

int main(int argc, char* argv[]) {
    rclcpp::init(argc, argv);
    rclcpp::spin(std::make_shared<face_recognition_ros2::FaceStreamServer>());
    rclcpp::shutdown();
    return 0;
}