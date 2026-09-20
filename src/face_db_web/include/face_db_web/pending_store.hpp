#pragma once

#ifndef FACE_DB_WEB_PENDING_STORE_HPP
#define FACE_DB_WEB_PENDING_STORE_HPP

/**
 * Staging area for enrolments that a human has to decide on.
 *
 * When an upload is highly similar to someone already stored it must NOT be
 * written straight away. The image, the extracted embedding and everything
 * needed to write the row later are parked here under a token; the row is only
 * created when the operator resolves it via /api/faces/resolve.
 *
 * Kept in memory on purpose: a pending item is a decision, not data — if the
 * process restarts the operator simply re-imports. That also avoids leaving
 * half-written rows in the shared SQLite file.
 */

#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace face_db_web {

struct PendingEnroll {
    std::string token;
    std::string file;
    std::string name;
    std::string title;
    std::string scene;
    std::string map_location;
    std::string gender;

    std::vector<uint8_t> image;      // original bytes, re-served for the preview
    std::string          image_hash;
    std::vector<float>   embedding;

    std::string match_id;            // the gallery record it resembles
    std::string match_name;
    std::string match_scene;
    float       similarity  = 0.0f;
    bool        same_scene  = false;
    std::string reason;

    long long created_at = 0;        // unix seconds
};

class PendingStore {
public:
    /** Oldest items are dropped past `max_items`. */
    explicit PendingStore(std::size_t max_items = 500, long long ttl_seconds = 1800)
        : max_items_(max_items), ttl_seconds_(ttl_seconds) {}

    std::string add(PendingEnroll item);
    std::shared_ptr<PendingEnroll> peek(const std::string& token) const;
    /** Returns and REMOVES the item (a decision is consumed exactly once). */
    std::shared_ptr<PendingEnroll> take(const std::string& token);
    std::vector<PendingEnroll> list() const;
    std::size_t size() const { return size_; }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        items_.clear();
        order_.clear();
        size_ = 0;
    }

private:
    void purgeLocked();

    mutable std::mutex mutex_;
    std::unordered_map<std::string, PendingEnroll> items_;
    std::vector<std::string> order_;  // insertion order, for eviction
    std::size_t max_items_;
    long long   ttl_seconds_;
    std::size_t size_ = 0;
};

}  // namespace face_db_web

#endif  // FACE_DB_WEB_PENDING_STORE_HPP
