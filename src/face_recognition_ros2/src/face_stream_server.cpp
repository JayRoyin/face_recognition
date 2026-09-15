#include "face_recognition_ros2/face_stream_server.hpp"

#include <opencv2/imgcodecs.hpp>  // cv::imencode
#include <rmw/qos_profiles.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <sstream>
#include <thread>
#include <vector>

namespace face_recognition_ros2 {

namespace {

// Per-stream response state.
struct StreamCtx {
    FaceStreamServer* self = nullptr;

    enum Phase { Header, Boundary, Body, BodyDrain } phase = Header;
    std::vector<uint8_t> pending;     // remainder of the previous frame's body

    // The most recent frame_seq we have already started emitting. We don't
    // re-emit a frame with the same seq until the producer advances.
    uint64_t last_emitted_seq = 0;
    bool have_emitted_once = false;
};

// Format a multipart sub-header for the given body length into `buf`. Returns
// bytes written, or 0 if buf is too small (caller should retry with more space).
static size_t formatBoundary(char* buf, size_t max, size_t body_len) {
    if (max < 80) return 0;   // tiny buffers: retry with larger one
    int n = std::snprintf(buf, max,
                          "\r\n--frame\r\n"
                          "Content-Type: image/jpeg\r\n"
                          "Content-Length: %zu\r\n\r\n",
                          body_len);
    if (n < 0 || static_cast<size_t>(n) >= max) return 0;
    return static_cast<size_t>(n);
}

// MHD streaming callback. Called repeatedly per response. Contract:
//   * Return the number of bytes we filled into `buf` (0 means "nothing yet,
//     call me again").
//   * Returning MHD_CONTENT_READER_END_OF_DATA (-1) terminates the response.
// We never block here — if no frame is available, return 0 and let MHD poll.
ssize_t streamCallback(void* cls, uint64_t /*pos*/, char* buf, size_t max) {
    auto* ctx = static_cast<StreamCtx*>(cls);
    if (!ctx || !ctx->self || max == 0) return 0;

    // ---- Phase: emit the first multipart boundary, then move to Body. ----
    if (ctx->phase == StreamCtx::Header) {
        // Reserve room for a body of unknown length; we re-emit per-frame
        // boundaries before each JPEG, so this initial Content-Length is just
        // a placeholder (0). The real lengths follow per-frame.
        size_t n = formatBoundary(buf, max, 0);
        if (n == 0) return 0;
        ctx->phase = StreamCtx::Body;
        return static_cast<ssize_t>(n);
    }

    // ---- Phase: drain any leftover bytes from a previous frame's body. ----
    if (ctx->phase == StreamCtx::BodyDrain) {
        if (ctx->pending.empty()) {
            ctx->phase = StreamCtx::Body;
        } else {
            size_t n = std::min(max, ctx->pending.size());
            std::memcpy(buf, ctx->pending.data(), n);
            ctx->pending.erase(ctx->pending.begin(),
                               ctx->pending.begin() + static_cast<std::ptrdiff_t>(n));
            return static_cast<ssize_t>(n);
        }
    }

    // ---- Phase: get the latest frame and emit its boundary + body. ----
    std::vector<uint8_t> frame;
    uint64_t seq = 0;
    if (!ctx->self->copyLatestJpeg(frame, seq)) {
        // No frame yet — emit nothing.
        return 0;
    }

    // If we already emitted this exact seq (producer hasn't advanced),
    // back off so we don't spin re-sending the same JPEG at link speed.
    if (ctx->have_emitted_once && seq == ctx->last_emitted_seq) {
        return 0;
    }
    ctx->last_emitted_seq = seq;
    ctx->have_emitted_once = true;

    if (ctx->phase == StreamCtx::Body) {
        size_t hdr_len = formatBoundary(buf, max, frame.size());
        if (hdr_len == 0) return 0;

        size_t body_cap = max - hdr_len;
        size_t body_n   = std::min(body_cap, frame.size());
        std::memcpy(buf + hdr_len, frame.data(), body_n);

        if (body_n < frame.size()) {
            ctx->pending.assign(
                frame.begin() + static_cast<std::ptrdiff_t>(body_n),
                frame.end());
            ctx->phase = StreamCtx::BodyDrain;
        }
        return static_cast<ssize_t>(hdr_len + body_n);
    }

    return 0;  // unreachable
}

void streamCleanup(void* cls) {
    delete static_cast<StreamCtx*>(cls);
}

MHD_Result queueBuffer(MHD_Connection* conn, int status, const char* content_type,
                       const std::vector<uint8_t>& body) {
    MHD_Response* response = MHD_create_response_from_buffer(
        body.size(),
        const_cast<void*>(static_cast<const void*>(body.data())),
        MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(response, "Content-Type", content_type);
    MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");
    MHD_add_response_header(response, "Cache-Control", "no-store");
    MHD_Result rc = MHD_queue_response(conn,
                                       static_cast<unsigned int>(status),
                                       response);
    MHD_destroy_response(response);
    return rc;
}

std::string buildIndexPage(int port) {
    std::ostringstream os;
    os <<
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<title>Face Recognition Stream</title>"
        "<style>"
        "body{margin:0;background:#111;color:#eee;font-family:sans-serif;}"
        "header{padding:8px 12px;background:#222;}"
        "header a{color:#6cf;margin-right:14px;}"
        "img{display:block;max-width:100vw;max-height:100vh;margin:auto;}"
        "</style></head><body>"
        "<header>"
        "<b>Face Recognition</b> &middot; "
        "<a href='/stream'>/stream</a>"
        "<a href='/latest.jpg'>/latest.jpg</a>"
        "<a href='/healthz'>/healthz</a>"
        "</header>"
        "<img src='/stream' alt='annotated stream'/>"
        "</body></html>";
    (void)port;
    return os.str();
}

}  // namespace

// -----------------------------------------------------------------------------

FaceStreamServer* FaceStreamServer::s_instance_ = nullptr;

FaceStreamServer::FaceStreamServer(const rclcpp::NodeOptions& options)
    : Node("face_stream_server", options) {

    this->declare_parameter("image_topic",  "/face/annotated");
    this->declare_parameter("port",         8090);
    this->declare_parameter("jpeg_quality", 80);

    this->get_parameter("image_topic",  image_topic_);
    this->get_parameter("port",         port_);
    this->get_parameter("jpeg_quality", jpeg_quality_);

    // Use a small, lossy QoS so we don't sit on stale frames if the producer
    // outpaces us.
    rmw_qos_profile_t qos = rmw_qos_profile_sensor_data;
    qos.depth = 1;
    qos.reliability = RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT;

    image_sub_ = image_transport::create_subscription(
        this, image_topic_,
        [this](const sensor_msgs::msg::Image::ConstSharedPtr& msg) {
            this->imageCallback(msg);
        },
        "raw", qos);

    s_instance_ = this;
    daemon_ = MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD,
        static_cast<uint16_t>(port_),
        nullptr, nullptr,
        &FaceStreamServer::handleRequest, this,
        MHD_OPTION_NOTIFY_COMPLETED, &FaceStreamServer::requestCompleted,
        MHD_OPTION_CONNECTION_TIMEOUT, (unsigned int) 0,
        MHD_OPTION_END);

