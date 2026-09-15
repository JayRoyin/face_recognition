#include "face_recognition_standalone/video_source.hpp"

#include <opencv2/imgcodecs.hpp>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <iostream>

namespace face_recognition_standalone {

namespace {

bool is_http_url(const std::string& s) {
    return s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0;
}

bool is_rtsp_url(const std::string& s) {
    return s.rfind("rtsp://", 0) == 0 || s.rfind("rtsps://", 0) == 0;
}

bool looks_like_image_path(const std::string& p) {
    namespace fs = std::filesystem;
    if (!fs::exists(p)) return false;
    if (!fs::is_regular_file(p)) return false;
    std::string ext = fs::path(p).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".bmp" ||
           ext == ".webp" || ext == ".tiff" || ext == ".tif";
}

bool looks_like_video_path(const std::string& p) {
    namespace fs = std::filesystem;
    if (!fs::exists(p)) return false;
    if (!fs::is_regular_file(p)) return false;
    std::string ext = fs::path(p).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return ext == ".mp4" || ext == ".avi" || ext == ".mov" || ext == ".mkv" ||
           ext == ".webm" || ext == ".flv" || ext == ".m4v";
}

bool looks_like_dir(const std::string& p) {
    namespace fs = std::filesystem;
    return fs::exists(p) && fs::is_directory(p);
}

}  // namespace

const char* source_type_name(SourceType t) {
    switch (t) {
        case SourceType::USB_CAMERA: return "usb_camera";
        case SourceType::RTSP:       return "rtsp";
        case SourceType::HTTP:       return "http";
        case SourceType::FILE:       return "file";
        case SourceType::IMAGE_DIR:  return "image_dir";
    }
    return "unknown";
}

VideoSource::VideoSource() = default;
VideoSource::~VideoSource() { release(); }

void VideoSource::release() {
    if (cap_.isOpened()) cap_.release();
    last_frame_.release();
}

bool VideoSource::open(const SourceConfig& cfg, std::string& err) {
    cfg_ = cfg;

    switch (cfg_.type) {
        case SourceType::USB_CAMERA: {
            // `uri` is either a numeric index ("0", "1") or an explicit device
            // node ("/dev/video1"). Do NOT fall back to index 0 when a device
            // path is given — the old code did, which silently opened the wrong
            // camera for "/dev/video1".
            const bool is_index =
                !cfg_.uri.empty() &&
                std::all_of(cfg_.uri.begin(), cfg_.uri.end(),
                            [](unsigned char c) { return std::isdigit(c); });
            bool opened = false;
            if (is_index) {
                const int cam_index = std::stoi(cfg_.uri);
                opened = cap_.open(cam_index, cv::CAP_V4L2) || cap_.open(cam_index);
            } else if (!cfg_.uri.empty()) {
                opened = cap_.open(cfg_.uri, cv::CAP_V4L2) || cap_.open(cfg_.uri);
            }
            if (!opened) {
                err = "Failed to open USB camera: " + cfg_.uri;
                return false;
            }
            break;
        }
        case SourceType::RTSP: {
            if (!cap_.open(cfg_.uri, cv::CAP_FFMPEG)) {
                err = "Failed to open RTSP stream: " + cfg_.uri;
                return false;
            }
            break;
        }
        case SourceType::HTTP: {
            if (!cap_.open(cfg_.uri, cv::CAP_FFMPEG)) {
                err = "Failed to open HTTP stream: " + cfg_.uri;
                return false;
            }
            break;
        }
        case SourceType::FILE:
        case SourceType::IMAGE_DIR: {
            // IMAGE_DIR is handled in read() — open a dummy capture to report
            // width/height as 0 (we don't know it upfront).
            break;
        }
    }

    if (cap_.isOpened()) {
        if (cfg_.width > 0)  cap_.set(cv::CAP_PROP_FRAME_WIDTH,  cfg_.width);
        if (cfg_.height > 0) cap_.set(cv::CAP_PROP_FRAME_HEIGHT, cfg_.height);
        if (cfg_.fps > 0)    cap_.set(cv::CAP_PROP_FPS,          cfg_.fps);

        // V4L2 silently falls back to the closest supported mode when the
        // requested one is unavailable. Report the real value so the user is
        // not misled into thinking a 720p/1080p stream is in use.
        if (cfg_.width > 0 && cfg_.height > 0) {
            const double got_w = cap_.get(cv::CAP_PROP_FRAME_WIDTH);
            const double got_h = cap_.get(cv::CAP_PROP_FRAME_HEIGHT);
            if (got_w > 0 && got_h > 0 &&
                (got_w + 0.5 < cfg_.width || got_h + 0.5 < cfg_.height)) {
                std::cerr << "[WARN] camera does not support " << cfg_.width << "x"
                          << cfg_.height << ", using " << static_cast<int>(got_w) << "x"
                          << static_cast<int>(got_h)
                          << " (override with --width/--height, 0 = camera default)\n";
            }
        }
    }

    if (cfg_.type != SourceType::IMAGE_DIR && cfg_.type != SourceType::FILE &&
        !cap_.isOpened()) {
        err = "Video source not opened";
        return false;
    }

    return true;
}

bool VideoSource::isOpened() const {
    if (cfg_.type == SourceType::IMAGE_DIR || cfg_.type == SourceType::FILE) {
        return true;
    }
    return cap_.isOpened();
}

int VideoSource::width() const {
    if (cfg_.type == SourceType::IMAGE_DIR || cfg_.type == SourceType::FILE) {
        return 0;
    }
    return static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_WIDTH));
}

