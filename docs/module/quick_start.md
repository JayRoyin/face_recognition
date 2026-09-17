# 专项模块快速开始（quick_start）

本项目包含 5 个可独立构建/运行的应用层模块 + 1 个公共核心库。本文档给出每个模块的
**最短跑通路径**，详细说明请进入对应子文档。

---

## 0. 模块总览

| 模块 | 构建命令 | 运行入口 | 依赖 ROS | 详细文档 |
|---|---|---|---|---|
| 核心算法库 | `./build.sh CORE` | 静态/动态库，无独立进程 | ✗ | [core.md](core.md) |
| 非 ROS 实时识别 | `./build.sh STANDALONE` | `face_recognition_app` | ✗ | [standalone.md](standalone.md) |
| ROS2 节点 | `./build.sh ROS2` | `ros2 launch face_recognition_ros2 ...` | ✓ Humble | [ros2.md](ros2.md) |
| ROS1 节点 | `./build.sh ROS1` | `roslaunch face_recognition_ros1 ...` | ✓ Noetic | [ros1.md](ros1.md) |
| Web 人脸库后台 | `./build.sh WEB` | `face_db_web` | ✗ | [web.md](web.md) |

> 所有模块共用同一份模型权重、同一套 SQLite schema、同一个默认数据库路径
> `/tmp/face_db/faces.db`，因此**跨模块互通**（Web 录入 → ROS/standalone 立即识别）。

---

## 1. 前置：模型与数据库

```bash
cd ~/Royin_Project/face_recognition

# 下载检测 / 识别模型（必需）
./build.sh MODELS

# 准备人脸库目录（所有模块的默认路径）
mkdir -p /tmp/face_db/faces
```

模型清单：

| 文件 | 作用 | 说明 |
|---|---|---|
| `models/det_10g.onnx` | RetinaFace 人脸检测 | 9 输出，含 5 点关键点 |
| `models/w600k_r50.onnx` | ArcFace 特征提取 | 输出 512 维 embedding |
| `models/yolov8n.onnx` | YOLOv8 通用检测（备用 backend） | — |
| `models/1k3d68.onnx` / `2d106det.onnx` / `genderage.onnx` | 备用/扩展模型 | 当前主流程未使用 |

---

## 2. 核心算法库（face_recognition_core）

```bash
./build.sh CORE
```

产物：`build/core/libface_recognition_core.so*`

对外三类能力（C++ API）：

```cpp
FaceDetector   detector;   detector.initialize("models/det_10g.onnx", 0.5f, 0.5f, 640);
FaceRecognizer recognizer; recognizer.initialize("models/w600k_r50.onnx");
FaceDatabase   database;   database.initialize("/tmp/face_db/faces.db", "/tmp/face_db/faces");
```

详见 [core.md](core.md) 与 [../services/](../services/)。

---

## 3. 非 ROS 实时识别（face_recognition_standalone）

```bash
./build.sh STANDALONE

APP=./install/bin/face_recognition_app

# ① 实时识别（默认 USB 摄像头 /dev/video0）
$APP run --source 0 --input-size 320
```

按 `q` / `ESC` 退出。

**人脸从哪里来？** 推荐用 Web 后台传图录入（见 [第 6 节](#6-web-人脸库后台face_db_web)），
录入完成后再执行上面的 `run` 即可；CLI 的 `add` / `add-bulk` / `add-template`
适合批量与脚本化场景：

```bash
$APP add --image ~/photos/alice.jpg --name "张三" --title "工程师" --scene office
$APP list
```

详见 [standalone.md](standalone.md) 与 [web.md](web.md)。

---

## 4. ROS2 节点（face_recognition_ros2）

```bash
./build.sh ROS2
source scripts/setup_env.sh

# 一键：usb_cam + 识别节点 + 标注节点 + MJPEG 推流
ros2 launch face_recognition_ros2 usb_cam_face.launch.py
```

浏览器打开 <http://localhost:8090/> 查看实时标注画面。
详见 [ros2.md](ros2.md)。

---

## 5. ROS1 节点（face_recognition_ros1）

```bash
./build.sh ROS1
source build/ros1/devel/setup.bash

roslaunch face_recognition_ros1 face_recognition.launch \
    detection_model:=$PWD/models/det_10g.onnx \
    recognition_model:=$PWD/models/w600k_r50.onnx
```

详见 [ros1.md](ros1.md)。

---

## 6. Web 人脸库后台（face_db_web）

```bash
./build.sh WEB

./install/bin/face_db_web \
    --port 8080 \
    --db /tmp/face_db/faces.db \
    --faces-dir /tmp/face_db/faces \
    --detection-model "$(pwd)/models/det_10g.onnx" \
    --recognition-model "$(pwd)/models/w600k_r50.onnx"
```

浏览器打开 <http://localhost:8080/> 录入/删除人脸。
详见 [web.md](web.md)。

---

## 7. 推荐组合工作流

```
① Web 后台批量录入  ──────────┐
                              ├─► 同一份 SQLite（/tmp/face_db/faces.db）
② standalone CLI 批量入库 ────┘
                              │
                              ▼
            ③ ROS2 节点 / standalone run 实时识别
```

```bash
# 一次性准备
./build.sh MODELS
./build.sh WEB
./build.sh STANDALONE

# 终端 A：启动 Web 录入（录入时即完成检测 + 提特征）
./install/bin/face_recognition_app web --port 8080

# 浏览器打开 http://localhost:8080/ ，在 “Add Face” 表单传图：
#   · Name 必填（可另填 Title / Scene / Map Location）
#   · “Or Upload” 选本地照片，或在 “Image URL” 填图片链接
#   · 点 “Add Face” → 提示 Face added with embedding
#   · 下方 Face List 实时显示已录人脸

# 终端 B：实时识别（与 Web 共享同一个 DB，录完即可识别）
./install/bin/face_recognition_app run --source 0 --input-size 320
```

> `--detect-every-n` 默认即为 `2`，`--recognition-threshold` 默认 `0.5`，
> 无需再手动传。若画面一直显示 `Unknown`，见
> [../FAQ/troubleshooting.md](../FAQ/troubleshooting.md) 的 Q17。

**同一个人要补多张照片**（例如不戴眼镜 / 侧脸，提高召回）时用 CLI 的
`add-template`（Web 页面目前只提供新增 / 删除 / 清空）：

```bash
APP=./install/bin/face_recognition_app
$APP list                                                    # 取到 <face_id>
$APP add-template --id <face_id> --image ~/photos/alice2.jpg  # 补一枪
$APP list                                                    # templates 变成 2
```

若历史上出现过 `emb=NO` 的记录（例如早期 Web 端未加载识别模型），补全一次：

```bash
$APP backfill          # 只补 embedding 为空的记录
$APP backfill --all    # 强制重算全部（换过前端 / 模型后用）
# 或在 run 时自动补全
$APP run --source 0 --auto-backfill
```

---

## 8. 相关文档

- [源码开发快速开始](../setup.md)
- [架构说明](../architecture.md)
- [接口、协议与配置](../protocol/)
- [常见问题排查](../FAQ/troubleshooting.md)
