#include "face_db_web/http_server.hpp"

#include <cstring>
#include <cstdio>
#include <iostream>

namespace face_db_web {

namespace {
struct RequestContext {
    std::string body;
    bool body_complete;
};
}  // namespace
constexpr size_t kMaxRequestBody = 16 * 1024 * 1024;

HttpServer* HttpServer::instance_ = nullptr;

HttpServer::HttpServer(int port)
    : port_(port) {}

HttpServer::~HttpServer() {
    stop();
}

void HttpServer::get(const std::string& path, Handler handler) {
    routes_["GET"][path] = handler;
}

void HttpServer::post(const std::string& path, Handler handler) {
    routes_["POST"][path] = handler;
}

bool HttpServer::start() {
    instance_ = this;
    daemon_.reset(MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD,
        static_cast<uint16_t>(port_),
        nullptr, nullptr,
        &HttpServer::mhd_handler, this,
        MHD_OPTION_END));
    if (!daemon_) {
        std::cerr << "Failed to start HTTP server on port " << port_ << std::endl;
        return false;
    }
    std::cout << "HTTP server started on port " << port_ << std::endl;
    return true;
}

void HttpServer::stop() {
    daemon_.reset();
    if (instance_ == this) {
        instance_ = nullptr;
    }
}

enum MHD_Result HttpServer::mhd_handler(void* cls,
                                         struct MHD_Connection* connection,
                                         const char* url,
                                         const char* method,
                                         const char* version,
                                         const char* upload_data,
                                         size_t* upload_data_size,
                                         void** con_cls) {
    (void)cls;
    if (!instance_) return MHD_NO;

    if (*con_cls == nullptr) {
        auto* ctx = new RequestContext{ "", false };
        *con_cls = ctx;
        return MHD_YES;
    }

    auto* ctx = static_cast<RequestContext*>(*con_cls);

    if (*upload_data_size != 0) {
        if (*upload_data_size > kMaxRequestBody || ctx->body.size() > kMaxRequestBody - *upload_data_size) {
            delete ctx; *con_cls = nullptr; *upload_data_size = 0;
            return MHD_NO;
        }
        ctx->body.append(upload_data, *upload_data_size);
        *upload_data_size = 0;
        return MHD_YES;
    }

    HttpRequest req;
    req.method = method;
    req.url = url;

    if (!ctx->body.empty()) {
        req.body = ctx->body;
    }

    delete ctx;
    *con_cls = nullptr;

    HttpResponse resp;
    bool handled = false;

    auto method_it = instance_->routes_.find(req.method);
    if (method_it != instance_->routes_.end()) {
        for (const auto& [route_path, handler] : method_it->second) {
            bool matched = false;
            if (route_path == req.url) {
                matched = true;
            } else if (route_path.size() > 1 && route_path.back() == '/') {
                // Only treat multi-char paths ending in '/' as prefix routes,
                // so the root "/" doesn't greedily match every URL.
                if (req.url.size() > route_path.size() &&
                    req.url.compare(0, route_path.size(), route_path) == 0) {
                    matched = true;
                }
            }
            if (matched) {
                resp = handler(req);
                handled = true;
                break;
            }
        }
    }

    if (!handled) {
        resp.status_code = 404;
        resp.body = "<h1>404 Not Found</h1>";
    }

    struct MHD_Response* response = MHD_create_response_from_buffer(
        resp.body.size(),
        const_cast<char*>(resp.body.data()),
        MHD_RESPMEM_MUST_COPY);

    MHD_add_response_header(response, "Content-Type", resp.content_type.c_str());
    MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");
    MHD_add_response_header(response, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    MHD_add_response_header(response, "Access-Control-Allow-Headers", "Content-Type");

    int ret = MHD_queue_response(connection,
        static_cast<unsigned int>(resp.status_code),
        response);
    MHD_destroy_response(response);
    return static_cast<MHD_Result>(ret);
}

}  // namespace face_db_web
