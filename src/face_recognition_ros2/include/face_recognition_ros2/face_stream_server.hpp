#pragma once

#ifndef FACE_RECOGNITION_ROS2_FACE_STREAM_SERVER_HPP
#define FACE_RECOGNITION_ROS2_FACE_STREAM_SERVER_HPP

#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <image_transport/image_transport.hpp>
#include <cv_bridge/cv_bridge.h>

#include <microhttpd.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace face_recognition_ros2 {

// HTTP MJPEG stream server for ROS2 image topics.
//
// Why not rqt_image_view? rqt is Qt5-based; on hosts with snap GTK3 modules,
// Qt also pulls in the offending snap-core20 libpthread and the process dies
// at startup. A pure-libmicrohttpd MJPEG server has zero Qt/GTK deps and
// works headlessly.
//
// Endpoints:
//   GET /              → minimal HTML page that embeds <img src="/stream">
//   GET /stream        → multipart/x-mixed-replace JPEG stream
//   GET /latest.jpg    → most recent JPEG frame (single shot)
class FaceStreamServer : public rclcpp::Node {
public:
    explicit FaceStreamServer(const rclcpp::NodeOptions& options = rclcpp::NodeOptions());
    ~FaceStreamServer() override;

    // Public for the MJPEG streaming callback defined in the .cpp's anon
    // namespace. Returns false if no frame is available yet.
    bool copyLatestJpeg(std::vector<uint8_t>& out, uint64_t& seq);

private:
    void imageCallback(const sensor_msgs::msg::Image::ConstSharedPtr& msg);

    // libmicrohttpd handlers (static so they match MHD callback signature)
    static enum MHD_Result handleRequest(void* cls,
                                         struct MHD_Connection* connection,
                                         const char* url,
                                         const char* method,
                                         const char* version,
                                         const char* upload_data,
                                         size_t* upload_data_size,
                                         void** con_cls);

    static void requestCompleted(void* cls,
                                 struct MHD_Connection* connection,
                                 void** con_cls,
                                 enum MHD_RequestTerminationCode toe);

    // ---- Parameters ----
    std::string image_topic_;
    int port_;
    int jpeg_quality_;

    // ---- ROS I/O ----
    image_transport::Subscriber image_sub_;

    // ---- Latest frame ----
    std::mutex frame_mutex_;
    std::vector<uint8_t> latest_jpeg_;
    uint64_t frame_seq_ = 0;       // monotonic counter incremented per frame
    bool have_frame_ = false;

    // ---- HTTP ----
    struct MHD_Daemon* daemon_ = nullptr;

    // libmicrohttpd instance pointer, set in start() and read by static handler.
    static FaceStreamServer* s_instance_;
};

}  // namespace face_recognition_ros2

#endif