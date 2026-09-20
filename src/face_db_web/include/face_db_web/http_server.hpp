#pragma once

#ifndef FACE_DB_WEB_HTTP_SERVER_HPP
#define FACE_DB_WEB_HTTP_SERVER_HPP

#include <string>
#include <memory>
#include <map>
#include <functional>
#include <cstddef>
#include <microhttpd.h>

namespace face_db_web {

/** Body cap used when the caller does not pass one explicitly. */
constexpr std::size_t kDefaultMaxRequestBody = 512u * 1024u * 1024u;

struct HttpRequest {
    std::string method;
    std::string url;
    std::string body;
    /** Lower-cased header names, e.g. headers["content-type"]. */
    std::map<std::string, std::string> headers;
    std::map<std::string, std::string> query_params;

    std::string header(const std::string& lower_name) const {
        auto it = headers.find(lower_name);
        return it == headers.end() ? std::string() : it->second;
    }
};

struct HttpResponse {
    int status_code = 200;
    std::string body;
    std::string content_type = "text/html; charset=utf-8";
};

class HttpServer {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    /**
     * @param max_body_bytes  Hard cap on a request body. Bulk import posts a
     *                        whole archive (or a grid of images) in one request,
     *                        so the old fixed 16 MiB cap is far too small.
     */
    explicit HttpServer(int port, std::size_t max_body_bytes = kDefaultMaxRequestBody);
    ~HttpServer();

    void get(const std::string& path, Handler handler);
    void post(const std::string& path, Handler handler);

    bool start();
    void stop();

    int port() const { return port_; }
    std::size_t max_body_bytes() const { return max_body_bytes_; }

private:
    struct MHD_Deleter {
        void operator()(struct MHD_Daemon* d) { if (d) MHD_stop_daemon(d); }
    };

    int port_;
    std::size_t max_body_bytes_;
    std::unique_ptr<struct MHD_Daemon, MHD_Deleter> daemon_;

    std::map<std::string, std::map<std::string, Handler>> routes_;

    static enum MHD_Result mhd_handler(void* cls,
                                       struct MHD_Connection* connection,
                                       const char* url,
                                       const char* method,
                                       const char* version,
                                       const char* upload_data,
                                       size_t* upload_data_size,
                                       void** con_cls);

    static HttpServer* instance_;
};

}  // namespace face_db_web

#endif
