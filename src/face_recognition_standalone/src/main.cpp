#include "face_recognition_standalone/cli.hpp"
#include "face_recognition_standalone/recognition_pipeline.hpp"
#include "face_recognition_standalone/video_source.hpp"

#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <algorithm>
#include <cctype>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

using face_recognition_standalone::CliArgs;
using face_recognition_standalone::FrameStats;
using face_recognition_standalone::PipelineConfig;
using face_recognition_standalone::RecognitionPipeline;
using face_recognition_standalone::RecognizedFace;
using face_recognition_standalone::SourceConfig;
using face_recognition_standalone::VideoSource;

namespace {

std::atomic<bool> g_stop{false};

void on_sigint(int) { g_stop = true; }

std::string get_opt(const CliArgs& args, const std::string& key, const std::string& def = {}) {
    auto it = args.opts.find(key);
    if (it == args.opts.end()) return def;
    return it->second;
}

bool has_opt(const CliArgs& args, const std::string& key) {
    return args.opts.count(key) > 0;
}

PipelineConfig build_pipeline_config(const CliArgs& args) {
    PipelineConfig cfg;
    cfg.detection_model =
        get_opt(args, "--detection-model", "models/det_10g.onnx");
    cfg.recognition_model =
        get_opt(args, "--recognition-model", "models/w600k_r50.onnx");
    // Defaults match the convention used by the ROS2 node and the Web UI so
    // that web-uploaded faces are immediately visible to the standalone app
    // (and vice versa) without any extra configuration.
    cfg.db_path   = get_opt(args, "--db",      "/data/hhqs_data/face_db/faces.db");
    cfg.faces_dir = get_opt(args, "--faces-dir", "/data/hhqs_data/face_db/faces");

    if (auto v = get_opt(args, "--detection-threshold", "");   !v.empty()) cfg.detection_threshold   = std::stof(v);
    if (auto v = get_opt(args, "--recognition-threshold", ""); !v.empty()) cfg.recognition_threshold = std::stof(v);
    if (auto v = get_opt(args, "--nms-threshold", "");         !v.empty()) cfg.nms_threshold         = std::stof(v);
    if (auto v = get_opt(args, "--input-size", "");            !v.empty()) cfg.input_size            = std::stoi(v);
    if (auto v = get_opt(args, "--max-faces", "");             !v.empty()) cfg.max_faces             = std::stoi(v);
    if (auto v = get_opt(args, "--detect-every-n", "");        !v.empty()) cfg.detect_every_n_frames = std::stoi(v);
    if (auto v = get_opt(args, "--downscale", "");             !v.empty()) cfg.downscale_max_side    = std::stoi(v);
    cfg.run_recognition = !has_opt(args, "--no-recognition");

    // Alignment + cohort normalisation.
    if (auto v = get_opt(args, "--z-threshold", ""); !v.empty()) cfg.z_threshold = std::stof(v);
    if (auto v = get_opt(args, "--min-cohort", "");  !v.empty()) cfg.min_cohort  = std::stoi(v);
    if (auto v = get_opt(args, "--min-face-size", ""); !v.empty()) cfg.min_face_size = std::stoi(v);
    // Alignment is ON by default (see PipelineConfig::align); `--no-align`
    // switches back to the bounding-box crop. NOTE: this must NOT be
    // `has_opt("--align")`, otherwise the flag being absent silently forces the
    // default off and overrides PipelineConfig.
    cfg.align           = !has_opt(args, "--no-align");
    cfg.allow_unaligned = has_opt(args, "--allow-unaligned");
    cfg.cohort_norm     = !has_opt(args, "--no-cohort-norm");
    cfg.cohort_db       = get_opt(args, "--cohort-db", "");
    return cfg;
}

// -----------------------------------------------------------------------------
// cameras: list the local V4L2 devices so a multi-camera host can pick one.
// -----------------------------------------------------------------------------
int cmd_cameras(const CliArgs& args) {
    const bool probe = has_opt(args, "--probe");
    auto cams = face_recognition_standalone::enumerate_cameras(probe);

    if (cams.empty()) {
        std::printf("No V4L2 video device found under /dev/video*.\n\n");
        std::printf("  - is the camera plugged in and powered?\n");
        std::printf("  - running inside a container? pass --device=/dev/video0\n");
        std::printf("  - quick check: ls -l /dev/video*\n");
        return 1;
    }

    std::printf("\nCameras on this host (V4L2):\n\n");
    std::printf("  %-6s %-13s %-38s %-18s %s\n",
                "index", "device", "name", "mode", "status");
    int usable = 0;
    for (const auto& c : cams) {
        char mode[48];
        if (c.openable) {
            std::snprintf(mode, sizeof(mode), "%dx%d@%.0f %s",
                          c.width, c.height, c.fps, c.fourcc.c_str());
            ++usable;
        } else {
            std::snprintf(mode, sizeof(mode), "-");
        }
        std::printf("  %-6d %-13s %-38s %-18s %s\n",
                    c.index, c.device.c_str(),
                    c.name.empty() ? "(unknown)" : c.name.c_str(), mode,
                    c.openable ? "ok"
                               : "cannot open (metadata node / busy / no capture)");
        for (const auto& r : c.probe_results)
            std::printf("         probe: %s\n", r.c_str());
    }

    std::printf("\n%d usable camera(s) out of %zu node(s).\n", usable, cams.size());
    if (usable < (int)cams.size()) {
        std::printf("A camera usually publishes several nodes (capture + metadata);\n"
                    "'cannot open' on the extra ones is expected. If a camera you\n"
                    "expect is missing entirely, check that the user is in the `video`\n"
                    "group and that no other program is using it.\n");
    }
    std::printf("\nUse one like this:\n\n");
    std::printf("  face_recognition_app run --camera 0\n");
    std::printf("  face_recognition_app run --camera 0 --width 1280 --height 720 --fps 30\n");
    std::printf("  face_recognition_app run --camera 0 --fourcc MJPG\n");
    std::printf("  face_recognition_app run --camera 0 --width 0 --height 0   "
                "# keep the camera's own default\n");
    if (!probe)
        std::printf("\nAdd --probe to also try the common resolutions on every usable\n"
                    "camera (slower: re-opens each device once per candidate mode).\n");
    return 0;
}

int cmd_run(const CliArgs& args) {
    // Validate the capture-selection options FIRST. A typo in --camera /
    // --fourcc should fail immediately, not after the ONNX models and the
    // database have been loaded.
    //
    // `--camera` exists because on a multi-camera host there is no way to know
    // which /dev/videoN is which physical camera without probing: run the
    // `cameras` command first, then pass the index it printed. It is equivalent
    // to --source, except a wrong device is rejected with an actionable message
    // instead of a generic "Video source not opened".
    std::string camera_uri;
    if (auto cam_arg = get_opt(args, "--camera", ""); !cam_arg.empty()) {
        std::string cam_err;
        if (!face_recognition_standalone::normalize_camera_selector(cam_arg, camera_uri,
                                                                    cam_err)) {
            std::fprintf(stderr, "Invalid --camera %s: %s\n\n%s\n",
                         cam_arg.c_str(), cam_err.c_str(),
                         "List the cameras attached to this host with:\n"
                         "    face_recognition_app cameras");
            return 2;
        }
    }
    std::string fourcc_arg = get_opt(args, "--fourcc", "");
    if (!fourcc_arg.empty()) {
        std::transform(fourcc_arg.begin(), fourcc_arg.end(), fourcc_arg.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (fourcc_arg.size() != 4) {
            std::fprintf(stderr, "--fourcc must be a 4-character V4L2 format code "
                                 "(e.g. MJPG, YUYV)\n");
            return 2;
        }
    }

    auto pipe_cfg = build_pipeline_config(args);

    RecognitionPipeline pipe;
    std::string err;
    if (!pipe.initialize(pipe_cfg, err)) {
        std::fprintf(stderr, "Pipeline init failed: %s\n", err.c_str());
        return 1;
    }

    // Optional one-shot backfill so web uploads (or DB rows that were
    // written without the recognition model loaded) become recognizable
    // automatically on the next run.
    if (has_opt(args, "--auto-backfill")) {
        int total = 0, skipped = 0, failed = 0;
        int updated = pipe.backfill_embeddings(&total, &skipped, &failed);
        std::printf("[INFO] auto-backfill: total=%d updated=%d already_embedded=%d failed=%d\n",
                    total, updated, skipped, failed);
    }

    SourceConfig src_cfg;
    // `--camera` was already validated at the top of cmd_run; it wins over
    // --source when both are given.
    std::string source_uri = get_opt(args, "--source", "0");
    if (!camera_uri.empty()) {
        if (has_opt(args, "--source")) {
            std::fprintf(stderr, "[WARN] --camera %s overrides --source %s\n",
                         camera_uri.c_str(), source_uri.c_str());
        }
        source_uri = camera_uri;
    }
    if (source_uri.empty()) source_uri = "0";
    if (source_uri.size() >= 2 && source_uri.back() == '/') {
        // Strip ONE trailing slash so "/dev/video0/" and "/dev/video0" resolve to
        // the same device. The previous condition additionally required the last
        // character to be a digit — false for exactly the example it documented —
        // so the slash was never removed and the device failed to open.
        source_uri.pop_back();
    }
    src_cfg = face_recognition_standalone::infer_source_from_uri(source_uri);
    if (auto v = get_opt(args, "--width",  ""); !v.empty()) src_cfg.width  = std::stoi(v);
    if (auto v = get_opt(args, "--height", ""); !v.empty()) src_cfg.height = std::stoi(v);
    if (auto v = get_opt(args, "--fps",    ""); !v.empty()) src_cfg.fps    = std::stoi(v);
    if (!fourcc_arg.empty()) src_cfg.fourcc = fourcc_arg;
    src_cfg.loop = has_opt(args, "--loop");

    // ---- Recognition frame-rate cap (default: 24 FPS) --------------------
    // The loop sleeps out the remainder of each frame budget so the
    // recognition rate is capped instead of spinning at full CPU. Pass
    // `--target-fps 0` to run as fast as the hardware allows.
    double target_fps = 24.0;
    if (auto v = get_opt(args, "--target-fps", ""); !v.empty()) target_fps = std::stod(v);
    const bool   pace = target_fps > 0.0;
    const double frame_budget_ms = pace ? 1000.0 / target_fps : 0.0;
    // Bulk inputs (image directory / single still image) are processed as fast
    // as possible — pacing them would only slow down the batch run.
    const bool batch_mode =
        (src_cfg.type == face_recognition_standalone::SourceType::IMAGE_DIR) ||
        (src_cfg.type == face_recognition_standalone::SourceType::FILE &&
         !src_cfg.loop);

    VideoSource src;
    if (!src.open(src_cfg, err)) {
        std::fprintf(stderr, "Failed to open source '%s': %s\n",
                     source_uri.c_str(), err.c_str());
        return 1;
    }
    std::printf("[INFO] Source opened: type=%s uri=%s",
                face_recognition_standalone::source_type_name(src.type()),
                src.uri().c_str());
    if (src.type() != face_recognition_standalone::SourceType::IMAGE_DIR) {
        std::printf(" (%dx%d @ %.1f fps)", src.width(), src.height(), src.fps());
        const std::string fcc = src.fourcc();
        if (!fcc.empty()) std::printf(" %s", fcc.c_str());
    }
    std::printf("\n");
    const std::string fps_cap =
        pace ? (std::to_string(static_cast<int>(target_fps)) + " fps") : "off";
    std::printf("[INFO] recognition: threshold=%.2f detect-every-n=%d input-size=%d "
                "align=%s min-face=%dpx frame-cap=%s%s\n",
                pipe_cfg.recognition_threshold, pipe_cfg.detect_every_n_frames,
                pipe_cfg.input_size,
                pipe_cfg.align ? (pipe_cfg.allow_unaligned ? "on(soft)" : "on(strict)")
                               : "off",
                pipe_cfg.min_face_size, fps_cap.c_str(),
                pipe_cfg.run_recognition ? "" : " (detection only)");
    std::printf("[INFO] cohort norm: %s z>=%.2f min-cohort=%d (external cohort=%d)\n",
                pipe_cfg.cohort_norm ? "on" : "off", pipe_cfg.z_threshold,
                pipe_cfg.min_cohort, pipe.database().cohortSize());

    bool display = !has_opt(args, "--no-display");
    if (display) {
        try {
            cv::namedWindow("Face Recognition", cv::WINDOW_NORMAL);
            cv::resizeWindow("Face Recognition", 960, 540);
        } catch (const cv::Exception& e) {
            std::fprintf(stderr, "[WARN] OpenCV GUI unavailable (%s); falling back to headless.\n",
                         e.what());
            display = false;
        }
    }

    std::string save_video = get_opt(args, "--save-video", "");
    cv::VideoWriter writer;
    bool writer_open = false;
    if (!save_video.empty()) {
        std::filesystem::path p(save_video);
        if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
    }

    std::string snapshot_dir = get_opt(args, "--snapshot-dir", "");
    if (!snapshot_dir.empty()) {
        std::filesystem::create_directories(snapshot_dir);
    }

    std::signal(SIGINT, on_sigint);

    cv::Mat frame;
    int frame_idx = 0;
    auto t_last_print = std::chrono::steady_clock::now();
    int frames_since_print = 0;
    double fps_smooth = 0.0;

    bool single_image_mode = (src_cfg.type ==
                               face_recognition_standalone::SourceType::FILE) &&
                              !src_cfg.loop;

    while (!g_stop) {
        auto t_frame = std::chrono::steady_clock::now();
        if (!src.read(frame) || frame.empty()) {
            if (src_cfg.type == face_recognition_standalone::SourceType::IMAGE_DIR) {
                break;
            }
            // A non-loop FILE source is a single image; once exhausted, stop.
            if (single_image_mode) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        ++frame_idx;

        FrameStats stats;
        auto faces = pipe.process(frame, &stats, display);

        if (!save_video.empty()) {
            if (!writer_open) {
                int fourcc = cv::VideoWriter::fourcc('m', 'p', '4', 'v');
                writer.open(save_video, fourcc,
                            src.fps() > 0 ? src.fps() : 25.0,
                            cv::Size(frame.cols, frame.rows));
                if (writer.isOpened()) writer_open = true;
            }
            if (writer_open) writer.write(frame);
        }

        if (!snapshot_dir.empty()) {
            for (const auto& rf : faces) {
                if (!rf.recognized) continue;
                char fname[512];
                std::time_t t = std::time(nullptr);
                std::tm tm{};
                localtime_r(&t, &tm);
                std::snprintf(fname, sizeof(fname),
                              "%s/%s_%04d%02d%02d_%02d%02d%02d_%d.jpg",
                              snapshot_dir.c_str(), rf.name.c_str(),
                              tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                              tm.tm_hour, tm.tm_min, tm.tm_sec, frame_idx);
                std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 90};
                cv::imwrite(fname, frame, params);
            }
        }

        if (display) {
            char overlay[256];
            std::snprintf(overlay, sizeof(overlay),
                          "Frame %d | %d faces | %d recognized | %.1f ms%s",
                          stats.frame_index, stats.faces_detected,
                          stats.faces_recognized, stats.total_ms,
                          pipe_cfg.run_recognition ? "" : " | DETECT-ONLY");
            cv::putText(frame, overlay, cv::Point(10, 24),
                        cv::FONT_HERSHEY_SIMPLEX, 0.55,
                        cv::Scalar(0, 255, 255), 1, cv::LINE_AA);

            cv::imshow("Face Recognition", frame);
            int key = cv::waitKey(1);
            if (key == 27 /*ESC*/ || key == 'q' || key == 'Q') break;
        }

        // Frame-rate cap: sleep out the unused remainder of this frame budget.
        // If the pipeline is slower than the target (usual case on CPU), this
        // is a no-op — the meter below will report the real achieved rate.
        if (pace && !batch_mode) {
            double spent_ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - t_frame).count();
            if (spent_ms < frame_budget_ms) {
                std::this_thread::sleep_for(
                    std::chrono::duration<double, std::milli>(frame_budget_ms - spent_ms));
            }
        }

        ++frames_since_print;
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - t_last_print).count();
        if (elapsed >= 2.0) {
            fps_smooth = frames_since_print / elapsed;
            std::printf("[INFO] ~%.1f FPS | last frame: %.1f ms (det=%.1f, rec=%.1f)\n",
                        fps_smooth, stats.total_ms, stats.detect_ms, stats.recognize_ms);
            std::fflush(stdout);
            t_last_print = now;
            frames_since_print = 0;
        }
    }

