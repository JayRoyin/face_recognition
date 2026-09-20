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
    // Default-initialised: reading an uninitialised time_t is UB and shows up as
    // a garbage timestamp that is very hard to trace back to its origin.
    time_t timestamp = 0;
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
    time_t created_at = 0;
    time_t updated_at = 0;

    /**
     * Gallery-level gender, stored in the database (not a UI-only field).
     * Free-form but conventionally "unknown" / "male" / "female".
     */
    std::string gender = "unknown";

    /**
     * SHA-256 of the stored image bytes.
     *
     * Lets the importer recognise the very same photo even when it was renamed
     * or re-exported, which is the only reliable "already enrolled" signal:
     * two different photos of one person are NOT duplicates (they are extra
     * templates), while one photo imported twice always is.
     */
    std::string image_hash;

    /**
     * Auto-maintained integer id (see face_database.hpp). Used as the stable
     * sort order and as the human-facing "record #N" inside the gallery.
     * Internal: never serialised to the HTTP API.
     */
    long long uid = 0;
};

struct RecognitionResult {
    bool success = false;
    std::string message;
    std::string id;
    std::string name;
    float similarity = 0.0f;
    BoundingBox bbox;
};

/**
 * One stored embedding ("shot") of an identity. A person can own several
 * templates (e.g. with and without glasses, different poses/lighting), which
 * makes recognition robust to intra-class appearance changes that a single
 * template cannot represent.
 */
struct FaceTemplate {
    long long          id = 0;
    std::string        face_id;
    std::vector<float> embedding;
    std::string        image_path;
    time_t             created_at = 0;
};

/** One identity's best score for a given query, used for ranking. */
struct MatchCandidate {
    std::string face_id;
    std::string name;
    std::string title;
    float       similarity     = 0.0f;
    long long   template_id    = -1;   // -1 => the primary faces.embedding
    int         template_count = 0;
    /**
     * Scene of the matched identity. Needed by the re-enrolment policy, which
     * treats "same person, same scene" differently from "same person, another
     * scene": without it the caller has to re-read the whole gallery.
     */
    std::string scene;
};

/**
 * Outcome of a 1:N query, including the cohort-normalised score.
 *
 * `raw_similarity` is the plain cosine of the best template. `z_score` is
 * (raw - median_cohort) / MAD_cohort, i.e. how far the score sits from the
 * "how well does this query match unrelated people" distribution. Cohort
 * normalisation is what makes a threshold usable across people: a template
 * that happens to match everyone (a "hub") raises the cohort median and
 * therefore needs a proportionally higher score to be accepted.
 */
struct MatchResult {
    bool        accepted = false;
    std::string face_id;
    std::string name;
    std::string title;
    float       raw_similarity = 0.0f;
    float       z_score        = 0.0f;
    float       cohort_median  = 0.0f;
    float       cohort_mad     = 0.0f;
    int         cohort_size    = 0;
    bool        normalized     = false;   // cohort was large enough to normalise
    std::string reason;
};

enum class ImageSourceType {
    ROS_TOPIC,
    RTSP,
    HTTP,
    FILE
};

struct ImageSource {
    ImageSourceType type = ImageSourceType::FILE;
    std::string url_or_topic;
};

inline float compute_similarity(const std::vector<float>& emb1, const std::vector<float>& emb2) {
    if (emb1.size() != emb2.size() || emb1.empty()) return 0.0f;
    float dot = 0;
    for (size_t i = 0; i < emb1.size(); i++) {
        dot += emb1[i] * emb2[i];
    }
    // Cosine of two L2-normalised embeddings. Negative values are meaningful:
    // unrelated identities land well below zero. Clamping them to 0 (as this
    // function used to) collapses every "clearly different" pair onto the same
    // value, which destroys the margin AND corrupts the cohort statistics
    // (median/MAD become 0, so the z-score is meaningless). Only the upper
    // bound is clamped, to absorb the rounding overshoot at 1.0.
    return std::min(1.0f, dot);
}

}  // namespace face_recognition

#endif
