# ROS2 节点模块使用文档（face_recognition_ros2）

ROS2 Humble 版本的人脸识别功能包，包含 **3 个可执行节点**：

| 可执行 | 节点名 | 作用 |
|---|---|---|
| `face_recognition_node` | `face_recognition_node` | 订阅图像 → 检测 + 识别 → 发布结果 / 提供入库 Service |
| `face_viewer_node` | `face_viewer_node` | 把识别结果画到图像上 → 发布 `/face/annotated`（**不链接 highgui**） |
| `face_stream_server` | `face_stream_server` | 把标注图以 HTTP MJPEG 推流，浏览器直接看（无 Qt/GTK） |

- 源码：`src/face_recognition_ros2/`
- 接口包：`src/face_recognition_ros2_interfaces/`
- 版本：`1.0.0`，License：MIT（见 `package.xml`）

---

## 1. 编译

```bash
./build.sh ROS2          # = vendored SQLite3 + CORE + colcon build
source install/ros2/setup.bash
```

等价的手工流程：

```bash
source /opt/ros/humble/setup.bash
colcon build --packages-select \
    face_recognition_core face_recognition_ros2_interfaces face_recognition_ros2 \
    --cmake-args -DCMAKE_BUILD_TYPE=Release
source install/ros2/setup.bash
```

---

## 2. 启动方式

### 2.1 USB 摄像头一键启动（推荐）

`usb_cam_face.launch.py` 会同时拉起
**usb_cam → face_recognition_node → face_viewer_node → face_stream_server**：

```bash
source scripts/setup_env.sh
ros2 launch face_recognition_ros2 usb_cam_face.launch.py
```

启动后**浏览器打开** <http://localhost:8090/>

页面实时显示摄像头画面 + 识别结果（绿色边框 + 姓名 + 职位 + UUID + 置信度）。

> **为什么不用 `rqt_image_view` / OpenCV 窗口？**
> `cv::imshow` 与 rqt（Qt5）都走 GTK3 / Qt → GIO 模块加载。在装有 snap 应用
> （VS Code 等）的机器上，GIO 会从 snap 路径加载 GTK 模块，进而拉入
> snap-core20 的 `libpthread.so.0`（glibc 2.31），与系统 glibc 2.35 ABI 不兼容：
> `symbol lookup error: __libc_pthread_init, version GLIBC_PRIVATE`。
> `face_stream_server` 只依赖 libmicrohttpd（无 Qt/GTK），headless 安全。

### 2.2 不带 usb_cam / viewer

适用于已有图像源（`image_publisher`、`v4l2_camera` 等）：

```bash
ros2 launch face_recognition_ros2 face_recognition.launch.py
```

`v0.4` 起默认也会拉起 `face_viewer_node` 与 `face_stream_server`，关闭方式：

```bash
ros2 launch face_recognition_ros2 face_recognition.launch.py \
    launch_viewer:=false enable_stream_server:=false
```

---

## 3. Launch 参数

### 3.1 `usb_cam_face.launch.py`

| 参数 | 默认值 | 说明 |
|---|---|---|
| `detection_model` | `<project>/models/det_10g.onnx` | RetinaFace 检测模型 |
| `recognition_model` | `<project>/models/w600k_r50.onnx` | ArcFace 模型 |
| `image_topic` | `/image_raw` | 输入图像话题 |
| `result_topic` | `/face/recognition_result` | 识别结果话题 |
| `annotated_topic` | `/face/annotated` | 标注图话题 |
| `confidence_threshold` | `0.5` | 识别相似度阈值 |
| `video_device` | `/dev/video0` | V4L2 摄像头设备 |
| `image_width` / `image_height` | `640` / `480` | 分辨率 |
| `pixel_format` | `yuyv` | `yuyv` / `mjpeg` 等 |
| `framerate` | `30` | 帧率 |
| `stream_port` | `8090` | MJPEG HTTP 端口 |
| `stream_image_topic` | 跟随 `annotated_topic` | 推流图像话题 |
| `jpeg_quality` | `80` | JPEG 质量（0–100） |
| `enable_stream_server` | `true` | 是否拉起推流服务 |
| `show_window` | `false` | **DEPRECATED**，viewer 不再调用 `cv::imshow` |
| `window_wait_ms` | `30` | **DEPRECATED** |