    if (writer_open) writer.release();
    src.release();
    if (display) cv::destroyAllWindows();
    std::printf("[INFO] Stopped after %d frames\n", frame_idx);
    return 0;
}

int cmd_add(const CliArgs& args) {
    std::string image = get_opt(args, "--image", "");
    std::string name  = get_opt(args, "--name", "");
    if (image.empty() || name.empty()) {
        std::fprintf(stderr, "`add` requires --image <path> and --name <name>\n");
        return 2;
    }

    auto cfg = build_pipeline_config(args);
    RecognitionPipeline pipe;
    std::string err;
    if (!pipe.initialize(cfg, err)) {
        std::fprintf(stderr, "Pipeline init failed: %s\n", err.c_str());
        return 1;
    }

    std::string title  = get_opt(args, "--title", "");
    std::string scene  = get_opt(args, "--scene", "default");
    std::string maploc = get_opt(args, "--map-location", "unknown");

    std::string add_err;
    std::string id = pipe.register_face_from_image(image, name, title, scene, maploc, &add_err);
    if (id.empty()) {
        std::fprintf(stderr, "Failed to add face: %s\n", add_err.c_str());
        return 1;
    }
    std::printf("OK id=%s name=%s\n", id.c_str(), name.c_str());
    return 0;
}

