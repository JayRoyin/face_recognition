#include "face_db_web/face_handler.hpp"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cstring>

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

FaceHandler::FaceHandler(std::shared_ptr<face_recognition::FaceDatabase> database,
                         std::shared_ptr<face_recognition::FaceDetector>   detector,
                         std::shared_ptr<face_recognition::FaceRecognizer> recognizer,
                         bool require_embedding)
    : database_(std::move(database)),
      detector_(std::move(detector)),
      recognizer_(std::move(recognizer)),
      require_embedding_(require_embedding) {

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
        json << "{"
             << "\"id\":\"" << f.id << "\","
             << "\"name\":\"" << jsonEscape(f.name) << "\","
             << "\"title\":\"" << jsonEscape(f.title) << "\","
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

    std::string name, title, image_data_str, scene, map_location;

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

        std::vector<float> embedding;

        // If a recognizer is wired in, decode the uploaded image and extract
        // a 512-D ArcFace embedding. This is what makes the record matchable
        // by the ROS2 recognition node later.
        if (!image_data.empty() && detector_ && recognizer_) {
            cv::Mat buf(1, static_cast<int>(image_data.size()), CV_8U, image_data.data());
            cv::Mat decoded_img = cv::imdecode(buf, cv::IMREAD_COLOR);
            if (!decoded_img.empty()) {
                auto detections = detector_->detect(decoded_img, 1);
                if (!detections.empty()) {
                    embedding = recognizer_->extract_embedding(
                        decoded_img, detections.front().bbox, detections.front().landmarks);
                    if (embedding.empty()) {
                        std::cerr << "[FaceHandler] warning: embedding extraction returned empty for '" << name << "'\n";
                    }
                } else {
                    std::cerr << "[FaceHandler] warning: no face detected in uploaded image for '" << name << "'\n";
                }
            } else {
                std::cerr << "[FaceHandler] warning: failed to decode uploaded image for '" << name << "'\n";
            }
        }

        // Guard: a record without an embedding can never be matched by the
        // recognizer, so refuse it unless the operator explicitly opted in.
        if (embedding.empty() && require_embedding_) {
            resp.status_code = 409;
            resp.body =
                "{\"success\":false,\"id\":\"\",\"has_embedding\":false,"
                "\"message\":\"Refused: no embedding could be extracted (no face "
                "detected in the image, or the detection/recognition models are not "
                "loaded). Such a record could never be matched. Load the models or "
                "start the server with --allow-no-embedding.\"}";
            resp.content_type = "application/json; charset=utf-8";
            return resp;
        }

        std::string id = database_->add_face(
            name, embedding, image_data, title,
            scene.empty() ? "default" : scene,
            map_location.empty() ? "unknown" : map_location);

        std::string msg;
        if (!id.empty()) {
            msg = embedding.empty()
                ? "Face added (without embedding — no face detected or models not loaded)"
                : "Face added with embedding";
        } else {
            msg = "Failed to add face";
        }

        std::ostringstream json;
        json << "{\"success\":" << (!id.empty() ? "true" : "false")
             << ",\"id\":\"" << id << "\""
             << ",\"has_embedding\":" << (!embedding.empty() ? "true" : "false")
             << ",\"message\":\"" << msg << "\"}";
        resp.body = json.str();
    }
    resp.content_type = "application/json; charset=utf-8";
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
    resp.content_type = "image/jpeg";
    return resp;
}

