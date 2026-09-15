#pragma once

#ifndef FACE_RECOGNITION_CORE_FACE_DETECTOR_HPP
#define FACE_RECOGNITION_CORE_FACE_DETECTOR_HPP

#include "types.hpp"
#include <opencv2/core.hpp>
#include <vector>
#include <memory>
#include <string>

namespace face_recognition {

/**
 * Face detector supporting two backends:
 *   - RetinaFace (InsightFace det_10g.onnx, 9 outputs)
 *   - Generic YOLOv8 (single [1, 84, N] output)
 *
 * The backend is auto-detected by output tensor shape.
 */
class FaceDetector {
public:
    FaceDetector();
    ~FaceDetector();

    /**
     * Initialize the detector.
     *
     * @param model_path            Path to ONNX model file.
     * @param confidence_threshold  Minimum face confidence (0.0 - 1.0).
     * @param nms_threshold         IoU threshold for NMS (0.0 - 1.0).
     * @param input_size            Square input size; common: 640.
     */
    bool initialize(const std::string& model_path,
                    float confidence_threshold = 0.5f,
                    float nms_threshold = 0.5f,
                    int input_size = 640);

    std::vector<FaceDetection> detect(const cv::Mat& image, int max_faces = 10);

    void setInputSize(int width, int height);

    /**
     * Returns the active backend name for debugging.
     * One of: "retinaface", "yolov8", "unknown".
     */
    std::string backend() const;

    std::string getLastError() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace face_recognition

#endif
