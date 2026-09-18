#include "face_recognition_standalone/video_source.hpp"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/core/utils/logger.hpp>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <vector>

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

bool all_digits(const std::string& s) {
    return !s.empty() &&
           std::all_of(s.begin(), s.end(),
                       [](unsigned char c) { return std::isdigit(c); });
}

/** "0" / "video2" / "/dev/video2" -> 2 ; returns -1 when not an index form. */
int video_index_from_string(const std::string& s) {
    if (all_digits(s)) return std::stoi(s);
    const std::string prefix = "/dev/video";
    if (s.rfind(prefix, 0) == 0 && all_digits(s.substr(prefix.size())))
        return std::stoi(s.substr(prefix.size()));
    if (s.rfind("video", 0) == 0 && all_digits(s.substr(5)))
        return std::stoi(s.substr(5));
    return -1;
}

std::string fourcc_to_string(double v) {
    const int code = static_cast<int>(v);
    if (code <= 0) return {};
    std::string s(4, '\0');
    for (int i = 0; i < 4; ++i)
        s[i] = static_cast<char>((code >> (8 * i)) & 0xFF);
    // Trim the padding V4L2 puts in for unused bytes.
    while (!s.empty() && (s.back() == '\0' || s.back() == ' ')) s.pop_back();
    return s;
}

int fourcc_from_string(const std::string& s) {
    if (s.size() != 4) return 0;
    return cv::VideoWriter::fourcc(s[0], s[1], s[2], s[3]);
}