### 3.2 `face_recognition.launch.py`

| 参数 | 默认值 | 说明 |
|---|---|---|
| `detection_model` | `<project>/models/det_10g.onnx` | 检测模型 |
| `recognition_model` | `<project>/models/w600k_r50.onnx` | 识别模型 |
| `image_topic` | `/image_raw` | 输入图像话题 |
| `result_topic` | `/face/recognition_result` | 结果话题 |
| `confidence_threshold` | `0.5` | 相似度阈值 |
| `launch_viewer` | `true` | 是否拉起 `face_viewer_node` |
| `annotated_topic` | `/face/annotated` | 标注图话题 |
| `stream_port` | `8090` | 推流端口（`0` 表示禁用） |
| `enable_stream_server` | `true` | 是否拉起推流服务 |

### 3.3 手动指定参数示例

```bash
ros2 launch face_recognition_ros2 usb_cam_face.launch.py \
    image_topic:=/camera/image_raw \
    confidence_threshold:=0.6 \
    detection_model:=/opt/face_models/det_10g.onnx \
    recognition_model:=/opt/face_models/arcface.onnx
```

---

## 4. ROS 参数（节点级）

`face_recognition_node` 声明以下参数（定义见
`src/face_recognition_ros2/config/params.yaml`）：

| 参数 | 默认值 | 说明 |
|---|---|---|
| `image_topic` | `/image_raw` | 输入图像话题 |
| `result_topic` | `/face/recognition_result` | 结果话题 |
| `confidence_threshold` | `0.5` | 识别相似度阈值 |
| `db_path` | `/tmp/face_db/faces.db` | SQLite 数据库路径 |
| `faces_dir` | `/tmp/face_db/faces` | 缩略图目录 |
| `detection_model` | `models/det_10g.onnx` | 检测模型 |
| `recognition_model` | `models/w600k_r50.onnx` | 识别模型 |

`db_path` / `faces_dir` **不在 launch 文件中暴露**，需要修改 `params.yaml` 后传入：

```bash
ros2 launch face_recognition_ros2 face_recognition.launch.py \
    --params-file src/face_recognition_ros2/config/params.yaml
```

`face_stream_server` 的参数：

| 参数 | 默认值 | 说明 |
|---|---|---|
| `image_topic` | `/face/annotated` | 要推流的图像话题 |
| `port` | `8090` | HTTP 端口 |
| `jpeg_quality` | `80` | JPEG 质量 |

---

## 5. 启动成功日志

```
[INFO] [face_recognition_node]: Initializing Face Recognition Node...
[INFO] [face_recognition_node]: Detection model: .../models/det_10g.onnx
[INFO] [face_recognition_node]: Recognition model: .../models/w600k_r50.onnx
[INFO] [face_recognition_node]: Face detector initialized
[INFO] [face_recognition_node]: Face recognizer initialized
[INFO] [face_recognition_node]: Face database initialized with 0 faces
[INFO] [face_recognition_node]: Face Recognition Node initialized
```

`face_stream_server` 还会打印：

```
[INFO] [face_stream_server]: Face stream server ready
[INFO] [face_stream_server]:   http://localhost:8090/            (auto-refresh page)
[INFO] [face_stream_server]:   http://localhost:8090/stream      (raw MJPEG stream)
[INFO] [face_stream_server]:   http://localhost:8090/latest.jpg  (single frame)
```

---

## 6. 节点启动时的自动 embedding 补全

`face_recognition_node` 构造时会遍历人脸库：对**`embedding` 为空但
`image_path` 存在**的记录，重新执行检测 + 特征提取并写回数据库。

