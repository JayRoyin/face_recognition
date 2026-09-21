#pragma once

#ifndef FACE_DB_WEB_FACE_HANDLER_HPP
#define FACE_DB_WEB_FACE_HANDLER_HPP

#include "face_db_web/enroll_policy.hpp"
#include "face_db_web/http_server.hpp"
#include "face_db_web/pending_store.hpp"
#include "face_recognition_core/face_database.hpp"
#include "face_recognition_core/face_detector.hpp"
#include "face_recognition_core/face_recognizer.hpp"
#include "face_recognition_core/gender_classifier.hpp"

#include <opencv2/core.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace face_db_web {

class FaceHandler {
public:
    FaceHandler(std::shared_ptr<face_recognition::FaceDatabase> database,
                std::shared_ptr<face_recognition::FaceDetector>   detector   = nullptr,
                std::shared_ptr<face_recognition::FaceRecognizer> recognizer = nullptr,
                bool require_embedding = false,
                EnrollPolicy policy = EnrollPolicy{},
                std::shared_ptr<face_recognition::GenderClassifier> gender = nullptr);

    HttpResponse index(const HttpRequest& req);
    HttpResponse listFaces(const HttpRequest& req);
    HttpResponse addFace(const HttpRequest& req);
    HttpResponse removeFace(const HttpRequest& req);
    HttpResponse clearFaces(const HttpRequest& req);
    HttpResponse getImage(const HttpRequest& req);

    /** POST /api/faces/import-archive — bulk enrolment from a zip/tar/tar.gz. */
    HttpResponse importArchive(const HttpRequest& req);
    /** POST /api/faces/import-batch — bulk enrolment from an edited image grid. */
    HttpResponse importBatch(const HttpRequest& req);
    /** POST /api/faces/update — edit metadata and/or rebuild the embedding. */
    HttpResponse updateFace(const HttpRequest& req);
    /**
     * POST /api/faces/add-template — append ANOTHER photo of an existing
     * person as an extra template (multi-shot), instead of replacing the
     * primary embedding.
     */
    HttpResponse addTemplate(const HttpRequest& req);

    /** GET /api/config — which optional behaviours are active (UI hints). */
    HttpResponse getConfig(const HttpRequest& req);
    /** GET /api/faces/pending — enrolments waiting for a human decision. */
    HttpResponse listPending(const HttpRequest& req);
    /** GET /api/pending/image/<token> — preview of a staged upload. */
    HttpResponse pendingImage(const HttpRequest& req);
    /**
     * POST /api/faces/resolve — apply the operator's decisions.
     *
     * This is the ONLY path that writes a highly-similar face into the
     * gallery; the import endpoints park such uploads instead of storing them.
     */
    HttpResponse resolvePending(const HttpRequest& req);

private:
    /** Per-image outcome of a bulk import, reported back to the UI verbatim. */
    struct EnrollOutcome {
        bool        ok             = false;
        bool        has_embedding  = false;
        bool        duplicate      = false;
        std::string id;
        std::string reason;  // empty when ok
        /** Effective gender after the auto-fill (may be the caller's value). */
        std::string gender;
        /** Set when the image was recognised as an already-stored one. */
        std::string existing_id;
        std::string existing_name;
        /** Set when the FACE looks like someone already in the gallery. */
        std::string similar_name;
        float       similarity = 0.0f;

        /** High-similarity hit: parked, nothing written yet. */
        bool        needs_confirm = false;
        std::string token;
        std::string match_id;
        std::string match_scene;
        bool        same_scene = false;
    };

    /**
     * Duplicate bookkeeping for ONE import request.
     *
     * Three independent signals, strongest first:
     *
     *  1. content hash (SHA-256 of the image bytes) — the same photo is always
     *     a duplicate, even after renaming or re-exporting. Checked against
     *     this request AND against the stored `image_hash` column.
     *  2. file name — only within the same request. The gallery may legitimately
     *     hold several different photos of one person, so a repeated name
     *     against the DB is NOT treated as a duplicate.
     *  3. face similarity (cosine of the 512-D embedding) — a different photo
     *     of someone already enrolled. Reported as a hint by default (extra
     *     templates are a supported workflow); set `skip_similar` to skip.
     */
    struct DedupContext {
        std::unordered_map<std::string, std::string> hashes;      // hash -> name
        std::unordered_map<std::string, std::string> file_names;  // basename -> name
        float similarity_threshold = 0.70f;
        bool  skip_similar         = false;
    };

    /**
     * Decode + detect + embed one image. On failure `note` says why.
     *
     * The embedding ALWAYS goes through face_recognition::make_embedding(); a
     * bulk import that called the recognizer directly would fill the gallery
     * with vectors from a different feature space than the single-image path,
     * which is invisible until every match score looks wrong.
     *
     * Detection runs at the configured threshold; if nothing is found, one
     * rescue pass at a very low threshold follows — curated gallery photos
     * occasionally score below it even though they are perfectly usable
     * (SCRFD dips low when the face fills the frame). The normal quality gate
     * still decides whether the resulting embedding is accepted.
     */
    bool extractEmbedding(const std::vector<uint8_t>& image_bytes,
                          std::vector<float>& embedding,
                          std::string& note,
                          cv::Mat* face_crop = nullptr);

    /**
     * Fill `gender` from the gender model when the upload did not carry one.
     * Never overrides an explicit value, and stays "unknown" on low confidence
     * or when the model is unavailable.
     */
    std::string guessGender(const cv::Mat& face_crop, const std::string& current) const;

    /**
     * @param file_key  basename used for the within-request name check ("" to skip)
     * @param dedup     per-request duplicate bookkeeping, updated on success
     */
    EnrollOutcome enrollImage(const std::string& name,
                              const std::string& title,
                              const std::string& scene,
                              const std::string& map_location,
                              const std::string& gender,
                              const std::vector<uint8_t>& image_bytes,
                              const std::string& file_key,
                              DedupContext& dedup);

    /**
     * "title_name" -> (title, name). Split on the LAST underscore: a title
     * such as "资深_工程师" is common, a person's name containing one is not.
     * Without an underscore the whole stem becomes the name.
     */
    static void parseTitleName(const std::string& stem, std::string& title, std::string& name);

    /** Accepted image suffix for bulk import: png / jpg / jpeg only. */
    static bool isSupportedImage(const std::string& basename);
    /** Stem of a file name (no directory, no extension). */
    static std::string stemOf(const std::string& basename);
    static std::string trimmed(const std::string& s);

    std::string renderIndex();
    std::string renderFaceList();

    std::string urlDecode(const std::string& str);
    std::string base64Decode(const std::string& encoded);
    std::string base64Encode(const std::vector<uint8_t>& data);

    std::shared_ptr<face_recognition::FaceDatabase>   database_;
    std::shared_ptr<face_recognition::FaceDetector>   detector_;
    std::shared_ptr<face_recognition::FaceRecognizer> recognizer_;
    std::string templates_dir_;
    EnrollPolicy policy_;
    PendingStore pending_;
    std::shared_ptr<face_recognition::GenderClassifier> gender_;
    // When true, an enrolment that yields no embedding is rejected instead of
    // silently creating a record the recognizer can never match.
    bool require_embedding_ = false;
};

/** Hard cap on the number of items one bulk request may carry. */
constexpr std::size_t kMaxBatchItems = 2000;

/** Threshold of the permissive retry when the first detection pass is empty. */
constexpr float kRescueDetectionThreshold = 0.1f;

/** Default cosine above which two embeddings are reported as the same person. */
constexpr float kDefaultSimilarThreshold = 0.70f;

}  // namespace face_db_web

#endif
