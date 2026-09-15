# face_recognition_standalone

A **standalone C++17** real-time face recognition application. It uses the same
core algorithm library as the ROS1/ROS2 packages in this repo (`face_recognition_core`)
but does **not depend on ROS** at all — only OpenCV, ONNX Runtime, and SQLite3.

## Features

- Camera / RTSP / HTTP / video-file / image-directory input (auto-detected from
  the `--source` URI).
- Per-frame face detection (RetinaFace or YOLOv8 via ONNX) + ArcFace embedding
  recognition against a local SQLite database.
- OpenCV window display with bounding boxes, names, similarity scores, and
  landmark dots.
- Optional MP4 output (`--save-video`) and per-recognition JPEG snapshots
  (`--snapshot-dir`).
- Subcommands to manage the face database from the CLI:
  - `run`    — real-time recognition loop
  - `add`    — register a face from an image
  - `list`   — list faces in the DB
  - `remove` — delete a face by ID
  - `clear`  — wipe the database

## Build

The standalone app is built by the top-level `build.sh`:

```bash
./build.sh STANDALONE
```

The resulting binary lives at:
```
build/face_recognition_standalone/face_recognition_app
```

It links to the shared `libface_recognition_core.so` from the same `build/`
tree, so set the `LD_LIBRARY_PATH` as printed by `build.sh` (or run the wrapper
script `./scripts/run_standalone.sh` if you prefer).

## Quick start

```bash
# 1. Show help
./build/face_recognition_standalone/face_recognition_app help

# 2. Register a face from a still image
./build/face_recognition_standalone/face_recognition_app add \
    --image alice.jpg --name Alice --title "CEO" --scene office

# 3. List faces
./build/face_recognition_standalone/face_recognition_app list

# 4. Real-time recognition from the default USB camera
./build/face_recognition_standalone/face_recognition_app run --source 0

# 5. From an RTSP stream, headless, saving snapshots
./build/face_recognition_standalone/face_recognition_app run \
    --source rtsp://user:pass@192.168.1.10/stream1 \
    --no-display --snapshot-dir snapshots

# 6. From a video file with annotated output
./build/face_recognition_standalone/face_recognition_app run \
    --source input.mp4 --save-video annotated.mp4
```

Press `q` / `ESC` in the window to stop, or `Ctrl+C` in a terminal.

## Paths & defaults

| Option                  | Default                   |
| ----------------------- | ------------------------- |
| `--detection-model`     | `models/det_10g.onnx`     |
| `--recognition-model`   | `models/w600k_r50.onnx`   |
| `--db`                  | `data/faces.db`           |
| `--faces-dir`           | `data/faces`              |
| `--detection-threshold` | `0.5`                     |
| `--recognition-threshold` | `0.7`                   |
| `--nms-threshold`       | `0.5`                     |
| `--input-size`          | `640`                     |

All paths are resolved relative to the **current working directory**, so you
should `cd` to the repository root (or pass absolute paths).

## Architecture

```
main.cpp                 ──┐
recognition_pipeline.cpp   ├─►  face_recognition_app (executable)
video_source.cpp           │
cli.cpp                  ──┘
                                    │
                                    ▼
                       face_recognition_core  (shared library)
                                    │
                ┌───────────────────┼────────────────────┐
                ▼                   ▼                    ▼
        FaceDetector         FaceRecognizer        FaceDatabase
        (RetinaFace /        (ArcFace w600k_r50)   (SQLite3)
         YOLOv8 ONNX)
```

`face_recognition_core` is the exact same library the ROS packages use — only
the application layer (`face_recognition_app`) is new.
