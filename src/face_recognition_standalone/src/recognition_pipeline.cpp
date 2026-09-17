#include "face_recognition_standalone/recognition_pipeline.hpp"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

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
    recognizer_->setAlignmentEnabled(cfg_.align);

    database_ = std::make_unique<face_recognition::FaceDatabase>();
    if (!database_->initialize(cfg_.db_path, cfg_.faces_dir)) {
        err = "Database init failed at " + cfg_.db_path;
        detector_.reset();
        recognizer_.reset();
        database_.reset();
        return false;
    }
    database_->setNormalizationDefaults(cfg_.z_threshold, cfg_.min_cohort,
                                       cfg_.cohort_norm);

    // Optional external impostor cohort, used for score normalisation.
    if (!cfg_.cohort_db.empty()) {
        auto cohort = std::make_unique<face_recognition::FaceDatabase>();
        if (cohort->initialize(cfg_.cohort_db, cfg_.faces_dir)) {
            std::vector<std::vector<float>> embs;
            for (const auto& f : cohort->list_faces()) {
                if (!f.embedding.empty()) embs.push_back(f.embedding);
            }
            for (const auto& t : cohort->list_templates("")) {
                if (!t.embedding.empty()) embs.push_back(t.embedding);
            }
            std::printf("[INFO] cohort DB: %s (%zu embeddings)\n",
                        cfg_.cohort_db.c_str(), embs.size());
            database_->setCohortEmbeddings(std::move(embs));
        } else {
            std::fprintf(stderr, "[WARN] cannot open cohort DB: %s\n",
                         cfg_.cohort_db.c_str());
        }
    }

    ready_ = true;
    return true;
}

bool RecognitionPipeline::detect_largest(const cv::Mat& image,
                                         face_recognition::FaceDetection& out,
                                         std::string& err) {
    auto detections = detector_->detect(image, 10);
    if (detections.empty()) {
        err = "no face detected";
        return false;
    }
    // Prefer the largest face: for enrolment/verification the subject is
    // normally the closest face in the frame.
    size_t best = 0;
    double best_area = 0.0;
    for (size_t i = 0; i < detections.size(); ++i) {
        const double area = static_cast<double>(detections[i].bbox.width) *
                            static_cast<double>(detections[i].bbox.height);
        if (area > best_area) { best_area = area; best = i; }
    }
    out = detections[best];
    return true;
}

bool RecognitionPipeline::prepare_embedding(const cv::Mat& image,
                                            const face_recognition::FaceDetection& det,
                                            std::vector<float>& embedding,
                                            std::string& reason) {
    const int short_side = static_cast<int>(
        std::min(det.bbox.width, det.bbox.height));
    if (short_side < cfg_.min_face_size) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "too small (%dx%d < %d px)",
                      det.bbox.width, det.bbox.height, cfg_.min_face_size);
        reason = buf;
        return false;
    }

    bool used_alignment = false;
    embedding = recognizer_->extract_embedding(image, det.bbox, det.landmarks,
                                              &used_alignment);
    if (embedding.empty()) {
        reason = "no embedding";
        return false;
    }

    // Refuse to mix front-ends: a bbox-crop embedding is not comparable with
    // the aligned embeddings that a normal gallery contains.
    if (cfg_.align && !used_alignment && !cfg_.allow_unaligned) {
        reason = "no landmarks";
        return false;
    }
    return true;
}

std::vector<RecognizedFace> RecognitionPipeline::process(cv::Mat& image,
                                                       FrameStats* stats,
                                                       bool draw) {
    std::vector<RecognizedFace> out;
    if (!ready_ || image.empty()) return out;

    ++frame_counter_;

    auto t_total = std::chrono::steady_clock::now();

    // Optional pre-resize for the DETECTOR only; recognition always uses the
    // original coordinates so embedding quality is unaffected.
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
        last_detections_ = detections;
    } else if (!last_detections_.empty()) {
        detections = last_detections_;
    }

    if (cfg_.run_recognition) {
        auto t_rec = std::chrono::steady_clock::now();
        for (const auto& det : detections) {
            RecognizedFace rf;
            rf.detection = det;

            std::vector<float> embedding;
            std::string qerr;
            if (!prepare_embedding(image, det, embedding, qerr)) {
                rf.reason = qerr;
                out.push_back(std::move(rf));
                continue;
            }

            auto res = database_->match_face(embedding, cfg_.recognition_threshold,
                                            cfg_.z_threshold, cfg_.min_cohort,
                                            cfg_.cohort_norm);
            rf.similarity    = res.raw_similarity;
            rf.z_score       = res.z_score;
            rf.cohort_median = res.cohort_median;
            rf.cohort_size   = res.cohort_size;
            rf.cohort_used   = res.normalized;
            rf.reason        = res.reason;
            if (res.accepted) {
                rf.id    = res.face_id;
                rf.name  = res.name;
                rf.title = res.title;
                rf.recognized = true;
            }
            out.push_back(std::move(rf));
        }
        rec_ms = ms_since(t_rec);
    } else {
        for (const auto& det : detections) {
            RecognizedFace rf;
            rf.detection = det;
            rf.reason = "recognition disabled";
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

    face_recognition::FaceDetection det;
    std::string derr;
    if (!detect_largest(img, det, derr)) {
        if (err) *err = derr + " in " + image_path;
        return {};
    }

    std::vector<float> embedding;
    std::string qerr;
    if (!prepare_embedding(img, det, embedding, qerr)) {
        if (err) *err = "Face rejected [" + qerr + "]: " + image_path;
        return {};
    }

    std::vector<uint8_t> image_data;
    {
        std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 95};
        cv::imencode(".jpg", img, image_data, params);
    }

    std::string id = database_->add_face(name, embedding, image_data, title, scene,
                                        map_location);
    if (id.empty() && err) *err = "Failed to insert face into DB";
    return id;
}

