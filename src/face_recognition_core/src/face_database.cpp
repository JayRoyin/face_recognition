#include "face_recognition_core/face_database.hpp"

#include <sqlite3.h>
#include <uuid/uuid.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <system_error>
#include <unordered_map>

namespace face_recognition {

namespace {

/** Median of a copy of `v` (empty input => 0). */
float median_of(std::vector<float> v) {
    if (v.empty()) return 0.0f;
    const size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + mid, v.end());
    const float hi = v[mid];
    if (v.size() % 2) return hi;
    std::nth_element(v.begin(), v.begin() + mid - 1, v.end());
    return 0.5f * (hi + v[mid - 1]);
}

/** Median absolute deviation (robust scale estimate). */
float mad_of(const std::vector<float>& v, float med) {
    if (v.empty()) return 0.0f;
    std::vector<float> dev(v.size());
    for (size_t i = 0; i < v.size(); ++i) dev[i] = std::fabs(v[i] - med);
    return median_of(std::move(dev));
}

}  // namespace

// -----------------------------------------------------------------------------

class FaceDatabase::Impl {
public:
    sqlite3* db = nullptr;
    std::string faces_dir;

    // ---------------------------------------------------------------- schema
    bool createTable() {
        const char* faces_sql = R"(
            CREATE TABLE IF NOT EXISTS faces (
                id TEXT PRIMARY KEY,
                name TEXT NOT NULL,
                title TEXT,
                embedding BLOB,
                image_path TEXT,
                scene TEXT,
                map_location TEXT,
                created_at INTEGER,
                updated_at INTEGER
            )
        )";

