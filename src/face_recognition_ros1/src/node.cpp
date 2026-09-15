#include "face_recognition_ros1/node.hpp"

#include <ctime>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <opencv2/imgcodecs.hpp>

namespace face_recognition_ros1 {

FaceRecognitionNode::FaceRecognitionNode()
    : nh_("~"), pnh_(""), it_(nh_) {

    nh_.param<float>("confidence_threshold", confidence_threshold_, 0.7f);
    nh_.param<std::string>("db_path", db_path_, "/tmp/face_db/faces.db");
    nh_.param<std::string>("faces_dir", faces_dir_, "/tmp/face_db/faces");
    nh_.param<std::string>("detection_model", detection_model_path_, "/models/det_10g.onnx");
    nh_.param<std::string>("recognition_model", recognition_model_path_, "/models/w600k_r50.onnx");

    ROS_INFO("Initializing Face Recognition Node...");
    ROS_INFO("Detection model: %s", detection_model_path_.c_str());
    ROS_INFO("Recognition model: %s", recognition_model_path_.c_str());

    detector_ = std::make_unique<face_recognition::FaceDetector>();
    if (!detector_->initialize(detection_model_path_, confidence_threshold_)) {
        ROS_ERROR("Failed to initialize detector");
    } else {
        ROS_INFO("Face detector initialized");
    }

    recognizer_ = std::make_unique<face_recognition::FaceRecognizer>();
    if (!recognizer_->initialize(recognition_model_path_)) {
        ROS_ERROR("Failed to initialize recognizer");
    } else {
        ROS_INFO("Face recognizer initialized");
    }

    database_ = std::make_unique<face_recognition::FaceDatabase>();
    if (!database_->initialize(db_path_, faces_dir_)) {
        ROS_ERROR("Failed to initialize database");
    } else {
        for (const auto& face : database_->list_faces()) {
            if (!detector_ || !recognizer_) break;
            if (!face.embedding.empty() || face.image_path.empty()) continue;
            cv::Mat image = cv::imread(face.image_path, cv::IMREAD_COLOR);
            if (image.empty()) continue;
            auto detections = detector_->detect(image, 1);
            if (!detections.empty()) {
                auto embedding = recognizer_->extract_embedding(image, detections.front().bbox);
                if (!embedding.empty()) database_->update_embedding(face.id, embedding);
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

    auto detections = detector_->detect(image, 10);
    if (detections.empty()) {
        return;
    }

    face_recognition_ros1_interfaces::FaceResult result_msg;
    result_msg.header = msg->header;

    for (const auto& det : detections) {
        auto embedding = recognizer_->extract_embedding(image, det.bbox);
        if (embedding.empty()) {
            continue;
        }

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

    if (!result_msg.faces.empty()) {
        result_pub_.publish(result_msg);
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
            if (!detections.empty()) embedding = recognizer_->extract_embedding(image, detections.front().bbox);
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

}  // namespace face_recognition_ros1
