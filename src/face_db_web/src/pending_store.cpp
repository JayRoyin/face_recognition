#include "face_db_web/pending_store.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <sstream>

namespace face_db_web {
namespace {

long long nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/** Cheap unique-enough token; not a secret, just a queue handle. */
std::string makeToken() {
    static std::mt19937_64 rng(
        static_cast<unsigned long long>(nowSeconds()) ^
        reinterpret_cast<std::uintptr_t>(&rng));
    std::ostringstream oss;
    oss << std::hex << rng() << std::hex << rng();
    return oss.str();
}

}  // namespace

std::string PendingStore::add(PendingEnroll item) {
    std::lock_guard<std::mutex> lock(mutex_);
    purgeLocked();

    item.created_at = nowSeconds();
    if (item.token.empty()) item.token = makeToken();
    while (items_.count(item.token)) item.token = makeToken();

    const std::string token = item.token;
    item.token = token;
    items_[token] = std::make_shared<PendingEnroll>(std::move(item));
    order_.push_back(token);

    while (order_.size() > max_items_) {
        const std::string oldest = order_.front();
        order_.erase(order_.begin());
        items_.erase(oldest);
    }
    size_ = items_.size();
    return token;
}

std::shared_ptr<const PendingEnroll> PendingStore::peek(const std::string& token) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = items_.find(token);
    if (it == items_.end()) return nullptr;
    return it->second;  // shares the payload: no per-preview photo copy
}

std::shared_ptr<const PendingEnroll> PendingStore::findByHash(
    const std::string& image_hash) const {
    if (image_hash.empty()) return nullptr;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& token : order_) {
        auto it = items_.find(token);
        if (it == items_.end()) continue;
        if (it->second->image_hash == image_hash) return it->second;
    }
    return nullptr;
}

std::shared_ptr<const PendingEnroll> PendingStore::take(const std::string& token) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = items_.find(token);
    if (it == items_.end()) return nullptr;
    // The shared_ptr is returned, so the caller keeps the data alive even
    // though the map entry is gone (a decision must be consumed exactly once).
    auto item = it->second;
    items_.erase(it);
    order_.erase(std::find(order_.begin(), order_.end(), token));
    size_ = items_.size();
    return item;
}

std::vector<std::shared_ptr<const PendingEnroll>> PendingStore::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::shared_ptr<const PendingEnroll>> out;
    out.reserve(items_.size());
    for (const auto& token : order_) {
        auto it = items_.find(token);
        if (it != items_.end()) out.push_back(it->second);
    }
    return out;
}

void PendingStore::purgeLocked() {
    if (ttl_seconds_ <= 0) return;
    const long long cutoff = nowSeconds() - ttl_seconds_;
    for (auto it = items_.begin(); it != items_.end();) {
        if (it->second->created_at < cutoff) {
            order_.erase(std::find(order_.begin(), order_.end(), it->first));
            it = items_.erase(it);
        } else {
            ++it;
        }
    }
    size_ = items_.size();
}

}  // namespace face_db_web
