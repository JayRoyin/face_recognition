#include "face_recognition_ros1/node.hpp"

#include "face_recognition_core/embedding_policy.hpp"

#include <opencv2/imgcodecs.hpp>

#include <ctime>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <opencv2/imgcodecs.hpp>

namespace face_recognition_ros1 {

FaceRecognitionNode::FaceRecognitionNode()
    : nh_("~"), pnh_(""), it_(nh_) {

    // 0.5, matching the standalone app / web UI. This value doubles as BOTH the
    // detector confidence and the recognition threshold, and 0.7 sits inside the
    // "genuine" cluster: it silently rejects a large share of correct matches.
    nh_.param<float>("confidence_threshold", confidence_threshold_, 0.5f);
    nh_.param<std::string>("db_path", db_path_, "/data/hhqs_data/face_db/faces.db");
    nh_.param<std::string>("faces_dir", faces_dir_, "/data/hhqs_data/face_db/faces");
    // Relative to the working directory, matching the ROS2 node and the rest of
    // the project (models live in <repo>/models). The previous "/models/..." was
    // an absolute path into the filesystem root, which never exists — the node
    // then reported "Failed to initialize detector/recognizer" with no hint.
    nh_.param<std::string>("detection_model", detection_model_path_, "models/det_10g.onnx");
    nh_.param<std::string>("recognition_model", recognition_model_path_, "models/w600k_r50.onnx");

    ROS_INFO("Initializing Face Recognition Node...");
    ROS_INFO("Detection model: %s", detection_model_path_.c_str());
    ROS_INFO("Recognition model: %s", recognition_model_path_.c_str());

    detector_ = std::make_unique<face_recognition::FaceDetector>();
    if (!detector_->initialize(detection_model_path_, confidence_threshold_)) {
        ROS_ERROR("Failed to initialize detector");
    } else {
        detector_ready_ = true;
        ROS_INFO("Face detector initialized");
    }

    recognizer_ = std::make_unique<face_recognition::FaceRecognizer>();
    if (!recognizer_->initialize(recognition_model_path_)) {
        ROS_ERROR("Failed to initialize recognizer");
    } else {
        recognizer_ready_ = true;
        ROS_INFO("Face recognizer initialized");
    }

    database_ = std::make_unique<face_recognition::FaceDatabase>();
    if (!database_->initialize(db_path_, faces_dir_)) {
        // Fail fast: a database that never opened would make every service report
        // a bogus business result for an infrastructure failure.
        ROS_FATAL("Cannot open face database at '%s' (faces_dir '%s')",
                  db_path_.c_str(), faces_dir_.c_str());
        throw std::runtime_error("face_recognition_ros1: database init failed");
    } else {
        for (const auto& face : database_->list_faces()) {
            if (!detector_ || !recognizer_) break;
            if (!face.embedding.empty() || face.image_path.empty()) continue;
            cv::Mat image = cv::imread(face.image_path, cv::IMREAD_COLOR);
            if (image.empty()) continue;
            auto detections = detector_->detect(image, 1);
            if (!detections.empty()) {
                // Route through the core gate so this node applies exactly the
                // same quality rules as the standalone app and the web UI.
                face_recognition::EmbeddingPolicy policy;
                auto outcome = face_recognition::make_embedding(
                    *recognizer_, image, detections.front(), policy);
                if (outcome.ok()) {
                    database_->update_embedding(face.id, outcome.embedding);
                } else {
                    ROS_WARN("backfill skipped id=%s: %s",
                             face.id.c_str(), outcome.reject_reason.c_str());
                }
            }
        }
        ROS_INFO("Face database initialized with %d faces", database_->get_face_count());
    }

    std::string image_topic, result_topic;
    nh_.param<std::string>("image_topic", image_topic, "/image_raw");
    nh_.param<std::string>("result_topic", result_topic, "/face/recognition_result");

    image_sub_ = it_.subscribe(image_topic, 1,
        &FaceRecognitionNode::imageCallback, this);

    result_pub_ = nh_.advertise<face_recognition_ros1_interfaces::FaceResult>(result_topic, 10);

    add_srv_ = nh_.advertiseService("/face_db/add",
        &FaceRecognitionNode::addFaceCallback, this);
    remove_srv_ = nh_.advertiseService("/face_db/remove",
        &FaceRecognitionNode::removeFaceCallback, this);
    list_srv_ = nh_.advertiseService("/face_db/list",
        &FaceRecognitionNode::listFacesCallback, this);
    clear_srv_ = nh_.advertiseService("/face_db/clear",
        &FaceRecognitionNode::clearFacesCallback, this);
    add_template_srv_ = nh_.advertiseService("/face_db/add_template",
        &FaceRecognitionNode::addTemplateCallback, this);
    verify_srv_ = nh_.advertiseService("/face_db/verify",
        &FaceRecognitionNode::verifyCallback, this);

    ROS_INFO("Face Recognition Node initialized");
}

FaceRecognitionNode::~FaceRecognitionNode() = default;

void FaceRecognitionNode::spin() {
    ros::spin();
}

void FaceRecognitionNode::imageCallback(const sensor_msgs::ImageConstPtr& msg) {
    cv_bridge::CvImageConstPtr cv_ptr;
    try {
        cv_ptr = cv_bridge::toCvShare(msg, sensor_msgs::image_encodings::BGR8);
    } catch (const cv_bridge::Exception& e) {
        ROS_ERROR("CV Bridge error: %s", e.what());
        return;
    }

    const cv::Mat& image = cv_ptr->image;
    if (image.empty()) {
        return;
    }

    if (!detector_ready_ || !recognizer_ready_) {
        ROS_ERROR_THROTTLE(5.0,
            "models not loaded (detector: %s | recognizer: %s) -- publishing no faces",
            detector_->getLastError().c_str(), recognizer_->getLastError().c_str());
        face_recognition_ros1_interfaces::FaceResult empty_msg;
        empty_msg.header = msg->header;
        result_pub_.publish(empty_msg);
        return;
    }

    auto detections = detector_->detect(image, 10);

    face_recognition_ros1_interfaces::FaceResult result_msg;
    result_msg.header = msg->header;

    for (const auto& det : detections) {
        face_recognition::EmbeddingPolicy policy;
        auto outcome = face_recognition::make_embedding(*recognizer_, image, det, policy);
        if (!outcome.ok()) {
            continue;
        }
        auto embedding = std::move(outcome.embedding);

        auto match = database_->find_matching_face(embedding, confidence_threshold_);
        if (!match) {
            continue;
        }

        face_recognition_ros1_interfaces::FaceInfo face;
        face.id = match->id;
        face.name = match->name;
        face.title = match->title;
        face.confidence = face_recognition::compute_similarity(embedding, match->embedding);
        face.scene = match->scene;
        face.map_location = match->map_location;
        face.timestamp = ros::Time::now();
        face.x = det.bbox.x;
        face.y = det.bbox.y;
        face.width = det.bbox.width;
        face.height = det.bbox.height;

        result_msg.faces.push_back(face);
    }

    // ALWAYS publish, including with zero faces. The viewer resets its overlay on
    // every message it receives, so staying silent when a face leaves the frame
    // (or nothing matches) leaves the previous boxes painted on the stream.
    result_pub_.publish(result_msg);
}

std::vector<uint8_t> FaceRecognitionNode::base64Decode(const std::string& encoded) {
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

    return decoded;
}

std::string FaceRecognitionNode::base64Encode(const std::vector<uint8_t>& data) {
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

bool FaceRecognitionNode::addFaceCallback(face_recognition_ros1_interfaces::AddFace::Request& req,
                                          face_recognition_ros1_interfaces::AddFace::Response& res) {
    // With an image but no model, the record would be stored with an EMPTY
    // embedding: it can never be matched, yet the caller is told "Face added
    // successfully". A metadata-only Add (no image) stays allowed.
    if (!req.image_data.empty() && (!detector_ready_ || !recognizer_ready_)) {
        res.success = false;
        res.message = "models not loaded: " + detector_->getLastError() + " | " +
                      recognizer_->getLastError();
        return true;
    }

    if (req.name.empty()) {
        res.success = false;
        res.message = "Name is required";
        return true;
    }

    std::vector<uint8_t> image_data;
    if (!req.image_data.empty()) {
        image_data = base64Decode(req.image_data);
    }

    std::vector<float> embedding;
    if (!image_data.empty() && detector_ && recognizer_) {
        cv::Mat buf(1, static_cast<int>(image_data.size()), CV_8U, image_data.data());
        cv::Mat image = cv::imdecode(buf, cv::IMREAD_COLOR);
        if (!image.empty()) {
            auto detections = detector_->detect(image, 1);
            if (!detections.empty()) {
                face_recognition::EmbeddingPolicy policy;
                auto outcome = face_recognition::make_embedding(
                    *recognizer_, image, detections.front(), policy);
                if (outcome.ok()) {
                    embedding = std::move(outcome.embedding);
                }
            }
        }
    }
    std::string id = database_->add_face(
        req.name,
        embedding,
        image_data,
        req.title,
        req.scene.empty() ? "default" : req.scene,
        req.map_location.empty() ? "unknown" : req.map_location
    );

    res.id = id;
    res.success = !id.empty();
    res.message = id.empty() ? "Failed to add face" : "Face added successfully";
    return true;
}

bool FaceRecognitionNode::removeFaceCallback(face_recognition_ros1_interfaces::RemoveFace::Request& req,
                                             face_recognition_ros1_interfaces::RemoveFace::Response& res) {
    if (req.id.empty()) {
        res.success = false;
        res.message = "Face ID is required";
        return true;
    }

    bool success = database_->remove_face(req.id);
    res.success = success;
    res.message = success ? "Face removed" : "Face not found";
    return true;
}

bool FaceRecognitionNode::listFacesCallback(face_recognition_ros1_interfaces::ListFaces::Request& req,
                                            face_recognition_ros1_interfaces::ListFaces::Response& res) {
    auto faces = database_->list_faces();
    res.count = static_cast<int32_t>(faces.size());

    for (const auto& face : faces) {
        face_recognition_ros1_interfaces::FaceInfo info;
        info.id = face.id;
        info.name = face.name;
        info.title = face.title;
        info.scene = face.scene;
        info.map_location = face.map_location;
        res.faces.push_back(info);
    }
    return true;
}

bool FaceRecognitionNode::clearFacesCallback(face_recognition_ros1_interfaces::ClearFaces::Request& req,
                                             face_recognition_ros1_interfaces::ClearFaces::Response& res) {
    res.deleted_count = database_->clear_all();
    res.success = true;
    return true;
}

bool FaceRecognitionNode::addTemplateCallback(face_recognition_ros1_interfaces::AddTemplate::Request& req,
                                              face_recognition_ros1_interfaces::AddTemplate::Response& res) {
    // A template must come from a model; without one the old code answered
    // "no face detected" instead of "the model is not loaded".
    if (!detector_ready_ || !recognizer_ready_) {
        res.success = false;
        res.message = "models not loaded: " + detector_->getLastError() + " | " +
                      recognizer_->getLastError();
        return true;
    }

    if (req.id.empty()) {
        res.success = false;
        res.message = "id is required";
        return true;
    }
    if (req.image_data.empty()) {
        res.success = false;
        res.message = "image_data is required";
        return true;
    }
    if (!database_->get_face(req.id)) {
        res.success = false;
        res.message = "unknown id: " + req.id;
        return true;
    }

    auto image_data = base64Decode(req.image_data);
    if (image_data.empty()) {
        res.success = false;
        res.message = "image_data is not valid base64";
        return true;
    }
    cv::Mat buf(1, static_cast<int>(image_data.size()), CV_8U, image_data.data());
    cv::Mat image = cv::imdecode(buf, cv::IMREAD_COLOR);
    if (image.empty()) {
        res.success = false;
        res.message = "could not decode image_data";
        return true;
    }

    auto detections = detector_->detect(image, 1);
    if (detections.empty()) {
        res.success = false;
        res.message = "no face detected";
        return true;
    }

    // Same gate as every other writer (see embedding_policy.hpp).
    face_recognition::EmbeddingPolicy policy;
    auto outcome = face_recognition::make_embedding(
        *recognizer_, image, detections.front(), policy);
    if (!outcome.ok()) {
        res.success = false;
        res.message = "refused: " + outcome.reject_reason;
        return true;
    }

    if (!database_->add_template(req.id, outcome.embedding)) {
        res.success = false;
        res.message = "failed to store template";
        return true;
    }

    res.templates = database_->template_count(req.id);
    res.success = true;
    res.message = "template added";
    return true;
}

bool FaceRecognitionNode::verifyCallback(face_recognition_ros1_interfaces::Verify::Request& req,
                                         face_recognition_ros1_interfaces::Verify::Response& res) {
    res.success = false;
    res.matched = false;

    if (!detector_ready_ || !recognizer_ready_) {
        // Distinguishing this from a genuine miss is the point of the service:
        // reporting NO_FACE here would blame the face for a broken model.
        res.decision = "ERROR";
        res.message = "models not loaded: " + detector_->getLastError() + " | " +
                      recognizer_->getLastError();
        return true;
    }

    if (req.image_data.empty()) {
        res.decision = "ERROR";
        res.message = "image_data is required";
        return true;
    }

    auto image_data = base64Decode(req.image_data);
    if (image_data.empty()) {
        res.decision = "ERROR";
        res.message = "image_data is not valid base64";
        return true;
    }
    cv::Mat buf(1, static_cast<int>(image_data.size()), CV_8U, image_data.data());
    cv::Mat image = cv::imdecode(buf, cv::IMREAD_COLOR);
    if (image.empty()) {
        res.decision = "ERROR";
        res.message = "could not decode image_data";
        return true;
    }

    auto detections = detector_->detect(image, 1);
    if (detections.empty()) {
        res.decision = "NO_FACE";
        res.message = "no face detected";
        return true;
    }

    face_recognition::EmbeddingPolicy policy;
    auto outcome = face_recognition::make_embedding(
        *recognizer_, image, detections.front(), policy);
    if (!outcome.ok()) {
        // The gate's reason is the main point of this service: it separates
        // "too small / landmarks unusable" from "genuinely a different person".
        res.decision = "NO_FACE";
        res.message = "refused by quality gate: " + outcome.reject_reason;
        return true;
    }

    const float threshold = (req.threshold > 0.0f) ? req.threshold
                                                   : confidence_threshold_;

    const auto ranked = database_->rank_faces(outcome.embedding);
    auto best = database_->find_matching_face(outcome.embedding, threshold);

    res.threshold = threshold;
    res.success   = true;
    res.matched   = static_cast<bool>(best);
    res.decision  = best ? "ACCEPT" : "REJECT";
    res.message   = best ? "matched" : "no identity above threshold";

    if (!ranked.empty()) {
        res.score = ranked.front().similarity;
        res.id    = best ? best->id : ranked.front().face_id;
        res.name  = best ? best->name : ranked.front().name;
    }

    for (const auto& candidate : ranked) {
        face_recognition_ros1_interfaces::FaceInfo info;
        info.id         = candidate.face_id;
        info.name       = candidate.name;
        info.title      = candidate.title;
        info.confidence = candidate.similarity;
        res.candidates.push_back(info);
    }
    return true;
}

}  // namespace face_recognition_ros1