    if (!daemon_) {
        RCLCPP_ERROR(this->get_logger(),
                     "Failed to start HTTP server on port %d (errno=%d: %s)",
                     port_, errno, std::strerror(errno));
        throw std::runtime_error("MHD_start_daemon failed");
    }

    RCLCPP_INFO(this->get_logger(), "Face stream server ready");
    RCLCPP_INFO(this->get_logger(), "  image_topic   = %s", image_topic_.c_str());
    RCLCPP_INFO(this->get_logger(), "  port          = %d", port_);
    RCLCPP_INFO(this->get_logger(), "  jpeg_quality  = %d", jpeg_quality_);
    RCLCPP_INFO(this->get_logger(), "  Open in browser:");
    RCLCPP_INFO(this->get_logger(), "    http://localhost:%d/             (auto-refresh page)", port_);
    RCLCPP_INFO(this->get_logger(), "    http://localhost:%d/stream       (raw MJPEG stream)", port_);
    RCLCPP_INFO(this->get_logger(), "    http://localhost:%d/latest.jpg   (single frame)", port_);
}

FaceStreamServer::~FaceStreamServer() {
    if (daemon_) {
        MHD_stop_daemon(daemon_);
        daemon_ = nullptr;
    }
    if (s_instance_ == this) s_instance_ = nullptr;
}

void FaceStreamServer::imageCallback(const sensor_msgs::msg::Image::ConstSharedPtr& msg) {
    cv_bridge::CvImageConstPtr cv_ptr;
    try {
        cv_ptr = cv_bridge::toCvShare(msg, "bgr8");
    } catch (const cv_bridge::Exception& e) {
        RCLCPP_ERROR_THROTTLE(this->get_logger(), *this, 2000,
                              "cv_bridge: %s", e.what());
        return;
    }

    std::vector<uint8_t> buf;
    std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, jpeg_quality_};
    if (!cv::imencode(".jpg", cv_ptr->image, buf, params)) {
        RCLCPP_ERROR_THROTTLE(this->get_logger(), *this, 2000,
                              "cv::imencode failed");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        latest_jpeg_.swap(buf);
        ++frame_seq_;
        have_frame_ = true;
    }
}

