#pragma once

#ifndef FACE_RECOGNITION_CORE_EMBEDDING_POLICY_HPP
#define FACE_RECOGNITION_CORE_EMBEDDING_POLICY_HPP

#include "face_recognition_core/face_recognizer.hpp"
#include "face_recognition_core/types.hpp"

#include <opencv2/core.hpp>

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace face_recognition {

/**
 * Quality policy for turning a detection into a stored / comparable embedding.
 *
 * This belongs to the core, not to one of the application modules. The gate used
 * to live only in the standalone pipeline, so face_db_web and both ROS nodes
 * called FaceRecognizer::extract_embedding() directly and bypassed it. The
 * gallery then held embeddings produced under different rules — which looks
 * exactly like a broken model from the outside: the enrolled person scores low
 * while an unrelated face scores high, and nothing in the data says why.
 */
struct EmbeddingPolicy {
    /** Reject faces whose bounding-box short side is below this (0 = no limit). */
    int  min_face_size     = 80;
    /** Require the aligned front-end, i.e. refuse the bbox-crop fallback. */
    bool require_alignment = true;
    /** With require_alignment set, still accept a fallback embedding. NOT advised. */
    bool allow_unaligned   = false;
};

/** Outcome of applying an EmbeddingPolicy. Empty `reject_reason` == accepted. */
struct EmbeddingOutcome {
    std::vector<float> embedding;
    bool               used_alignment = false;
    std::string        reject_reason;

    bool ok() const { return reject_reason.empty() && !embedding.empty(); }
};

/**
 * The single supported way to produce an embedding for a detection.
 *
 * Every writer (enrolment, extra template, backfill) and every reader (run-time
 * recognition, verify) must go through this, so that one gallery can never mix
 * embeddings produced under different rules.
 *
 * Note it forwards the detection's landmarks — a caller that drops them silently
 * falls back to the bbox-crop front-end, which is a different feature sub-space.
 */
inline EmbeddingOutcome make_embedding(FaceRecognizer& recognizer,
                                       const cv::Mat& image,
                                       const FaceDetection& det,
                                       const EmbeddingPolicy& policy) {
    EmbeddingOutcome out;

    const int short_side = static_cast<int>(
        std::min(det.bbox.width, det.bbox.height));
    if (policy.min_face_size > 0 && short_side < policy.min_face_size) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "too small (%ux%u < %d px)",
                      det.bbox.width, det.bbox.height, policy.min_face_size);
        out.reject_reason = buf;
        return out;
    }

    out.embedding = recognizer.extract_embedding(image, det.bbox, det.landmarks,
                                                &out.used_alignment);
    if (out.embedding.empty()) {
        out.reject_reason = "no embedding";
        return out;
    }

    if (policy.require_alignment && !out.used_alignment && !policy.allow_unaligned) {
        out.reject_reason = "no landmarks";
        return out;
    }
    return out;
}

}  // namespace face_recognition

#endif  // FACE_RECOGNITION_CORE_EMBEDDING_POLICY_HPP
