#pragma once

#ifndef FACE_RECOGNITION_CORE_GENDER_CLASSIFIER_HPP
#define FACE_RECOGNITION_CORE_GENDER_CLASSIFIER_HPP

/**
 * Optional gender/age estimator (InsightFace `genderage.onnx`).
 *
 * Only used to PRE-FILL the gallery's gender field during import; the operator
 * always sees the value and can change it. If the model is missing or fails to
 * load, classify() simply returns "unknown" — enrolment never depends on it.
 */

#include <memory>
#include <string>

#include <opencv2/core.hpp>

namespace face_recognition {

struct GenderAgeResult {
    /** "male" / "female" / "unknown". */
    std::string gender = "unknown";
    /** Softmax score of the winning gender, 0..1. */
    float       confidence = 0.0f;
    /** Estimated age, or -1 when the model does not expose it. */
    float       age = -1.0f;
};

class GenderClassifier {
public:
    GenderClassifier();
    ~GenderClassifier();

    /**
     * @param model_path  genderage.onnx
     * @param male_index  which of the two gender logits means "male".
     *                    InsightFace exports differ between versions; the value
     *                    is configurable so a flipped model is a flag, not a
     *                    rebuild (see --gender-male-index).
     */
    bool initialize(const std::string& model_path, int male_index = 1);

    bool ready() const;
    std::string getLastError() const;
    /** Input/output description, for the start-up log. */
    std::string shapeInfo() const;

    /** @param face_bgr  cropped face (BGR), any size. */
    GenderAgeResult classify(const cv::Mat& face_bgr) const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace face_recognition

#endif  // FACE_RECOGNITION_CORE_GENDER_CLASSIFIER_HPP
