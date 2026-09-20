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
    /**
     * @param gender      stored gender ("unknown" / "male" / "female").
     * @param image_hash  caller-supplied SHA-256 of `image_data` (see
     *                    FaceRecord::image_hash). Pass "" to skip; the importer
     *                    always supplies it so duplicates can be recognised.
     */
    std::string add_face(const std::string& name,
                        const std::vector<float>& embedding,
                        const std::vector<uint8_t>& image_data = {},
                        const std::string& title = "",
                        const std::string& scene = "default",
                        const std::string& map_location = "unknown",
                        const std::string& gender = "unknown",
                        const std::string& image_hash = "");
    bool remove_face(const std::string& face_id);
    std::shared_ptr<FaceRecord> get_face(const std::string& face_id);
    std::vector<FaceRecord> list_faces();
    int  clear_all();
    int  get_face_count();

    /**
     * Edit the mutable metadata of an existing identity (used by the web
     * gallery editor). A field left EMPTY keeps its current value, so a
     * caller that only wants to change `scene` does not have to re-send
     * (and risk blanking) the other fields.
     *
     * Returns false when `face_id` does not exist or the update fails.
     *
     * `image_path` follows the same rule; pass the value returned by
     * save_image() after replacing a record's photo.
     */
    bool update_face(const std::string& face_id,
                     const std::string& name,
                     const std::string& title,
                     const std::string& scene,
                     const std::string& map_location,
                     const std::string& image_path = "",
                     const std::string& gender = "",
                     const std::string& image_hash = "");

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

    // --- gallery metadata ----------------------------------------------------
    /**
     * Gallery-wide key/value facts. Used to record which feature space the
     * stored embeddings belong to (see feature_space.hpp), so that a change of
     * front-end / normalisation / model is DETECTED instead of silently making
     * every match worse.
     */
    bool set_meta(const std::string& key, const std::string& value);
    /** Read it back. Returns an empty string when the key is absent. */
    std::string get_meta(const std::string& key) const;

    /**
     * Look up an already-stored image by its content hash — the importer's
     * "is this exact photo already enrolled?" question.
     *
     * Returns nullptr when `image_hash` is empty or no record matches.
     */
    std::shared_ptr<FaceRecord> find_by_image_hash(const std::string& image_hash);

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
