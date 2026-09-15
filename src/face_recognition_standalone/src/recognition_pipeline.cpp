#include "face_recognition_standalone/recognition_pipeline.hpp"

#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <iomanip>

namespace face_recognition_standalone {

namespace {

double ms_since(std::chrono::steady_clock::time_point t0) {
    using namespace std::chrono;
    return duration_cast<duration<double, std::milli>>(steady_clock::now() - t0).count();
}

void draw_label(cv::Mat& img, const std::string& text, int x, int y,
                cv::Scalar bg = cv::Scalar(0, 0, 0), cv::Scalar fg = cv::Scalar(255, 255, 255)) {
    int baseline = 0;
    double font_scale = 0.5;
    int thickness = 1;
    cv::Size sz = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, font_scale, thickness, &baseline);
    cv::rectangle(img, cv::Point(x, y - sz.height - 4),
                  cv::Point(x + sz.width + 4, y),
                  bg, cv::FILLED);
    cv::putText(img, text, cv::Point(x + 2, y - 2),
                cv::FONT_HERSHEY_SIMPLEX, font_scale, fg, thickness, cv::LINE_AA);
}

}  // namespace

RecognitionPipeline::RecognitionPipeline() = default;
RecognitionPipeline::~RecognitionPipeline() = default;

bool RecognitionPipeline::initialize(const PipelineConfig& cfg, std::string& err) {
    cfg_ = cfg;

    detector_ = std::make_unique<face_recognition::FaceDetector>();
    if (!detector_->initialize(cfg_.detection_model,
                               cfg_.detection_threshold,
                               cfg_.nms_threshold,
                               cfg_.input_size)) {
        err = "Detector init failed: " + detector_->getLastError();
        detector_.reset();
        return false;
    }

    recognizer_ = std::make_unique<face_recognition::FaceRecognizer>();
    if (!recognizer_->initialize(cfg_.recognition_model)) {
        err = "Recognizer init failed: " + recognizer_->getLastError();
        detector_.reset();
        recognizer_.reset();
        return false;
    }

    database_ = std::make_unique<face_recognition::FaceDatabase>();
    if (!database_->initialize(cfg_.db_path, cfg_.faces_dir)) {
        err = "Database init failed at " + cfg_.db_path;
        detector_.reset();
        recognizer_.reset();
        database_.reset();
        return false;
    }

    ready_ = true;
    return true;
}

std::vector<RecognizedFace> RecognitionPipeline::process(cv::Mat& image,
                                                       FrameStats* stats,
                                                       bool draw) {
    std::vector<RecognizedFace> out;
    if (!ready_ || image.empty()) return out;

    ++frame_counter_;

    auto t_total = std::chrono::steady_clock::now();

    // Optional pre-resize. If downscale_max_side > 0 and the input is larger,
    // we shrink the longest side. Recognition uses the *original* bbox
    // coordinates so embedding quality is unaffected — only the detector
    // pays the smaller-resolution cost.
    cv::Mat detect_view = image;
    double x_scale = 1.0, y_scale = 1.0;
    if (cfg_.downscale_max_side > 0) {
        int max_side = std::max(image.cols, image.rows);
        if (max_side > cfg_.downscale_max_side) {
            double s = static_cast<double>(cfg_.downscale_max_side) / max_side;
            cv::Mat resized;
            cv::resize(image, resized, cv::Size(), s, s, cv::INTER_AREA);
            x_scale = 1.0 / s;
            y_scale = 1.0 / s;
            detect_view = resized;
        }
    }

    bool run_detection = (cfg_.detect_every_n_frames <= 1) ||
                         (frame_counter_ % cfg_.detect_every_n_frames == 1);

    std::vector<face_recognition::FaceDetection> detections;
    double det_ms = 0.0;
    double rec_ms = 0.0;

    if (run_detection) {
        auto t_det = std::chrono::steady_clock::now();
        detections = detector_->detect(detect_view, cfg_.max_faces);
        det_ms = ms_since(t_det);

        // Scale bbox coords back to original-frame coords.
        if (x_scale != 1.0 || y_scale != 1.0) {
            for (auto& d : detections) {
                d.bbox.x = static_cast<uint32_t>(d.bbox.x * x_scale);
                d.bbox.y = static_cast<uint32_t>(d.bbox.y * y_scale);
                d.bbox.width  = static_cast<uint32_t>(d.bbox.width  * x_scale);
                d.bbox.height = static_cast<uint32_t>(d.bbox.height * y_scale);
                if (!d.landmarks.empty()) {
                    for (size_t i = 0; i + 1 < d.landmarks.size(); i += 2) {
                        d.landmarks[i]     = static_cast<float>(d.landmarks[i]     * x_scale);
                        d.landmarks[i + 1] = static_cast<float>(d.landmarks[i + 1] * y_scale);
                    }
                }
            }
        }
        last_detections_ = detections;  // cache for skipped frames
    } else if (!last_detections_.empty()) {
        // On skipped frames, still try to recognize against the cached bboxes
        // so the visual annotation stays consistent.
        detections = last_detections_;
    }

    if (cfg_.run_recognition) {
        auto t_rec = std::chrono::steady_clock::now();
        for (const auto& det : detections) {
            RecognizedFace rf;
            rf.detection = det;

            std::vector<float> embedding = recognizer_->extract_embedding(image, det.bbox);
            if (embedding.empty()) continue;

            auto match = database_->find_matching_face(embedding, cfg_.recognition_threshold);
            if (match) {
                rf.id = match->id;
                rf.name = match->name;
                rf.title = match->title;
                rf.similarity = face_recognition::compute_similarity(embedding, match->embedding);
                rf.recognized = true;
            }
            out.push_back(std::move(rf));
        }
        rec_ms = ms_since(t_rec);
    } else {
        // Detection-only: just echo the detections.
        for (const auto& det : detections) {
            RecognizedFace rf;
            rf.detection = det;
            out.push_back(std::move(rf));
        }
    }

    if (draw) {
        draw_results(image, out);
    }

    if (stats) {
        stats->frame_index = frame_counter_;
        stats->faces_detected = static_cast<int>(detections.size());
        stats->faces_recognized = 0;
        for (const auto& rf : out) if (rf.recognized) ++stats->faces_recognized;
        stats->detect_ms = det_ms;
        stats->recognize_ms = rec_ms;
        stats->total_ms = ms_since(t_total);
    }

    return out;
}

