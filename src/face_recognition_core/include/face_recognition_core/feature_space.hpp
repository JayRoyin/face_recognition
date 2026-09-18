#pragma once

#ifndef FACE_RECOGNITION_CORE_FEATURE_SPACE_HPP
#define FACE_RECOGNITION_CORE_FEATURE_SPACE_HPP

#include "face_recognition_core/face_database.hpp"

#include <sys/stat.h>

#include <cstdio>
#include <string>

namespace face_recognition {

/** metadata key under which the gallery records its feature space. */
inline const char* kFeatureSpaceKey = "feature_space_id";

/**
 * Identifier of the recipe that produced a stored embedding.
 *
 * Two embeddings can only be compared when they come from the SAME recipe: same
 * detector decoding, same front-end (aligned vs bbox crop), same template
 * geometry, same normalisation, same recognition model. A stored 512-float blob
 * carries no trace of which recipe produced it, so a gallery silently mixing
 * recipes is indistinguishable from "the model got worse": the enrolled person
 * scores low while an unrelated face scores high, and nothing in the data
 * explains why. Recording the fingerprint turns that invisible failure into a
 * loud, actionable message.
 *
 * Build it ONLY from values that are compile-time constants of the pipeline
 * (plus the model file identity), so it changes automatically whenever the
 * recipe changes.
 *
 * The model is identified by size + mtime rather than a content hash: a full
 * hash of a 170 MB ONNX file per start-up is not worth the second it costs, and
 * any model swap changes at least one of the two.
 *
 * @param recognition_model_path path to the ArcFace ONNX model (may be empty)
 * @param aligned         whether the recogniser uses landmark alignment
 * @param allow_unaligned whether a bbox-crop fallback is accepted alongside it
 */
inline std::string feature_space_id(const std::string& recognition_model_path,
                                    bool aligned = true,
                                    bool allow_unaligned = false) {
    long long size  = 0;
    long long mtime = 0;
    struct stat st;
    if (!recognition_model_path.empty() &&
        ::stat(recognition_model_path.c_str(), &st) == 0) {
        size  = static_cast<long long>(st.st_size);
        mtime = static_cast<long long>(st.st_mtime);
    }

    // Front-end tag. The two front-ends are NOT interchangeable, so the front
    // end has to be part of the fingerprint: otherwise `--no-align` would query
    // an aligned gallery through the bbox crop and the guard would still report
    // OK — silently producing the "owner scores low, stranger scores high"
    // failure it exists to catch.
    const char* front =
        aligned ? (allow_unaligned ? "arcface112-align-or-bbox" : "arcface112-align")
                : "bboxcrop";

    // Recipe constants. Keep them in sync with the code they describe:
    //   det      face_detector.cpp    SCRFD decoding, plain stride scaling
    //   front    face_recognizer.cpp  align_crop() -> ArcFace 112x112 template
    //   norm     face_recognizer.cpp  blobFromImage(1/255, mean 0, swapRB=true)
    //                                 i.e. BGR -> RGB, scaled to [0,1], then L2
    //   model    w600k_r50.onnx (or whatever --recognition-model points at)
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "fs1|det=scrfd-stride|front=%s"
                  "|norm=rgb-1div255|model=%lld:%lld",
                  front, size, mtime);
    return std::string(buf);
}

/** Outcome of comparing a gallery's recorded feature space with the current one. */
enum class FeatureSpaceStatus {
    OK,        ///< recorded id matches the running code
    STAMPED,   ///< gallery had no id yet (legacy DB); the current one was written
    MISMATCH   ///< gallery was built by a different recipe — re-run `backfill --all`
};

/**
 * Compare the gallery's recorded feature space against @p current_id.
 *
 * Stamps the gallery when it has no id yet, so databases created before this
 * check existed do not warn forever (their embeddings are simply assumed to
 * come from the current recipe — the first `backfill --all` makes that true).
 */
inline FeatureSpaceStatus check_feature_space(FaceDatabase& db,
                                             const std::string& current_id,
                                             std::string* stored_id = nullptr) {
    const std::string stored = db.get_meta(kFeatureSpaceKey);
    if (stored_id) *stored_id = stored;

    if (stored.empty()) {
        db.set_meta(kFeatureSpaceKey, current_id);
        return FeatureSpaceStatus::STAMPED;
    }
    return (stored == current_id) ? FeatureSpaceStatus::OK
                                  : FeatureSpaceStatus::MISMATCH;
}

}  // namespace face_recognition

#endif  // FACE_RECOGNITION_CORE_FEATURE_SPACE_HPP