int cmd_list(const CliArgs& args) {
    auto cfg = build_pipeline_config(args);
    RecognitionPipeline pipe;
    std::string err;
    if (!pipe.initialize(cfg, err)) {
        std::fprintf(stderr, "Pipeline init failed: %s\n", err.c_str());
        return 1;
    }
    auto faces = pipe.database().list_faces();
    std::printf("Faces in DB: %zu\n", faces.size());
    for (const auto& f : faces) {
        bool has_emb = !f.embedding.empty();
        const int extra = pipe.database().template_count(f.id);
        std::printf("  id=%s name=%-20s title=%-14s scene=%-8s map=%-10s "
                    "emb=%-5s templates=%d img=%s\n",
                    f.id.c_str(), f.name.c_str(), f.title.c_str(),
                    f.scene.c_str(), f.map_location.c_str(),
                    has_emb ? "yes" : "NO",
                    1 + extra,
                    f.image_path.empty() ? "(none)" : f.image_path.c_str());
    }
    return 0;
}

int cmd_backfill(const CliArgs& args) {
    auto cfg = build_pipeline_config(args);
    RecognitionPipeline pipe;
    std::string err;
    if (!pipe.initialize(cfg, err)) {
        std::fprintf(stderr, "Pipeline init failed: %s\n", err.c_str());
        return 1;
    }
    int total = 0, skipped = 0, failed = 0;
    const bool force = has_opt(args, "--all");
    int updated = pipe.backfill_embeddings(&total, &skipped, &failed, force);
    std::printf("backfill done: total=%d updated=%d already_embedded=%d failed=%d%s\n",
                total, updated, skipped, failed,
                force ? "  (--all: every record re-extracted)" : "");
    return failed > 0 ? 1 : 0;
}