std::string RecognitionPipeline::register_face_from_image(const std::string& image_path,
                                                         const std::string& name,
                                                         const std::string& title,
                                                         const std::string& scene,
                                                         const std::string& map_location,
                                                         std::string* err) {
    if (!ready_) {
        if (err) *err = "Pipeline not initialized";
        return {};
    }

    cv::Mat img = cv::imread(image_path, cv::IMREAD_COLOR);
    if (img.empty()) {
        if (err) *err = "Failed to read image: " + image_path;
        return {};
    }

    auto detections = detector_->detect(img, 1);
    if (detections.empty()) {
        if (err) *err = "No face detected in " + image_path;
        return {};
    }

    std::vector<float> embedding = recognizer_->extract_embedding(img, detections.front().bbox);
    if (embedding.empty()) {
        if (err) *err = "Failed to extract embedding";
        return {};
    }

    std::vector<uint8_t> image_data;
    {
        std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 90};
        cv::imencode(".jpg", img, image_data, params);
    }

    std::string id = database_->add_face(name, embedding, image_data, title, scene, map_location);
    if (id.empty() && err) *err = "Failed to insert face into DB";
    return id;
}

int RecognitionPipeline::backfill_embeddings(int* total, int* skipped, int* failed,
                                             bool force) {
    if (!ready_) return 0;

    auto faces = database_->list_faces();
    int n_total = static_cast<int>(faces.size());
    int n_done = 0, n_skip = 0, n_fail = 0;

    for (const auto& face : faces) {
        if (!face.embedding.empty() && !force) {
            ++n_skip;
            continue;
        }
        if (face.image_path.empty()) {
            ++n_fail;
            std::fprintf(stderr,
                "[backfill] id=%s name=%s : no image_path, skipping\n",
                face.id.c_str(), face.name.c_str());
            continue;
        }
        cv::Mat img = cv::imread(face.image_path, cv::IMREAD_COLOR);
        if (img.empty()) {
            ++n_fail;
            std::fprintf(stderr,
                "[backfill] id=%s name=%s : failed to read %s\n",
                face.id.c_str(), face.name.c_str(), face.image_path.c_str());
            continue;
        }
        auto detections = detector_->detect(img, 1);
        if (detections.empty()) {
            ++n_fail;
            std::fprintf(stderr,
                "[backfill] id=%s name=%s : no face detected in %s\n",
                face.id.c_str(), face.name.c_str(), face.image_path.c_str());
            continue;
        }
        std::vector<float> emb = recognizer_->extract_embedding(img, detections.front().bbox);
        if (emb.empty() || !database_->update_embedding(face.id, emb)) {
            ++n_fail;
            std::fprintf(stderr,
                "[backfill] id=%s name=%s : failed to extract/update embedding\n",
                face.id.c_str(), face.name.c_str());
            continue;
        }
        ++n_done;
        std::printf("[backfill] updated id=%s name=%s (%zu-d embedding)\n",
                    face.id.c_str(), face.name.c_str(), emb.size());
    }

    if (total)   *total   = n_total;
    if (skipped) *skipped = n_skip;
    if (failed)  *failed  = n_fail;
    return n_done;
}

void draw_results(cv::Mat& image, const std::vector<RecognizedFace>& faces) {
    for (const auto& rf : faces) {
        const auto& b = rf.detection.bbox;
        cv::Scalar color = rf.recognized ? cv::Scalar(0, 200, 0) : cv::Scalar(0, 0, 220);

        cv::rectangle(image,
                      cv::Point(static_cast<int>(b.x), static_cast<int>(b.y)),
                      cv::Point(static_cast<int>(b.x + b.width),
                                static_cast<int>(b.y + b.height)),
                      color, 2);

        std::string label;
        if (rf.recognized) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s (%.2f)", rf.name.c_str(), rf.similarity);
            label = buf;
            if (!rf.title.empty()) {
                label += " - " + rf.title;
            }
        } else {
            label = "Unknown";
        }
        int x = static_cast<int>(b.x);
        int y = static_cast<int>(b.y);
        if (y < 12) y = 12;
        draw_label(image, label, x, y, color, cv::Scalar(255, 255, 255));

        // Landmarks (if available)
        if (rf.detection.landmarks.size() == 10) {
            for (size_t i = 0; i < 5; ++i) {
                cv::circle(image,
                           cv::Point(static_cast<int>(rf.detection.landmarks[2 * i]),
                                     static_cast<int>(rf.detection.landmarks[2 * i + 1])),
                           2, cv::Scalar(0, 255, 255), cv::FILLED, cv::LINE_AA);
            }
        }
    }
}

}  // namespace face_recognition_standalone