        // Extra embeddings ("shots") beyond the primary faces.embedding.
        const char* templates_sql = R"(
            CREATE TABLE IF NOT EXISTS face_templates (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                face_id TEXT NOT NULL,
                embedding BLOB,
                image_path TEXT,
                created_at INTEGER
            )
        )";
        const char* index_sql =
            "CREATE INDEX IF NOT EXISTS idx_face_templates_face_id "
            "ON face_templates(face_id)";

        // Key/value store for gallery-wide facts. Currently only the
        // feature-space fingerprint (which preprocessing recipe produced the
        // embeddings in this file) — see feature_space.hpp.
        const char* metadata_sql = R"(
            CREATE TABLE IF NOT EXISTS metadata (
                key   TEXT PRIMARY KEY,
                value TEXT
            )
        )";

        for (const char* sql : {faces_sql, templates_sql, index_sql, metadata_sql}) {
            char* errMsg = nullptr;
            if (sqlite3_exec(db, sql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
                std::cerr << "[FaceDatabase] createTable failed: "
                          << (errMsg ? errMsg : "?") << std::endl;
                sqlite3_free(errMsg);
                return false;
            }
        }
        return true;
    }

    std::string generateId() {
        uuid_t uuid;
        uuid_generate_random(uuid);
        char id[37];
        uuid_unparse_lower(uuid, id);
        return std::string(id);
    }

    // -------------------------------------------------------------- metadata
    bool setMeta(const std::string& key, const std::string& value) {
        const char* sql =
            "INSERT INTO metadata (key, value) VALUES (?, ?) "
            "ON CONFLICT(key) DO UPDATE SET value = excluded.value";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            std::cerr << "[FaceDatabase] setMeta prepare failed: "
                      << sqlite3_errmsg(db) << std::endl;
            return false;
        }
        sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);
        const bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
        return ok;
    }

    std::string getMeta(const std::string& key) {
        const char* sql = "SELECT value FROM metadata WHERE key = ?";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return std::string();
        }
        sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
        std::string out;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* text = sqlite3_column_text(stmt, 0);
            if (text) out = reinterpret_cast<const char*>(text);
        }
        sqlite3_finalize(stmt);
        return out;
    }

    bool saveImageFile(const std::string& path, const std::vector<uint8_t>& data) {
        std::ofstream file(path, std::ios::binary);
        if (!file) return false;
        file.write(reinterpret_cast<const char*>(data.data()), data.size());
        return file.good();
    }

    /** Only delete files we own (inside faces_dir). */
    void removeOwnedFile(const std::string& path) {
        if (path.empty() || faces_dir.empty()) return;
        // Require a path SEPARATOR after the prefix. A plain string prefix match
        // also accepts a sibling directory ("/db/faces_backup/x.jpg" starts with
        // "/db/faces"), and this function deletes what it accepts.
        const bool inside =
            (path.size() > faces_dir.size() &&
             path.compare(0, faces_dir.size(), faces_dir) == 0 &&
             (faces_dir.back() == '/' || path[faces_dir.size()] == '/'));
        if (!inside) return;
        std::remove(path.c_str());
    }

    static void bindEmbedding(sqlite3_stmt* stmt, int idx,
                              const std::vector<float>& embedding) {
        if (embedding.empty()) {
            sqlite3_bind_null(stmt, idx);
        } else {
            sqlite3_bind_blob(stmt, idx, embedding.data(),
                              static_cast<int>(embedding.size() * sizeof(float)),
                              SQLITE_TRANSIENT);
        }
    }

    static std::vector<float> readEmbedding(sqlite3_stmt* stmt, int idx) {
        std::vector<float> out;
        if (sqlite3_column_type(stmt, idx) == SQLITE_NULL) return out;
        const void* data = sqlite3_column_blob(stmt, idx);
        const int bytes = sqlite3_column_bytes(stmt, idx);
        if (!data || bytes <= 0) return out;
        const size_t count = static_cast<size_t>(bytes) / sizeof(float);
        out.assign(reinterpret_cast<const float*>(data),
                   reinterpret_cast<const float*>(data) + count);
        return out;
    }

    // --------------------------------------------------------------- faces CRUD
    bool insertFace(const FaceRecord& record) {
        const char* sql =
            "INSERT INTO faces (id, name, title, embedding, image_path, scene, "
            "map_location, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            std::cerr << "[FaceDatabase] insertFace prepare failed: "
                      << sqlite3_errmsg(db) << std::endl;
            return false;
        }

        sqlite3_bind_text(stmt, 1, record.id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, record.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, record.title.c_str(), -1, SQLITE_TRANSIENT);
        bindEmbedding(stmt, 4, record.embedding);
        sqlite3_bind_text(stmt, 5, record.image_path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, record.scene.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, record.map_location.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 8, record.created_at);
        sqlite3_bind_int64(stmt, 9, record.updated_at);

        const int rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            std::cerr << "[FaceDatabase] insertFace step failed (rc=" << rc
                      << "): " << sqlite3_errmsg(db) << std::endl;
            sqlite3_finalize(stmt);
            return false;
        }
        sqlite3_finalize(stmt);
        return true;
    }

    bool deleteFaceById(const std::string& face_id) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, "DELETE FROM faces WHERE id = ?", -1, &stmt,
                               nullptr) != SQLITE_OK) {
            return false;
        }
        sqlite3_bind_text(stmt, 1, face_id.c_str(), -1, SQLITE_TRANSIENT);
        const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
        return ok;
    }

    std::vector<FaceRecord> getAllFaces() {
        std::vector<FaceRecord> records;
        const char* sql =
            "SELECT id, name, title, embedding, image_path, scene, map_location, "
            "created_at, updated_at FROM faces";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return records;
        }

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            FaceRecord r;
            const char* s = nullptr;
            s = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)); r.id   = s ? s : "";
            s = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)); r.name = s ? s : "";
            s = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)); r.title = s ? s : "";
            r.embedding  = readEmbedding(stmt, 3);
            s = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4)); r.image_path   = s ? s : "";
            s = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5)); r.scene        = s ? s : "";
            s = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6)); r.map_location = s ? s : "";
            r.created_at = sqlite3_column_int64(stmt, 7);
            r.updated_at = sqlite3_column_int64(stmt, 8);
            records.push_back(std::move(r));
        }
        sqlite3_finalize(stmt);
        return records;
    }

    std::string getImagePath(const std::string& face_id) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT image_path FROM faces WHERE id = ?", -1,
                               &stmt, nullptr) != SQLITE_OK) {
            return "";
        }
        sqlite3_bind_text(stmt, 1, face_id.c_str(), -1, SQLITE_TRANSIENT);
        std::string path;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            path = p ? p : "";
        }
        sqlite3_finalize(stmt);
        return path;
    }

    // ------------------------------------------------------------ templates
    bool insertTemplate(const FaceTemplate& t) {
        const char* sql =
            "INSERT INTO face_templates (face_id, embedding, image_path, created_at) "
            "VALUES (?, ?, ?, ?)";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return false;
        }
        sqlite3_bind_text(stmt, 1, t.face_id.c_str(), -1, SQLITE_TRANSIENT);
        bindEmbedding(stmt, 2, t.embedding);
        sqlite3_bind_text(stmt, 3, t.image_path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 4, t.created_at);
        const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
        return ok;
    }

    /** @param face_id  empty => every identity. */
    std::vector<FaceTemplate> getTemplates(const std::string& face_id) {
        std::vector<FaceTemplate> out;
        const char* sql_all =
            "SELECT id, face_id, embedding, image_path, created_at FROM face_templates";
        const char* sql_one =
            "SELECT id, face_id, embedding, image_path, created_at FROM face_templates "
            "WHERE face_id = ?";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, face_id.empty() ? sql_all : sql_one, -1, &stmt,
                               nullptr) != SQLITE_OK) {
            return out;
        }
        if (!face_id.empty()) {
            sqlite3_bind_text(stmt, 1, face_id.c_str(), -1, SQLITE_TRANSIENT);
        }
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            FaceTemplate t;
            t.id = sqlite3_column_int64(stmt, 0);
            const char* s = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            t.face_id    = s ? s : "";
            t.embedding  = readEmbedding(stmt, 2);
            s = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            t.image_path = s ? s : "";
            t.created_at = sqlite3_column_int64(stmt, 4);
            out.push_back(std::move(t));
        }
        sqlite3_finalize(stmt);
        return out;
    }

    int countTemplates(const std::string& face_id) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
                "SELECT COUNT(*) FROM face_templates WHERE face_id = ?", -1,
                &stmt, nullptr) != SQLITE_OK) {
            return 0;
        }
        sqlite3_bind_text(stmt, 1, face_id.c_str(), -1, SQLITE_TRANSIENT);
        int n = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        return n;
    }

    bool deleteTemplates(const std::string& face_id, bool delete_files) {
        if (delete_files) {
            for (const auto& t : getTemplates(face_id)) removeOwnedFile(t.image_path);
        }
        sqlite3_stmt* stmt = nullptr;
        const char* sql = face_id.empty() ? "DELETE FROM face_templates"
                                          : "DELETE FROM face_templates WHERE face_id = ?";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return false;
        }
        if (!face_id.empty()) {
            sqlite3_bind_text(stmt, 1, face_id.c_str(), -1, SQLITE_TRANSIENT);
        }
        const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
        return ok;
    }
};

