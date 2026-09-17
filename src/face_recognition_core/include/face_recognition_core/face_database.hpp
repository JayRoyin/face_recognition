#pragma once

#ifndef FACE_RECOGNITION_CORE_FACE_DATABASE_HPP
#define FACE_RECOGNITION_CORE_FACE_DATABASE_HPP

#include "types.hpp"
#include <string>
#include <vector>
#include <memory>
#include <mutex>

namespace face_recognition {

/**
 * SQLite-backed face gallery.
 *
 * Storage model
 * -------------
 *   faces           one row per identity (metadata + the PRIMARY embedding)
 *   face_templates  0..N EXTRA embeddings for an identity (multi-shot)
 *
 * Matching scores a query against every template of every identity and keeps
 * the best score per identity, then optionally applies cohort normalisation
 * before comparing against the threshold.
 */
class FaceDatabase {
public:
    FaceDatabase();
    ~FaceDatabase();

    bool initialize(const std::string& db_path, const std::string& faces_dir);

    // --- identities ---------------------------------------------------------
    std::string add_face(const std::string& name,
                        const std::vector<float>& embedding,
                        const std::vector<uint8_t>& image_data = {},
                        const std::string& title = "",
                        const std::string& scene = "default",
                        const std::string& map_location = "unknown");
    bool remove_face(const std::string& face_id);
    std::shared_ptr<FaceRecord> get_face(const std::string& face_id);
    std::vector<FaceRecord> list_faces();
    int  clear_all();
    int  get_face_count();

    // --- extra templates (multi-shot) ---------------------------------------
    bool add_template(const std::string& face_id,
                      const std::vector<float>& embedding,
                      const std::string& image_path = "");
    std::vector<FaceTemplate> list_templates(const std::string& face_id);
    int  template_count(const std::string& face_id);
    bool remove_templates(const std::string& face_id);

    // --- matching -----------------------------------------------------------
    /**
     * Score the query against every template and return one candidate per
     * identity, sorted by descending best similarity.
     */
    std::vector<MatchCandidate> rank_faces(const std::vector<float>& embedding);

    /**
     * Full decision: rank, cohort-normalise, then apply the thresholds.
     *
     * @param raw_threshold  minimum best-template cosine to be considered
     * @param z_threshold    minimum cohort z-score (ignored when the cohort is
     *                       smaller than `min_cohort`)
     * @param min_cohort     minimum cohort samples needed to normalise
     */
    MatchResult match_face(const std::vector<float>& embedding,
                           float raw_threshold,
                           float z_threshold = 3.0f,
                           int   min_cohort  = 3,
                           bool  normalize   = true);

    /** Back-compat wrapper used by the ROS nodes. */
    std::shared_ptr<FaceRecord> find_matching_face(const std::vector<float>& embedding,
                                                   float threshold = 0.7f);

    /** External impostor cohort used for normalisation (e.g. a second DB). */
    void setCohortEmbeddings(std::vector<std::vector<float>> cohort);
    void setNormalizationDefaults(float z_threshold, int min_cohort, bool enabled);
    int  cohortSize() const;

    // --- images / embeddings -------------------------------------------------
    bool save_image(const std::string& face_id, const std::vector<uint8_t>& data,
                    std::string& out_path);
    bool update_embedding(const std::string& face_id,
                          const std::vector<float>& embedding);

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
    mutable std::recursive_mutex mutex_;

    float z_threshold_ = 3.0f;
    int   min_cohort_  = 3;
    bool  normalize_   = true;

    /** External impostor embeddings used by cohort normalisation. */
    std::vector<std::vector<float>> cohort_embeddings_;
};

}  // namespace face_recognition

#endif
