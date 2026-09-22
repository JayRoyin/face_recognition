#include "face_db_web/face_handler.hpp"

#include "face_db_web/archive_reader.hpp"
#include "face_db_web/json.hpp"
#include "face_db_web/multipart.hpp"
#include "face_db_web/index_html.hpp"

#include "face_recognition_core/embedding_policy.hpp"
#include "face_recognition_core/sha256.hpp"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <cctype>
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cstring>
#include <filesystem>

namespace face_db_web {

static std::string jsonEscape(const std::string& value) {
    std::string out;
    for (char c : value) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out += c;
    }
    return out;
}

namespace {

/** Directory holding this executable ("" when undeterminable). */
std::string exeDir() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return std::string();
    buf[n] = '\0';
    return std::filesystem::path(buf).parent_path().string();
}

/** One row of the per-image report returned by both bulk endpoints. */
struct ItemReport {
    std::string file;
    std::string name;
    std::string title;
    bool        ok            = false;
    bool        has_embedding = false;
    bool        duplicate     = false;
    bool        needs_confirm = false;
    std::string id;
    std::string reason;        // why it failed / was skipped / is parked
    std::string existing_name; // the record it duplicates
    std::string similar_name;  // same person, different photo (hint only)
    std::string token;         // pending handle, when needs_confirm
    std::string match_id;
    std::string match_scene;
    bool        same_scene   = false;
    float       similarity   = 0.0f;
    std::string gender;        // effective gender (after auto-fill)
};

void appendReport(std::ostringstream& json, const ItemReport& r) {
    json << "{\"file\":\"" << jsonEscape(r.file) << "\""
         << ",\"name\":\"" << jsonEscape(r.name) << "\""
         << ",\"title\":\"" << jsonEscape(r.title) << "\""
         << ",\"success\":" << (r.ok ? "true" : "false")
         << ",\"id\":\"" << jsonEscape(r.id) << "\""
         << ",\"has_embedding\":" << (r.has_embedding ? "true" : "false")
         << ",\"duplicate\":" << (r.duplicate ? "true" : "false")
         << ",\"needs_confirm\":" << (r.needs_confirm ? "true" : "false")
         << ",\"token\":\"" << jsonEscape(r.token) << "\""
         << ",\"existing_name\":\"" << jsonEscape(r.existing_name) << "\""
         << ",\"similar_name\":\"" << jsonEscape(r.similar_name) << "\""
         << ",\"match_id\":\"" << jsonEscape(r.match_id) << "\""
         << ",\"match_scene\":\"" << jsonEscape(r.match_scene) << "\""
         << ",\"same_scene\":" << (r.same_scene ? "true" : "false")
         << ",\"gender\":\"" << jsonEscape(r.gender) << "\""
         << ",\"similarity\":" << (static_cast<int>(r.similarity * 1000) / 1000.0)
         << ",\"reason\":\"" << jsonEscape(r.reason) << "\"}";
}

/**
 * Content-Type derived from the payload's MAGIC BYTES.
 *
 * The gallery stores every image as `<faces_dir>/<id>.jpg` (that is the
 * documented naming convention, see database_schema.md), but the bytes inside
 * are stored verbatim — a PNG upload stays a PNG so that `image_hash` keeps
 * identifying the exact photo that was imported. The extension therefore says
 * nothing about the format, and the response header must come from the
 * content itself.
 *
 * Shared by /api/image/<id> and /api/pending/image/<token> so both endpoints
 * can never disagree again.
 */
std::string sniffImageContentType(const void* data, std::size_t size) {
    if (data == nullptr || size == 0) return "application/octet-stream";
    const auto* b = static_cast<const unsigned char*>(data);

    if (size >= 8 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G' &&
        b[4] == 0x0D && b[5] == 0x0A && b[6] == 0x1A && b[7] == 0x0A) {
        return "image/png";
    }
    if (size >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return "image/jpeg";
    if (size >= 12 && std::memcmp(b, "RIFF", 4) == 0 &&
        std::memcmp(b + 8, "WEBP", 4) == 0) {
        return "image/webp";
    }
    if (size >= 2 && b[0] == 'B' && b[1] == 'M') return "image/bmp";
    if (size >= 6 && (std::memcmp(b, "GIF87a", 6) == 0 ||
                      std::memcmp(b, "GIF89a", 6) == 0)) {
        return "image/gif";
    }
    // Unknown: be honest rather than claiming JPEG. Browsers still render it
    // inside <img> (we do not send X-Content-Type-Options: nosniff).
    return "application/octet-stream";
}

/**
 * RAII restore of the detector's confidence threshold.
 *
 * The threshold lives on the (shared) detector, so a rescue pass that lowered
 * it must put it back — including when detection throws.
 */
class ScopedThresholdGuard {
public:
    ScopedThresholdGuard(face_recognition::FaceDetector& detector, float temporary)
        : detector_(&detector), saved_(detector.confidenceThreshold()) {
        detector_->setConfidenceThreshold(temporary);
    }
    ~ScopedThresholdGuard() { detector_->setConfidenceThreshold(saved_); }

    ScopedThresholdGuard(const ScopedThresholdGuard&) = delete;
    ScopedThresholdGuard& operator=(const ScopedThresholdGuard&) = delete;

private:
    face_recognition::FaceDetector* detector_;
    float saved_;
};

/** Cosine threshold from a request field; falls back on junk input. */
float parseThreshold(const std::string& raw, float fallback) {
    if (raw.empty()) return fallback;
    try {
        const float v = std::stof(raw);
        return (v > 0.0f && v < 1.0f) ? v : fallback;
    } catch (const std::exception&) {
        return fallback;
    }
}

/** "unknown" unless the caller sent a recognised value. */
std::string normaliseGender(const std::string& raw) {
    const std::string v = [&] {
        std::string s;
        for (char c : raw) s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        return s;
    }();
    if (v == "male" || v == "m" || v == "男" || v == "男性") return "male";
    if (v == "female" || v == "f" || v == "女" || v == "女性") return "female";
    return "unknown";
}

/** Strip a "data:image/png;base64," prefix if the client left it in. */
std::string stripDataUrlPrefix(const std::string& value) {
    if (value.compare(0, 5, "data:") != 0) return value;
    const size_t comma = value.find(',');
    return (comma == std::string::npos) ? value : value.substr(comma + 1);
}

}  // namespace

FaceHandler::FaceHandler(std::shared_ptr<face_recognition::FaceDatabase> database,
                         std::shared_ptr<face_recognition::FaceDetector>   detector,
                         std::shared_ptr<face_recognition::FaceRecognizer> recognizer,
                         bool require_embedding,
                         EnrollPolicy policy,
                         std::shared_ptr<face_recognition::GenderClassifier> gender)
    : database_(std::move(database)),
      detector_(std::move(detector)),
      recognizer_(std::move(recognizer)),
      require_embedding_(require_embedding),
      policy_(std::move(policy)),
      gender_(std::move(gender)) {

    if (const char* env_p = std::getenv("FACE_DB_WEB_TEMPLATES")) {
        templates_dir_ = env_p;
    } else {
        templates_dir_ = "./templates";
    }
}

