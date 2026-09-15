#include "face_recognition_core/face_database.hpp"
#include <sqlite3.h>
#include <uuid/uuid.h>
#include <fstream>
#include <algorithm>
#include <sys/stat.h>
#include <sys/types.h>
#include <cstdio>
#include <iostream>
#include <filesystem>

namespace face_recognition {

class FaceDatabase::Impl {
public:
    sqlite3* db = nullptr;
    std::string faces_dir;

    bool createTable() {
        const char* sql = R"(
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

        char* errMsg = nullptr;
        int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
        if (rc != SQLITE_OK) {
            std::cerr << "[FaceDatabase] createTable failed: " << errMsg << std::endl;
            sqlite3_free(errMsg);
            return false;
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

    bool saveImageFile(const std::string& path, const std::vector<uint8_t>& data) {
        std::ofstream file(path, std::ios::binary);
        if (!file) return false;
        file.write(reinterpret_cast<const char*>(data.data()), data.size());
        return file.good();
    }

    bool insertFace(const FaceRecord& record) {
        std::string sql = "INSERT INTO faces (id, name, title, embedding, image_path, scene, map_location, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)";
        sqlite3_stmt* stmt = nullptr;

        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            std::cerr << "[FaceDatabase] insertFace prepare failed: "
                      << sqlite3_errmsg(db) << std::endl;
            return false;
        }

        sqlite3_bind_text(stmt, 1, record.id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, record.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, record.title.c_str(), -1, SQLITE_TRANSIENT);
        if (record.embedding.empty()) {
            sqlite3_bind_null(stmt, 4);
        } else {
            sqlite3_bind_blob(stmt, 4, record.embedding.data(),
                              static_cast<int>(record.embedding.size() * sizeof(float)),
                              SQLITE_TRANSIENT);
        }
        sqlite3_bind_text(stmt, 5, record.image_path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, record.scene.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, record.map_location.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 8, record.created_at);
        sqlite3_bind_int64(stmt, 9, record.updated_at);

        int step_rc = sqlite3_step(stmt);
        if (step_rc != SQLITE_DONE) {
            std::cerr << "[FaceDatabase] insertFace step failed (rc=" << step_rc
                      << "): " << sqlite3_errmsg(db) << std::endl;
            sqlite3_finalize(stmt);
            return false;
        }
        sqlite3_finalize(stmt);
        return true;
    }

    bool deleteFaceById(const std::string& face_id) {
        std::string sql = "DELETE FROM faces WHERE id = ?";
        sqlite3_stmt* stmt = nullptr;

        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            return false;
        }

        sqlite3_bind_text(stmt, 1, face_id.c_str(), -1, SQLITE_TRANSIENT);
        bool success = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
        return success;
    }

    std::vector<FaceRecord> getAllFaces() {
        std::vector<FaceRecord> records;
        const char* sql = "SELECT id, name, title, embedding, image_path, scene, map_location, created_at, updated_at FROM faces";
        sqlite3_stmt* stmt = nullptr;

        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return records;
        }

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            FaceRecord record;

            const char* id_str = (const char*)sqlite3_column_text(stmt, 0);
            record.id = id_str ? id_str : "";

            const char* name_str = (const char*)sqlite3_column_text(stmt, 1);
            record.name = name_str ? name_str : "";

            const char* title_str = (const char*)sqlite3_column_text(stmt, 2);
            record.title = title_str ? title_str : "";

            const void* emb_data = sqlite3_column_blob(stmt, 3);
            int emb_size = sqlite3_column_bytes(stmt, 3);
            if (sqlite3_column_type(stmt, 3) != SQLITE_NULL && emb_data && emb_size > 0) {
                size_t count = emb_size / sizeof(float);
                record.embedding.assign((const float*)emb_data, (const float*)emb_data + count);
            }

            const char* img_str = (const char*)sqlite3_column_text(stmt, 4);
            record.image_path = img_str ? img_str : "";

            const char* scene_str = (const char*)sqlite3_column_text(stmt, 5);
            record.scene = scene_str ? scene_str : "";

            const char* map_str = (const char*)sqlite3_column_text(stmt, 6);
            record.map_location = map_str ? map_str : "";

            record.created_at = sqlite3_column_int64(stmt, 7);
            record.updated_at = sqlite3_column_int64(stmt, 8);

            records.push_back(record);
        }

        sqlite3_finalize(stmt);
        return records;
    }

    std::string getImagePath(const std::string& face_id) {
        std::string sql = "SELECT image_path FROM faces WHERE id = ?";
        sqlite3_stmt* stmt = nullptr;

        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            return "";
        }

        sqlite3_bind_text(stmt, 1, face_id.c_str(), -1, SQLITE_TRANSIENT);

        std::string path;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* path_str = (const char*)sqlite3_column_text(stmt, 0);
            path = path_str ? path_str : "";
        }

        sqlite3_finalize(stmt);
        return path;
    }
};

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
        return false;
    }

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
    return "";
}

bool FaceDatabase::remove_face(const std::string& face_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::string image_path = pImpl->getImagePath(face_id);

    if (!image_path.empty()) {
        std::remove(image_path.c_str());
    }

    return pImpl->deleteFaceById(face_id);
}

std::shared_ptr<FaceRecord> FaceDatabase::get_face(const std::string& face_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto all = pImpl->getAllFaces();
    for (auto& r : all) {
        if (r.id == face_id) {
            return std::make_shared<FaceRecord>(r);
        }
    }
    return nullptr;
}

std::vector<FaceRecord> FaceDatabase::list_faces() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return pImpl->getAllFaces();
}

std::shared_ptr<FaceRecord> FaceDatabase::find_matching_face(const std::vector<float>& embedding, float threshold) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto all = pImpl->getAllFaces();
    float best_sim = threshold;
    std::shared_ptr<FaceRecord> best_match = nullptr;

    for (auto& record : all) {
        float sim = compute_similarity(embedding, record.embedding);
        if (sim > best_sim) {
            best_sim = sim;
            best_match = std::make_shared<FaceRecord>(record);
        }
    }

    return best_match;
}

int FaceDatabase::clear_all() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto all = pImpl->getAllFaces();
    for (auto& r : all) {
        if (!r.image_path.empty()) {
            std::remove(r.image_path.c_str());
        }
        pImpl->deleteFaceById(r.id);
    }
    return static_cast<int>(all.size());
}

int FaceDatabase::get_face_count() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return static_cast<int>(pImpl->getAllFaces().size());
}

bool FaceDatabase::save_image(const std::string& face_id, const std::vector<uint8_t>& data, std::string& out_path) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    out_path = pImpl->faces_dir + "/" + face_id + ".jpg";
    return pImpl->saveImageFile(out_path, data);
}

bool FaceDatabase::update_embedding(const std::string& face_id, const std::vector<float>& embedding) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (face_id.empty() || embedding.empty() || !pImpl->db) return false;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE faces SET embedding = ?, updated_at = ? WHERE id = ?";
    if (sqlite3_prepare_v2(pImpl->db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_blob(stmt, 1, embedding.data(), static_cast<int>(embedding.size() * sizeof(float)), SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, time(nullptr));
    sqlite3_bind_text(stmt, 3, face_id.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(pImpl->db) == 1;
    sqlite3_finalize(stmt);
    return ok;
}

}  // namespace face_recognition
