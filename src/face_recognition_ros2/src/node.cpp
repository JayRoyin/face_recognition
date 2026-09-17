#include "face_recognition_ros2/node.hpp"

#include "face_recognition_core/embedding_policy.hpp"
#include "face_recognition_core/feature_space.hpp"

#include <ctime>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <algorithm>
#include <cstring>

namespace face_recognition_ros2 {

FaceRecognitionNode::FaceRecognitionNode()
    : Node("face_recognition_node") {

    this->declare_parameter("image_topic", "/image_raw");
    this->declare_parameter("result_topic", "/face/recognition_result");
    // 0.5, matching the standalone app / web UI. This value doubles as BOTH the
    // detector confidence and the recognition threshold, and 0.7 sits inside the
    // "genuine" cluster: it silently rejects a large share of correct matches.
    this->declare_parameter("confidence_threshold", 0.5f);
    this->declare_parameter("db_path", "/tmp/face_db/faces.db");
    this->declare_parameter("faces_dir", "/tmp/face_db/faces");
    this->declare_parameter("detection_model", "models/det_10g.onnx");
    this->declare_parameter("recognition_model", "models/w600k_r50.onnx");

    std::string image_topic, result_topic;
    this->get_parameter("image_topic", image_topic);
    this->get_parameter("result_topic", result_topic);
    this->get_parameter("confidence_threshold", confidence_threshold_);
    this->get_parameter("db_path", db_path_);
    this->get_parameter("faces_dir", faces_dir_);
    this->get_parameter("detection_model", detection_model_path_);
    this->get_parameter("recognition_model", recognition_model_path_);

    RCLCPP_INFO(this->get_logger(), "Initializing Face Recognition Node...");
    RCLCPP_INFO(this->get_logger(), "Detection model: %s", detection_model_path_.c_str());
    RCLCPP_INFO(this->get_logger(), "Recognition model: %s", recognition_model_path_.c_str());

    detector_ = std::make_unique<face_recognition::FaceDetector>();
    if (!detector_->initialize(detection_model_path_, confidence_threshold_)) {
        RCLCPP_ERROR(this->get_logger(), "Failed to initialize detector: %s",
                    detector_->getLastError().c_str());
    } else {
        RCLCPP_INFO(this->get_logger(), "Face detector initialized");
    }

    recognizer_ = std::make_unique<face_recognition::FaceRecognizer>();
    if (!recognizer_->initialize(recognition_model_path_)) {
        RCLCPP_ERROR(this->get_logger(), "Failed to initialize recognizer: %s",
                    recognizer_->getLastError().c_str());
    } else {
        RCLCPP_INFO(this->get_logger(), "Face recognizer initialized");
    }

    database_ = std::make_unique<face_recognition::FaceDatabase>();
    if (!database_->initialize(db_path_, faces_dir_)) {
        RCLCPP_ERROR(this->get_logger(), "Failed to initialize database");
    } else {
        for (const auto& face : database_->list_faces()) {
            if (!detector_ || !recognizer_) break;
            if (!face.embedding.empty() || face.image_path.empty()) continue;
            cv::Mat image = cv::imread(face.image_path, cv::IMREAD_COLOR);
            if (image.empty()) continue;
            auto detections = detector_->detect(image, 1);
            if (!detections.empty()) {
                // NOTE: this used to call extract_embedding(image, bbox) WITHOUT
                // landmarks, which silently fell back to the bbox-crop front-end
                // while this same node's recognition path used alignment. The two
                // front-ends live in different feature sub-spaces, so a record
                // backfilled here could never match a query from this very node.
                face_recognition::EmbeddingPolicy policy;
                auto outcome = face_recognition::make_embedding(
                    *recognizer_, image, detections.front(), policy);
                if (outcome.ok()) {
                    database_->update_embedding(face.id, outcome.embedding);
                } else {
                    RCLCPP_WARN(this->get_logger(),
                                "backfill skipped id=%s: %s",
                                face.id.c_str(), outcome.reject_reason.c_str());
                }
            }
        }
        RCLCPP_INFO(this->get_logger(), "Face database initialized with %d faces",
                   database_->get_face_count());

        // Guard against a gallery built by a different recipe (see
        // feature_space.hpp). Silent here would mean every match is meaningless.
        const std::string current_fs =
            face_recognition::feature_space_id(recognition_model_path_);
        std::string stored_fs;
        const auto status = face_recognition::check_feature_space(
            *database_, current_fs, &stored_fs);
        if (status == face_recognition::FeatureSpaceStatus::MISMATCH) {
            RCLCPP_WARN(this->get_logger(),
                        "gallery feature space mismatch: stored='%s' current='%s'. "
                        "Every comparison is meaningless until the gallery is "
                        "rebuilt (face_recognition_app backfill --all).",
                        stored_fs.c_str(), current_fs.c_str());
        } else if (status == face_recognition::FeatureSpaceStatus::STAMPED) {
            RCLCPP_INFO(this->get_logger(), "gallery feature space recorded: %s",
                        current_fs.c_str());
        }
    }

    image_sub_ = this->create_subscription<sensor_msgs::msg::Image>(
        image_topic, 10,
        std::bind(&FaceRecognitionNode::imageCallback, this, std::placeholders::_1));

    result_pub_ = this->create_publisher<face_recognition_ros2_interfaces::msg::FaceResult>(result_topic, 10);

    add_srv_ = this->create_service<face_recognition_ros2_interfaces::srv::AddFace>(
        "/face_db/add",
        [this](const std::shared_ptr<face_recognition_ros2_interfaces::srv::AddFace::Request> req,
               const std::shared_ptr<face_recognition_ros2_interfaces::srv::AddFace::Response> res) {
            this->addFaceCallback(req, res);
        });

    remove_srv_ = this->create_service<face_recognition_ros2_interfaces::srv::RemoveFace>(
        "/face_db/remove",
        [this](const std::shared_ptr<face_recognition_ros2_interfaces::srv::RemoveFace::Request> req,
               const std::shared_ptr<face_recognition_ros2_interfaces::srv::RemoveFace::Response> res) {
            this->removeFaceCallback(req, res);
        });

    list_srv_ = this->create_service<face_recognition_ros2_interfaces::srv::ListFaces>(
        "/face_db/list",
        [this](const std::shared_ptr<face_recognition_ros2_interfaces::srv::ListFaces::Request> req,
               const std::shared_ptr<face_recognition_ros2_interfaces::srv::ListFaces::Response> res) {
            this->listFacesCallback(req, res);
        });

    clear_srv_ = this->create_service<face_recognition_ros2_interfaces::srv::ClearFaces>(
        "/face_db/clear",
        [this](const std::shared_ptr<face_recognition_ros2_interfaces::srv::ClearFaces::Request> req,
               const std::shared_ptr<face_recognition_ros2_interfaces::srv::ClearFaces::Response> res) {
            this->clearFacesCallback(req, res);
        });

    add_template_srv_ = this->create_service<face_recognition_ros2_interfaces::srv::AddTemplate>(
        "/face_db/add_template",
        [this](const std::shared_ptr<face_recognition_ros2_interfaces::srv::AddTemplate::Request> req,
               const std::shared_ptr<face_recognition_ros2_interfaces::srv::AddTemplate::Response> res) {
            this->addTemplateCallback(req, res);
        });

    verify_srv_ = this->create_service<face_recognition_ros2_interfaces::srv::Verify>(
        "/face_db/verify",
        [this](const std::shared_ptr<face_recognition_ros2_interfaces::srv::Verify::Request> req,
               const std::shared_ptr<face_recognition_ros2_interfaces::srv::Verify::Response> res) {
            this->verifyCallback(req, res);
        });

    RCLCPP_INFO(this->get_logger(), "Face Recognition Node initialized");
}

FaceRecognitionNode::~FaceRecognitionNode() = default;

void FaceRecognitionNode::imageCallback(const sensor_msgs::msg::Image::ConstSharedPtr& msg) {
    try {
        cv_bridge::CvImageConstPtr cv_ptr = cv_bridge::toCvShare(msg, "bgr8");
        const cv::Mat& image = cv_ptr->image;

        if (image.empty()) {
            return;
        }

        auto detections = detector_->detect(image, 10);
        if (detections.empty()) {
            return;
        }

        face_recognition_ros2_interfaces::msg::FaceResult result_msg;
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

            face_recognition_ros2_interfaces::msg::FaceInfo face;
            face.id = match->id;
            face.name = match->name;
            face.title = match->title;
            face.confidence = face_recognition::compute_similarity(embedding, match->embedding);
            face.scene = match->scene;
            face.map_location = match->map_location;
            face.x = det.bbox.x;
            face.y = det.bbox.y;
            face.width = det.bbox.width;
            face.height = det.bbox.height;

            result_msg.faces.push_back(face);
        }

        if (!result_msg.faces.empty()) {
            result_pub_->publish(result_msg);
        }

    } catch (const cv_bridge::Exception& e) {
        RCLCPP_ERROR(this->get_logger(), "CV Bridge error: %s", e.what());
    } catch (const std::exception& e) {
        RCLCPP_ERROR(this->get_logger(), "Error: %s", e.what());
    }
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

void FaceRecognitionNode::addFaceCallback(const std::shared_ptr<face_recognition_ros2_interfaces::srv::AddFace::Request> req,
                                        const std::shared_ptr<face_recognition_ros2_interfaces::srv::AddFace::Response> res) {
    std::vector<uint8_t> image_data;

    if (!req->image_data.empty()) {
        image_data = base64Decode(req->image_data);
    }

    if (req->name.empty()) {
        res->success = false;
        res->message = "Name is required";
        return;
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
        req->name,
        embedding,
        image_data,
        req->title,
        req->scene.empty() ? "default" : req->scene,
        req->map_location.empty() ? "unknown" : req->map_location
    );

    res->id = id;
    res->success = !id.empty();
    res->message = id.empty() ? "Failed to add face" : "Face added successfully";
}

void FaceRecognitionNode::removeFaceCallback(const std::shared_ptr<face_recognition_ros2_interfaces::srv::RemoveFace::Request> req,
                                           const std::shared_ptr<face_recognition_ros2_interfaces::srv::RemoveFace::Response> res) {
    if (req->id.empty()) {
        res->success = false;
        res->message = "Face ID is required";
        return;
    }

    bool success = database_->remove_face(req->id);
    res->success = success;
    res->message = success ? "Face removed" : "Face not found";
}

void FaceRecognitionNode::listFacesCallback(const std::shared_ptr<face_recognition_ros2_interfaces::srv::ListFaces::Request> req,
                                          const std::shared_ptr<face_recognition_ros2_interfaces::srv::ListFaces::Response> res) {
    auto faces = database_->list_faces();
    res->count = static_cast<int>(faces.size());

    for (const auto& face : faces) {
        face_recognition_ros2_interfaces::msg::FaceInfo info;
        info.id = face.id;
        info.name = face.name;
        info.title = face.title;
        info.scene = face.scene;
        info.map_location = face.map_location;
        res->faces.push_back(info);
    }
}

void FaceRecognitionNode::clearFacesCallback(const std::shared_ptr<face_recognition_ros2_interfaces::srv::ClearFaces::Request> req,
                                           const std::shared_ptr<face_recognition_ros2_interfaces::srv::ClearFaces::Response> res) {
    res->deleted_count = database_->clear_all();
    res->success = true;
}

void FaceRecognitionNode::addTemplateCallback(
        const std::shared_ptr<face_recognition_ros2_interfaces::srv::AddTemplate::Request> req,
        const std::shared_ptr<face_recognition_ros2_interfaces::srv::AddTemplate::Response> res) {
    if (req->id.empty()) {
        res->success = false;
        res->message = "id is required";
        return;
    }
    if (req->image_data.empty()) {
        res->success = false;
        res->message = "image_data is required";
        return;
    }
    if (!database_->get_face(req->id)) {
        res->success = false;
        res->message = "unknown id: " + req->id;
        return;
    }

    auto image_data = base64Decode(req->image_data);
    if (image_data.empty()) {
        res->success = false;
        res->message = "image_data is not valid base64";
        return;
    }
    cv::Mat buf(1, static_cast<int>(image_data.size()), CV_8U, image_data.data());
    cv::Mat image = cv::imdecode(buf, cv::IMREAD_COLOR);
    if (image.empty()) {
        res->success = false;
        res->message = "could not decode image_data";
        return;
    }

    auto detections = detector_->detect(image, 1);
    if (detections.empty()) {
        res->success = false;
        res->message = "no face detected";
        return;
    }

    // Same gate as every other writer. A template produced under different rules
    // would sit in a different feature sub-space than the rest of the gallery.
    face_recognition::EmbeddingPolicy policy;
    auto outcome = face_recognition::make_embedding(
        *recognizer_, image, detections.front(), policy);
    if (!outcome.ok()) {
        res->success = false;
        res->message = "refused: " + outcome.reject_reason;
        return;
    }

    if (!database_->add_template(req->id, outcome.embedding)) {
        res->success = false;
        res->message = "failed to store template";
        return;
    }

    res->templates = database_->template_count(req->id);
    res->success = true;
    res->message = "template added";
}

void FaceRecognitionNode::verifyCallback(
        const std::shared_ptr<face_recognition_ros2_interfaces::srv::Verify::Request> req,
        const std::shared_ptr<face_recognition_ros2_interfaces::srv::Verify::Response> res) {
    res->success = false;
    res->matched = false;

    if (req->image_data.empty()) {
        res->decision = "ERROR";
        res->message = "image_data is required";
        return;
    }

    auto image_data = base64Decode(req->image_data);
    if (image_data.empty()) {
        res->decision = "ERROR";
        res->message = "image_data is not valid base64";
        return;
    }
    cv::Mat buf(1, static_cast<int>(image_data.size()), CV_8U, image_data.data());
    cv::Mat image = cv::imdecode(buf, cv::IMREAD_COLOR);
    if (image.empty()) {
        res->decision = "ERROR";
        res->message = "could not decode image_data";
        return;
    }

    auto detections = detector_->detect(image, 1);
    if (detections.empty()) {
        res->decision = "NO_FACE";
        res->message = "no face detected";
        return;
    }

    face_recognition::EmbeddingPolicy policy;
    auto outcome = face_recognition::make_embedding(
        *recognizer_, image, detections.front(), policy);
    if (!outcome.ok()) {
        // Reporting the gate's reason is the main point of this service: it
        // separates "face too small / landmarks unusable" from "genuinely a
        // different person", which the run-time result message cannot express.
        res->decision = "NO_FACE";
        res->message = "refused by quality gate: " + outcome.reject_reason;
        return;
    }

    const float threshold = (req->threshold > 0.0f) ? req->threshold
                                                    : confidence_threshold_;

    // Ranking carries the per-identity scores; the decision itself goes through
    // find_matching_face() so it follows exactly the same code path as the
    // run-time imageCallback.
    const auto ranked = database_->rank_faces(outcome.embedding);
    auto best = database_->find_matching_face(outcome.embedding, threshold);

    res->threshold = threshold;
    res->success   = true;
    res->matched   = static_cast<bool>(best);
    res->decision  = best ? "ACCEPT" : "REJECT";
    res->message   = best ? "matched" : "no identity above threshold";

    if (!ranked.empty()) {
        res->score = ranked.front().similarity;
        res->id    = best ? best->id : ranked.front().face_id;
        res->name  = best ? best->name : ranked.front().name;
    }

    for (const auto& candidate : ranked) {
        face_recognition_ros2_interfaces::msg::FaceInfo info;
        info.id           = candidate.face_id;
        info.name         = candidate.name;
        info.title        = candidate.title;
        info.confidence   = candidate.similarity;
        res->candidates.push_back(info);
    }
}

}  // namespace face_recognition_ros2
