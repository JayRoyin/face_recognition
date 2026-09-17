# face_recognition_standalone

A **standalone C++17** real-time face recognition application. It uses the same core
algorithm library as the ROS1/ROS2 packages (`face_recognition_core`) but does **not**
depend on ROS — only OpenCV, ONNX Runtime and SQLite3.

> **完整文档（命令、参数、默认值、调优、排障）统一维护在
> [`docs/module/standalone.md`](../../docs/module/standalone.md)。**
> 本文件只保留包级概览，避免两处重复维护、相互矛盾。

---

## Build

```bash
./build.sh STANDALONE                 # 或
make -C build/standalone -j$(nproc)
```

Binary:

```
install/bin/face_recognition_app
```

It links the installed `libface_recognition_core.so` from `<repo>/install/lib` —
the same shared library the web UI and the ROS nodes use, resolved via RPATH
(no manual `LD_LIBRARY_PATH` needed). Build the core first
(`./build.sh CORE`); `./build.sh STANDALONE` does it for you.

---

## Features

- Input auto-detected from `--source`: USB camera index / device node / RTSP /
  HTTP-MJPEG / video file / single image / image directory.
- Per-frame detection (`det_10g.onnx`, SCRFD) + 5-point landmark alignment +
  ArcFace (`w600k_r50.onnx`) 512-d embedding matching against a SQLite gallery.
- Two front-ends (landmark-aligned crop by default, bounding-box crop via
  `--no-align`). They are **not** interchangeable inside one gallery — see the
  module doc before switching.
- OpenCV window with boxes, names, similarity scores and landmark dots; headless
  fallback when no GUI is available.
- Optional MP4 output (`--save-video`) and per-recognition JPEG snapshots
  (`--snapshot-dir`).

## Subcommands

```
run           real-time recognition loop
add           register a face from an image
add-bulk      register every image in a directory (filename -> name)
add-template  add an extra shot to an existing identity (multi-template)
verify        score one image against the whole gallery and show the decision
backfill      re-extract embeddings from stored thumbnails (--all = every row)
list          list registered faces
remove        delete a face by id
clear         wipe the database
web           launch the bundled face_db_web UI on the same DB
help          print usage
```

## Quick start

```bash
cd <repo root>
APP=./install/bin/face_recognition_app

$APP help
$APP add --image alice.jpg --name Alice --title "CEO" --scene office
$APP list
$APP verify --image alice_another_photo.jpg      # 看排名与判定原因
$APP run --source 0                              # 实时识别
```

Press `q` / `ESC` in the window to stop, or `Ctrl+C` in a terminal.

## Defaults (摘要，完整表格见模块文档)

| Option | Default |
| --- | --- |
| `--detection-model` | `models/det_10g.onnx` |
| `--recognition-model` | `models/w600k_r50.onnx` |
| `--db` | `/tmp/face_db/faces.db` |
| `--faces-dir` | `/tmp/face_db/faces` |
| `--detection-threshold` | `0.5` |
| `--recognition-threshold` | `0.5` |
| `--nms-threshold` | `0.5` |
| `--input-size` | `640` |
| `--align` | on (strict) |
| `--min-face-size` | `80` |
| `--detect-every-n` | `2` |
| `--target-fps` | `24` |

All paths are resolved relative to the **current working directory**, so `cd` to the
repository root or pass absolute paths.