// =============================================================================

FaceDatabase::FaceDatabase() : pImpl(std::make_unique<Impl>()) {}

FaceDatabase::~FaceDatabase() {
    if (pImpl->db) {
        sqlite3_close(pImpl->db);
        pImpl->db = nullptr;
    }
}

bool FaceDatabase::initialize(const std::string& db_path, const std::string& faces_dir) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    pImpl->faces_dir = faces_dir;

    // Re-initialising the same object must not leak the previous connection.
    if (pImpl->db) {
        sqlite3_close(pImpl->db);
        pImpl->db = nullptr;
    }

    std::error_code ec;
    std::filesystem::create_directories(faces_dir, ec);
    if (ec) return false;
    auto parent = std::filesystem::path(db_path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) return false;
    }

    if (sqlite3_open(db_path.c_str(), &pImpl->db) != SQLITE_OK) {
        std::cerr << "Cannot open database: " << sqlite3_errmsg(pImpl->db) << std::endl;
        // sqlite3_open allocates a handle even when it fails. Leaving it both
        // leaks the connection (and its file lock) and keeps pImpl->db non-null,
        // so later calls would run against a half-open database.
        sqlite3_close(pImpl->db);
        pImpl->db = nullptr;
        return false;
    }

    // One database file is shared by several front-ends (CLI, web UI, ROS nodes).
    // Without a busy timeout a concurrent writer fails immediately with
    // SQLITE_BUSY, which the callers only see as an unexplained "add failed";
    // WAL additionally lets readers proceed while a writer holds the lock.
    sqlite3_busy_timeout(pImpl->db, 5000);
    char* pragma_err = nullptr;
    if (sqlite3_exec(pImpl->db, "PRAGMA journal_mode=WAL;", nullptr, nullptr,
                     &pragma_err) != SQLITE_OK) {
        // Non-fatal: some filesystems (network mounts) refuse WAL. The database
        // still works, just with the default rollback journal.
        std::cerr << "[FaceDatabase] PRAGMA journal_mode=WAL failed: "
                  << (pragma_err ? pragma_err : "?") << std::endl;
    }
    sqlite3_free(pragma_err);

    return pImpl->createTable();
}

std::string FaceDatabase::add_face(const std::string& name,
                                  const std::vector<float>& embedding,
                                  const std::vector<uint8_t>& image_data,
                                  const std::string& title,
                                  const std::string& scene,
                                  const std::string& map_location) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (name.empty()) return "";

    FaceRecord record;
    record.id = pImpl->generateId();
    record.name = name;
    record.title = title;
    record.embedding = embedding;
    record.scene = scene;
    record.map_location = map_location;
    record.created_at = time(nullptr);
    record.updated_at = time(nullptr);

    if (!image_data.empty()) {
        std::string path = pImpl->faces_dir + "/" + record.id + ".jpg";
        if (pImpl->saveImageFile(path, image_data)) {
            record.image_path = path;
        }
    }

    if (pImpl->insertFace(record)) {
        return record.id;
    }

    // The image is written before the row. If the insert failed (busy database,
    // constraint, disk full) the file would stay behind with nothing referencing
    // it — an orphan that no later cleanup path ever looks at, because it is only
    // discoverable through the row that does not exist.
    pImpl->removeOwnedFile(record.image_path);
    return "";
}

