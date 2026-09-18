#pragma once

#include <ros/ros.h>
#include <sensor_msgs/Image.h>
#include <image_transport/image_transport.h>
#include <cv_bridge/cv_bridge.h>

#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "face_recognition_core/types.hpp"
#include "face_recognition_core/face_detector.hpp"
#include "face_recognition_core/face_recognizer.hpp"
#include "face_recognition_core/face_database.hpp"

#include "face_recognition_ros1_interfaces/FaceInfo.h"
#include "face_recognition_ros1_interfaces/FaceResult.h"
#include "face_recognition_ros1_interfaces/AddFace.h"
#include "face_recognition_ros1_interfaces/RemoveFace.h"
#include "face_recognition_ros1_interfaces/ListFaces.h"
#include "face_recognition_ros1_interfaces/ClearFaces.h"
#include "face_recognition_ros1_interfaces/AddTemplate.h"
#include "face_recognition_ros1_interfaces/Verify.h"

namespace face_recognition_ros1 {

class FaceRecognitionNode {
public:
    FaceRecognitionNode();
    ~FaceRecognitionNode();

    void spin();

private:
    void imageCallback(const sensor_msgs::ImageConstPtr& msg);

    bool addFaceCallback(face_recognition_ros1_interfaces::AddFace::Request& req,
                         face_recognition_ros1_interfaces::AddFace::Response& res);
    bool removeFaceCallback(face_recognition_ros1_interfaces::RemoveFace::Request& req,
                            face_recognition_ros1_interfaces::RemoveFace::Response& res);
    bool listFacesCallback(face_recognition_ros1_interfaces::ListFaces::Request& req,
                           face_recognition_ros1_interfaces::ListFaces::Response& res);
    bool clearFacesCallback(face_recognition_ros1_interfaces::ClearFaces::Request& req,
                            face_recognition_ros1_interfaces::ClearFaces::Response& res);
    bool addTemplateCallback(face_recognition_ros1_interfaces::AddTemplate::Request& req,
                             face_recognition_ros1_interfaces::AddTemplate::Response& res);
    bool verifyCallback(face_recognition_ros1_interfaces::Verify::Request& req,
                        face_recognition_ros1_interfaces::Verify::Response& res);

    std::vector<uint8_t> base64Decode(const std::string& encoded);
    std::string base64Encode(const std::vector<uint8_t>& data);

    std::unique_ptr<face_recognition::FaceDetector> detector_;
    std::unique_ptr<face_recognition::FaceRecognizer> recognizer_;
    std::unique_ptr<face_recognition::FaceDatabase> database_;

    ros::NodeHandle nh_;
    ros::NodeHandle pnh_;
    image_transport::ImageTransport it_;
    image_transport::Subscriber image_sub_;
    ros::Publisher result_pub_;

    ros::ServiceServer add_srv_;
    ros::ServiceServer remove_srv_;
    ros::ServiceServer list_srv_;
    ros::ServiceServer clear_srv_;
    ros::ServiceServer add_template_srv_;
    ros::ServiceServer verify_srv_;

    float confidence_threshold_;
    std::string db_path_;
    std::string faces_dir_;
    std::string detection_model_path_;
    std::string recognition_model_path_;

    // A failed initialize() is NOT visible through the pointers: make_unique()
    // never yields null, so `if (!detector_)` can never fire. Without these flags
    // a missing model surfaces as the business answer "no face detected" instead
    // of "the detector is not loaded".
    bool detector_ready_ = false;
    bool recognizer_ready_ = false;
};

}  // namespace face_recognition_ros1