int cmd_add_bulk(const CliArgs& args) {
    std::string dir = get_opt(args, "--dir", "");
    if (dir.empty()) {
        std::fprintf(stderr, "`add-bulk` requires --dir <path>\n");
        return 2;
    }
    auto cfg = build_pipeline_config(args);
    RecognitionPipeline pipe;
    std::string err;
    if (!pipe.initialize(cfg, err)) {
        std::fprintf(stderr, "Pipeline init failed: %s\n", err.c_str());
        return 1;
    }
    std::string scene     = get_opt(args, "--scene", "default");
    std::string maploc    = get_opt(args, "--map-location", "unknown");
    bool recursive         = has_opt(args, "--recursive");

    namespace fs = std::filesystem;
    if (!fs::exists(dir) || !fs::is_directory(dir)) {
        std::fprintf(stderr, "Not a directory: %s\n", dir.c_str());
        return 2;
    }

    int total = 0, ok = 0, fail = 0;
    auto walker = [&](const fs::path& d) {
        for (const auto& entry : fs::directory_iterator(d)) {
            if (!entry.is_regular_file()) continue;
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (ext != ".jpg" && ext != ".jpeg" && ext != ".png" &&
                ext != ".bmp" && ext != ".webp") continue;
            ++total;
            std::string name = entry.path().stem().string();
            std::string err2;
            std::string id = pipe.register_face_from_image(
                entry.path().string(), name, "", scene, maploc, &err2);
            if (id.empty()) {
                ++fail;
                std::fprintf(stderr, "[add-bulk] %s: %s\n",
                             entry.path().string().c_str(), err2.c_str());
            } else {
                ++ok;
                std::printf("[add-bulk] %s -> id=%s name=%s\n",
                            entry.path().string().c_str(), id.c_str(), name.c_str());
            }
        }
    };

    walker(dir);
    if (recursive) {
        for (const auto& sub : fs::recursive_directory_iterator(dir)) {
            if (sub.is_directory() && sub.path() != dir) walker(sub.path());
        }
    }

    std::printf("add-bulk done: total=%d ok=%d failed=%d\n", total, ok, fail);
    return fail > 0 ? 1 : 0;
}

