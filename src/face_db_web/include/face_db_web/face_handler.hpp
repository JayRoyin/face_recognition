#pragma once

#ifndef FACE_DB_WEB_FACE_HANDLER_HPP
#define FACE_DB_WEB_FACE_HANDLER_HPP

#include "face_db_web/http_server.hpp"
#include "face_recognition_core/face_database.hpp"
#include "face_recognition_core/face_detector.hpp"
#include "face_recognition_core/face_recognizer.hpp"

#include <memory>
#include <string>

namespace face_db_web {

class FaceHandler {
public:
    FaceHandler(std::shared_ptr<face_recognition::FaceDatabase> database,
                std::shared_ptr<face_recognition::FaceDetector>   detector   = nullptr,
                std::shared_ptr<face_recognition::FaceRecognizer> recognizer = nullptr);

    HttpResponse index(const HttpRequest& req);
    HttpResponse listFaces(const HttpRequest& req);
    HttpResponse addFace(const HttpRequest& req);
    HttpResponse removeFace(const HttpRequest& req);
    HttpResponse clearFaces(const HttpRequest& req);
    HttpResponse getImage(const HttpRequest& req);

private:
    std::shared_ptr<face_recognition::FaceDatabase>   database_;
    std::shared_ptr<face_recognition::FaceDetector>   detector_;
    std::shared_ptr<face_recognition::FaceRecognizer> recognizer_;
    std::string templates_dir_;

    std::string renderIndex();
    std::string renderFaceList();

    std::string urlDecode(const std::string& str);
    std::string base64Decode(const std::string& encoded);
    std::string base64Encode(const std::vector<uint8_t>& data);
};

}  // namespace face_db_web

#endif