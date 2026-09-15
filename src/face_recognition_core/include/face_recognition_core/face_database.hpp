#pragma once

#ifndef FACE_RECOGNITION_CORE_FACE_DATABASE_HPP
#define FACE_RECOGNITION_CORE_FACE_DATABASE_HPP

#include "types.hpp"
#include <string>
#include <vector>
#include <memory>
#include <mutex>

namespace face_recognition {

class FaceDatabase {
public:
    FaceDatabase();
    ~FaceDatabase();

    bool initialize(const std::string& db_path, const std::string& faces_dir);
    std::string add_face(const std::string& name,
                        const std::vector<float>& embedding,
                        const std::vector<uint8_t>& image_data = {},
                        const std::string& title = "",
                        const std::string& scene = "default",
                        const std::string& map_location = "unknown");
    bool remove_face(const std::string& face_id);
    std::shared_ptr<FaceRecord> get_face(const std::string& face_id);
    std::vector<FaceRecord> list_faces();
    std::shared_ptr<FaceRecord> find_matching_face(const std::vector<float>& embedding, float threshold = 0.7f);
    int clear_all();
    int get_face_count();
    bool save_image(const std::string& face_id, const std::vector<uint8_t>& data, std::string& out_path);
    bool update_embedding(const std::string& face_id, const std::vector<float>& embedding);

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
    mutable std::recursive_mutex mutex_;
};

}  // namespace face_recognition

#endif