std::string FaceHandler::urlDecode(const std::string& str) {
    std::string decoded;
    decoded.reserve(str.size());
    for (size_t i = 0; i < str.size(); ++i) {
        if (str[i] == '%' && i + 2 < str.size()) {
            int hex = 0;
            std::istringstream iss(std::string(str.begin() + i + 1, str.begin() + i + 3));
            iss >> std::hex >> hex;
            decoded.push_back(static_cast<char>(hex));
            i += 2;
        } else if (str[i] == '+') {
            decoded.push_back(' ');
        } else {
            decoded.push_back(str[i]);
        }
    }
    return decoded;
}

std::string FaceHandler::base64Decode(const std::string& encoded) {
    std::vector<uint8_t> decoded;
    const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string input = encoded;
    size_t padding = (4 - input.length() % 4) % 4;
    input.append(padding, '=');

    for (size_t i = 0; i < input.length(); i += 4) {
        int val[4];
        for (int j = 0; j < 4; j++) {
            char c = input[i + j];
            if (c == '=') {
                val[j] = 0;
            } else {
                const char* p = std::strchr(chars, c);
                val[j] = p ? static_cast<int>(p - chars) : 0;
            }
        }

        decoded.push_back(static_cast<uint8_t>((val[0] << 2) | (val[1] >> 4)));
        if (input[i + 2] != '=') {
            decoded.push_back(static_cast<uint8_t>((val[1] << 4) | (val[2] >> 2)));
        }
        if (input[i + 3] != '=') {
            decoded.push_back(static_cast<uint8_t>((val[2] << 6) | val[3]));
        }
    }

    return std::string(decoded.begin(), decoded.end());
}

std::string FaceHandler::base64Encode(const std::vector<uint8_t>& data) {
    const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve((data.size() + 2) / 3 * 4);

    size_t i = 0;
    while (i + 2 < data.size()) {
        encoded.push_back(chars[(data[i] >> 2) & 0x3F]);
        encoded.push_back(chars[((data[i] << 4) | (data[i+1] >> 4)) & 0x3F]);
        encoded.push_back(chars[((data[i+1] << 2) | (data[i+2] >> 6)) & 0x3F]);
        encoded.push_back(chars[data[i+2] & 0x3F]);
        i += 3;
    }
    if (i < data.size()) {
        encoded.push_back(chars[(data[i] >> 2) & 0x3F]);
        if (i + 1 < data.size()) {
            encoded.push_back(chars[((data[i] << 4) | (data[i+1] >> 4)) & 0x3F]);
            encoded.push_back(chars[(data[i+1] << 2) & 0x3F]);
            encoded.push_back('=');
        } else {
            encoded.push_back(chars[(data[i] << 4) & 0x3F]);
            encoded.append("==");
        }
    }

    return encoded;
}

HttpResponse FaceHandler::index(const HttpRequest& req) {
    HttpResponse resp;
    resp.body = renderIndex();
    resp.content_type = "text/html; charset=utf-8";
    return resp;
}

HttpResponse FaceHandler::listFaces(const HttpRequest& req) {
    HttpResponse resp;
    auto faces = database_->list_faces();

    std::ostringstream json;
    json << "{\"count\":" << faces.size() << ",\"faces\":[";
    bool first = true;
    for (const auto& f : faces) {
        if (!first) json << ",";
        first = false;
        // `uid` is intentionally NOT serialised: it is the database's internal
        // sort key / record number, not part of the public API.
        json << "{"
             << "\"id\":\"" << f.id << "\","
             << "\"name\":\"" << jsonEscape(f.name) << "\","
             << "\"title\":\"" << jsonEscape(f.title) << "\","
             << "\"gender\":\"" << jsonEscape(f.gender) << "\","
             << "\"scene\":\"" << jsonEscape(f.scene) << "\","
             << "\"map_location\":\"" << jsonEscape(f.map_location) << "\","
             << "\"image_path\":\"" << jsonEscape(f.image_path) << "\""
             << "}";
    }
    json << "]}";

    resp.body = json.str();
    resp.content_type = "application/json; charset=utf-8";
    return resp;
}

HttpResponse FaceHandler::addFace(const HttpRequest& req) {
    HttpResponse resp;

    std::string name, title, image_data_str, scene, map_location, gender;

    // form-urlencoded: pairs separated by '&'. Also tolerate CRLF as a
    // separator (some browsers insert it).
    auto consume = [&](std::string& body, std::string& field) {
        // Find next separator ('&' or '\n'). CRLF case is handled by skipping
        // an optional '\r' after '\n'.
        size_t sep = std::string::npos;
        for (size_t i = 0; i < body.size(); ++i) {
            if (body[i] == '&' || body[i] == '\n') { sep = i; break; }
        }
        if (sep == std::string::npos) {
            field = body;
            body.clear();
        } else {
            field = body.substr(0, sep);
            body.erase(0, sep + 1);
            if (!body.empty() && body[0] == '\r') body.erase(0, 1);
        }
    };

    std::string body = req.body;
    while (!body.empty()) {
        std::string pair;
        consume(body, pair);
        size_t eq = pair.find('=');
        if (eq == std::string::npos) continue;
        std::string key = urlDecode(pair.substr(0, eq));
        std::string val = urlDecode(pair.substr(eq + 1));
        if      (key == "name")          name          = val;
        else if (key == "title")         title         = val;
        else if (key == "image_data")    image_data_str = val;
        else if (key == "scene")         scene         = val;
        else if (key == "map_location")  map_location  = val;
        else if (key == "gender")        gender        = val;
    }

    if (name.empty()) {
        resp.status_code = 400;
        resp.body = "{\"success\":false,\"message\":\"Name is required\"}";
    } else {
        std::vector<uint8_t> image_data;
        if (!image_data_str.empty()) {
            std::string decoded = base64Decode(image_data_str);
            image_data.assign(decoded.begin(), decoded.end());
        }

        // Same decision pipeline as the bulk importer: duplicate detection,
        // then the high-similarity confirmation gate.
        DedupContext dedup;
        auto outcome = enrollImage(name, title, scene, map_location,
                                   normaliseGender(gender), image_data, "", dedup);

        if (outcome.duplicate) {
            resp.status_code = 409;
            resp.body =
                "{\"success\":false,\"duplicate\":true,\"id\":\"" +
                jsonEscape(outcome.existing_id) +
                "\",\"has_embedding\":true,\"message\":\"Refused: " +
                jsonEscape(outcome.reason) + "\"}";
            resp.content_type = "application/json; charset=utf-8";
            return resp;
        }

        if (outcome.needs_confirm) {
            resp.status_code = 409;
            resp.body =
                "{\"success\":false,\"needs_confirm\":true,\"id\":\"\","
                "\"token\":\"" + jsonEscape(outcome.token) + "\""
                ",\"similar_name\":\"" + jsonEscape(outcome.similar_name) + "\""
                ",\"match_id\":\"" + jsonEscape(outcome.match_id) + "\""
                ",\"match_scene\":\"" + jsonEscape(outcome.match_scene) + "\""
                ",\"same_scene\":" + (outcome.same_scene ? "true" : "false") +
                ",\"similarity\":" +
                std::to_string(static_cast<int>(outcome.similarity * 1000) / 1000.0) +
                ",\"message\":\"" + jsonEscape(outcome.reason) +
                " — 请调用 /api/faces/resolve 人工确认\"}";
            resp.content_type = "application/json; charset=utf-8";
            return resp;
        }

        if (!outcome.ok && require_embedding_) {
            resp.status_code = 409;
            resp.body =
                "{\"success\":false,\"id\":\"\",\"has_embedding\":false,"
                "\"message\":\"Refused: no embedding could be extracted (" +
                jsonEscape(outcome.reason) + "). Such a record could never be "
                "matched. Load the models or start the server with "
                "--allow-no-embedding.\"}";
            resp.content_type = "application/json; charset=utf-8";
            return resp;
        }

        const std::string msg = outcome.ok
            ? (outcome.has_embedding
                   ? "Face added with embedding"
                   : "Face added (without embedding — no face detected or models not loaded)")
            : "Failed to add face";

        std::ostringstream json;
        json << "{\"success\":" << (outcome.ok ? "true" : "false")
             << ",\"id\":\"" << outcome.id << "\""
             << ",\"has_embedding\":" << (outcome.has_embedding ? "true" : "false")
             << ",\"similar_name\":\"" << jsonEscape(outcome.similar_name) << "\""
             << ",\"similarity\":" << (static_cast<int>(outcome.similarity * 1000) / 1000.0)
             << ",\"message\":\"" << msg << "\"}";
        resp.body = json.str();
    }
    resp.content_type = "application/json; charset=utf-8";
    return resp;
}