bool FaceDatabase::remove_face(const std::string& face_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const std::string image_path = pImpl->getImagePath(face_id);
    pImpl->removeOwnedFile(image_path);
    pImpl->deleteTemplates(face_id, /*delete_files=*/true);
    return pImpl->deleteFaceById(face_id);
}

std::shared_ptr<FaceRecord> FaceDatabase::get_face(const std::string& face_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (auto& r : pImpl->getAllFaces()) {
        if (r.id == face_id) return std::make_shared<FaceRecord>(r);
    }
    return nullptr;
}

std::vector<FaceRecord> FaceDatabase::list_faces() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return pImpl->getAllFaces();
}

int FaceDatabase::clear_all() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto all = pImpl->getAllFaces();
    for (auto& r : all) {
        pImpl->removeOwnedFile(r.image_path);
        pImpl->deleteFaceById(r.id);
    }
    pImpl->deleteTemplates("", /*delete_files=*/true);
    return static_cast<int>(all.size());
}

int FaceDatabase::get_face_count() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return static_cast<int>(pImpl->getAllFaces().size());
}

// ---------------------------------------------------------------- templates

bool FaceDatabase::add_template(const std::string& face_id,
                                const std::vector<float>& embedding,
                                const std::string& image_path) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (face_id.empty() || embedding.empty()) return false;
    FaceTemplate t;
    t.face_id = face_id;
    t.embedding = embedding;
    t.image_path = image_path;
    t.created_at = time(nullptr);
    return pImpl->insertTemplate(t);
}

std::vector<FaceTemplate> FaceDatabase::list_templates(const std::string& face_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return pImpl->getTemplates(face_id);
}

int FaceDatabase::template_count(const std::string& face_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return pImpl->countTemplates(face_id);
}

bool FaceDatabase::remove_templates(const std::string& face_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return pImpl->deleteTemplates(face_id, /*delete_files=*/true);
}

// ---------------------------------------------------------------- matching

std::vector<MatchCandidate> FaceDatabase::rank_faces(const std::vector<float>& embedding) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<MatchCandidate> out;
    if (embedding.empty()) return out;

    auto faces = pImpl->getAllFaces();

    // identity metadata + primary template
    std::unordered_map<std::string, MatchCandidate> best;
    std::unordered_map<std::string, int> counts;
    for (const auto& f : faces) {
        MatchCandidate c;
        c.face_id = f.id;
        c.name    = f.name;
        c.title   = f.title;
        c.template_id = -1;
        if (!f.embedding.empty()) {
            c.similarity = compute_similarity(embedding, f.embedding);
            counts[f.id] += 1;
        }
        // keep entry even without a primary embedding: extra templates may exist
        best[f.id] = c;
    }

    for (const auto& t : pImpl->getTemplates("")) {
        if (t.embedding.empty()) continue;
        auto it = best.find(t.face_id);
        if (it == best.end()) continue;  // orphan template (identity deleted)
        const float sim = compute_similarity(embedding, t.embedding);
        counts[t.face_id] += 1;
        if (sim > it->second.similarity) {
            it->second.similarity  = sim;
            it->second.template_id = t.id;
        }
    }

    for (auto& kv : best) {
        kv.second.template_count = counts[kv.first];
        if (kv.second.template_count == 0) continue;  // nothing to match against
        out.push_back(kv.second);
    }

    std::sort(out.begin(), out.end(),
              [](const MatchCandidate& a, const MatchCandidate& b) {
                  return a.similarity > b.similarity;
              });
    return out;
}

