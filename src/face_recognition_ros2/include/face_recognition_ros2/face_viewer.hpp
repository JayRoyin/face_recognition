#pragma once

#ifndef FACE_RECOGNITION_ROS2_FACE_VIEWER_HPP
#define FACE_RECOGNITION_ROS2_FACE_VIEWER_HPP

#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <face_recognition_ros2_interfaces/msg/face_result.hpp>
#include <image_transport/image_transport.hpp>
#include <cv_bridge/cv_bridge.h>
#include <opencv2/core.hpp>

#include <map>
#include <mutex>

namespace face_recognition_ros2 {

// FaceViewer subscribes to the camera topic + recognition result topic,
// draws bounding boxes + labels, and re-publishes the annotated frame
// on /face/annotated for any image_transport subscriber to consume.
//
// Why no GUI window here?
//   cv::imshow() requires libopencv_highgui, which on this host pulls in
//   libgtk-3 -> snap-core20 libpthread.so.0 -> GLIBC_PRIVATE mismatch.
//   Decoupling the annotator (this node) from the viewer (rqt_image_view
//   or any other subscriber) keeps the node headless-safe and portable.
class FaceViewer : public rclcpp::Node {
public:
    explicit FaceViewer(const rclcpp::NodeOptions& options = rclcpp::NodeOptions());

private:
    void imageCallback(const sensor_msgs::msg::Image::ConstSharedPtr& msg);
    void resultCallback(const face_recognition_ros2_interfaces::msg::FaceResult::ConstSharedPtr& msg);

    cv::Mat drawAnnotations(const cv::Mat& frame);

    std::string image_topic_;
    std::string result_topic_;
    std::string annotated_topic_;

    rclcpp::Subscription<sensor_msgs::msg::Image>::SharedPtr                       image_sub_;
    rclcpp::Subscription<face_recognition_ros2_interfaces::msg::FaceResult>::SharedPtr result_sub_;
    image_transport::Publisher                                                    annotated_pub_;

    std::mutex results_mutex_;
    std::map<std::string, face_recognition_ros2_interfaces::msg::FaceInfo> latest_faces_;
};

}  // namespace face_recognition_ros2

#endif