int VideoSource::height() const {
    if (cfg_.type == SourceType::IMAGE_DIR || cfg_.type == SourceType::FILE) {
        return 0;
    }
    return static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_HEIGHT));
}

double VideoSource::fps() const {
    if (cfg_.type == SourceType::IMAGE_DIR || cfg_.type == SourceType::FILE) {
        return 0.0;
    }
    return cap_.get(cv::CAP_PROP_FPS);
}

bool VideoSource::read(cv::Mat& frame) {
    frame.release();

    if (cfg_.type == SourceType::IMAGE_DIR) {
        namespace fs = std::filesystem;
        std::vector<fs::path> files;
        for (const auto& entry : fs::directory_iterator(cfg_.uri)) {
            if (!entry.is_regular_file()) continue;
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".bmp" ||
                ext == ".webp" || ext == ".tiff" || ext == ".tif") {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());
        for (const auto& p : files) {
            cv::Mat img = cv::imread(p.string(), cv::IMREAD_COLOR);
            if (!img.empty()) {
                frame = img.clone();
                last_frame_ = frame;
                return true;
            }
        }
        return false;
    }

    if (cfg_.type == SourceType::FILE) {
        if (!cap_.isOpened()) {
            if (!cap_.open(cfg_.uri, cv::CAP_FFMPEG)) {
                cap_.open(cfg_.uri);
            }
        }
        if (!cap_.isOpened()) return false;
        if (cap_.read(frame)) {
            last_frame_ = frame;
            return true;
        }
        if (cfg_.loop) {
            cap_.release();
            if (!cap_.open(cfg_.uri, cv::CAP_FFMPEG)) {
                cap_.open(cfg_.uri);
            }
            if (cap_.isOpened() && cap_.read(frame)) {
                last_frame_ = frame;
                return true;
            }
        }
        return false;
    }

    if (!cap_.isOpened()) return false;
    if (cap_.read(frame)) {
        last_frame_ = frame;
        return true;
    }
    return false;
}

// -----------------------------------------------------------------------------
// Helper used by CLI parsing.
// -----------------------------------------------------------------------------
SourceConfig infer_source_from_uri(const std::string& uri) {
    SourceConfig cfg;
    cfg.uri = uri;

    if (is_rtsp_url(uri)) {
        cfg.type = SourceType::RTSP;
    } else if (is_http_url(uri)) {
        // Could be MJPEG stream or a plain image URL — try stream first.
        cfg.type = SourceType::HTTP;
    } else if (looks_like_dir(uri)) {
        cfg.type = SourceType::IMAGE_DIR;
    } else if (looks_like_image_path(uri)) {
        // Single image → process exactly once unless the user passes --loop.
        cfg.type = SourceType::FILE;
        cfg.loop = false;
    } else if (looks_like_video_path(uri)) {
        cfg.type = SourceType::FILE;
    } else {
        // Pure digits → USB camera index. Also accept an explicit V4L2 device
        // node such as "/dev/video1".
        const bool all_digits = !uri.empty() &&
            std::all_of(uri.begin(), uri.end(),
                        [](unsigned char c) { return std::isdigit(c); });
        if (all_digits || uri.rfind("/dev/video", 0) == 0) {
            cfg.type = SourceType::USB_CAMERA;
            // Prefer a higher capture resolution than the 640x480 V4L2
            // default: the recognizer crops the face and resizes it to
            // 112x112, so a 720p frame carries substantially more facial
            // detail than a 480p one. Overridden by --width/--height
            // (pass 0 to keep whatever the camera defaults to).
            cfg.width  = 1280;
            cfg.height = 720;
        } else {
            // Fall back: let VideoCapture try to open it.
            cfg.type = SourceType::FILE;
        }
    }
    return cfg;
}

}  // namespace face_recognition_standalone
