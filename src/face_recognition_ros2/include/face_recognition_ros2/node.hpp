#pragma once

#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <image_transport/image_transport.hpp>
#include <cv_bridge/cv_bridge.h>

#include <memory>
#include <string>
#include <vector>

#include "face_recognition_core/types.hpp"
#include "face_recognition_core/face_detector.hpp"
#include "face_recognition_core/face_recognizer.hpp"
#include "face_recognition_core/face_database.hpp"

#include "face_recognition_ros2_interfaces/msg/face_info.hpp"
#include "face_recognition_ros2_interfaces/msg/face_result.hpp"
#include "face_recognition_ros2_interfaces/srv/add_face.hpp"
#include "face_recognition_ros2_interfaces/srv/remove_face.hpp"
#include "face_recognition_ros2_interfaces/srv/list_faces.hpp"
#include "face_recognition_ros2_interfaces/srv/clear_faces.hpp"

namespace face_recognition_ros2 {

class FaceRecognitionNode : public rclcpp::Node {
public:
    FaceRecognitionNode();
    ~FaceRecognitionNode();

private:
    void imageCallback(const sensor_msgs::msg::Image::ConstSharedPtr& msg);

    void addFaceCallback(const std::shared_ptr<face_recognition_ros2_interfaces::srv::AddFace::Request> req,
                        const std::shared_ptr<face_recognition_ros2_interfaces::srv::AddFace::Response> res);
    void removeFaceCallback(const std::shared_ptr<face_recognition_ros2_interfaces::srv::RemoveFace::Request> req,
                           const std::shared_ptr<face_recognition_ros2_interfaces::srv::RemoveFace::Response> res);
    void listFacesCallback(const std::shared_ptr<face_recognition_ros2_interfaces::srv::ListFaces::Request> req,
                          const std::shared_ptr<face_recognition_ros2_interfaces::srv::ListFaces::Response> res);
    void clearFacesCallback(const std::shared_ptr<face_recognition_ros2_interfaces::srv::ClearFaces::Request> req,
                           const std::shared_ptr<face_recognition_ros2_interfaces::srv::ClearFaces::Response> res);

    std::vector<uint8_t> base64Decode(const std::string& encoded);

    std::unique_ptr<face_recognition::FaceDetector> detector_;
    std::unique_ptr<face_recognition::FaceRecognizer> recognizer_;
    std::unique_ptr<face_recognition::FaceDatabase> database_;

    rclcpp::Subscription<sensor_msgs::msg::Image>::SharedPtr image_sub_;
    rclcpp::Publisher<face_recognition_ros2_interfaces::msg::FaceResult>::SharedPtr result_pub_;

    rclcpp::Service<face_recognition_ros2_interfaces::srv::AddFace>::SharedPtr add_srv_;
    rclcpp::Service<face_recognition_ros2_interfaces::srv::RemoveFace>::SharedPtr remove_srv_;
    rclcpp::Service<face_recognition_ros2_interfaces::srv::ListFaces>::SharedPtr list_srv_;
    rclcpp::Service<face_recognition_ros2_interfaces::srv::ClearFaces>::SharedPtr clear_srv_;

    float confidence_threshold_;
    std::string db_path_;
    std::string faces_dir_;
    std::string detection_model_path_;
    std::string recognition_model_path_;
};

}  // namespace face_recognition_ros2
