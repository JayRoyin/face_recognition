# 配置项文档（config）

本文档汇总全项目的配置来源：**配置文件 / 启动参数 / 命令行参数 / 环境变量**。

---

## 1. 配置来源总览

| 模块 | 配置文件 | 启动参数 | 命令行参数 | 环境变量 |
|---|---|---|---|---|
| ROS2 节点 | `src/face_recognition_ros2/config/params.yaml` | `launch/*.launch.py` | — | `ROS_LOG_DIR` |
| ROS1 节点 | `src/face_recognition_ros1/config/face_recognition.yaml` | `launch/face_recognition.launch` | — | — |
| standalone | — | — | 全部参数 | — |
| face_db_web | — | — | `--port` `--db` `--faces-dir` `--detection-model` `--recognition-model` | `FACE_DETECTION_MODEL` `FACE_RECOGNITION_MODEL` `FACE_DB_WEB_TEMPLATES` |
| 构建 | — | — | `./build.sh <TARGET>` | `BUILD_SPATIALITE` `FACE_RECOGNITION_VENDORED_PREFIX` `LD_LIBRARY_PATH` `PKG_CONFIG_PATH` |

---

## 2. 路径约定

| 项 | 默认值 | 说明 |
|---|---|---|
| 检测模型 | `models/det_10g.onnx` | RetinaFace |
| 识别模型 | `models/w600k_r50.onnx` | ArcFace，512 维 |
| SQLite 数据库 | `/tmp/face_db/faces.db` | ROS / standalone / web 共享 |
| 缩略图目录 | `/tmp/face_db/faces` | 文件名 = `<id>.jpg` |
| 构建日志 | `build.log`（项目根） | 所有 build.sh 输出 |

> standalone 的所有路径**相对于当前工作目录**，其余模块建议使用绝对路径。

---

## 3. ROS2 配置

### 3.1 `config/params.yaml`

```yaml
/**:
  ros__parameters:
    image_topic: "/image_raw"
    result_topic: "/face/recognition_result"

    confidence_threshold: 0.5
    nms_threshold: 0.5

    db_path: "/tmp/face_db/faces.db"
    faces_dir: "/tmp/face_db/faces"

    detection_model: "/models/det_10g.onnx"
    recognition_model: "/models/w600k_r50.onnx"

    default_scene: "default"
    default_map_location: "unknown"
```

使用方式：

```bash
ros2 launch face_recognition_ros2 face_recognition.launch.py \
    --params-file src/face_recognition_ros2/config/params.yaml
```

### 3.2 节点参数

| 参数 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `image_topic` | string | `/image_raw` | 输入图像话题 |
| `result_topic` | string | `/face/recognition_result` | 结果话题 |
| `confidence_threshold` | float | `0.5` | 识别相似度阈值（同时传给检测器） |
| `db_path` | string | `/tmp/face_db/faces.db` | 数据库路径 |
| `faces_dir` | string | `/tmp/face_db/faces` | 缩略图目录 |
| `detection_model` | string | `models/det_10g.onnx` | 检测模型 |
| `recognition_model` | string | `models/w600k_r50.onnx` | 识别模型 |

`face_stream_server` 参数：

| 参数 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `image_topic` | string | `/face/annotated` | 推流图像话题 |
| `port` | int | `8090` | HTTP 端口 |
| `jpeg_quality` | int | `80` | JPEG 质量（0–100） |

### 3.3 Launch 参数

