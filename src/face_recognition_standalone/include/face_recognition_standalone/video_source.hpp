#pragma once

#ifndef FACE_RECOGNITION_STANDALONE_VIDEO_SOURCE_HPP
#define FACE_RECOGNITION_STANDALONE_VIDEO_SOURCE_HPP

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>
#include <string>

namespace face_recognition_standalone {

enum class SourceType {
    USB_CAMERA,
    RTSP,
    HTTP,
    FILE,
    IMAGE_DIR
};

struct SourceConfig {
    SourceType type = SourceType::USB_CAMERA;
    std::string uri;            // e.g. "0" for /dev/video0, "rtsp://...", "http://...", "/path/file.mp4"
    int width = 0;
    int height = 0;
    int fps = 0;
    bool loop = false;          // loop video files
};

class VideoSource {
public:
    VideoSource();
    ~VideoSource();

    bool open(const SourceConfig& cfg, std::string& err);
    bool read(cv::Mat& frame);
    void release();
    bool isOpened() const;
    int width() const;
    int height() const;
    double fps() const;
    SourceType type() const { return cfg_.type; }
    const std::string& uri() const { return cfg_.uri; }

private:
    cv::VideoCapture cap_;
    SourceConfig cfg_;
    cv::Mat last_frame_;
};

const char* source_type_name(SourceType t);

SourceConfig infer_source_from_uri(const std::string& uri);

}  // namespace face_recognition_standalone

#endif