这意味着用 Web 后台录入（且未加载识别模型）的人脸，**在节点启动后立即变为可识别**，
无需手动 `backfill`。

---

## 7. 验证节点状态

```bash
# 节点列表
ros2 node list
# → /face_recognition_node
# → /face_viewer_node
# → /face_stream_server

# 话题列表
ros2 topic list
# → /image_raw
# → /face/recognition_result
# → /face/annotated

# Service 列表
ros2 service list | grep face_db
# → /face_db/add
# → /face_db/remove
# → /face_db/list
# → /face_db/clear
# → /face_db/add_template
# → /face_db/verify

# 图像流频率
ros2 topic hz /image_raw
```

---

## 8. 订阅识别结果

```bash
ros2 topic echo /face/recognition_result
```

单帧输出示例：

```yaml
header:
  stamp: {sec: 1725960000, nanosec: 123456789}
  frame_id: ''
faces:
- id: 550e8400-e29b-41d4-a716-446655440000
  name: '张三'
  title: '工程师'
  confidence: 0.9123
  scene: 'office'
  map_location: '5F-A区'
  x: 320
  y: 240
  width: 128
  height: 128
```

---

## 9. 人脸库管理（Service）

```bash
# 新增（image_data 为 base64 字符串）
ros2 service call /face_db/add face_recognition_ros2_interfaces/srv/AddFace \
  "{name: '张三', title: '工程师', image_data: '<BASE64>', \
    scene: 'office', map_location: '5F-A区'}"

# 列出
ros2 service call /face_db/list face_recognition_ros2_interfaces/srv/ListFaces

# 删除（id 来自 ListFaces 或 AddFace 返回值）
ros2 service call /face_db/remove face_recognition_ros2_interfaces/srv/RemoveFace \
  "{id: '<UUID>'}"

# 清空
ros2 service call /face_db/clear face_recognition_ros2_interfaces/srv/ClearFaces

# 追加一"枪"（多模板：外观变化时用同一人的另一张照片补强）
ros2 service call /face_db/add_template face_recognition_ros2_interfaces/srv/AddTemplate \
  "{id: '<UUID>', image_data: '<BASE64>'}"

# 校验：对全库打分并返回判定，不入库（等价于 face_recognition_app verify）
ros2 service call /face_db/verify face_recognition_ros2_interfaces/srv/Verify \
  "{image_data: '<BASE64>', threshold: 0.0}"
```

字段与返回定义见 [../protocol/ros_interfaces.md](../protocol/ros_interfaces.md)。

---

## 10. HTTP 推流接口验证

```bash
# 健康检查
curl -s http://localhost:8090/healthz
# → ok (200) 或 no_frame_yet (503)

# 单帧 JPEG
curl -o frame.jpg http://localhost:8090/latest.jpg
file frame.jpg
# → frame.jpg: JPEG image data, ..., 640x480, components 3
```

详见 [../protocol/web_api.md](../protocol/web_api.md)。

---

## 11. 故障排查要点

| 现象 | 排查 |
|---|---|
| 无任何人脸检出 | `ros2 topic hz /image_raw` 确认图像流有数据 |
| 模型加载失败 | 检查 `<project>/models/*.onnx` 是否存在，先 `./build.sh MODELS` |
| 日志持续刷 `[FaceDetector] detect() exception` | 模型输出 layout 不是 RetinaFace / YOLOv8，检查模型文件 |
| 浏览器打不开 8090 | 确认 `enable_stream_server:=true`，端口未被占用 |
| ROS2 日志目录不可写 | `export ROS_LOG_DIR=/tmp/roslog` |

更多见 [../FAQ/troubleshooting.md](../FAQ/troubleshooting.md)。

---

## 12. 相关文档

- [核心算法库模块说明](core.md)
- [ROS 接口文档](../protocol/ros_interfaces.md)
- [源码开发快速开始](../setup.md)
