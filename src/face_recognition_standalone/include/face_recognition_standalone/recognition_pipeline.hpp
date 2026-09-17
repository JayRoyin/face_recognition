#pragma once

#ifndef FACE_RECOGNITION_STANDALONE_PIPELINE_HPP
#define FACE_RECOGNITION_STANDALONE_PIPELINE_HPP

#include "face_recognition_core/face_database.hpp"
#include "face_recognition_core/face_detector.hpp"
#include "face_recognition_core/face_recognizer.hpp"
#include "face_recognition_core/types.hpp"

#include <opencv2/core.hpp>
#include <memory>
#include <string>
#include <vector>

namespace face_recognition_standalone {

struct PipelineConfig {
    std::string detection_model;
    std::string recognition_model;
    std::string db_path;
    std::string faces_dir;

    float detection_threshold = 0.5f;
    // Recognition similarity threshold (raw cosine).
    // Measured with this project's own models:
    //   same identity, different photos ....... 0.68 ~ 0.70
    //   different identity (impostor) ......... 0.21 ~ 0.32
    float recognition_threshold = 0.5f;
    float nms_threshold = 0.5f;
    int input_size = 640;
    int max_faces = 10;
    // Detect every N frames; skipped frames reuse the cached bboxes while
    // recognition still runs. 2 halves the detector cost (the dominant CPU
    // bottleneck) without hurting detection quality.
    int detect_every_n_frames = 2;
    int downscale_max_side = 0;      // 0 = no downscale before detection
    bool run_recognition = true;     // false = detection-only mode

    // ---- quality gate ----------------------------------------------------
    // Faces whose bounding box is smaller than this (short side, pixels) are
    // detected and drawn but NOT identified. A small crop has to be upsampled
    // into the 112x112 network input, which destroys identity detail and
    // produces embeddings that behave like "hubs" — they match many different
    // people. This is the main source of false accepts on distant faces.
    int min_face_size = 80;

    // ---- alignment (ON by default) ---------------------------------------
    // Warp the face onto the canonical 112x112 ArcFace template using the
    // detector's 5 landmarks.
    //
    // Earlier revisions kept this off because the 5 landmarks were decoded with
    // the RetinaFace variance/anchor convention instead of the SCRFD one,
    // which squeezed them to ~0.4x of their true spread. That made the
    // similarity transform zoom into the eyes/nose, and alignment measured
    // *worse* than a crop. The decoder is fixed, so alignment is now the
    // accurate front-end.
    //
    // Measured with this project's models (input 640), gallery = enrolled
    // selfie, queries = one real ID photo of the same person + several
    // different people:
    //     aligned   : genuine 0.68  |  best impostor 0.20
    //     bbox-crop : genuine 0.64  |  best impostor 0.73   <-- unusable
    //
    // NEVER mix the two front-ends inside one gallery — re-enrol (or run
    // `backfill --force`) after toggling this flag.
    bool align = true;
    bool allow_unaligned = false;

    // ---- cohort normalisation -------------------------------------------
    // Gate the raw cosine by a z-score against unrelated identities. A
    // template that matches everybody (a "hub", usually a blurry/low-quality
    // enrolment) inflates the cohort statistics and therefore needs a
    // proportionally higher score to be accepted.
    bool  cohort_norm   = true;
    float z_threshold   = 3.0f;
    int   min_cohort    = 3;         // below this, fall back to the raw gate
    std::string cohort_db;           // optional second DB of impostor faces
};

struct RecognizedFace {
    face_recognition::FaceDetection detection;
    std::string id;
    std::string name;
    std::string title;
    float similarity = 0.0f;         // raw cosine of the best template
    float z_score = 0.0f;            // cohort z-score (valid when cohort_used)
    float cohort_median = 0.0f;
    int   cohort_size = 0;
    bool  cohort_used = false;
    bool  recognized = false;
    std::string reason;              // populated when recognised == false
};

struct FrameStats {
    int frame_index = 0;
    int faces_detected = 0;
    int faces_recognized = 0;
    double detect_ms = 0.0;
    double recognize_ms = 0.0;
    double total_ms = 0.0;
};

/** Result of `verify`: the full ranking for one image. */
struct VerifyReport {
    bool ok = false;
    std::string error;
    int faces_detected = 0;
    face_recognition::MatchResult decision;
    std::vector<face_recognition::MatchCandidate> ranked;
};

class RecognitionPipeline {
public:
    RecognitionPipeline();
    ~RecognitionPipeline();

    bool initialize(const PipelineConfig& cfg, std::string& err);

    std::vector<RecognizedFace> process(cv::Mat& image, FrameStats* stats = nullptr,
                                        bool draw = true);

    /**
     * Register a face from an image file: detects the largest face, extracts
     * the embedding and stores both identity and template.
     */
    std::string register_face_from_image(const std::string& image_path,
                                         const std::string& name,
                                         const std::string& title = "",
                                         const std::string& scene = "default",
                                         const std::string& map_location = "unknown",
                                         std::string* err = nullptr);

    /** Add an extra shot to an existing identity (multi-template). */
    std::string add_template_from_image(const std::string& face_id,
                                        const std::string& image_path,
                                        std::string* err = nullptr);

    /** Rank one image against the whole gallery, with cohort statistics. */
    VerifyReport verify_image(const std::string& image_path);

    /**
     * Re-extract embeddings from the stored thumbnails.
     *
     * @param force  also redo records that already have an embedding (required
     *               after the recognizer front-end changes). Extra templates
     *               are rebuilt from their own image_path as well.
     */
    int backfill_embeddings(int* total = nullptr, int* skipped = nullptr,
                            int* failed = nullptr, bool force = false);

    face_recognition::FaceDatabase& database() { return *database_; }
    face_recognition::FaceDetector& detector()  { return *detector_; }
    face_recognition::FaceRecognizer& recognizer() { return *recognizer_; }

    const PipelineConfig& config() const { return cfg_; }
    bool ready() const { return ready_; }

private:
    PipelineConfig cfg_;
    std::unique_ptr<face_recognition::FaceDetector>   detector_;
    std::unique_ptr<face_recognition::FaceRecognizer> recognizer_;
    std::unique_ptr<face_recognition::FaceDatabase>   database_;
    bool ready_ = false;
    int frame_counter_ = 0;
    std::vector<face_recognition::FaceDetection> last_detections_;

    /** Detect the single largest face in an image; empty on failure. */
    bool detect_largest(const cv::Mat& image, face_recognition::FaceDetection& out,
                        std::string& err);

    /**
     * Shared quality gate + embedding extraction used by BOTH the live loop
     * and every enrolment path, so a gallery can never end up mixing
     * front-ends (aligned vs bbox-crop) or containing tiny-face embeddings.
     *
     * @return false and fills `reason` with "too small" / "no landmarks" /
     *         "no embedding" when the face must not be used.
     */
    bool prepare_embedding(const cv::Mat& image,
                           const face_recognition::FaceDetection& det,
                           std::vector<float>& embedding,
                           std::string& reason);
};

void draw_results(cv::Mat& image, const std::vector<RecognizedFace>& faces);

}  // namespace face_recognition_standalone

#endif