int cmd_remove(const CliArgs& args) {
    std::string id = get_opt(args, "--id", "");
    if (id.empty()) {
        std::fprintf(stderr, "`remove` requires --id <face_id>\n");
        return 2;
    }
    auto cfg = build_pipeline_config(args);
    RecognitionPipeline pipe;
    std::string err;
    if (!pipe.initialize(cfg, err)) {
        std::fprintf(stderr, "Pipeline init failed: %s\n", err.c_str());
        return 1;
    }
    bool ok = pipe.database().remove_face(id);
    std::printf(ok ? "Removed %s\n" : "Not found: %s\n", id.c_str());
    return ok ? 0 : 1;
}

int cmd_clear(const CliArgs& args) {
    auto cfg = build_pipeline_config(args);
    RecognitionPipeline pipe;
    std::string err;
    if (!pipe.initialize(cfg, err)) {
        std::fprintf(stderr, "Pipeline init failed: %s\n", err.c_str());
        return 1;
    }
    int n = pipe.database().clear_all();
    std::printf("Cleared %d faces\n", n);
    return 0;
}

int cmd_add_template(const CliArgs& args) {
    const std::string id    = get_opt(args, "--id", "");
    const std::string image = get_opt(args, "--image", "");
    if (id.empty() || image.empty()) {
        std::fprintf(stderr, "`add-template` requires --id <face_id> and --image <path>\n");
        return 2;
    }
    auto cfg = build_pipeline_config(args);
    RecognitionPipeline pipe;
    std::string err;
    if (!pipe.initialize(cfg, err)) {
        std::fprintf(stderr, "Pipeline init failed: %s\n", err.c_str());
        return 1;
    }
    std::string stored = pipe.add_template_from_image(id, image, &err);
    if (stored.empty()) {
        std::fprintf(stderr, "Failed to add template: %s\n", err.c_str());
        return 1;
    }
    std::printf("OK template added to id=%s (stored %s)\n", id.c_str(), stored.c_str());
    std::printf("    total templates for this id: %d\n",
                pipe.database().template_count(id) + 1);
    return 0;
}