bool FaceStreamServer::copyLatestJpeg(std::vector<uint8_t>& out, uint64_t& seq) {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (!have_frame_) return false;
    out = latest_jpeg_;
    seq = frame_seq_;
    return true;
}

void FaceStreamServer::requestCompleted(void*, struct MHD_Connection*, void** con_cls,
                                         enum MHD_RequestTerminationCode) {
    if (con_cls && *con_cls) {
        delete[] static_cast<char*>(*con_cls);
        *con_cls = nullptr;
    }
}

enum MHD_Result FaceStreamServer::handleRequest(
        void* cls,
        struct MHD_Connection* connection,
        const char* url,
        const char* method,
        const char* /*version*/,
        const char* /*upload_data*/,
        size_t* upload_data_size,
        void** con_cls) {

    auto* self = static_cast<FaceStreamServer*>(cls);

    if (*con_cls == nullptr) {
        // Per-request scratch marker so MHD knows we'll respond later.
        // `requestCompleted` cleans this up.
        *con_cls = static_cast<void*>(new char[1]);
        return MHD_YES;
    }

    if (*upload_data_size != 0) {
        return MHD_YES;
    }

    if (std::strcmp(method, "GET") != 0) {
        const char* msg = "405 Method Not Allowed";
        return queueBuffer(connection, 405, "text/plain",
                           std::vector<uint8_t>(msg, msg + std::strlen(msg)));
    }

    if (std::strcmp(url, "/") == 0) {
        std::string body = buildIndexPage(self->port_);
        std::vector<uint8_t> bytes(body.begin(), body.end());
        return queueBuffer(connection, 200, "text/html; charset=utf-8", bytes);
    }

    if (std::strcmp(url, "/healthz") == 0) {
        const char* msg = self->have_frame_ ? "ok\n" : "no_frame_yet\n";
        return queueBuffer(connection, self->have_frame_ ? 200 : 503,
                           "text/plain",
                           std::vector<uint8_t>(msg, msg + std::strlen(msg)));
    }

    if (std::strcmp(url, "/latest.jpg") == 0) {
        std::vector<uint8_t> bytes;
        uint64_t seq_unused_ = 0;
        if (!self->copyLatestJpeg(bytes, seq_unused_)) {
            const char* msg = "no frame yet";
            return queueBuffer(connection, 503, "text/plain",
                               std::vector<uint8_t>(msg, msg + std::strlen(msg)));
        }
        return queueBuffer(connection, 200, "image/jpeg", bytes);
    }

    if (std::strcmp(url, "/stream") == 0) {
        auto* ctx = new StreamCtx{self, StreamCtx::Header, {}};
        MHD_Response* resp = MHD_create_response_from_callback(
            MHD_SIZE_UNKNOWN, 16 * 1024, streamCallback, ctx, streamCleanup);
        MHD_add_response_header(resp, "Content-Type",
                                "multipart/x-mixed-replace; boundary=frame");
        MHD_add_response_header(resp, "Cache-Control", "no-store");
        MHD_add_response_header(resp, "Access-Control-Allow-Origin", "*");
        MHD_Result rc = MHD_queue_response(connection, 200, resp);
        MHD_destroy_response(resp);
        return rc;
    }

    const char* msg = "404 Not Found";
    return queueBuffer(connection, 404, "text/plain",
                       std::vector<uint8_t>(msg, msg + std::strlen(msg)));
}

}  // namespace face_recognition_ros2