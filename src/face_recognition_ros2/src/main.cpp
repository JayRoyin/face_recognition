#include <rclcpp/rclcpp.hpp>
#include "face_recognition_ros2/node.hpp"

int main(int argc, char* argv[]) {
    rclcpp::init(argc, argv);

    auto node = std::make_shared<face_recognition_ros2::FaceRecognitionNode>();

    RCLCPP_INFO(node->get_logger(), "Face Recognition Node started");
    RCLCPP_INFO(node->get_logger(), "Subscribe to image topic and wait for face recognition results...");

    rclcpp::spin(node);

    rclcpp::shutdown();
    return 0;
}