MatchResult FaceDatabase::match_face(const std::vector<float>& embedding,
                                     float raw_threshold,
                                     float z_threshold,
                                     int   min_cohort,
                                     bool  normalize) {
    MatchResult res;
    auto ranked = rank_faces(embedding);
    if (ranked.empty()) {
        res.reason = "gallery empty (no usable templates)";
        return res;
    }

    const MatchCandidate& top = ranked.front();
    res.face_id        = top.face_id;
    res.name           = top.name;
    res.title          = top.title;
    res.raw_similarity = top.similarity;

    // Cohort = how well this query matches UNRELATED identities. Any template
    // of another identity, plus the externally supplied impostor cohort, is a
    // valid sample. A "hub" template that matches everyone inflates this
    // distribution, which is exactly what we want to penalise.
    std::vector<float> cohort;
    for (size_t i = 1; i < ranked.size(); ++i) {
        // one sample per other identity (its best template)
        cohort.push_back(ranked[i].similarity);
    }
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        for (const auto& e : cohort_embeddings_) {
            if (e.empty()) continue;
            cohort.push_back(compute_similarity(embedding, e));
        }
    }

    res.cohort_size = static_cast<int>(cohort.size());
    const bool can_normalize = normalize && res.cohort_size >= min_cohort;
    if (can_normalize) {
        res.cohort_median = median_of(cohort);
        res.cohort_mad    = mad_of(cohort, res.cohort_median);
        // 1.4826 * MAD ~= sigma for a normal distribution.
        const float sigma = std::max(1e-4f, 1.4826f * res.cohort_mad);
        res.z_score  = (res.raw_similarity - res.cohort_median) / sigma;
        res.normalized = true;
    }

    if (res.raw_similarity < raw_threshold) {
        res.accepted = false;
        res.reason   = "below raw threshold";
        return res;
    }
    if (can_normalize && res.z_score < z_threshold) {
        res.accepted = false;
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "below cohort z-score (%.2f < %.2f, median=%.3f mad=%.3f n=%d)",
                      res.z_score, z_threshold, res.cohort_median, res.cohort_mad,
                      res.cohort_size);
        res.reason = buf;
        return res;
    }

    res.accepted = true;
    res.reason   = can_normalize ? "accepted (raw + cohort)" : "accepted (raw only)";
    return res;
}

std::shared_ptr<FaceRecord> FaceDatabase::find_matching_face(
        const std::vector<float>& embedding, float threshold) {
    float z = 3.0f;
    int   mc = 3;
    bool  norm = true;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        z = z_threshold_;
        mc = min_cohort_;
        norm = normalize_;
    }
    const MatchResult r = match_face(embedding, threshold, z, mc, norm);
    if (!r.accepted) return nullptr;
    return get_face(r.face_id);
}

// -------------------------------------------------------------- cohort setup

void FaceDatabase::setCohortEmbeddings(std::vector<std::vector<float>> cohort) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    cohort_embeddings_ = std::move(cohort);
}

void FaceDatabase::setNormalizationDefaults(float z_threshold, int min_cohort,
                                            bool enabled) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    z_threshold_ = z_threshold;
    min_cohort_  = min_cohort;
    normalize_   = enabled;
}

int FaceDatabase::cohortSize() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return static_cast<int>(cohort_embeddings_.size());
}

// -------------------------------------------------------------- images / emb

bool FaceDatabase::save_image(const std::string& face_id,
                             const std::vector<uint8_t>& data,
                             std::string& out_path) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    out_path = pImpl->faces_dir + "/" + face_id + ".jpg";
    return pImpl->saveImageFile(out_path, data);
}

bool FaceDatabase::update_embedding(const std::string& face_id,
                                    const std::vector<float>& embedding) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (face_id.empty() || embedding.empty() || !pImpl->db) return false;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE faces SET embedding = ?, updated_at = ? WHERE id = ?";
    if (sqlite3_prepare_v2(pImpl->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_blob(stmt, 1, embedding.data(),
                      static_cast<int>(embedding.size() * sizeof(float)),
                      SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, time(nullptr));
    sqlite3_bind_text(stmt, 3, face_id.c_str(), -1, SQLITE_TRANSIENT);
    const bool executed = (sqlite3_step(stmt) == SQLITE_DONE);
    const int  changed  = executed ? sqlite3_changes(pImpl->db) : 0;
    const std::string dbmsg = sqlite3_errmsg(pImpl->db);
    sqlite3_finalize(stmt);

    if (!executed) {
        std::cerr << "[FaceDatabase] update_embedding failed for id=" << face_id
                  << ": " << dbmsg << std::endl;
        return false;
    }
    if (changed == 0) {
        // A successful statement that matched no row means the id does not exist.
        // Reporting it as a plain failure made the caller's log ("failed to
        // update embedding") indistinguishable from a real execution error.
        std::cerr << "[FaceDatabase] update_embedding: no face with id=" << face_id
                  << " (row not found)" << std::endl;
        return false;
    }
    return true;
}

bool FaceDatabase::set_meta(const std::string& key, const std::string& value) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return pImpl->setMeta(key, value);
}

std::string FaceDatabase::get_meta(const std::string& key) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return pImpl->getMeta(key);
}

}  // namespace face_recognition