见 [../module/ros2.md](../module/ros2.md#3-launch-参数)。

> **注意**：launch 文件中**未暴露** `db_path` / `faces_dir`，需要改 `params.yaml`
> 或用 `--params-file` 传入。

---

## 4. ROS1 配置

`config/face_recognition.yaml`：

```yaml
face_recognition_node:
  ros__parameters:
    image_topic: "/image_raw"
    result_topic: "/face/recognition_result"
    confidence_threshold: 0.5
    db_path: "/tmp/face_db/faces.db"
    faces_dir: "/tmp/face_db/faces"
    detection_model: "/models/det_10g.onnx"
    recognition_model: "/models/w600k_r50.onnx"
```

Launch 参数与节点参数同名，直接通过 `<arg>` / `<param>` 传递，见
[../module/ros1.md](../module/ros1.md#3-launch-参数)。

---

## 5. standalone 参数

### 5.1 全局参数

| 参数 | 默认值 |
|---|---|
| `--detection-model` | `models/det_10g.onnx` |
| `--recognition-model` | `models/w600k_r50.onnx` |
| `--db` | `/tmp/face_db/faces.db` |
| `--faces-dir` | `/tmp/face_db/faces` |
| `--detection-threshold` | `0.5` |
| `--recognition-threshold` | `0.5`（取值依据见 [face_recognizer.md §阈值取值](../services/face_recognizer.md#阈值取值)） |
| `--nms-threshold` | `0.5` |
| `--input-size` | `640` |
| `--max-faces` | `10` |
| `--min-face-size` | `80`（人脸框短边低于此值的**只检测不识别**） |
| `--z-threshold` | `3.0`（队列归一化的 z 门限） |
| `--min-cohort` | `3`（队列样本不足时退化为纯原始门限） |
| `--cohort-db` | 空（外部队列库路径） |
| `--align` | **开**（关键点对齐默认启用；见 [face_recognizer.md §2.1](../services/face_recognizer.md)） |
| `--no-align` | 关（显式关闭对齐，改用 bbox 裁剪前端；仅用于对比 / 排障） |
| `--allow-unaligned` | 关（对齐不可用时是否允许回退到 bbox 前端；**不建议**，会导致库内前端混用） |
| `--no-cohort-norm` | 关（**队列归一化默认开启**） |

### 5.2 按命令划分

| 命令 | 专有参数 |
|---|---|
| `run` | `--source` `--camera`（编号或 `/dev/videoN`，优先于 `--source`）`--width` `--height`（USB 摄像头默认 `1280x720`，传 `0` 用摄像头默认）`--fps` `--fourcc`（`MJPG` / `YUYV`，缺省 ≥720p 自动 MJPG）`--loop` `--no-display` `--detect-every-n`（默认 `2`）`--downscale` `--no-recognition` `--target-fps`（默认 `24`）`--auto-backfill` `--save-video` `--snapshot-dir` |
| `cameras` | `--probe`（额外试出 1920x1080 / 1280x720 / 640x480 / 320x240 各档实际协商结果） |
| `add` | `--image`（必需）`--name`（必需）`--title` `--scene` `--map-location` |
| `add-bulk` | `--dir`（必需）`--scene` `--map-location` `--recursive` |
| `add-template` | `--id`（必需）`--image`（必需）——给已有身份追加一"枪" |
| `verify` | `--image`（必需）——对全库排序并打印判定过程 |
| `backfill` | `--all`（重算**全部**记录；缺省只补 `embedding` 为空的记录） |
| `list` / `clear` / `help` | 仅全局参数 |
| `remove` | `--id`（必需） |
| `web` | `--port` `--web-binary` |

完整说明见 [../module/standalone.md](../module/standalone.md#5-参数速查)。

---

## 6. face_db_web 参数

| 参数 | 默认 | 必需 |
|---|---|---|
| `--port` | `8080` | 否 |
| `--db` | — | **是** |
| `--faces-dir` | — | **是** |
| `--detection-model` | — | 否 |
| `--recognition-model` | — | 否 |
| `--help` / `-h` | — | 否 |

### 环境变量

| 变量 | 作用 |
|---|---|
| `FACE_DETECTION_MODEL` | `--detection-model` 未传时的回退值 |
| `FACE_RECOGNITION_MODEL` | `--recognition-model` 未传时的回退值 |
| `FACE_DB_WEB_TEMPLATES` | 覆盖 `index.html` 的模板目录（默认 `./templates`） |

> 环境变量回退的**前提是 `--detection-model` 已生效**：`--recognition-model`
> 只有在 detector 存在时才会被加载。

---

## 7. 构建与环境变量

| 变量 | 默认 | 说明 |
|---|---|---|
| `BUILD_SPATIALITE` | `OFF` | 设为 `ON` 时额外构建 vendored SpatiaLite |
| `FACE_RECOGNITION_VENDORED_PREFIX` | `<root>/install/vendored` | vendored 依赖安装前缀，`build.sh` 自动导出 |
| `LD_LIBRARY_PATH` | — | `scripts/setup_env.sh` 前置 `install/vendored/lib` |
| `PKG_CONFIG_PATH` | — | 同上，前置 `install/vendored/lib/pkgconfig` |

`build.sh` 接受的目标：`MODELS` `CORE` `ROS1` `ROS2` `STANDALONE` `WEB` `ALL`
`CLEAN` `HELP`。

---

## 8. 阈值调参建议

| 参数 | 调高 | 调低 |
|---|---|---|
| `confidence_threshold` / `recognition_threshold` | 误识别少，遮挡/侧脸易漏 | 召回高，误识别风险上升 |
| `detection-threshold` | 误检框少，小脸易漏 | 误检框增多 |
| `nms-threshold` | 重叠框保留更多 | 重叠框抑制更激进 |
| `input-size` | 小脸召回好，FPS 低 | FPS 高，小脸召回差 |

推荐起点：检测 `0.5`、识别 `0.5`、NMS `0.5`（standalone 实时场景再加
`--input-size 320`，`--detect-every-n 2` 已是默认）。

> **识别阈值不要沿用 0.7**：它落在"本人"与"不同人"两簇之间偏上的位置，会漏判大量
> 现场画面。取值依据见 [face_recognizer.md §阈值取值](../services/face_recognizer.md#阈值取值)。
>
> ⚠️ 识别器**前端（对齐 / bbox 裁剪）或预处理配方**变更会**使全部已存特征失效**，
> 升级后需执行 `face_recognition_app backfill --all` 重建，否则会出现"本人识别不出、
> 他人也能识别"。详见 [../FAQ/troubleshooting.md](../FAQ/troubleshooting.md)。
>
> ROS1 / ROS2 节点的 `confidence_threshold` 参数默认已统一为 `0.5`，且**同时**作为
> 检测置信度与识别相似度阈值传入，现场使用建议一并下调（见
> [../FAQ/troubleshooting.md](../FAQ/troubleshooting.md)）。

---

## 9. 相关文档

- [ROS 接口文档](ros_interfaces.md)
- [Web HTTP API](web_api.md)
- [数据库 schema](database_schema.md)
- [源码开发快速开始](../setup.md)