std::string FaceHandler::renderIndex() {
    std::string path = templates_dir_ + "/index.html";
    std::ifstream file(path);
    if (file) {
        std::ostringstream oss;
        oss << file.rdbuf();
        return oss.str();
    }

    return R"HTML_RAW(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>Face DB Web</title>
<style>
body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Arial, sans-serif;
       margin: 0; padding: 20px; background: #f5f5f5; }
h1 { color: #333; }
.container { max-width: 1200px; margin: 0 auto; background: white; padding: 20px;
             border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }
table { width: 100%; border-collapse: collapse; margin-top: 20px; }
th, td { padding: 10px; text-align: left; border-bottom: 1px solid #eee; }
th { background: #f8f8f8; }
button { padding: 6px 12px; margin: 2px; cursor: pointer; border: none;
         border-radius: 4px; background: #4a90e2; color: white; }
button.danger { background: #e74c3c; }
button:hover { opacity: 0.9; }
input, textarea { padding: 6px; margin: 4px; border: 1px solid #ccc;
                  border-radius: 4px; width: 200px; }
.form-group { margin: 8px 0; }
img.thumb { max-width: 100px; max-height: 100px; border-radius: 4px; }
</style>
</head>
<body>
<div class="container">
<h1>Face Database Management</h1>
<div>
  <h3>Add Face</h3>
  <div class="form-group">Name: <input id="name" placeholder="Name"></div>
  <div class="form-group">Title: <input id="title" placeholder="Title"></div>
  <div class="form-group">Scene: <input id="scene" placeholder="Scene" value="default"></div>
  <div class="form-group">Map Location: <input id="map" placeholder="Map" value="unknown"></div>
  <div class="form-group">Image URL: <input id="imgurl" placeholder="http://..." onchange="loadImage(this.value)"></div>
  <div class="form-group">Or Upload: <input type="file" id="file" accept="image/*"></div>
  <img id="preview" style="display:none;max-width:200px;"/>
  <div><button onclick="addFace()">Add Face</button></div>
</div>
<hr>
<h3>Face List (<span id="count">0</span>)</h3>
<table>
<thead><tr><th>Image</th><th>Name</th><th>Title</th><th>Scene</th><th>Map</th><th>ID</th><th>Action</th></tr></thead>
<tbody id="faces"></tbody>
</table>
</div>
<script>
let imageBase64 = '';
function loadImage(url) {
  const img = new Image();
  img.crossOrigin = "anonymous";
  img.onload = function() {
    const canvas = document.createElement('canvas');
    canvas.width = img.width; canvas.height = img.height;
    const ctx = canvas.getContext('2d');
    ctx.drawImage(img, 0, 0);
    imageBase64 = canvas.toDataURL('image/jpeg').split(',')[1];
    document.getElementById('preview').src = canvas.toDataURL('image/jpeg');
    document.getElementById('preview').style.display = 'block';
  };
  img.onerror = function() { alert('Failed to load image from URL'); };
  img.src = url;
}
document.getElementById('file').onchange = function(e) {
  const file = e.target.files[0];
  const reader = new FileReader();
  reader.onload = function(ev) {
    imageBase64 = ev.target.result.split(',')[1];
    document.getElementById('preview').src = ev.target.result;
    document.getElementById('preview').style.display = 'block';
  };
  reader.readAsDataURL(file);
};
function addFace() {
  const body = new URLSearchParams();
  body.set('name', document.getElementById('name').value);
  body.set('title', document.getElementById('title').value);
  body.set('scene', document.getElementById('scene').value);
  body.set('map_location', document.getElementById('map').value);
  if (imageBase64) body.set('image_data', imageBase64);
  fetch('/api/faces/add', { method: 'POST', body: body })
    .then(r => r.json()).then(j => {
      alert(j.message); loadFaces();
    });
}
function removeFace(id) {
  const body = new URLSearchParams(); body.set('id', id);
  fetch('/api/faces/remove', { method: 'POST', body: body })
    .then(r => r.json()).then(j => { alert(j.message); loadFaces(); });
}
function clearAll() {
  if (!confirm('Clear all faces?')) return;
  fetch('/api/faces/clear', { method: 'POST' })
    .then(r => r.json()).then(j => { alert('Deleted: ' + j.deleted_count); loadFaces(); });
}
function loadFaces() {
  fetch('/api/faces').then(r => r.json()).then(j => {
    document.getElementById('count').textContent = j.count;
    const tbody = document.getElementById('faces');
    tbody.innerHTML = '';
    j.faces.forEach(f => {
      const tr = document.createElement('tr');
      const imgSrc = '/api/image/' + f.id;
      tr.innerHTML = '<td></td><td></td><td></td><td></td><td></td><td></td><td></td>';
      const img = document.createElement('img');
      img.className = 'thumb'; img.src = '/api/image/' + encodeURIComponent(f.id);
      tr.children[0].appendChild(img);
      [f.name, f.title, f.scene, f.map_location, f.id].forEach((v, i) => {
        tr.children[i + 1].textContent = v || '';
      });
      const button = document.createElement('button');
      button.className = 'danger'; button.textContent = 'Delete';
      button.onclick = () => removeFace(f.id);
      tr.children[6].appendChild(button);
      tbody.appendChild(tr);
    });
  });
}
loadFaces();
</script>
</body>
</html>)HTML_RAW";
}

}  // namespace face_db_web
