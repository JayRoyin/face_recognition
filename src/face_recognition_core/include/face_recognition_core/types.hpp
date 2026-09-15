#pragma once

#ifndef FACE_RECOGNITION_CORE_TYPES_HPP
#define FACE_RECOGNITION_CORE_TYPES_HPP

#include <string>
#include <vector>
#include <cstdint>
#include <ctime>
#include <algorithm>

namespace face_recognition {

struct BoundingBox {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct FaceDetection {
    BoundingBox bbox;
    float confidence = 0.0f;
    // 5 facial landmarks in (x, y) pairs (10 floats total).
    // Filled by detectors that produce landmarks (e.g. RetinaFace).
    // Empty when the detector does not provide them.
    std::vector<float> landmarks;
};

struct FaceInfo {
    std::string id;
    std::string name;
    std::string title;
    float confidence = 0.0f;
    std::string scene;
    std::string map_location;
    time_t timestamp;
    BoundingBox bbox;
};

struct FaceRecord {
    std::string id;
    std::string name;
    std::string title;
    std::vector<float> embedding;
    std::string image_path;
    std::string scene;
    std::string map_location;
    time_t created_at;
    time_t updated_at;
};

struct RecognitionResult {
    bool success;
    std::string message;
    std::string id;
    std::string name;
    float similarity;
    BoundingBox bbox;
};

enum class ImageSourceType {
    ROS_TOPIC,
    RTSP,
    HTTP,
    FILE
};

struct ImageSource {
    ImageSourceType type;
    std::string url_or_topic;
};

inline float compute_similarity(const std::vector<float>& emb1, const std::vector<float>& emb2) {
    if (emb1.size() != emb2.size() || emb1.empty()) return 0.0f;
    float dot = 0;
    for (size_t i = 0; i < emb1.size(); i++) {
        dot += emb1[i] * emb2[i];
    }
    return std::max(0.0f, std::min(1.0f, dot));
}

}  // namespace face_recognition

#endif
