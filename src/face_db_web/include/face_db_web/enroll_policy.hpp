#pragma once

#ifndef FACE_DB_WEB_ENROLL_POLICY_HPP
#define FACE_DB_WEB_ENROLL_POLICY_HPP

/**
 * Re-enrolment policy: what happens when an uploaded face is very similar to
 * someone already in the gallery.
 *
 * The default is strictly "never write without a human decision": at or above
 * the high-similarity threshold the image is parked in the pending queue and
 * nothing is stored until the operator answers. The knobs below only ever
 * RELAX that (and every relaxation is logged at start-up), they never make an
 * unconfirmed write possible by accident.
 */

#include <optional>
#include <string>
#include <unordered_map>

namespace face_db_web {

/** Per-scene override. Absent fields fall back to the global settings. */
struct SceneRule {
    /** Own similarity threshold for this scene. */
    std::optional<float> threshold;
    /** Forces / suppresses the confirmation step for this scene. */
    std::optional<bool>  confirm_required;
    /** Allow automatic enrolment when the match lives in ANOTHER scene. */
    std::optional<bool>  allow_auto_cross_scene;
    /** Allow "replace the matched record" as a resolution for this scene. */
    std::optional<bool>  allow_replace;
};

struct EnrollPolicy {
    /** At/above this cosine the upload is treated as a possible re-enrolment. */
    float high_similarity = 0.80f;

    /** Master switch. When false nothing is ever held for confirmation. */
    bool require_confirmation = true;

    /** Confirm when the match lives in the SAME scene (requirement: always). */
    bool confirm_on_same_scene  = true;
    /** Confirm when the match lives in ANOTHER scene (may be relaxed). */
    bool confirm_on_cross_scene = true;

    /** Allow "replace the matched record" as an operator action. */
    bool allow_replace = true;

    std::unordered_map<std::string, SceneRule> scene_rules;

    float thresholdFor(const std::string& scene) const {
        auto it = scene_rules.find(scene);
        if (it != scene_rules.end() && it->second.threshold) return *it->second.threshold;
        return high_similarity;
    }

    bool replaceAllowed(const std::string& scene) const {
        auto it = scene_rules.find(scene);
        if (it != scene_rules.end() && it->second.allow_replace) return *it->second.allow_replace;
        return allow_replace;
    }

    /**
     * @param scene       scene the upload would be stored in
     * @param same_scene  the best match is already in that scene
     */
    bool needsConfirmation(const std::string& scene, bool same_scene) const {
        if (!require_confirmation) return false;

        auto it = scene_rules.find(scene);
        if (it != scene_rules.end()) {
            const SceneRule& r = it->second;
            if (r.confirm_required) return *r.confirm_required;
            if (!same_scene && r.allow_auto_cross_scene && *r.allow_auto_cross_scene) {
                return false;
            }
        }
        return same_scene ? confirm_on_same_scene : confirm_on_cross_scene;
    }
};

/** Reads a JSON policy file. Missing file = keep the defaults. */
bool loadEnrollPolicy(const std::string& path, EnrollPolicy& out, std::string& error);

/** JSON template documenting every knob (written by --print-enroll-policy). */
std::string defaultPolicyJson();

}  // namespace face_db_web

#endif  // FACE_DB_WEB_ENROLL_POLICY_HPP
