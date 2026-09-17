#pragma once

#ifndef FACE_RECOGNITION_CORE_FACE_RECOGNIZER_HPP
#define FACE_RECOGNITION_CORE_FACE_RECOGNIZER_HPP

#include "types.hpp"
#include <opencv2/core.hpp>
#include <vector>
#include <memory>
#include <string>

namespace face_recognition {

/**
 * ArcFace (w600k_r50) embedding extractor.
 *
 * Two front-ends are supported:
 *
 *   1. **Landmark alignment** (preferred). When the detector supplies the 5
 *      facial landmarks, the face is warped onto the canonical 112x112
 *      ArcFace template with a closed-form least-squares similarity transform
 *      — the same estimator insightface uses (`norm_crop`). This is what the
 *      model was trained on, so it is what keeps the angular-margin geometry
 *      meaningful. It also largely removes roll/scale variance.
 *
 *   2. **Bounding-box crop** (fallback). Used when landmarks are missing or
 *      fail the reliability gate (tiny/degenerate detections).
 *
 * Both front-ends share the exact same normalisation
 * (`blobFromImage(scale=1/255, mean=0, swapRB=true)`), so callers only need
 * to pass the landmarks they already have.
 *
 * NOTE: switching between the two front-ends changes the embedding space.
 * Every producer and consumer of embeddings must use the same build.
 */
class FaceRecognizer {
public:
    FaceRecognizer();
    ~FaceRecognizer();

    bool initialize(const std::string& model_path);

    /**
     * Extract a 512-d L2-normalised embedding.
     *
     * @param landmarks  5 (x,y) pairs = 10 floats, as produced by
     *                   FaceDetection::landmarks. Pass an empty vector to
     *                   force the bounding-box fallback.
     * @param used_alignment
     *        Optional out-parameter: true when the 5-point alignment was
     *        actually used, false when the bounding-box fallback ran.
     *
     * IMPORTANT: the two front-ends produce embeddings that are NOT
     * interchangeable. Callers must never mix them inside one gallery —
     * query the out-parameter and reject (or re-enrol) when it differs from
     * the front-end the gallery was built with.
     */
    std::vector<float> extract_embedding(const cv::Mat& image,
                                        const BoundingBox& bbox);
    std::vector<float> extract_embedding(const cv::Mat& image,
                                        const BoundingBox& bbox,
                                        const std::vector<float>& landmarks,
                                        bool* used_alignment = nullptr);

    float compute_similarity(const std::vector<float>& emb1,
                             const std::vector<float>& emb2);

    void setInputSize(int width, int height);

    /** Enable/disable landmark alignment (enabled by default). */
    void setAlignmentEnabled(bool enabled);
    bool alignmentEnabled() const;

    /** Diagnostics: how many crops used alignment vs the bbox fallback. */
    void alignmentStats(int* aligned, int* fallback) const;

    std::string getLastError() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace face_recognition

#endif