std::string RecognitionPipeline::add_template_from_image(const std::string& face_id,
                                                        const std::string& image_path,
                                                        std::string* err) {
    if (!ready_) {
        if (err) *err = "Pipeline not initialized";
        return {};
    }
    if (!database_->get_face(face_id)) {
        if (err) *err = "Unknown face id: " + face_id;
        return {};
    }
    cv::Mat img = cv::imread(image_path, cv::IMREAD_COLOR);
    if (img.empty()) {
        if (err) *err = "Failed to read image: " + image_path;
        return {};
    }
    face_recognition::FaceDetection det;
    std::string derr;
    if (!detect_largest(img, det, derr)) {
        if (err) *err = derr + " in " + image_path;
        return {};
    }
    std::vector<float> embedding;
    std::string qerr;
    if (!prepare_embedding(img, det, embedding, qerr)) {
        if (err) *err = "Face rejected [" + qerr + "]: " + image_path;
        return {};
    }

    // Keep our own copy of the shot so it can be rebuilt by `backfill --all`.
    std::vector<uint8_t> image_data;
    {
        std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 95};
        cv::imencode(".jpg", img, image_data, params);
    }
    std::string stored_path;
    const int n = database_->template_count(face_id);
    char fname[512];
    std::snprintf(fname, sizeof(fname), "%s_t%d", face_id.c_str(), n + 1);
    if (database_->save_image(fname, image_data, stored_path)) {
        // save_image appends ".jpg" to the id we passed.
    }

    if (!database_->add_template(face_id, embedding, stored_path)) {
        if (err) *err = "Failed to insert template";
        return {};
    }
    return stored_path;
}

VerifyReport RecognitionPipeline::verify_image(const std::string& image_path) {
    VerifyReport rep;
    if (!ready_) { rep.error = "Pipeline not initialized"; return rep; }

    cv::Mat img = cv::imread(image_path, cv::IMREAD_COLOR);
    if (img.empty()) { rep.error = "Failed to read image: " + image_path; return rep; }

    face_recognition::FaceDetection det;
    std::string derr;
    if (!detect_largest(img, det, derr)) { rep.error = derr; return rep; }
    rep.faces_detected = 1;

    // Same gate as the live loop / enrolment, so `verify` reports exactly the
    // decision the system would make at runtime.
    std::vector<float> embedding;
    std::string qerr;
    if (!prepare_embedding(img, det, embedding, qerr)) {
        rep.error = "face rejected by quality gate [" + qerr + "]";
        return rep;
    }

    rep.ranked = database_->rank_faces(embedding);
    rep.decision = database_->match_face(embedding, cfg_.recognition_threshold,
                                        cfg_.z_threshold, cfg_.min_cohort,
                                        cfg_.cohort_norm);
    rep.ok = true;
    return rep;
}

