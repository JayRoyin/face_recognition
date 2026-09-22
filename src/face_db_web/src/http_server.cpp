#include "face_db_web/http_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <iostream>

namespace face_db_web {

namespace {
struct RequestContext {
    std::string body;
    bool body_complete;
    /**
     * Set once the body exceeds the configured cap. The remaining upload is
     * drained (not buffered) so the request can finish normally and be
     * answered with a real 413 instead of a silently dropped connection.
     */
    bool too_large = false;
};
}  // namespace

HttpServer* HttpServer::instance_ = nullptr;

HttpServer::HttpServer(int port, std::size_t max_body_bytes)
    : port_(port), max_body_bytes_(max_body_bytes) {}

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

    // Bind to loopback by default. MHD's own default is 0.0.0.0, and this server
    // has NO authentication: every route — including clear (which also deletes
    // the image files) — would otherwise be reachable from the whole network,
    // while the program kept advertising "http://localhost".
    //
    // Set FACE_WEB_BIND=0.0.0.0 (or a specific address) to expose it on purpose.
    const char* bind_env = std::getenv("FACE_WEB_BIND");
    const std::string bind_addr = (bind_env && *bind_env) ? bind_env : "127.0.0.1";

    // MHD requires the port argument to be 0 when MHD_OPTION_SOCK_ADDR is used;
    // the port then comes from the sockaddr. `static` so the address outlives
    // this call (MHD keeps the pointer).
    static struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(port_));
    if (::inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "Invalid bind address: " << bind_addr << std::endl;
        return false;
    }

    daemon_.reset(MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD,
        0,
        nullptr, nullptr,
        &HttpServer::mhd_handler, this,
        MHD_OPTION_SOCK_ADDR, &addr,
        MHD_OPTION_END));
    if (!daemon_) {
        std::cerr << "Failed to start HTTP server on " << bind_addr << ":" << port_
                  << std::endl;
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
        const std::size_t cap = instance_->max_body_bytes_;
        if (!ctx->too_large &&
            (*upload_data_size > cap || ctx->body.size() > cap - *upload_data_size)) {
            std::cerr << "[HttpServer] request body exceeds the limit (" << cap
                      << " bytes); freeing what was buffered and answering 413. "
                         "Raise --max-upload-mb if this is a legitimate import."
                      << std::endl;
            ctx->too_large = true;
            std::string().swap(ctx->body);  // release the memory immediately
        }
        if (!ctx->too_large) {
            ctx->body.append(upload_data, *upload_data_size);
        }
        // Always consume the chunk: MHD only finishes the request once every
        // byte has been acknowledged, and we want to send a response.
        *upload_data_size = 0;
        return MHD_YES;
    }

    HttpRequest req;
    req.method = method;
    req.url = url;

    // Multipart (bulk import) cannot be parsed without the boundary, which only
    // exists in Content-Type — so the header has to reach the handlers.
    if (const char* ct = MHD_lookup_connection_value(connection, MHD_HEADER_KIND,
                                                     MHD_HTTP_HEADER_CONTENT_TYPE)) {
        req.headers["content-type"] = ct;
    }
    if (const char* cl = MHD_lookup_connection_value(connection, MHD_HEADER_KIND,
                                                     MHD_HTTP_HEADER_CONTENT_LENGTH)) {
        req.headers["content-length"] = cl;
    }

    if (!ctx->body.empty()) {
        req.body = std::move(ctx->body);
    }

    const bool too_large = ctx->too_large;
    delete ctx;
    *con_cls = nullptr;

    HttpResponse resp;
    bool handled = false;

    if (too_large) {
        // Report it instead of resetting the connection: a bare disconnect
        // reaches the browser/curl as "connection reset", which sends users
        // hunting for a network problem instead of raising --max-upload-mb.
        resp.status_code = 413;
        resp.content_type = "application/json; charset=utf-8";
        resp.body = "{\"success\":false,\"message\":\"request body exceeds the server "
                    "limit of " + std::to_string(instance_->max_body_bytes()) +
                    " bytes; raise --max-upload-mb\"}";
        handled = true;
    }

    auto method_it = instance_->routes_.find(req.method);
    if (!handled && method_it != instance_->routes_.end()) {
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
                // MHD is a C library: an exception escaping this callback crosses
                // a C frame and reaches std::terminate, killing the whole server.
                // A single bad request must not be able to take the admin UI down.
                try {
                    resp = handler(req);
                } catch (const std::exception& e) {
                    std::cerr << "[HttpServer] handler threw: " << e.what() << std::endl;
                    resp.status_code = 500;
                    resp.body = "internal error";
                } catch (...) {
                    std::cerr << "[HttpServer] handler threw an unknown exception"
                              << std::endl;
                    resp.status_code = 500;
                    resp.body = "internal error";
                }
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