int cmd_verify(const CliArgs& args) {
    const std::string image = get_opt(args, "--image", "");
    if (image.empty()) {
        std::fprintf(stderr, "`verify` requires --image <path>\n");
        return 2;
    }
    auto cfg = build_pipeline_config(args);
    RecognitionPipeline pipe;
    std::string err;
    if (!pipe.initialize(cfg, err)) {
        std::fprintf(stderr, "Pipeline init failed: %s\n", err.c_str());
        return 1;
    }
    auto rep = pipe.verify_image(image);
    if (!rep.ok) {
        std::fprintf(stderr, "verify failed: %s\n", rep.error.c_str());
        return 1;
    }

    std::printf("image          : %s\n", image.c_str());
    std::printf("faces detected : %d\n", rep.faces_detected);
    std::printf("gallery ranking (raw cosine per identity, best template):\n");
    if (rep.ranked.empty()) {
        std::printf("   (gallery has no usable templates)\n");
    }
    for (const auto& c : rep.ranked) {
        std::printf("   %.4f  name=%-18s templates=%d  id=%s\n",
                    c.similarity, c.name.c_str(), c.template_count, c.face_id.c_str());
    }

    const auto& d = rep.decision;
    std::printf("raw similarity : %.4f (threshold %.2f)\n",
                d.raw_similarity, cfg.recognition_threshold);
    if (d.normalized) {
        std::printf("cohort         : n=%d median=%.4f mad=%.4f -> z=%.2f "
                    "(threshold %.2f)\n",
                    d.cohort_size, d.cohort_median, d.cohort_mad, d.z_score,
                    cfg.z_threshold);
    } else {
        std::printf("cohort         : n=%d (needs >= %d -> normalisation skipped)\n",
                    d.cohort_size, cfg.min_cohort);
    }
    std::printf("decision       : %s -- %s\n", d.accepted ? "ACCEPT" : "REJECT",
                d.reason.c_str());
    if (d.accepted) std::printf("=> %s\n", d.name.c_str());
    return 0;
}

