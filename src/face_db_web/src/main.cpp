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

/** Flags that relax the confirmation gate; applied after the file is read. */
struct PolicyOverrides {
    bool auto_cross_scene = false;
    bool no_confirm       = false;
} policy_overrides;

const char* kDetName = "det_10g.onnx";
const char* kRecName = "w600k_r50.onnx";
const char* kGenderName = "genderage.onnx";

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
        << "  --det-threshold F            Detection confidence threshold, 0..1\n"
        << "                               (default: 0.5; lower it, e.g. 0.3, when\n"
        << "                               curated ID photos are missed)\n"
        << "  --recognition-model PATH     ArcFace .onnx (default: auto-discovered)\n"
        << "  --allow-no-embedding         Permit saving records WITHOUT an embedding.\n"
        << "                               Such records can never be matched by the\n"
        << "                               recognizer, so this is off by default.\n"
        << "  --max-upload-mb MB           Max request body, for bulk import\n"
        << "                               (default: 512; a zip of photos or a\n"
        << "                               grid of images is posted in one request)\n"
        << "\n"
        << "Re-enrolment policy (uploads that look like someone already stored):\n"
        << "  --enroll-policy PATH         JSON rules file (env: FACE_ENROLL_POLICY)\n"
        << "  --high-similarity F          Similarity that triggers confirmation\n"
        << "                               (default: 0.80)\n"
        << "  --auto-cross-scene           Do NOT ask when the match is in another\n"
        << "                               scene (same-scene still always asks)\n"
        << "  --no-confirm                 DANGEROUS: write everything without\n"
        << "                               asking, even above the threshold\n"
        << "  --print-enroll-policy        Print a documented policy template\n"
        << "\n"
        << "Gender auto-fill (optional, only fills an EMPTY gender field):\n"
        << "  --auto-gender                Use genderage.onnx to pre-fill gender\n"
        << "  --gender-model PATH          Gender/age .onnx (default: auto-discovered)\n"
        << "  --gender-male-index N        Which output logit means \"male\"\n"
        << "                               (default: 1; flip to 0 if results look\n"
        << "                               inverted for your model export)\n"
        << "\n"
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
    int  max_upload_mb = 512;
    float det_threshold = 0.5f;
    std::string enroll_policy_path;
    bool cli_high_similarity_set = false;
    float cli_high_similarity = 0.80f;
    std::string gender_model;
    bool auto_gender = false;
    int  gender_male_index = 1;

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
        } else if (arg == "--max-upload-mb" && i + 1 < argc) {
            max_upload_mb = std::atoi(argv[++i]);
        } else if (arg == "--det-threshold" && i + 1 < argc) {
            det_threshold = static_cast<float>(std::atof(argv[++i]));
        } else if (arg == "--enroll-policy" && i + 1 < argc) {
            enroll_policy_path = argv[++i];
        } else if (arg == "--high-similarity" && i + 1 < argc) {
            cli_high_similarity = static_cast<float>(std::atof(argv[++i]));
            cli_high_similarity_set = true;
        } else if (arg == "--auto-cross-scene") {
            policy_overrides.auto_cross_scene = true;
        } else if (arg == "--no-confirm") {
            policy_overrides.no_confirm = true;
        } else if (arg == "--auto-gender") {
            auto_gender = true;
        } else if (arg == "--gender-model" && i + 1 < argc) {
            gender_model = argv[++i];
        } else if (arg == "--gender-male-index" && i + 1 < argc) {
            gender_male_index = std::atoi(argv[++i]);
        } else if (arg == "--print-enroll-policy") {
            std::cout << face_db_web::defaultPolicyJson() << std::endl;
            return 0;
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
    if (max_upload_mb < 1) max_upload_mb = 1;
    if (det_threshold <= 0.0f || det_threshold >= 1.0f) det_threshold = 0.5f;

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
        if (!detector->initialize(detection_model, det_threshold)) {
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

    // ---- re-enrolment policy ----------------------------------------------
    face_db_web::EnrollPolicy policy;
    if (enroll_policy_path.empty()) {
        if (const char* e = std::getenv("FACE_ENROLL_POLICY")) enroll_policy_path = e;
    }
    std::string policy_err;
    if (!enroll_policy_path.empty()) {
        if (!face_db_web::loadEnrollPolicy(enroll_policy_path, policy, policy_err)) {
            std::cerr << "[ERROR] enroll policy: " << policy_err << std::endl;
            return 1;
        }
        std::cout << "Enroll policy:      " << enroll_policy_path << std::endl;
    }
    if (cli_high_similarity_set) policy.high_similarity = cli_high_similarity;
    if (policy_overrides.auto_cross_scene) policy.confirm_on_cross_scene = false;
    if (policy_overrides.no_confirm) {
        policy.require_confirmation = false;
        std::cerr << "\n[WARNING] --no-confirm: highly similar faces will be written "
                     "WITHOUT asking.\n" << std::endl;
    }

    std::cout << "Re-enrolment:       confirm at similarity >= "
              << policy.high_similarity
              << (policy.require_confirmation
                      ? std::string(" (same-scene: ") +
                            (policy.confirm_on_same_scene ? "ask" : "auto") +
                            ", cross-scene: " +
                            (policy.confirm_on_cross_scene ? "ask" : "auto") + ")"
                      : std::string(" DISABLED"))
              << std::endl;

    // ---- optional gender auto-fill ----------------------------------------
    std::shared_ptr<face_recognition::GenderClassifier> gender_clf;
    if (auto_gender) {
        if (gender_model.empty()) {
            if (const char* e = std::getenv("FACE_GENDER_MODEL")) gender_model = e;
        }
        if (gender_model.empty()) gender_model = discover_model(kGenderName);
        if (gender_model.empty()) {
            std::cerr << "[WARN] --auto-gender: " << kGenderName
                      << " not found, gender stays 'unknown'\n";
        } else {
            gender_clf = std::make_shared<face_recognition::GenderClassifier>();
            if (!gender_clf->initialize(gender_model, gender_male_index)) {
                std::cerr << "[WARN] gender model failed to load: "
                          << gender_clf->getLastError() << "\n";
                gender_clf.reset();
            } else {
                std::cout << "Gender model:       " << gender_model << "\n"
                          << "Gender model shape: " << gender_clf->shapeInfo() << "\n";
            }
        }
    }

    face_db_web::FaceHandler handler(database, detector, recognizer, require_embedding,
                                     policy, gender_clf);

    // Bulk import posts a whole archive (or a grid of images) in one request,
    // so the body cap has to be much larger than the old fixed 16 MiB.
    const std::size_t max_body =
        static_cast<std::size_t>(max_upload_mb) * 1024u * 1024u;

    g_server = std::make_unique<face_db_web::HttpServer>(port, max_body);
    g_server->get("/", [&handler](const face_db_web::HttpRequest& req) { return handler.index(req); });
    g_server->get("/api/faces", [&handler](const face_db_web::HttpRequest& req) { return handler.listFaces(req); });
    g_server->post("/api/faces/add", [&handler](const face_db_web::HttpRequest& req) { return handler.addFace(req); });
    g_server->post("/api/faces/remove", [&handler](const face_db_web::HttpRequest& req) { return handler.removeFace(req); });
    g_server->post("/api/faces/clear", [&handler](const face_db_web::HttpRequest& req) { return handler.clearFaces(req); });
    g_server->post("/api/faces/update", [&handler](const face_db_web::HttpRequest& req) { return handler.updateFace(req); });
    g_server->post("/api/faces/add-template", [&handler](const face_db_web::HttpRequest& req) { return handler.addTemplate(req); });
    g_server->post("/api/faces/import-batch", [&handler](const face_db_web::HttpRequest& req) { return handler.importBatch(req); });
    g_server->post("/api/faces/import-archive", [&handler](const face_db_web::HttpRequest& req) { return handler.importArchive(req); });
    g_server->post("/api/faces/resolve", [&handler](const face_db_web::HttpRequest& req) { return handler.resolvePending(req); });
    g_server->get("/api/faces/pending", [&handler](const face_db_web::HttpRequest& req) { return handler.listPending(req); });
    g_server->get("/api/config", [&handler](const face_db_web::HttpRequest& req) { return handler.getConfig(req); });
    g_server->get("/api/pending/image/", [&handler](const face_db_web::HttpRequest& req) { return handler.pendingImage(req); });
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
    std::cout << "Max request body:    " << max_upload_mb << " MB" << std::endl;
    std::cout << "Press Ctrl+C to stop" << std::endl;

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(60));
    }

    return 0;
}
