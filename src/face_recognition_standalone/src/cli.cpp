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

    // Flags that REQUIRE a value (everything parsed with get_opt() downstream).
    // Every other flag is a boolean switch, for which "no value" means "1".
    static const char* kValueFlags[] = {
        "--detection-model", "--recognition-model", "--db", "--faces-dir",
        "--detection-threshold", "--recognition-threshold", "--nms-threshold",
        "--input-size", "--max-faces", "--detect-every-n", "--downscale",
        "--z-threshold", "--min-cohort", "--min-face-size", "--cohort-db",
        "--camera", "--fourcc", "--source", "--width", "--height", "--fps",
        "--target-fps", "--save-video", "--snapshot-dir", "--image", "--name",
        "--title", "--scene", "--map-location", "--dir", "--id",
        "--web-binary", "--port",
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

        bool takes_value = false;
        for (const char* f : kValueFlags) {
            if (key == f) { takes_value = true; break; }
        }

        // Treat next token as value if it does not start with '-'
        if (i + 1 < argc && argv[i + 1][0] != '-') {
            out.opts[key] = argv[i + 1];
            ++i;
        } else if (takes_value) {
            // Fail loudly instead of storing "1": with the old behaviour a
            // forgotten value (`verify --image`) or a swapped pair
            // (`--db --name x`) put the literal string "1" into the config and
            // it resurfaced much later as a bogus path or face id.
            std::fprintf(stderr,
                         "Missing value for %s.\n"
                         "  Value-taking flags need an argument; if the value "
                         "itself starts with '-',\n"
                         "  write it as %s=<value>.\n",
                         key.c_str(), key.c_str());
            std::exit(2);
        } else {
            out.opts[key] = "1";
        }
    }
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
        "  add-template Add an EXTRA shot to an existing identity (multi-template).\n"
        "              More shots per person = robust to glasses on/off, pose, light.\n"
        "  verify      Score one image against the whole gallery and print the\n"
        "              ranking, cohort statistics and the final accept/reject reason.\n"
        "              Use this to see the real numbers behind a decision.\n"
        "  backfill    Re-extract embeddings from the stored thumbnails and write them\n"
        "              back into the DB. By default only rows with an EMPTY embedding are\n"
        "              processed; pass --all to re-extract EVERY row (needed whenever the\n"
        "              recognizer preprocessing changes, since that invalidates all\n"
        "              previously stored embeddings).\n"
        "  cameras     List the local cameras (/dev/video*) with their names and the\n"
        "              capture mode each one negotiates, then pick one with\n"
        "              `run --camera <index>`. Pass --probe to also try the common\n"
        "              resolutions on every usable camera.\n"
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
        "  --db <path>                  SQLite database file (default: /data/hhqs_data/face_db/faces.db)\n"
        "  --faces-dir <path>           Directory to store face thumbnails (default: /data/hhqs_data/face_db/faces)\n"
        "  --detection-threshold <f>    Face detection confidence (default: 0.5)\n"
        "  --recognition-threshold <f>  Recognition similarity (default: 0.5)\n"
        "                              NOTE: 0.7 (the old default) rejects most live-camera\n"
        "                              matches of the same person. Raise only if you see\n"
        "                              false positives across different people.\n"
        "  --nms-threshold <f>          NMS IoU threshold (default: 0.5)\n"
        "  --input-size <n>             Detector square input size (default: 640)\n"
        "  --align                      Use 5-point landmark alignment (ON by default).\n"
        "                              Earlier revisions kept this OFF because the\n"
        "                              detector decoded its landmarks with the wrong\n"
        "                              (RetinaFace) convention, squeezing them to ~0.4x\n"
        "                              and making alignment score worse than a crop.\n"
        "                              With the decoder fixed: aligned = 0.68 genuine /\n"
        "                              0.20 best impostor, bbox crop = 0.64 / 0.73.\n"
        "                              Re-run `backfill --all` after toggling -- the two\n"
        "                              front-ends are NOT interchangeable.\n"
        "  --no-align                   Use the bounding-box crop instead of alignment.\n"
        "                              Kept for comparison / debugging only.\n"
        "  --min-face-size <n>          Faces whose bounding box short side is below this\n"
        "                              many pixels are detected and drawn but NOT\n"
        "                              identified (default: 80). Tiny faces have to be\n"
        "                              upsampled into the 112x112 input, which produces\n"
        "                              hub-like embeddings that match many people -- the\n"
        "                              main cause of \"everyone gets recognised\".\n"
        "  --allow-unaligned            With alignment on, still accept an embedding when\n"
        "                              the landmarks were unusable (bbox-crop fallback).\n"
        "                              NOT recommended: it mixes two different feature\n"
        "                              sub-spaces inside one gallery, which makes the\n"
        "                              enrolled person score lower than strangers.\n"
        "  --no-cohort-norm             Disable the cohort z-score gate.\n"
        "  --z-threshold <f>            Minimum cohort z-score (default: 3.0). Only\n"
        "                              applied when the cohort has >= --min-cohort\n"
        "                              samples; a template that matches everybody (a\n"
        "                              'hub') raises the cohort and must then score much\n"
        "                              higher to be accepted.\n"
        "  --min-cohort <n>             Cohort samples needed to normalise (default: 3).\n"
        "  --cohort-db <path>           Second face DB of OTHER people, used purely as an\n"
        "                              impostor cohort for normalisation.\n"
        "\n"
        "Options for `run`:\n"
        "  --source <uri>               '0' = default USB camera, '/dev/video1' (digits), an RTSP/HTTP URL,\n"
        "                              a video file path, or a directory of images.\n"
        "                              If omitted, tries the default USB camera (index 0).\n"
        "  --camera <n|node>            Pick the local camera explicitly: an index ('0')\n"
        "                              or a device node ('/dev/video2'). Validated against\n"
        "                              /dev/video*; run `cameras` to see what is attached.\n"
        "                              Overrides --source.\n"
        "  --width <n>  --height <n>    Requested capture resolution (default: 1280x720).\n"
        "                              More pixels => more facial detail for the\n"
        "                              recognizer. Use 0 to keep the camera default,\n"
        "                              or e.g. 1920x1080 on a capable webcam.\n"
        "  --fps <n>                    Requested capture FPS.\n"
        "  --fourcc <FMT>               Force the V4L2 pixel format (MJPG / YUYV).\n"
        "                              Default: MJPG whenever the frame is >= 720p, since\n"
        "                              raw YUYV at that size exceeds USB2 bandwidth and the\n"
        "                              camera silently drops to a few fps. A negotiated\n"
        "                              mode that misses your request is reported as a WARN.\n"
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
        "Options for `backfill`:\n"
        "  --all                        Re-extract every record, including ones that already\n"
        "                              have an embedding. Use this after upgrading the build\n"
        "                              (the recognizer preprocessing changed, so embeddings\n"
        "                              produced by an older binary no longer match).\n"
        "\n"
        "Options for `add-template`:\n"
        "  --id <face_id>               Target identity id (from `list`). REQUIRED.\n"
        "  --image <path>               Extra photo of the SAME person. REQUIRED.\n"
        "                              Tip: enrol one shot WITH glasses and one\n"
        "                              WITHOUT to fix \"works only with glasses\".\n"
        "\n"
        "Options for `verify`:\n"
        "  --image <path>               Image to score against the gallery. REQUIRED.\n"
        "\n"
        "Options for `web`:\n"
        "  --port <n>                   HTTP port (default: 8080).\n"
        "  --web-binary <path>          Path to face_db_web binary (default: ./install/bin/face_db_web).\n"
        "  --db / --faces-dir / --detection-model / --recognition-model are forwarded to the\n"
        "  web server so it shares the exact same database as the other commands.\n"
        "\n"
        "Options for `cameras`:\n"
        "  --probe                      Also try 1920x1080 / 1280x720 / 640x480 / 320x240 on\n"
        "                              every usable camera and report what each one actually\n"
        "                              negotiates (slower: re-opens each device per mode).\n"
        "\n"
        "Examples:\n"
        "  %s cameras --probe\n"
        "  %s run --source 0 --downscale 480\n"
        "  %s run --camera 0 --width 1280 --height 720 --fps 30\n"
        "  %s run --source rtsp://user:pass@192.168.1.10/stream1 --no-display --save-video out.mp4\n"
        "  %s run --source 0 --detect-every-n 3 --downscale 480\n"
        "  %s add --image alice.jpg --name Alice --title CEO\n"
        "  %s add-bulk --dir ./photos --scene office\n"
        "  %s add-template --id abc123 --image alice2.jpg\n"
        "  %s verify --image alice.jpg\n"
        "  %s backfill --all\n"
        "  %s list\n"
        "  %s remove --id abc123\n"
        "  %s web --port 8080\n",
        prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog,
        prog);
}

}  // namespace face_recognition_standalone
