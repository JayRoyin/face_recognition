#include <csignal>
#include <iostream>
#include <memory>
#include <cstdlib>
#include <thread>
#include <chrono>

#include "face_db_web/http_server.hpp"
#include "face_db_web/face_handler.hpp"
#include "face_recognition_core/face_database.hpp"
#include "face_recognition_core/face_detector.hpp"
#include "face_recognition_core/face_recognizer.hpp"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <cstdlib>

namespace {
std::unique_ptr<face_db_web::HttpServer> g_server;
}

void signalHandler(int signal) {
    std::cout << "Caught signal " << signal << ", shutting down..." << std::endl;
    if (g_server) g_server->stop();
    std::exit(0);
}

void printUsage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  --port PORT                  HTTP server port (default: 8080)\n"
              << "  --db PATH                    SQLite database path (required)\n"
              << "  --faces-dir PATH             Directory for face images (required)\n"
              << "  --detection-model PATH       YOLOv8-face .onnx (optional, enables embedding extraction)\n"
              << "  --recognition-model PATH     ArcFace .onnx (optional, requires --detection-model)\n"
              << "  --help                       Show this help\n";
}

int main(int argc, char* argv[]) {
    int port = 8080;
    std::string db_path;
    std::string faces_dir;
    std::string detection_model;
    std::string recognition_model;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if (arg == "--db" && i + 1 < argc) {
            db_path = argv[++i];
        } else if (arg == "--faces-dir" && i + 1 < argc) {
            faces_dir = argv[++i];
        } else if (arg == "--detection-model" && i + 1 < argc) {
            detection_model = argv[++i];
        } else if (arg == "--recognition-model" && i + 1 < argc) {
            recognition_model = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << std::endl;
            printUsage(argv[0]);
            return 1;
        }
    }

    if (db_path.empty() || faces_dir.empty()) {
        std::cerr << "Error: --db and --faces-dir are required" << std::endl;
        printUsage(argv[0]);
        return 1;
    }

    auto database = std::make_shared<face_recognition::FaceDatabase>();
    if (!database->initialize(db_path, faces_dir)) {
        std::cerr << "Failed to initialize database at: " << db_path << std::endl;
        return 1;
    }

    std::shared_ptr<face_recognition::FaceDetector>   detector;
    std::shared_ptr<face_recognition::FaceRecognizer> recognizer;

    // Allow env-var fallback so the user doesn't always need CLI flags.
    if (detection_model.empty()) {
        if (const char* e = std::getenv("FACE_DETECTION_MODEL"))   detection_model   = e;
    }
    if (recognition_model.empty()) {
        if (const char* e = std::getenv("FACE_RECOGNITION_MODEL")) recognition_model = e;
    }

    if (!detection_model.empty()) {
        detector = std::make_shared<face_recognition::FaceDetector>();
        if (!detector->initialize(detection_model)) {
            std::cerr << "[WARN] detection model failed to load: " << detection_model
                      << " (" << detector->getLastError() << ")"
                      << " — embeddings will be NULL.\n";
            detector.reset();
        } else {
            std::cout << "Detection model: " << detection_model << std::endl;
        }
    }
    if (!recognition_model.empty() && detector) {
        recognizer = std::make_shared<face_recognition::FaceRecognizer>();
        if (!recognizer->initialize(recognition_model)) {
            std::cerr << "[WARN] recognition model failed to load: " << recognition_model
                      << " (" << recognizer->getLastError() << ")"
                      << " — embeddings will be NULL.\n";
            recognizer.reset();
        } else {
            std::cout << "Recognition model: " << recognition_model << std::endl;
        }
    }

    face_db_web::FaceHandler handler(database, detector, recognizer);

    g_server = std::make_unique<face_db_web::HttpServer>(port);
    g_server->get("/", [&handler](const face_db_web::HttpRequest& req) { return handler.index(req); });
    g_server->get("/api/faces", [&handler](const face_db_web::HttpRequest& req) { return handler.listFaces(req); });
    g_server->post("/api/faces/add", [&handler](const face_db_web::HttpRequest& req) { return handler.addFace(req); });
    g_server->post("/api/faces/remove", [&handler](const face_db_web::HttpRequest& req) { return handler.removeFace(req); });
    g_server->post("/api/faces/clear", [&handler](const face_db_web::HttpRequest& req) { return handler.clearFaces(req); });
    g_server->get("/api/image/", [&handler](const face_db_web::HttpRequest& req) { return handler.getImage(req); });

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    if (!g_server->start()) {
        std::cerr << "Failed to start HTTP server" << std::endl;
        return 1;
    }

    std::cout << "Face DB Web running on http://localhost:" << port << std::endl;
    std::cout << "Database: " << db_path << std::endl;
    std::cout << "Faces dir: " << faces_dir << std::endl;
    if (detector && recognizer) {
        std::cout << "Embedding extraction: ENABLED" << std::endl;
    } else {
        std::cout << "Embedding extraction: DISABLED (records will be stored with NULL embedding)"
                  << std::endl;
    }
    std::cout << "Press Ctrl+C to stop" << std::endl;

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(60));
    }

    return 0;
}
