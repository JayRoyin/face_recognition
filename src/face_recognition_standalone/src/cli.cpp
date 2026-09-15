#include "face_recognition_standalone/cli.hpp"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

namespace face_recognition_standalone {

CliArgs parse_cli(int argc, char** argv) {
    CliArgs out;
    if (argc < 2) {
        out.command = "help";
        return out;
    }

    out.command = argv[1];

    auto need_value = [&](int i, const char* flag) -> std::string {
        if (i + 1 >= argc) {
            std::fprintf(stderr, "Missing value for %s\n", flag);
            std::exit(2);
        }
        return argv[i + 1];
    };

    for (int i = 2; i < argc; ++i) {
        const char* a = argv[i];
        if (a[0] != '-' || a[1] == '\0') {
            out.positional.emplace_back(a);
            continue;
        }
        std::string flag = a;
        // --key=value
        auto eq = flag.find('=');
        std::string key, val;
        bool has_inline_val = false;
        if (eq != std::string::npos) {
            key = flag.substr(0, eq);
            val = flag.substr(eq + 1);
            has_inline_val = true;
        } else {
            key = flag;
        }

        // Boolean flags with no value
        if (key == "-h" || key == "--help") {
            out.opts["help"] = "1";
            continue;
        }

        if (has_inline_val) {
            out.opts[key] = val;
            continue;
        }

        // Treat next token as value if it does not start with '-'
        if (i + 1 < argc && argv[i + 1][0] != '-') {
            out.opts[key] = argv[i + 1];
            ++i;
        } else {
            out.opts[key] = "1";
        }
    }
    (void)need_value;  // silence unused-lambda warning when no error triggered
    return out;
}

void print_usage(const char* prog) {
    std::printf(
        "Usage: %s <command> [options]\n"
        "\n"
        "Commands:\n"
        "  run         Real-time face recognition from a camera / stream / file.\n"
        "  add         Register a face from an image file into the database.\n"
        "  add-bulk    Register every image in a directory (filename -> name).\n"
        "  backfill    Re-extract embeddings for existing DB rows whose image_path is set\n"
        "              but embedding is empty. Use after importing a DB that contains face\n"
        "              thumbnails but no embeddings yet.\n"
        "  list        List all registered faces.\n"
        "  remove      Remove a face by ID.\n"
        "  clear       Remove all faces from the database.\n"
        "  web         Launch the bundled face_db_web UI on the same DB. Use --web-binary\n"
        "              if face_db_web is not on PATH; it defaults to ./install/bin/face_db_web.\n"
        "  help        Print this help message.\n"
        "\n"
        "Common options (all commands):\n"
        "  --detection-model <path>     ONNX detection model (default: models/det_10g.onnx)\n"
        "  --recognition-model <path>   ONNX recognition model (default: models/w600k_r50.onnx)\n"
        "  --db <path>                  SQLite database file (default: /tmp/face_db/faces.db)\n"
        "  --faces-dir <path>           Directory to store face thumbnails (default: /tmp/face_db/faces)\n"
        "  --detection-threshold <f>    Face detection confidence (default: 0.5)\n"
        "  --recognition-threshold <f>  Recognition similarity (default: 0.5)\n"
        "                              NOTE: 0.7 (the old default) rejects most live-camera\n"
        "                              matches of the same person. Raise only if you see\n"
        "                              false positives across different people.\n"
        "  --nms-threshold <f>          NMS IoU threshold (default: 0.5)\n"
        "  --input-size <n>             Detector square input size (default: 640)\n"
        "\n"
        "Options for `run`:\n"
        "  --source <uri>               '0' = default USB camera, '/dev/video1' (digits), an RTSP/HTTP URL,\n"
        "                              a video file path, or a directory of images.\n"
        "                              If omitted, tries the default USB camera (index 0).\n"
        "  --width <n>  --height <n>    Requested capture resolution (default: 1280x720).\n"
        "                              More pixels => more facial detail for the\n"
        "                              recognizer. Use 0 to keep the camera default,\n"
        "                              or e.g. 1920x1080 on a capable webcam.\n"
        "  --fps <n>                    Requested capture FPS.\n"
        "  --loop                       Loop the video when it ends.\n"
        "  --no-display                 Don't open an OpenCV window (headless mode).\n"
        "  --max-faces <n>              Max faces per frame (default: 10).\n"
        "  --detect-every-n <n>         Skip detection on N-1 frames (default: 2 = detect on\n"
        "                              every 2nd frame; skipped frames reuse the cached boxes\n"
        "                              while recognition still runs). Use 1 for every frame.\n"
        "  --downscale <n>              Shrink the frame so its longest side <= n BEFORE detection.\n"
        "                              Big FPS win for HD cameras; default 640 (no shrink).\n"
        "                              Try 480 or 320 if FPS is low.\n"
        "  --no-recognition             Skip recognition, only draw detected boxes.\n"
        "  --target-fps <n>             Cap the recognition loop at N frames per second\n"
        "                              (default: 24; 0 = unlimited). Recognition still runs\n"
        "                              on every processed frame; this only limits the loop\n"
        "                              rate on hardware faster than N fps.\n"
        "  --auto-backfill              At startup, re-extract embeddings for any DB row that\n"
        "                              has an image_path but an empty embedding (handy after\n"
        "                              web uploads or when the DB was populated without models).\n"
        "  --save-video <path>          Save annotated output to an MP4 file.\n"
        "  --snapshot-dir <path>        When a recognized face appears, save a snapshot.\n"
        "\n"
        "Options for `add`:\n"
        "  --image <path>               Path to the image containing the face. REQUIRED.\n"
        "  --name <string>              Person name. REQUIRED.\n"
        "  --title <string>             Optional title.\n"
        "  --scene <string>             Scene tag (default: default).\n"
        "  --map-location <string>      Map location tag (default: unknown).\n"
        "\n"
        "Options for `add-bulk`:\n"
        "  --dir <path>                 Directory containing face images. REQUIRED.\n"
        "                              Each filename (without extension) becomes the name.\n"
        "  --scene <string>             Scene tag (default: default).\n"
        "  --map-location <string>      Map location tag (default: unknown).\n"
        "  --recursive                  Recurse into subdirectories (default: off).\n"
        "\n"
        "Options for `web`:\n"
        "  --port <n>                   HTTP port (default: 8080).\n"
        "  --web-binary <path>          Path to face_db_web binary (default: ./install/bin/face_db_web).\n"
        "  --db / --faces-dir / --detection-model / --recognition-model are forwarded to the\n"
        "  web server so it shares the exact same database as the other commands.\n"
        "\n"
        "Examples:\n"
        "  %s run --source 0 --downscale 480\n"
        "  %s run --source rtsp://user:pass@192.168.1.10/stream1 --no-display --save-video out.mp4\n"
        "  %s run --source 0 --detect-every-n 3 --downscale 480\n"
        "  %s add --image alice.jpg --name Alice --title CEO\n"
        "  %s add-bulk --dir ./photos --scene office\n"
        "  %s backfill\n"
        "  %s list\n"
        "  %s remove --id abc123\n"
        "  %s clear\n"
        "  %s web --port 8080\n",
        prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog);
}

}  // namespace face_recognition_standalone