// ------------------------------------------------------------ bulk import --

std::string FaceHandler::trimmed(const std::string& s) {
    auto isSpace = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    };
    size_t b = 0, e = s.size();
    while (b < e && isSpace(s[b])) ++b;
    while (e > b && isSpace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::string FaceHandler::stemOf(const std::string& basename) {
    const size_t dot = basename.find_last_of('.');
    return (dot == std::string::npos) ? basename : basename.substr(0, dot);
}

bool FaceHandler::isSupportedImage(const std::string& basename) {
    const size_t dot = basename.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = basename.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    // The naming/format contract for bulk import: png and jpg only.
    return ext == "png" || ext == "jpg" || ext == "jpeg";
}

void FaceHandler::parseTitleName(const std::string& stem, std::string& title,
                                 std::string& name) {
    size_t b = 0, e = stem.size();
    while (b < e && stem[b] == '_') ++b;
    while (e > b && stem[e - 1] == '_') --e;
    std::string s = stem.substr(b, e - b);

    // Drop a leading serial number: galleries exported from a directory are
    // commonly named "01张三" / "01工程师_张三". Only strip it when a real name
    // follows, so a stem that is all digits ("007") stays a valid name.
    size_t d = 0;
    while (d < s.size() && s[d] >= '0' && s[d] <= '9') ++d;
    if (d > 0 && d < s.size() && s[d] != '_') s = s.substr(d);

    const size_t pos = s.find_last_of('_');
    if (pos == std::string::npos) {
        title.clear();
        name = s;
        return;
    }
    title = trimmed(s.substr(0, pos));
    name  = trimmed(s.substr(pos + 1));
}

std::string FaceHandler::guessGender(const cv::Mat& face_crop,
                                     const std::string& current) const {
    // An explicit choice always wins; auto-fill only touches "unknown".
    if (!gender_ || !gender_->ready()) return current;
    if (!current.empty() && current != "unknown") return current;
    if (face_crop.empty()) return current;

    auto res = gender_->classify(face_crop);
    if (res.gender == "unknown" || res.confidence < 0.60f) return current;
    return res.gender;
}

bool FaceHandler::extractEmbedding(const std::vector<uint8_t>& image_bytes,
                                   std::vector<float>& embedding,
                                   std::string& note,
                                   cv::Mat* face_crop) {
    if (image_bytes.empty()) {
        note = "no image data";
        return false;
    }
    if (!detector_ || !recognizer_) {
        note = "detection/recognition models are not loaded";
        return false;
    }

    cv::Mat buf(1, static_cast<int>(image_bytes.size()), CV_8U,
                const_cast<uint8_t*>(image_bytes.data()));
    cv::Mat image = cv::imdecode(buf, cv::IMREAD_COLOR);
    if (image.empty()) {
        note = "cannot decode image (not a valid png/jpg?)";
        return false;
    }

    auto detections = detector_->detect(image, 1);
    if (detections.empty()) {
        // Rescue pass for curated photos where SCRFD scores a clear face
        // below the configured threshold. The threshold is a shared setting,
        // so it MUST be restored even if detection throws — otherwise every
        // later request would silently run at the permissive threshold.
        // The quality gate below still decides whether the result is usable.
        ScopedThresholdGuard guard(*detector_, kRescueDetectionThreshold);
        detections = detector_->detect(image, 1);
    }
    if (detections.empty()) {
        note = "no face detected";
        return false;
    }

    if (face_crop != nullptr) {
        // Auxiliary heads (gender/age) were trained on 5-point ALIGNED crops.
        // Feeding them a loose bounding box puts them out of distribution and
        // their output collapses towards one class, so prefer the same aligned
        // crop the embedding uses and fall back to the padded box only when
        // alignment is unavailable.
        if (recognizer_) {
            *face_crop = recognizer_->alignedFace(image, detections.front().landmarks);
        }
        if (face_crop->empty()) {
            const auto& bb = detections.front().bbox;
            int x = static_cast<int>(bb.x);
            int y = static_cast<int>(bb.y);
            int w = static_cast<int>(bb.width);
            int h = static_cast<int>(bb.height);
            const int dx = w / 6, dy = h / 6;
            x = std::max(0, x - dx);
            y = std::max(0, y - dy);
            w = std::min(image.cols - x, w + 2 * dx);
            h = std::min(image.rows - y, h + 2 * dy);
            if (w > 0 && h > 0) *face_crop = image(cv::Rect(x, y, w, h)).clone();
        }
    }

    // Same gate as the single-image route: a bulk import that bypassed it
    // would mix feature spaces inside one gallery.
    face_recognition::EmbeddingPolicy policy;
    auto emb = face_recognition::make_embedding(
        *recognizer_, image, detections.front(), policy);
    if (!emb.ok()) {
        note = emb.reject_reason;
        return false;
    }
    embedding = std::move(emb.embedding);
    return true;
}

FaceHandler::EnrollOutcome FaceHandler::enrollImage(
    const std::string& name,
    const std::string& title,
    const std::string& scene,
    const std::string& map_location,
    const std::string& gender,
    const std::vector<uint8_t>& image_bytes,
    const std::string& file_key,
    DedupContext& dedup) {

    EnrollOutcome out;
    if (name.empty()) {
        out.reason = "name is empty";
        return out;
    }

    // ---- duplicate check (1): content hash --------------------------------
    const std::string hash = image_bytes.empty()
        ? std::string()
        : face_recognition::sha256Hex(image_bytes.data(), image_bytes.size());

    if (!hash.empty()) {
        auto seen = dedup.hashes.find(hash);
        if (seen != dedup.hashes.end()) {
            out.duplicate     = true;
            out.existing_name = seen->second;
            out.reason = "与本次导入中的「" + seen->second + "」是同一张图片，已跳过";
            return out;
        }
        if (auto stored = database_->find_by_image_hash(hash)) {
            out.duplicate     = true;
            out.existing_id   = stored->id;
            out.existing_name = stored->name;
            out.reason = "库中已存在完全相同的图片（" + stored->name + "），已跳过";
            return out;
        }
    }

    // ---- duplicate check (2): file name, within this request only ---------
    if (!file_key.empty()) {
        auto seen = dedup.file_names.find(file_key);
        if (seen != dedup.file_names.end()) {
            out.duplicate     = true;
            out.existing_name = seen->second;
            out.reason = "本次导入中已存在同名文件（" + seen->second + "），已跳过";
            return out;
        }
    }

    std::vector<float> embedding;
    std::string note;
    cv::Mat crop;
    extractEmbedding(image_bytes, embedding, note, &crop);

    if (embedding.empty() && require_embedding_) {
        out.reason = note.empty() ? "no embedding" : note;
        return out;
    }

    // Auto-fill the gender when the caller did not say (model is optional).
    const std::string resolved_gender = guessGender(crop, gender);

    // ---- duplicate check (3): same person, different photo ---------------
    //
    // Above the HIGH-similarity threshold this is a possible re-enrolment and
    // must not be written without a decision; it is parked in the pending queue
    // instead and only /api/faces/resolve can store it.
    if (!embedding.empty()) {
        auto ranked = database_->rank_faces(embedding);
        if (!ranked.empty()) {
            const auto& top = ranked.front();
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%.3f", top.similarity);

            out.similarity   = top.similarity;
            out.similar_name = top.name;
            out.match_id     = top.face_id;
            out.match_scene  = top.scene;
            out.same_scene   = (top.scene == scene);

            const float high = policy_.thresholdFor(scene);
            if (top.similarity >= high) {
                if (dedup.skip_similar) {
                    out.duplicate     = true;
                    out.existing_id   = top.face_id;
                    out.existing_name = top.name;
                    out.reason = "与库中「" + top.name + "」相似度 " + buf + "，按 skip 策略已跳过";
                    return out;
                }

                if (policy_.needsConfirmation(scene, out.same_scene)) {
                    // Already parked and still unanswered? A parked upload is
                    // not in the gallery, so the hash lookup above missed it:
                    // re-importing the same archive must not queue it twice.
                    if (!hash.empty()) {
                        if (auto parked = pending_.findByHash(hash)) {
                            out.duplicate     = true;
                            out.existing_name = parked->name;
                            out.reason = "该图片已在「待确认入库」队列中（" + parked->name +
                                         "），未重复排队";
                            return out;
                        }
                    }

                    PendingEnroll item;
                    item.file         = file_key;
                    item.name         = name;
                    item.title        = title;
                    item.scene        = scene;
                    item.map_location = map_location;
                    item.gender       = resolved_gender.empty() ? "unknown" : resolved_gender;
                    item.image        = image_bytes;
                    item.image_hash   = hash;
                    item.embedding    = embedding;
                    item.match_id     = top.face_id;
                    item.match_name   = top.name;
                    item.match_scene  = top.scene;
                    item.similarity   = top.similarity;
                    item.same_scene   = out.same_scene;
                    item.reason       = out.reason;

                    out.token         = pending_.add(std::move(item));
                    out.needs_confirm = true;
                    out.reason = "与库中「" + top.name + "」相似度 " + buf +
                                 "（" + (out.same_scene ? "同场景" : "跨场景 " + top.scene) +
                                 "），等待人工确认后才会入库";
                    return out;  // NOT written
                }

                // Policy explicitly allows this case (e.g. cross-scene rule).
                out.reason = "按入库规则自动入库：与库中「" + top.name + "」相似度 " + buf +
                             "（" + (out.same_scene ? "同场景" : "跨场景 " + top.scene) + "）";
            } else if (top.similarity >= dedup.similarity_threshold) {
                if (dedup.skip_similar) {
                    out.duplicate     = true;
                    out.existing_id   = top.face_id;
                    out.existing_name = top.name;
                    out.reason = "与库中「" + top.name + "」相似度 " + buf + "，已跳过";
                    return out;
                }
                out.reason = "提示：与库中「" + top.name + "」相似度 " + buf;
            }
        }
    }

    const std::string id = database_->add_face(
        name, embedding, image_bytes, title,
        scene.empty() ? "default" : scene,
        map_location.empty() ? "unknown" : map_location,
        resolved_gender.empty() ? "unknown" : resolved_gender,
        hash);

    if (id.empty()) {
        out.reason = "database insert failed";
        return out;
    }

    // Remember what this request already took, so a later copy is caught.
    if (!hash.empty())     dedup.hashes[hash]         = name;
    if (!file_key.empty()) dedup.file_names[file_key] = name;

    out.ok            = true;
    out.id            = id;
    out.has_embedding = !embedding.empty();
    out.gender        = resolved_gender;
    if (!out.has_embedding) out.reason = note;  // kept only as a warning
    return out;
}

HttpResponse FaceHandler::importArchive(const HttpRequest& req) {
    HttpResponse resp;
    resp.content_type = "application/json; charset=utf-8";

    if (require_embedding_ && !(detector_ && recognizer_)) {
        resp.status_code = 503;
        resp.body =
            "{\"success\":false,\"message\":\"" +
            jsonEscape("Embedding extraction is unavailable (models not loaded). "
                       "Start the server with the models or pass --allow-no-embedding.") +
            "\"}";
        return resp;
    }

    const std::string ct = req.header("content-type");
    // The archive is referenced, never copied: the multipart part is moved out
    // of the form and its bytes are handed to readArchive directly. Copying a
    // 300 MB upload two or three times would multiply the peak memory of an
    // import for no benefit.
    std::string archive_data;
    const std::uint8_t* archive_ptr = nullptr;
    std::size_t archive_len = 0;
    std::string archive_name;
    std::string scene, map_location, gender = "unknown";
    DedupContext dedup;

    if (ct.rfind("multipart/", 0) == 0) {
        MultipartForm form;
        if (!form.parse(ct, req.body)) {
            resp.status_code = 400;
            resp.body =
                "{\"success\":false,\"message\":\"" +
                jsonEscape("cannot parse multipart body: " + form.error()) + "\"}";
            return resp;
        }
        MultipartPart part = form.take("archive");
        if (part.data.empty()) part = form.take("file");
        if (part.data.empty()) {
            resp.status_code = 400;
            resp.body =
                "{\"success\":false,\"message\":\"archive file is missing "
                "(expected a form field named 'archive')\"}";
            return resp;
        }
        archive_data = std::move(part.data);
        archive_ptr  = reinterpret_cast<const std::uint8_t*>(archive_data.data());
        archive_len  = archive_data.size();
        archive_name = part.filename;
        scene        = trimmed(form.value("scene"));
        map_location = trimmed(form.value("map_location"));
        gender       = normaliseGender(form.value("gender"));
        dedup.similarity_threshold = parseThreshold(form.value("similar_threshold"),
                                                    kDefaultSimilarThreshold);
        dedup.skip_similar = (trimmed(form.value("on_similar")) == "skip");
    } else {
        // Raw upload (curl --data-binary): the format comes from the magic
        // bytes, so no file name is needed.
        archive_ptr = reinterpret_cast<const std::uint8_t*>(req.body.data());
        archive_len = req.body.size();
    }

    std::vector<ArchiveEntry> entries;
    std::string err;
    if (!readArchive(archive_ptr, archive_len, archive_name, entries, err)) {
        resp.status_code = 400;
        resp.body = "{\"success\":false,\"message\":\"" + jsonEscape(err) + "\"}";
        return resp;
    }
    if (entries.empty()) {
        resp.status_code = 400;
        resp.body = "{\"success\":false,\"message\":\"archive contains no files\"}";
        return resp;
    }

    std::vector<ItemReport> reports;
    std::size_t skipped = 0;

    for (const auto& e : entries) {
        const bool hidden = !e.basename.empty() && e.basename[0] == '.';
        const bool apple  = e.path.rfind("__MACOSX/", 0) == 0 ||
                            e.basename.rfind("._", 0) == 0;
        if (hidden || apple || !isSupportedImage(e.basename)) {
            ++skipped;
            continue;
        }
        if (reports.size() >= kMaxBatchItems) {
            ++skipped;
            continue;
        }

        ItemReport rep;
        rep.file = e.path;

        std::string title, name;
        parseTitleName(stemOf(e.basename), title, name);
        rep.title = title;
        rep.name  = name;

        if (name.empty()) {
            rep.reason = "cannot derive a name from the file name "
                         "(expected <title>_<name>.png|jpg)";
            reports.push_back(std::move(rep));
            continue;
        }

        auto outcome = enrollImage(name, title, scene, map_location, gender,
                                   e.data, e.basename, dedup);
        rep.ok            = outcome.ok;
        rep.id            = outcome.id;
        rep.has_embedding = outcome.has_embedding;
        rep.reason        = outcome.reason;
        rep.duplicate     = outcome.duplicate;
        rep.needs_confirm = outcome.needs_confirm;
        rep.token         = outcome.token;
        rep.existing_name = outcome.existing_name;
        rep.similar_name  = outcome.similar_name;
        rep.match_id      = outcome.match_id;
        rep.match_scene   = outcome.match_scene;
        rep.same_scene    = outcome.same_scene;
        rep.similarity    = outcome.similarity;
        rep.gender        = outcome.gender;
        reports.push_back(std::move(rep));
    }

    std::size_t succeeded = 0, failed = 0, duplicates = 0, pending = 0;
    for (const auto& r : reports) {
        if (r.duplicate) { ++duplicates; continue; }
        if (r.needs_confirm) { ++pending; continue; }
        if (r.ok) ++succeeded; else ++failed;
    }

    std::ostringstream json;
    json << "{\"success\":" << (failed == 0 && succeeded > 0 ? "true" : "false")
         << ",\"archive\":\"" << jsonEscape(archive_name) << "\""
         << ",\"total\":" << entries.size()
         << ",\"succeeded\":" << succeeded
         << ",\"failed\":" << failed
         << ",\"duplicates\":" << duplicates
         << ",\"pending\":" << pending
         << ",\"skipped\":" << skipped
         << ",\"results\":[";
    for (std::size_t i = 0; i < reports.size(); ++i) {
        if (i) json << ",";
        appendReport(json, reports[i]);
    }
    json << "]}";
    resp.body = json.str();
    return resp;
}

HttpResponse FaceHandler::importBatch(const HttpRequest& req) {
    HttpResponse resp;
    resp.content_type = "application/json; charset=utf-8";

    if (require_embedding_ && !(detector_ && recognizer_)) {
        resp.status_code = 503;
        resp.body =
            "{\"success\":false,\"message\":\"" +
            jsonEscape("Embedding extraction is unavailable (models not loaded). "
                       "Start the server with the models or pass --allow-no-embedding.") +
            "\"}";
        return resp;
    }

    json::Parser parser(req.body);
    json::Value root;
    if (!parser.parse(root) || !root.isObject()) {
        resp.status_code = 400;
        resp.body =
            "{\"success\":false,\"message\":\"" +
            jsonEscape("invalid JSON body: " + parser.error()) + "\"}";
        return resp;
    }

    const json::Value* items = root.find("items");
    if (!items || !items->isArray() || items->array.empty()) {
        resp.status_code = 400;
        resp.body = "{\"success\":false,\"message\":\"'items' array is required\"}";
        return resp;
    }
    if (items->array.size() > kMaxBatchItems) {
        resp.status_code = 400;
        resp.body =
            "{\"success\":false,\"message\":\"too many items (limit " +
            std::to_string(kMaxBatchItems) + ")\"}";
        return resp;
    }

    const std::string default_scene = trimmed(root.stringOr("scene", "default"));
    const std::string default_map   = trimmed(root.stringOr("map_location", "unknown"));
    const std::string default_gender =
        normaliseGender(root.stringOr("gender", "unknown"));

    DedupContext dedup;
    dedup.similarity_threshold = static_cast<float>(root.numberOr("similar_threshold",
                                                                  kDefaultSimilarThreshold));
    dedup.skip_similar = root.boolOr("skip_similar", false);

    std::vector<ItemReport> reports;

    for (const auto& item : items->array) {
        ItemReport rep;
        rep.file = trimmed(item.stringOr("file"));

        std::string name  = trimmed(item.stringOr("name"));
        std::string title = trimmed(item.stringOr("title"));

        // The grid is pre-filled from the file name, but the operator may have
        // cleared a field: fall back to the same "title_name" rule.
        if (name.empty()) {
            const size_t slash = rep.file.find_last_of("/\\");
            const std::string base = (slash == std::string::npos)
                                         ? rep.file
                                         : rep.file.substr(slash + 1);
            std::string derived_title, derived_name;
            parseTitleName(stemOf(base), derived_title, derived_name);
            if (name.empty())  name  = derived_name;
            if (title.empty()) title = derived_title;
        }
        rep.name  = name;
        rep.title = title;

        if (name.empty()) {
            rep.reason = "name is empty and cannot be derived from the file name";
            reports.push_back(std::move(rep));
            continue;
        }

        std::vector<uint8_t> bytes;
        const std::string b64 = stripDataUrlPrefix(item.stringOr("image_data"));
        if (!b64.empty()) {
            const std::string decoded = base64Decode(b64);
            bytes.assign(decoded.begin(), decoded.end());
        }

        const std::string item_gender = trimmed(item.stringOr("gender"));
        auto outcome = enrollImage(
            name, title,
            trimmed(item.stringOr("scene")).empty()
                ? default_scene
                : trimmed(item.stringOr("scene")),
            trimmed(item.stringOr("map_location")).empty()
                ? default_map
                : trimmed(item.stringOr("map_location")),
            item_gender.empty() ? default_gender : normaliseGender(item_gender),
            bytes, rep.file, dedup);

        rep.ok            = outcome.ok;
        rep.id            = outcome.id;
        rep.has_embedding = outcome.has_embedding;
        rep.reason        = outcome.reason;
        rep.duplicate     = outcome.duplicate;
        rep.needs_confirm = outcome.needs_confirm;
        rep.token         = outcome.token;
        rep.existing_name = outcome.existing_name;
        rep.similar_name  = outcome.similar_name;
        rep.match_id      = outcome.match_id;
        rep.match_scene   = outcome.match_scene;
        rep.same_scene    = outcome.same_scene;
        rep.similarity    = outcome.similarity;
        rep.gender        = outcome.gender;
        reports.push_back(std::move(rep));
    }

    std::size_t succeeded = 0, failed = 0, duplicates = 0, pending = 0;
    for (const auto& r : reports) {
        if (r.duplicate) { ++duplicates; continue; }
        if (r.needs_confirm) { ++pending; continue; }
        if (r.ok) ++succeeded; else ++failed;
    }

    std::ostringstream json;
    json << "{\"success\":" << (failed == 0 && succeeded > 0 ? "true" : "false")
         << ",\"total\":" << reports.size()
         << ",\"succeeded\":" << succeeded
         << ",\"failed\":" << failed
         << ",\"duplicates\":" << duplicates
         << ",\"pending\":" << pending
         << ",\"skipped\":0"
         << ",\"results\":[";
    for (std::size_t i = 0; i < reports.size(); ++i) {
        if (i) json << ",";
        appendReport(json, reports[i]);
    }
    json << "]}";
    resp.body = json.str();
    return resp;
}

HttpResponse FaceHandler::updateFace(const HttpRequest& req) {
    HttpResponse resp;
    resp.content_type = "application/json; charset=utf-8";

    std::string id, name, title, scene, map_location, image_data_str, gender;

    auto consume = [](std::string& body, std::string& field) {
        size_t sep = std::string::npos;
        for (size_t i = 0; i < body.size(); ++i) {
            if (body[i] == '&' || body[i] == '\n') { sep = i; break; }
        }
        if (sep == std::string::npos) {
            field = body;
            body.clear();
        } else {
            field = body.substr(0, sep);
            body.erase(0, sep + 1);
            if (!body.empty() && body[0] == '\r') body.erase(0, 1);
        }
    };

    std::string body = req.body;
    while (!body.empty()) {
        std::string pair;
        consume(body, pair);
        const size_t eq = pair.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = urlDecode(pair.substr(0, eq));
        const std::string val = urlDecode(pair.substr(eq + 1));
        if      (key == "id")           id            = val;
        else if (key == "name")         name          = val;
        else if (key == "title")        title         = val;
        else if (key == "scene")        scene         = val;
        else if (key == "map_location") map_location  = val;
        else if (key == "image_data")   image_data_str = val;
        else if (key == "gender")       gender        = val;
    }

    id = trimmed(id);
    if (id.empty()) {
        resp.status_code = 400;
        resp.body = "{\"success\":false,\"message\":\"id is required\"}";
        return resp;
    }
    auto existing = database_->get_face(id);
    if (!existing) {
        resp.status_code = 404;
        resp.body = "{\"success\":false,\"message\":\"Face not found\"}";
        return resp;
    }

    std::vector<uint8_t> bytes;
    if (!image_data_str.empty()) {
        const std::string decoded = base64Decode(stripDataUrlPrefix(image_data_str));
        bytes.assign(decoded.begin(), decoded.end());
    }

    bool has_embedding = !existing->embedding.empty();
    std::string image_path;
    std::string note;
    std::string new_hash;

    // A new photo means the stored feature must be rebuilt: keeping the old
    // embedding would leave the record matching against someone else's face.
    if (!bytes.empty()) {
        new_hash = face_recognition::sha256Hex(bytes.data(), bytes.size());
        std::vector<float> embedding;
        if (extractEmbedding(bytes, embedding, note)) {
            std::string written;
            if (database_->save_image(id, bytes, written)) {
                image_path = written;
            }
            if (database_->update_embedding(id, embedding)) {
                has_embedding = true;
            } else {
                note = "failed to store the rebuilt embedding";
            }
        }
        if (!has_embedding && require_embedding_) {
            resp.status_code = 409;
            resp.body =
                "{\"success\":false,\"has_embedding\":false,\"message\":\"" +
                jsonEscape("Refused: the new image produced no embedding (" +
                           (note.empty() ? std::string("unknown reason") : note) +
                           "). The record was left unchanged.") + "\"}";
            return resp;
        }
    }

    // A new photo also carries a new content fingerprint, otherwise the record
    // would keep pointing at the hash of an image it no longer holds.
    const bool ok = database_->update_face(id, trimmed(name), trimmed(title),
                                           trimmed(scene), trimmed(map_location),
                                           image_path,
                                           normaliseGender(gender), new_hash);
    std::ostringstream json;
    json << "{\"success\":" << (ok ? "true" : "false")
         << ",\"id\":\"" << jsonEscape(id) << "\""
         << ",\"has_embedding\":" << (has_embedding ? "true" : "false")
         << ",\"message\":\"" << jsonEscape(ok ? "Face updated" : "Failed to update face")
         << "\"}";
    resp.body = json.str();
    return resp;
}

// ------------------------------------------------------- pending decisions

HttpResponse FaceHandler::listPending(const HttpRequest& req) {
    (void)req;
    HttpResponse resp;
    resp.content_type = "application/json; charset=utf-8";

    const auto items = pending_.list();
    std::ostringstream json;
    json << "{\"count\":" << items.size() << ",\"pending\":[";
    for (std::size_t i = 0; i < items.size(); ++i) {
        const PendingEnroll& it = *items[i];
        if (i) json << ",";
        json << "{\"token\":\"" << jsonEscape(it.token) << "\""
             << ",\"file\":\"" << jsonEscape(it.file) << "\""
             << ",\"name\":\"" << jsonEscape(it.name) << "\""
             << ",\"title\":\"" << jsonEscape(it.title) << "\""
             << ",\"scene\":\"" << jsonEscape(it.scene) << "\""
             << ",\"map_location\":\"" << jsonEscape(it.map_location) << "\""
             << ",\"gender\":\"" << jsonEscape(it.gender) << "\""
             << ",\"match_id\":\"" << jsonEscape(it.match_id) << "\""
             << ",\"match_name\":\"" << jsonEscape(it.match_name) << "\""
             << ",\"match_scene\":\"" << jsonEscape(it.match_scene) << "\""
             << ",\"same_scene\":" << (it.same_scene ? "true" : "false")
             << ",\"similarity\":" << (static_cast<int>(it.similarity * 1000) / 1000.0)
             << ",\"allow_replace\":" << (policy_.replaceAllowed(it.scene) ? "true" : "false")
             << ",\"created_at\":" << it.created_at
             << "}";
    }
    json << "]}";
    resp.body = json.str();
    return resp;
}

HttpResponse FaceHandler::pendingImage(const HttpRequest& req) {
    HttpResponse resp;
    const size_t pos = req.url.find_last_of('/');
    if (pos == std::string::npos || pos == req.url.size() - 1) {
        resp.status_code = 404;
        resp.body = "Not Found";
        return resp;
    }
    const std::string token = req.url.substr(pos + 1);
    auto item = pending_.peek(token);
    if (!item || item->image.empty()) {
        resp.status_code = 404;
        resp.body = "Image not found";
        return resp;
    }
    resp.body.assign(item->image.begin(), item->image.end());
    resp.content_type = sniffImageContentType(item->image.data(), item->image.size());
    return resp;
}

HttpResponse FaceHandler::resolvePending(const HttpRequest& req) {
    HttpResponse resp;
    resp.content_type = "application/json; charset=utf-8";

    json::Parser parser(req.body);
    json::Value root;
    if (!parser.parse(root) || !root.isObject()) {
        resp.status_code = 400;
        resp.body = "{\"success\":false,\"message\":\"invalid JSON body: " +
                    jsonEscape(parser.error()) + "\"}";
        return resp;
    }
    const json::Value* decisions = root.find("decisions");
    if (!decisions || !decisions->isArray() || decisions->array.empty()) {
        resp.status_code = 400;
        resp.body = "{\"success\":false,\"message\":\"'decisions' array is required\"}";
        return resp;
    }

    std::size_t enrolled = 0, replaced = 0, templated = 0, skipped = 0, failed = 0;
    std::ostringstream json;
    json << "{\"results\":[";

    for (std::size_t i = 0; i < decisions->array.size(); ++i) {
        const auto& d      = decisions->array[i];
        const std::string token  = d.stringOr("token");
        const std::string action = d.stringOr("action", "skip");

        std::string id;
        std::string message;
        bool ok = false;

        // take() removes it: a decision must be applied exactly once, and a
        // second call with the same token can never write a second row.
        auto item = pending_.take(token);
        if (!item) {
            message = "token 不存在或已处理";
            ++failed;
        } else if (action == "skip" || action == "discard") {
            message = "已丢弃，未写入人脸库";
            ok = true;
            ++skipped;
        } else if (action == "enroll" || action == "force") {
            const std::string name  = d.stringOr("name").empty()  ? item->name  : trimmed(d.stringOr("name"));
            const std::string title = d.stringOr("title").empty() ? item->title : trimmed(d.stringOr("title"));
            const std::string scene = d.stringOr("scene").empty() ? item->scene : trimmed(d.stringOr("scene"));
            const std::string mapv  = d.stringOr("map_location").empty()
                                          ? item->map_location : trimmed(d.stringOr("map_location"));
            const std::string gend  = d.stringOr("gender").empty()
                                          ? item->gender : normaliseGender(d.stringOr("gender"));
            id = database_->add_face(name, item->embedding, item->image, title, scene,
                                     mapv, gend, item->image_hash);
            ok = !id.empty();
            message = ok ? "已强制入库（新增一条记录）" : "写入失败";
            if (ok) ++enrolled; else ++failed;
        } else if (action == "template") {
            // Multi-shot: keep the existing record AND its photo, just add this
            // embedding as an extra template. Better than "replace" when the
            // new photo shows the same person from another angle.
            std::string path;
            database_->save_image(item->match_id + "-t" +
                                      std::to_string(database_->template_count(item->match_id) + 1),
                                  item->image, path);
            ok = database_->add_template(item->match_id, item->embedding, path);
            id = item->match_id;
            message = ok ? "已追加为库中该人的额外模板（多枪）" : "追加模板失败";
            if (ok) ++templated; else ++failed;
        } else if (action == "replace") {
            if (!policy_.replaceAllowed(item->scene)) {
                message = "当前场景规则不允许替换库中记录";
                ++failed;
            } else {
                const std::string name  = trimmed(d.stringOr("name"));
                const std::string title = trimmed(d.stringOr("title"));
                const std::string scene = trimmed(d.stringOr("scene"));
                const std::string mapv  = trimmed(d.stringOr("map_location"));
                const std::string gend  = d.stringOr("gender").empty()
                                              ? item->gender : normaliseGender(d.stringOr("gender"));
                std::string path;
                database_->save_image(item->match_id, item->image, path);
                const bool emb_ok = database_->update_embedding(item->match_id, item->embedding);
                const bool meta_ok = database_->update_face(
                    item->match_id, name, title, scene, mapv, path, gend, item->image_hash);
                ok = emb_ok && meta_ok;
                id = item->match_id;
                message = ok ? "已替换库中该记录的特征与照片" : "替换失败（记录可能已被删除）";
                if (ok) ++replaced; else ++failed;
            }
        } else {
            message = "未知 action：" + action;
            ++failed;
        }

        if (i) json << ",";
        json << "{\"token\":\"" << jsonEscape(token) << "\""
             << ",\"action\":\"" << jsonEscape(action) << "\""
             << ",\"success\":" << (ok ? "true" : "false")
             << ",\"id\":\"" << jsonEscape(id) << "\""
             << ",\"message\":\"" << jsonEscape(message) << "\"}";
    }

    json << "],\"enrolled\":" << enrolled
         << ",\"replaced\":" << replaced
         << ",\"templated\":" << templated
         << ",\"skipped\":" << skipped
         << ",\"failed\":" << failed << "}";
    resp.body = json.str();
    return resp;
}

HttpResponse FaceHandler::addTemplate(const HttpRequest& req) {
    HttpResponse resp;
    resp.content_type = "application/json; charset=utf-8";

    std::string id, image_data_str;
    auto consume = [](std::string& body, std::string& field) {
        size_t sep = std::string::npos;
        for (size_t i = 0; i < body.size(); ++i) {
            if (body[i] == '&' || body[i] == '\n') { sep = i; break; }
        }
        if (sep == std::string::npos) {
            field = body;
            body.clear();
        } else {
            field = body.substr(0, sep);
            body.erase(0, sep + 1);
            if (!body.empty() && body[0] == '\r') body.erase(0, 1);
        }
    };
    std::string body = req.body;
    while (!body.empty()) {
        std::string pair;
        consume(body, pair);
        const size_t eq = pair.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = urlDecode(pair.substr(0, eq));
        const std::string val = urlDecode(pair.substr(eq + 1));
        if      (key == "id")         id = val;
        else if (key == "image_data") image_data_str = val;
    }

    if (id.empty()) {
        resp.status_code = 400;
        resp.body = "{\"success\":false,\"message\":\"id is required\"}";
        return resp;
    }
    auto existing = database_->get_face(id);
    if (!existing) {
        resp.status_code = 404;
        resp.body = "{\"success\":false,\"message\":\"Face not found\"}";
        return resp;
    }

    std::vector<uint8_t> bytes;
    if (!image_data_str.empty()) {
        const std::string decoded = base64Decode(stripDataUrlPrefix(image_data_str));
        bytes.assign(decoded.begin(), decoded.end());
    }
    if (bytes.empty()) {
        resp.status_code = 400;
        resp.body = "{\"success\":false,\"message\":\"image_data is required\"}";
        return resp;
    }

    // An extra template must come from the SAME feature space as everything
    // else in the gallery, so it goes through the shared gate too.
    std::vector<float> embedding;
    std::string note;
    if (!extractEmbedding(bytes, embedding, note)) {
        resp.status_code = 409;
        resp.body = "{\"success\":false,\"message\":\"Refused: " + jsonEscape(note) + "\"}";
        return resp;
    }

    std::string image_path;
    database_->save_image(id + "-t" + std::to_string(database_->template_count(id) + 1),
                          bytes, image_path);
    const bool ok = database_->add_template(id, embedding, image_path);

    std::ostringstream json;
    json << "{\"success\":" << (ok ? "true" : "false")
         << ",\"id\":\"" << jsonEscape(id) << "\""
         << ",\"templates\":" << database_->template_count(id)
         << ",\"message\":\"" << jsonEscape(ok ? "已追加为该人的额外模板（多枪）"
                                               : "追加失败") << "\"}";
    resp.body = json.str();
    return resp;
}

HttpResponse FaceHandler::getConfig(const HttpRequest& req) {
    (void)req;
    HttpResponse resp;
    resp.content_type = "application/json; charset=utf-8";
    std::ostringstream json;
    json << "{\"auto_gender\":"
         << ((gender_ && gender_->ready()) ? "true" : "false")
         << ",\"high_similarity\":" << policy_.high_similarity
         << ",\"require_confirmation\":" << (policy_.require_confirmation ? "true" : "false")
         << ",\"confirm_on_same_scene\":" << (policy_.confirm_on_same_scene ? "true" : "false")
         << ",\"confirm_on_cross_scene\":" << (policy_.confirm_on_cross_scene ? "true" : "false")
         << ",\"allow_replace\":" << (policy_.allow_replace ? "true" : "false")
         << "}";
    resp.body = json.str();
    return resp;
}

HttpResponse FaceHandler::removeFace(const HttpRequest& req) {
    HttpResponse resp;

    std::string id;
    auto consume = [&](std::string& body, std::string& field) {
        size_t sep = std::string::npos;
        for (size_t i = 0; i < body.size(); ++i) {
            if (body[i] == '&' || body[i] == '\n') { sep = i; break; }
        }
        if (sep == std::string::npos) { field = body; body.clear(); }
        else {
            field = body.substr(0, sep);
            body.erase(0, sep + 1);
            if (!body.empty() && body[0] == '\r') body.erase(0, 1);
        }
    };

    std::string body = req.body;
    while (!body.empty() && id.empty()) {
        std::string pair;
        consume(body, pair);
        size_t eq = pair.find('=');
        if (eq == std::string::npos) continue;
        std::string key = urlDecode(pair.substr(0, eq));
        if (key == "id") {
            id = urlDecode(pair.substr(eq + 1));
        }
    }

    bool success = !id.empty() && database_->remove_face(id);
    std::ostringstream json;
    json << "{\"success\":" << (success ? "true" : "false")
         << ",\"message\":\"" << (success ? "Face removed" : "Face not found") << "\"}";
    resp.body = json.str();
    resp.content_type = "application/json; charset=utf-8";
    return resp;
}

HttpResponse FaceHandler::clearFaces(const HttpRequest& req) {
    HttpResponse resp;
    int deleted = database_->clear_all();
    std::ostringstream json;
    json << "{\"success\":true,\"deleted_count\":" << deleted << "}";
    resp.body = json.str();
    resp.content_type = "application/json; charset=utf-8";
    return resp;
}

HttpResponse FaceHandler::getImage(const HttpRequest& req) {
    HttpResponse resp;

    size_t pos = req.url.find_last_of('/');
    if (pos == std::string::npos || pos == req.url.size() - 1) {
        resp.status_code = 404;
        resp.body = "Not Found";
        return resp;
    }

    std::string face_id = req.url.substr(pos + 1);

    auto face = database_->get_face(face_id);
    if (!face || face->image_path.empty()) {
        resp.status_code = 404;
        resp.body = "Image not found";
        return resp;
    }

    std::ifstream file(face->image_path, std::ios::binary);
    if (!file) {
        resp.status_code = 404;
        resp.body = "Cannot read image";
        return resp;
    }

    std::ostringstream oss;
    oss << file.rdbuf();
    resp.body = oss.str();
    // Content-Type from the payload, NOT from the ".jpg" file name (see
    // sniffImageContentType): the stored bytes keep their original format.
    resp.content_type = sniffImageContentType(resp.body.data(), resp.body.size());
    return resp;
}

std::string FaceHandler::renderIndex() {
    // The real template file is preferred (it can be tweaked without
    // recompiling); the generated fallback keeps the UI alive when the binary
    // runs from a directory that has no ./templates next to it.
    std::vector<std::string> dirs;
    if (!templates_dir_.empty()) dirs.push_back(templates_dir_);
    dirs.push_back("./templates");

    const std::string exe = exeDir();
    if (!exe.empty()) {
        dirs.push_back(exe + "/templates");
        dirs.push_back(exe + "/../share/face_db_web/templates");
        dirs.push_back(exe + "/../../share/face_db_web/templates");
    }

    for (const auto& d : dirs) {
        std::ifstream file(d + "/index.html");
        if (!file) continue;
        std::ostringstream oss;
        oss << file.rdbuf();
        const std::string html = oss.str();
        if (!html.empty()) return html;
    }

    return embedded::index_html();
}

}  // namespace face_db_web