int RecognitionPipeline::backfill_embeddings(int* total, int* skipped, int* failed,
                                             bool force) {
    if (!ready_) return 0;

    auto faces = database_->list_faces();
    int n_total = static_cast<int>(faces.size());
    int n_done = 0, n_skip = 0, n_fail = 0;

    for (const auto& face : faces) {
        // ---- primary template (faces.embedding) --------------------------
        if (!face.embedding.empty() && !force) {
            ++n_skip;
        } else if (face.image_path.empty()) {
            ++n_fail;
            std::fprintf(stderr, "[backfill] id=%s name=%s : no image_path, skipping\n",
                         face.id.c_str(), face.name.c_str());
        } else {
            cv::Mat img = cv::imread(face.image_path, cv::IMREAD_COLOR);
            if (img.empty()) {
                ++n_fail;
                std::fprintf(stderr, "[backfill] id=%s name=%s : failed to read %s\n",
                             face.id.c_str(), face.name.c_str(),
                             face.image_path.c_str());
            } else {
                face_recognition::FaceDetection det;
                std::string derr;
                if (!detect_largest(img, det, derr)) {
                    ++n_fail;
                    std::fprintf(stderr, "[backfill] id=%s name=%s : %s in %s\n",
                                 face.id.c_str(), face.name.c_str(), derr.c_str(),
                                 face.image_path.c_str());
                } else {
                    std::vector<float> emb;
                    std::string qerr;
                    if (!prepare_embedding(img, det, emb, qerr) ||
                        !database_->update_embedding(face.id, emb)) {
                        ++n_fail;
                        std::fprintf(stderr, "[backfill] id=%s name=%s : %s\n",
                                     face.id.c_str(), face.name.c_str(),
                                     qerr.empty() ? "failed to update embedding"
                                                  : qerr.c_str());
                    } else {
                        ++n_done;
                        std::printf("[backfill] updated id=%s name=%s (%zu-d embedding)\n",
                                    face.id.c_str(), face.name.c_str(), emb.size());
                    }
                }
            }
        }

        // ---- extra templates --------------------------------------------
        // Only rebuilt when forcing: otherwise the stored templates are still
        // valid (they were produced by the same front-end that is running now).
        if (!force) continue;
        auto tpls = database_->list_templates(face.id);
        if (tpls.empty()) continue;

        std::vector<std::string> tpl_images;
        for (const auto& t : tpls) {
            if (!t.image_path.empty()) tpl_images.push_back(t.image_path);
        }
        database_->remove_templates(face.id);

        int rebuilt = 0;
        for (const auto& path : tpl_images) {
            cv::Mat img = cv::imread(path, cv::IMREAD_COLOR);
            if (img.empty()) {
                std::fprintf(stderr, "[backfill] template image unreadable: %s\n",
                             path.c_str());
                continue;
            }
            face_recognition::FaceDetection det;
            std::string derr;
            if (!detect_largest(img, det, derr)) {
                std::fprintf(stderr, "[backfill] template %s : %s\n", path.c_str(),
                             derr.c_str());
                continue;
            }
            std::vector<float> emb;
            std::string qerr;
            if (!prepare_embedding(img, det, emb, qerr)) {
                std::fprintf(stderr, "[backfill] template %s : %s\n", path.c_str(),
                             qerr.c_str());
                continue;
            }
            if (!database_->add_template(face.id, emb, path)) continue;
            ++rebuilt;
        }
        if (!tpl_images.empty()) {
            std::printf("[backfill] id=%s name=%s : rebuilt %d/%zu extra template(s)\n",
                        face.id.c_str(), face.name.c_str(), rebuilt, tpl_images.size());
        }
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
        char buf[192];
        if (rf.recognized) {
            // Show the z-score too when cohort normalisation was applied — it
            // is the quantity the decision actually used.
            if (rf.cohort_used) {
                std::snprintf(buf, sizeof(buf), "%s (%.2f z=%.1f)",
                              rf.name.c_str(), rf.similarity, rf.z_score);
            } else {
                std::snprintf(buf, sizeof(buf), "%s (%.2f)", rf.name.c_str(),
                              rf.similarity);
            }
            label = buf;
            if (!rf.title.empty()) label += " - " + rf.title;
        } else {
            // Quality-gate rejections carry a short reason. Show it instead of
            // a bare "Unknown" so it is obvious WHY nothing matched.
            std::string tag;
            if (rf.reason.rfind("too small", 0) == 0)          tag = "too small";
            else if (rf.reason.rfind("no landmarks", 0) == 0)  tag = "no-align";
            else if (rf.reason.rfind("no embedding", 0) == 0)  tag = "no-emb";

            if (!tag.empty()) {
                std::snprintf(buf, sizeof(buf), "[%s]", tag.c_str());
            } else if (rf.cohort_used) {
                std::snprintf(buf, sizeof(buf), "Unknown (%.2f z=%.1f)",
                              rf.similarity, rf.z_score);
            } else {
                std::snprintf(buf, sizeof(buf), "Unknown (%.2f)", rf.similarity);
            }
            label = buf;
        }
        int x = static_cast<int>(b.x);
        int y = static_cast<int>(b.y);
        if (y < 12) y = 12;
        draw_label(image, label, x, y, color, cv::Scalar(255, 255, 255));

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