int cmd_web(const CliArgs& args) {
    // Forward the same --db / --faces-dir / --detection-model /
    // --recognition-model defaults so the web server and standalone share
    // the exact same database out of the box.
    auto cfg = build_pipeline_config(args);

    std::string web_bin = get_opt(args, "--web-binary", "./install/bin/face_db_web");
    if (!std::filesystem::exists(web_bin)) {
        // Fall back to looking it up relative to this executable's location.
        std::fprintf(stderr,
            "face_db_web not found at %s\n"
            "Build it first:  ./build.sh WEB\n"
            "Or pass --web-binary /absolute/path/to/face_db_web\n",
            web_bin.c_str());
        return 1;
    }

    int port = 8080;
    if (auto v = get_opt(args, "--port", ""); !v.empty()) port = std::stoi(v);

    std::vector<std::string> cmd = {
        web_bin,
        "--port", std::to_string(port),
        "--db", cfg.db_path,
        "--faces-dir", cfg.faces_dir,
        "--detection-model", cfg.detection_model,
        "--recognition-model", cfg.recognition_model,
    };

    std::printf("[INFO] launching: ");
    for (const auto& a : cmd) std::printf("%s ", a.c_str());
    std::printf("\n");
    std::printf("[INFO] Open http://localhost:%d/ in your browser.\n", port);
    std::printf("[INFO] Faces uploaded here will be recognized by `run` "
                "(and vice versa) because both share --db=%s\n",
                cfg.db_path.c_str());
    std::fflush(stdout);

    std::filesystem::create_directories(std::filesystem::path(cfg.db_path).parent_path());
    std::filesystem::create_directories(cfg.faces_dir);

    std::vector<char*> argv;
    argv.reserve(cmd.size() + 1);
    for (auto& s : cmd) argv.push_back(s.data());
    argv.push_back(nullptr);

    std::signal(SIGINT, on_sigint);
    int rc = execvp(argv[0], argv.data());
    std::fprintf(stderr, "execvp failed: %s\n", std::strerror(errno));
    return rc == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    // Force line-buffered stdout so progress logs show up in non-TTY
    // contexts (piped to `head`, redirected to files, captured by CI, etc.).
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    CliArgs args = face_recognition_standalone::parse_cli(argc, argv);
    if (args.command.empty() || args.command == "help" || args.command == "-h" ||
        args.command == "--help" || has_opt(args, "--help") || has_opt(args, "-h")) {
        face_recognition_standalone::print_usage(argv[0]);
        return 0;
    }

    try {
        if      (args.command == "run")          return cmd_run(args);
        else if (args.command == "cameras" ||
                 args.command == "list-cameras") return cmd_cameras(args);
        else if (args.command == "add")          return cmd_add(args);
        else if (args.command == "add-bulk")     return cmd_add_bulk(args);
        else if (args.command == "add-template") return cmd_add_template(args);
        else if (args.command == "verify")       return cmd_verify(args);
        else if (args.command == "backfill")     return cmd_backfill(args);
        else if (args.command == "list")         return cmd_list(args);
        else if (args.command == "remove")       return cmd_remove(args);
        else if (args.command == "clear")        return cmd_clear(args);
        else if (args.command == "web")          return cmd_web(args);
        else {
            std::fprintf(stderr, "Unknown command: %s\n", args.command.c_str());
            face_recognition_standalone::print_usage(argv[0]);
            return 2;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Fatal: %s\n", e.what());
        return 1;
    }
}