/** Human-readable device name from sysfs ("HD Webcam: HD Webcam"). */
std::string read_sysfs_name(int index) {
    std::ifstream f("/sys/class/video4linux/video" + std::to_string(index) + "/name");
    std::string line;
    if (f && std::getline(f, line)) return line;
    return {};
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
    dir_files_.clear();
    dir_pos_ = 0;
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
            // IMAGE_DIR needs no VideoCapture; read() replays a file list.
            // Scan the directory ONCE here — see dir_files_.
            if (cfg_.type == SourceType::IMAGE_DIR) {
                namespace fs = std::filesystem;
                std::error_code ec;
                fs::directory_iterator it(cfg_.uri, ec);
                if (!ec) {
                    for (const auto& entry : it) {
                        if (!entry.is_regular_file()) continue;
                        std::string ext = entry.path().extension().string();
                        std::transform(ext.begin(), ext.end(), ext.begin(),
                                       [](unsigned char c) {
                                           return std::tolower(c);
                                       });
                        if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" ||
                            ext == ".bmp" || ext == ".webp" || ext == ".tiff" ||
                            ext == ".tif") {
                            dir_files_.push_back(entry.path().string());
                        }
                    }
                }
                std::sort(dir_files_.begin(), dir_files_.end());
                dir_pos_ = 0;
            }
            break;
        }
    }

    if (cap_.isOpened()) {
        if (cfg_.type == SourceType::USB_CAMERA) {
            // The pixel format has to be negotiated BEFORE width/height: V4L2
            // then picks a mode that satisfies the format.
            //
            // Uncompressed (YUYV) frames above ~640x480 exceed USB2 bandwidth
            // and make the camera fall back to a very low frame rate
            // (observed: 1280x720 at 10 fps). MJPG is compressed inside the
            // camera and keeps 720p/1080p at 30 fps.
            //
            // `--fourcc` overrides the heuristic — some cameras only expose
            // YUYV, some only expose MJPG above 480p.
            const int requested = fourcc_from_string(cfg_.fourcc);
            if (requested != 0) {
                cap_.set(cv::CAP_PROP_FOURCC, requested);
            } else if (cfg_.width >= 1280 || cfg_.height >= 720) {
                cap_.set(cv::CAP_PROP_FOURCC,
                         cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
            }
        }

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

        // Same story for the frame rate: a 1080p YUYV mode can only do a
        // fraction of the requested fps, and silently running at 5 fps looks
        // like a recognition problem rather than a capture-mode problem.
        if (cfg_.type == SourceType::USB_CAMERA && cfg_.fps > 0) {
            const double got_fps = cap_.get(cv::CAP_PROP_FPS);
            if (got_fps > 0 && got_fps + 0.5 < cfg_.fps * 0.6) {
                std::cerr << "[WARN] camera delivers "
                          << static_cast<int>(got_fps + 0.5)
                          << " fps instead of the requested " << cfg_.fps
                          << " fps -- try --fourcc MJPG, or a smaller "
                             "--width/--height\n";
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

std::string VideoSource::fourcc() const {
    if (cfg_.type != SourceType::USB_CAMERA || !cap_.isOpened()) return {};
    return fourcc_to_string(cap_.get(cv::CAP_PROP_FOURCC));
}

bool VideoSource::read(cv::Mat& frame) {
    frame.release();

    if (cfg_.type == SourceType::IMAGE_DIR) {
        // Walk the list captured by open(). Pre-scanning is what makes a
        // directory run terminate: re-scanning per call (the old behaviour)
        // returned the first readable image forever.
        while (dir_pos_ < dir_files_.size()) {
            cv::Mat img = cv::imread(dir_files_[dir_pos_++], cv::IMREAD_COLOR);
            if (img.empty()) continue;
            frame = img;
            last_frame_ = frame;
            return true;
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
// Camera discovery / selection.
// -----------------------------------------------------------------------------
bool normalize_camera_selector(const std::string& in, std::string& out, std::string& err) {
    namespace fs = std::filesystem;
    if (in.empty()) {
        err = "empty camera selector";
        return false;
    }

    const int idx = video_index_from_string(in);
    if (idx >= 0) {
        const std::string dev = "/dev/video" + std::to_string(idx);
        std::error_code ec;
        if (!fs::exists(dev, ec)) {
            err = dev + " does not exist";
            return false;
        }
        out = std::to_string(idx);   // index form keeps `open(index)` working
        return true;
    }

    if (in.rfind("/dev/", 0) == 0) {
        std::error_code ec;
        if (!fs::exists(in, ec)) {
            err = in + " does not exist";
            return false;
        }
        out = in;
        return true;
    }

    err = "unrecognised camera selector '" + in +
          "' (expected an index like 0, or a device node like /dev/video2)";
    return false;
}

std::vector<CameraInfo> enumerate_cameras(bool probe, int max_devices) {
    namespace fs = std::filesystem;
    std::vector<CameraInfo> out;

    // Every UVC camera publishes auxiliary "metadata" nodes next to its capture
    // node, and opening those makes OpenCV's V4L2 backend print
    // "can't open camera by index" for each one. Failing to open is the
    // *expected* outcome here and it is exactly how those nodes are told apart,
    // so silence the backend while probing.
    const auto prev_log_level = cv::utils::logging::getLogLevel();
    cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_SILENT);

    for (int i = 0; i < max_devices; ++i) {
        const std::string dev = "/dev/video" + std::to_string(i);
        std::error_code ec;
        if (!fs::exists(dev, ec)) continue;

        CameraInfo ci;
        ci.index  = i;
        ci.device = dev;
        ci.name   = read_sysfs_name(i);

        cv::VideoCapture cap;
        if (cap.open(i, cv::CAP_V4L2)) {
            cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
            ci.openable = true;
            ci.width  = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
            ci.height = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
            ci.fps    = cap.get(cv::CAP_PROP_FPS);
            ci.fourcc = fourcc_to_string(cap.get(cv::CAP_PROP_FOURCC));

            if (probe) {
                // A V4L2 device can only be held by one handle at a time, so the
                // handle used above to read the current mode has to be released
                // first — otherwise every probe open returns EBUSY and the probe
                // silently reports nothing.
                cap.release();

                const int candidates[][2] = {
                    {1920, 1080}, {1280, 720}, {640, 480}, {320, 240}};
                for (const auto& c : candidates) {
                    cv::VideoCapture p;
                    if (!p.open(i, cv::CAP_V4L2)) continue;
                    p.set(cv::CAP_PROP_FOURCC,
                          cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
                    p.set(cv::CAP_PROP_FRAME_WIDTH,  c[0]);
                    p.set(cv::CAP_PROP_FRAME_HEIGHT, c[1]);
                    p.set(cv::CAP_PROP_FPS, 30);

                    const int    gw = static_cast<int>(p.get(cv::CAP_PROP_FRAME_WIDTH));
                    const int    gh = static_cast<int>(p.get(cv::CAP_PROP_FRAME_HEIGHT));
                    const double gf = p.get(cv::CAP_PROP_FPS);
                    char want[32], got[64];
                    std::snprintf(want, sizeof(want), "%dx%d@30", c[0], c[1]);
                    std::snprintf(got, sizeof(got), "%dx%d@%.0f", gw, gh, gf);
                    ci.probe_results.push_back(
                        std::string(want) + "  ->  " + got +
                        ((gw == c[0] && gh == c[1]) ? "" : "   (not supported, downgraded)"));
                    p.release();
                }
                if (ci.probe_results.empty()) {
                    ci.probe_results.push_back(
                        "probe unavailable (device busy - another program may be using it)");
                }
            }
        }
        cap.release();
        out.push_back(std::move(ci));
    }

    cv::utils::logging::setLogLevel(prev_log_level);
    return out;
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
            cfg.fps    = 30;
        } else {
            // Fall back: let VideoCapture try to open it.
            cfg.type = SourceType::FILE;
        }
    }
    return cfg;
}

}  // namespace face_recognition_standalone
