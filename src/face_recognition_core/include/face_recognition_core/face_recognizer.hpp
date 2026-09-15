#pragma once

#ifndef FACE_RECOGNITION_CORE_FACE_RECOGNIZER_HPP
#define FACE_RECOGNITION_CORE_FACE_RECOGNIZER_HPP

#include "types.hpp"
#include <opencv2/core.hpp>
#include <vector>
#include <memory>
#include <string>

namespace face_recognition {

class FaceRecognizer {
public:
    FaceRecognizer();
    ~FaceRecognizer();

    bool initialize(const std::string& model_path);
    std::vector<float> extract_embedding(const cv::Mat& image, const BoundingBox& bbox);
    float compute_similarity(const std::vector<float>& emb1, const std::vector<float>& emb2);
    void setInputSize(int width, int height);
    std::string getLastError() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace face_recognition

#endif
