#include <ros/ros.h>
#include "face_recognition_ros1/node.hpp"

int main(int argc, char** argv) {
    ros::init(argc, argv, "face_recognition_node");

    face_recognition_ros1::FaceRecognitionNode node;

    ROS_INFO("Face Recognition Node started");
    ROS_INFO("Subscribe to image topic and wait for face recognition results...");

    node.spin();

    return 0;
}
