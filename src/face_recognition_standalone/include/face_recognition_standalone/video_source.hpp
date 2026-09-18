#pragma once

#ifndef FACE_RECOGNITION_STANDALONE_VIDEO_SOURCE_HPP
#define FACE_RECOGNITION_STANDALONE_VIDEO_SOURCE_HPP

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>
#include <cstddef>
#include <string>
#include <vector>

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
    // V4L2 pixel format ("MJPG" / "YUYV" / ...). Empty = auto: MJPG whenever the
    // frame is >= 720p, because raw YUYV at that size exceeds USB2 bandwidth and
    // the camera silently falls back to a handful of fps.
    std::string fourcc;
};

/**
 * One locally attached V4L2 video device.
 *
 * UVC cameras publish several nodes per physical camera (a capture node plus
 * auxiliary "metadata" nodes). They all show up under /dev/video*, so the only
 * reliable way to tell them apart is to try to open each one — which is what
 * normalize/enumerate below do.
 */
struct CameraInfo {
    int         index    = -1;
    std::string device;             // "/dev/videoN"
    std::string name;               // human-readable, from sysfs
    bool        openable = false;   // false for metadata nodes / busy / gone
    int         width    = 0;       // negotiated capture mode (0 when not openable)
    int         height   = 0;
    double      fps      = 0.0;
    std::string fourcc;             // negotiated pixel format
    std::vector<std::string> probe_results;   // only with probe == true
};

/**
 * Enumerate the local V4L2 video devices (indices 0 .. max_devices-1).
 *
 * @param probe        additionally try the common resolutions on each usable
 *                     camera and report what it actually negotiates. Slow: the
 *                     device is re-opened once per candidate mode.
 * @param max_devices  highest video index to look at.
 */
std::vector<CameraInfo> enumerate_cameras(bool probe = false, int max_devices = 16);

/**
 * Normalise a user-supplied camera selector into a value `infer_source_from_uri`
 * understands: "0" | "video2" | "/dev/video2" are all accepted.
 *
 * @return false and fills `err` when the syntax is invalid or the device does
 *         not exist (the caller should then point the user at `cameras`).
 */
bool normalize_camera_selector(const std::string& in, std::string& out, std::string& err);

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
    /** Negotiated V4L2 pixel format ("MJPG" / "YUYV"); empty for non-cameras. */
    std::string fourcc() const;
    SourceType type() const { return cfg_.type; }
    const std::string& uri() const { return cfg_.uri; }

private:
    cv::VideoCapture cap_;
    SourceConfig cfg_;
    cv::Mat last_frame_;

    // IMAGE_DIR cursor. open() scans the directory ONCE into this list and
    // read() walks it. Scanning inside read() (as this used to) always returned
    // the first readable image again, so `run --source <dir>` never terminated.
    std::vector<std::string> dir_files_;
    std::size_t              dir_pos_ = 0;
};

const char* source_type_name(SourceType t);

SourceConfig infer_source_from_uri(const std::string& uri);

}  // namespace face_recognition_standalone

#endif
