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
    // Recognition similarity threshold.
    // Measured with this project's own models (det_10g + w600k_r50):
    //   same identity, different photos ....... 0.50 ~ 0.76
    //   same identity, camera-like degradation  0.62 ~ 0.69 (640x480 + JPEG)
    //   different identity (impostor) ......... <= 0.40
    // 0.5 keeps genuine pairs while tolerating downscale / JPEG / non-frontal
    // pose. The previous default of 0.7 rejected every degraded genuine match.
    float recognition_threshold = 0.5f;
    float nms_threshold = 0.5f;
    int input_size = 640;
    int max_faces = 10;
    // Detect every N frames. Skipped frames reuse the cached bboxes while
    // recognition still runs, so this halves the detector cost (the dominant
    // bottleneck on CPU) without hurting detection quality.
    int detect_every_n_frames = 2;
    int downscale_max_side = 0;      // 0 = no downscale; >0 = resize frame so
                                     //     its longest side ≤ this before detect
    bool run_recognition = true;     // false = detection-only mode
};

struct RecognizedFace {
    face_recognition::FaceDetection detection;
    std::string id;
    std::string name;
    std::string title;
    float similarity = 0.0f;
    bool recognized = false;
};

struct FrameStats {
    int frame_index = 0;
    int faces_detected = 0;
    int faces_recognized = 0;
    double detect_ms = 0.0;
    double recognize_ms = 0.0;
    double total_ms = 0.0;
};

class RecognitionPipeline {
public:
    RecognitionPipeline();
    ~RecognitionPipeline();

    bool initialize(const PipelineConfig& cfg, std::string& err);

    /**
     * Run detection+recognition on a frame. If `draw` is true, draws bounding
     * boxes and labels onto `image` in place.
     */
    std::vector<RecognizedFace> process(cv::Mat& image, FrameStats* stats = nullptr,
                                        bool draw = true);

    /**
     * Convenience: register a face from an image file. Detects the largest
     * face in the image, extracts the embedding and stores everything in DB.
     */
    std::string register_face_from_image(const std::string& image_path,
                                         const std::string& name,
                                         const std::string& title = "",
                                         const std::string& scene = "default",
                                         const std::string& map_location = "unknown",
                                         std::string* err = nullptr);

    /**
     * For each face in the DB whose embedding is empty but image_path exists,
     * re-run detection+recognition on the stored image and write the
     * embedding back. Returns how many faces were updated.
     *
     * The standalone equivalent of the backfill logic in the ROS2 node.
     *
     * @param force  re-extract EVERY record, even ones that already have an
     *               embedding. Required after a change to the recognizer's
     *               preprocessing, which invalidates all stored embeddings.
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
};

void draw_results(cv::Mat& image, const std::vector<RecognizedFace>& faces);

}  // namespace face_recognition_standalone

#endif
