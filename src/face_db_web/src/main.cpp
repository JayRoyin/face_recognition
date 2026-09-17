#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <chrono>
#include <filesystem>
#include <vector>

#include <unistd.h>

#include "face_db_web/http_server.hpp"
#include "face_db_web/face_handler.hpp"
#include "face_recognition_core/face_database.hpp"
#include "face_recognition_core/face_detector.hpp"
#include "face_recognition_core/face_recognizer.hpp"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace {

std::unique_ptr<face_db_web::HttpServer> g_server;

const char* kDetName = "det_10g.onnx";
const char* kRecName = "w600k_r50.onnx";

void signalHandler(int signal) {
    std::cout << "Caught signal " << signal << ", shutting down..." << std::endl;
    if (g_server) g_server->stop();
    std::exit(0);
}

/** Directory holding this executable (empty when undeterminable). */
std::string exe_dir() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    return std::filesystem::path(buf).parent_path().string();
}

/**
 * Look for `filename` in the usual places, relative to the CWD and to the
 * executable. `build.sh WEB` installs the binary into install/bin, so
 * ../../models resolves to <project>/models.
 */
std::string discover_model(const std::string& filename) {
    const std::string exe = exe_dir();
    std::vector<std::filesystem::path> candidates = {
        std::filesystem::path("models") / filename,
    };
    if (!exe.empty()) {
        candidates.push_back(std::filesystem::path(exe) / "models" / filename);
        candidates.push_back(std::filesystem::path(exe) / ".." / ".." / "models" / filename);
        candidates.push_back(std::filesystem::path(exe) / ".." / ".." / ".." / "models" / filename);
    }
    for (const auto& p : candidates) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(p, ec)) {
            return std::filesystem::weakly_canonical(p, ec).string();
        }
    }
    return {};
}

void printUsage(const char* prog) {
    std::cout
        << "Usage: " << prog << " [options]\n"
        << "Options:\n"
        << "  --port PORT                  HTTP server port (default: 8080)\n"
        << "  --db PATH                    SQLite database path (required)\n"
        << "  --faces-dir PATH             Directory for face images (required)\n"
        << "  --detection-model PATH       RetinaFace/YOLO .onnx (default: auto-discovered)\n"
        << "  --recognition-model PATH     ArcFace .onnx (default: auto-discovered)\n"
        << "  --allow-no-embedding         Permit saving records WITHOUT an embedding.\n"
        << "                               Such records can never be matched by the\n"
        << "                               recognizer, so this is off by default.\n"
        << "  --help                       Show this help\n"
        << "\n"
        << "Embedding extraction is enabled automatically when both models can be\n"
        << "found (env FACE_DETECTION_MODEL / FACE_RECOGNITION_MODEL, ./models, or\n"
        << "<exe>/../../models).\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    int port = 8080;
    std::string db_path;
    std::string faces_dir;
    std::string detection_model;
    std::string recognition_model;
    bool allow_no_embedding = false;

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
        } else if (arg == "--allow-no-embedding") {
            allow_no_embedding = true;
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

    // ---- model resolution: CLI -> env -> auto-discovery ---------------------
    if (detection_model.empty()) {
        if (const char* e = std::getenv("FACE_DETECTION_MODEL")) detection_model = e;
    }
    if (recognition_model.empty()) {
        if (const char* e = std::getenv("FACE_RECOGNITION_MODEL")) recognition_model = e;
    }
    if (detection_model.empty())   detection_model   = discover_model(kDetName);
    if (recognition_model.empty()) recognition_model = discover_model(kRecName);

    std::shared_ptr<face_recognition::FaceDetector>   detector;
    std::shared_ptr<face_recognition::FaceRecognizer> recognizer;

    if (!detection_model.empty()) {
        detector = std::make_shared<face_recognition::FaceDetector>();
        if (!detector->initialize(detection_model)) {
            std::cerr << "[WARN] detection model failed to load: " << detection_model
                      << " (" << detector->getLastError() << ")\n";
            detector.reset();
        }
    }
    if (!recognition_model.empty() && detector) {
        recognizer = std::make_shared<face_recognition::FaceRecognizer>();
        if (!recognizer->initialize(recognition_model)) {
            std::cerr << "[WARN] recognition model failed to load: " << recognition_model
                      << " (" << recognizer->getLastError() << ")\n";
            recognizer.reset();
        }
    }

    const bool embedding_ready = (detector && recognizer);
    const bool require_embedding = embedding_ready || !allow_no_embedding;

    if (embedding_ready) {
        std::cout << "Detection model:   " << detection_model << std::endl;
        std::cout << "Recognition model: " << recognition_model << std::endl;
    } else {
        std::cerr <<
            "\n"
            "============================================================\n"
            " WARNING: embedding extraction is DISABLED\n"
            "============================================================\n"
            " Faces enrolled now would be stored WITHOUT an embedding and\n"
            " could NEVER be matched by the recognizer.\n"
            "\n"
            " Fix it by pointing the server at the models, e.g.\n"
            "   --detection-model   $(pwd)/models/det_10g.onnx\n"
            "   --recognition-model $(pwd)/models/w600k_r50.onnx\n"
            " or by running from the project root so ./models is found.\n"
            "\n"
            " Enrolment requests will be REJECTED until this is fixed.\n"
            " Pass --allow-no-embedding to override (not recommended).\n"
            "============================================================\n"
            << std::endl;
    }

    face_db_web::FaceHandler handler(database, detector, recognizer, require_embedding);

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
    std::cout << "Embedding extraction: "
              << (embedding_ready ? "ENABLED" : "DISABLED (see warning above)")
              << std::endl;
    std::cout << "Press Ctrl+C to stop" << std::endl;

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(60));
    }

    return 0;
}
