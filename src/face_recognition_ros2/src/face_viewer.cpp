#include "face_recognition_ros2/face_viewer.hpp"

#include <opencv2/imgproc.hpp>

#include <sstream>

namespace face_recognition_ros2 {

FaceViewer::FaceViewer(const rclcpp::NodeOptions& options)
    : Node("face_viewer_node", options) {

    this->declare_parameter("image_topic",      "/image_raw");
    this->declare_parameter("result_topic",     "/face/recognition_result");
    this->declare_parameter("annotated_topic",  "/face/annotated");

    this->get_parameter("image_topic",      image_topic_);
    this->get_parameter("result_topic",     result_topic_);
    this->get_parameter("annotated_topic",  annotated_topic_);

    // SensorDataQoS (BEST_EFFORT), matching camera drivers and the other nodes.
    // A RELIABLE subscriber cannot receive from a BEST_EFFORT publisher, and the
    // viewer would simply show nothing with no error.
    image_sub_ = this->create_subscription<sensor_msgs::msg::Image>(
        image_topic_, rclcpp::SensorDataQoS(),
        std::bind(&FaceViewer::imageCallback, this, std::placeholders::_1));

    result_sub_ = this->create_subscription<face_recognition_ros2_interfaces::msg::FaceResult>(
        result_topic_, 10,
        std::bind(&FaceViewer::resultCallback, this, std::placeholders::_1));

    annotated_pub_ = image_transport::create_publisher(this, annotated_topic_);

    // IMPORTANT: this node does NOT link libopencv_highgui on purpose.
    //   On hosts with snap GTK3 modules (vscode/etc.), loading highgui pulls
    //   in libgtk-3 -> GIO module search -> snap-core20 libpthread.so.0 ->
    //   symbol clash with system glibc. We annotate images in-process with
    //   imgproc (rectangle/putText) and publish the result. To view, use:
    //       ros2 run rqt_image_view rqt_image_view /face/annotated
    //   or any other image_transport subscriber (rviz, web_video_server).

    RCLCPP_INFO(this->get_logger(), "FaceViewer ready");
    RCLCPP_INFO(this->get_logger(), "  image_topic      = %s", image_topic_.c_str());
    RCLCPP_INFO(this->get_logger(), "  result_topic     = %s", result_topic_.c_str());
    RCLCPP_INFO(this->get_logger(), "  annotated_topic  = %s", annotated_topic_.c_str());
    RCLCPP_INFO(this->get_logger(),
        "  GUI: publish-only mode. To view, run:");
    RCLCPP_INFO(this->get_logger(),
        "    ros2 run rqt_image_view rqt_image_view %s", annotated_topic_.c_str());
}

void FaceViewer::resultCallback(
    const face_recognition_ros2_interfaces::msg::FaceResult::ConstSharedPtr& msg) {

    std::lock_guard<std::mutex> lock(results_mutex_);
    latest_faces_.clear();
    for (const auto& face : msg->faces) {
        latest_faces_[face.id] = face;
    }
    have_result_ = true;
    last_result_time_ = std::chrono::steady_clock::now();
}

void FaceViewer::imageCallback(const sensor_msgs::msg::Image::ConstSharedPtr& msg) {
    cv_bridge::CvImageConstPtr cv_ptr;
    try {
        cv_ptr = cv_bridge::toCvShare(msg, "bgr8");
    } catch (const cv_bridge::Exception& e) {
        RCLCPP_ERROR_THROTTLE(this->get_logger(), *this, 2000,
                              "cv_bridge exception: %s", e.what());
        return;
    }

    cv::Mat annotated = drawAnnotations(cv_ptr->image);

    if (annotated_pub_.getNumSubscribers() > 0) {
        std_msgs::msg::Header hdr = msg->header;
        cv_bridge::CvImage out(hdr, "bgr8", annotated);
        annotated_pub_.publish(out.toImageMsg());
    }
}

cv::Mat FaceViewer::drawAnnotations(const cv::Mat& frame) {
    cv::Mat out = frame.clone();

    std::map<std::string, face_recognition_ros2_interfaces::msg::FaceInfo> snapshot;
    {
        std::lock_guard<std::mutex> lock(results_mutex_);
        // Expire stale results (see kResultTtl): without this, a stopped
        // recognition node left its last boxes on screen indefinitely.
        const bool fresh = have_result_ &&
            (std::chrono::steady_clock::now() - last_result_time_) < kResultTtl;
        if (fresh) snapshot = latest_faces_;
    }

    for (const auto& [id, face] : snapshot) {
        cv::Rect r(static_cast<int>(face.x),
                   static_cast<int>(face.y),
                   static_cast<int>(face.width),
                   static_cast<int>(face.height));

        // Clip to image bounds.
        r &= cv::Rect(0, 0, out.cols, out.rows);
        if (r.width <= 0 || r.height <= 0) continue;

        cv::rectangle(out, r, cv::Scalar(0, 255, 0), 2);

        std::ostringstream label;
        label << face.name;
        if (!face.title.empty())     label << " (" << face.title << ")";
        label << " [" << face.id.substr(0, 8) << "] "
              << std::fixed; label.precision(2);
        label << face.confidence * 100.0f << "%";

        int baseline = 0;
        cv::Size text_size = cv::getTextSize(label.str(), cv::FONT_HERSHEY_SIMPLEX,
                                              0.5, 1, &baseline);

        // Place label above the box; if it would go off-screen, place below.
        int tx = r.x;
        int ty = r.y - 6;
        if (ty - text_size.height < 0) ty = r.y + text_size.height + 6;

        cv::rectangle(out,
                      cv::Point(tx, ty - text_size.height - 2),
                      cv::Point(tx + text_size.width + 4, ty + baseline + 2),
                      cv::Scalar(0, 255, 0), cv::FILLED);

        cv::putText(out, label.str(),
                    cv::Point(tx + 2, ty),
                    cv::FONT_HERSHEY_SIMPLEX, 0.5,
                    cv::Scalar(0, 0, 0), 1);
    }

    // Header overlay (face count).
    {
        std::ostringstream header;
        header << "faces: " << snapshot.size();
        cv::putText(out, header.str(),
                    cv::Point(10, 22),
                    cv::FONT_HERSHEY_SIMPLEX, 0.6,
                    cv::Scalar(0, 255, 255), 2);
    }

    return out;
}

}  // namespace face_recognition_ros2