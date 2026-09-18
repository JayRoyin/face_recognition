# ROS1 节点模块使用文档（face_recognition_ros1）

ROS1 Noetic 版本的人脸识别功能包。与 ROS2 版本**共用同一套
`face_recognition_core`、模型权重与 SQLite schema**，接口语义完全一致。

- 源码：`src/face_recognition_ros1/`
- 接口包：`src/face_recognition_ros1_interfaces/`
- 可执行：`face_recognition_node`（单个节点，无独立 viewer / stream server）
- License：MIT

> ROS1 版本不包含 `face_viewer_node` 与 `face_stream_server`。需要可视化时，
> 可使用 `image_view`、`rqt_image_view`，或改用 ROS2 版本 / standalone 版本。

---

## 1. 编译

```bash
./build.sh ROS1
```

内部流程：`build_vendored_sqlite` → `build_core` → `catkin_make`（在
`src/face_recognition_ros1/` 下执行）。

等价手工流程：

```bash
source /opt/ros/noetic/setup.bash
cd src/face_recognition_ros1
catkin_make -DCMAKE_BUILD_TYPE=Release
source devel/setup.bash
```

产物：`src/face_recognition_ros1/devel/lib/face_recognition_ros1/face_recognition_node`

---

## 2. 启动

```bash
source /opt/ros/noetic/setup.bash
source build/ros1/devel/setup.bash

roslaunch face_recognition_ros1 face_recognition.launch \
    detection_model:=$PWD/models/det_10g.onnx \
    recognition_model:=$PWD/models/w600k_r50.onnx
```

---

## 3. Launch 参数

`launch/face_recognition.launch`：

| 参数 | 默认值 | 说明 |
|---|---|---|
| `image_topic` | `/image_raw` | 输入图像话题 |
| `result_topic` | `/face/recognition_result` | 识别结果话题 |
| `confidence_threshold` | `0.5` | 识别相似度阈值 |
| `db_path` | `/data/hhqs_data/face_db/faces.db` | SQLite 数据库路径 |
| `faces_dir` | `/data/hhqs_data/face_db/faces` | 缩略图目录 |
| `detection_model` | `/models/det_10g.onnx` | 检测模型 |
| `recognition_model` | `/models/w600k_r50.onnx` | 识别模型 |

> 默认模型路径为绝对路径 `/models/...`，实际使用时**必须**通过 `detection_model:=`
> / `recognition_model:=` 覆盖为项目内 `models/` 的真实路径。

---

## 4. 节点参数

节点运行于私有命名空间（`nh_("~")`），参数读取自
`src/face_recognition_ros1/config/face_recognition.yaml`：

```yaml
face_recognition_node:
  ros__parameters:
    image_topic: "/image_raw"
    result_topic: "/face/recognition_result"
    confidence_threshold: 0.5
    db_path: "/data/hhqs_data/face_db/faces.db"
    faces_dir: "/data/hhqs_data/face_db/faces"
    detection_model: "/models/det_10g.onnx"
    recognition_model: "/models/w600k_r50.onnx"
```

---

## 5. 接口

**Topic**

| 话题 | 类型 | 方向 | 说明 |
|---|---|---|---|
| `/image_raw` | `sensor_msgs/Image` | 订阅 | 输入视频流（`image_transport`，队列 1） |
| `/face/recognition_result` | `face_recognition_ros1_interfaces/FaceResult` | 发布 | 每帧识别结果（队列 10） |

**Service**

| Service | 类型 | 用途 |
|---|---|---|
| `/face_db/add` | `AddFace` | 新增人脸（base64 图片可选） |
| `/face_db/remove` | `RemoveFace` | 按 ID 删除 |
| `/face_db/list` | `ListFaces` | 列出全部 |
| `/face_db/clear` | `ClearFaces` | 清空 |
| `/face_db/add_template` | `AddTemplate` | 给已有身份追加一"枪"（多模板） |
| `/face_db/verify` | `Verify` | 对全库打分并返回判定（不入库） |

字段定义见 [../protocol/ros_interfaces.md](../protocol/ros_interfaces.md)。

---

## 6. 启动日志

```
[INFO] [<ts>]: Initializing Face Recognition Node...
[INFO] [<ts>]: Detection model: /path/models/det_10g.onnx
[INFO] [<ts>]: Recognition model: /path/models/w600k_r50.onnx
[INFO] [<ts>]: Face detector initialized
[INFO] [<ts>]: Face recognizer initialized
[INFO] [<ts>]: Face database initialized with N faces
[INFO] [<ts>]: Face Recognition Node initialized
```

---

## 7. 自动 embedding 补全

与 ROS2 版本一致：节点构造时遍历人脸库，对 `embedding` 为空但 `image_path` 存在的
记录，重新检测 + 提取特征并写回。Web 后台录入的人脸在节点启动后即可识别。

---

## 8. 使用示例

```bash
# 查看节点 / 话题 / 服务
rosnode list
rostopic list
rosservice list | grep face_db

# 订阅识别结果
rostopic echo /face/recognition_result

# 通过 Service 录入（image_data 为 base64）
rosservice call /face_db/add "name: '张三'
title: '工程师'
image_data: '<BASE64>'
scene: 'office'
map_location: '5F-A区'"

# 列出 / 删除 / 清空
rosservice call /face_db/list
rosservice call /face_db/remove "id: '<UUID>'"
rosservice call /face_db/clear

# 追加一"枪"（多模板）与校验（入参为 base64 图片）
rosservice call /face_db/add_template "id: '<UUID>'
image_data: '<BASE64>'"
rosservice call /face_db/verify "image_data: '<BASE64>'
threshold: 0.0"
```

---

## 9. 故障排查

| 现象 | 排查 |
|---|---|
| `Failed to initialize detector` | `detection_model` 仍为默认 `/models/...`，需覆盖为真实路径 |
| 节点启动后无人脸检出 | `rostopic hz /image_raw` 确认图像流 |
| `Face database initialized with 0 faces` | `db_path` 与录入所用路径不一致 |
| catkin_make 找不到接口包 | 确认 `ros-noetic-message-generation` / `message-runtime` 已安装 |

更多见 [../FAQ/troubleshooting.md](../FAQ/troubleshooting.md)。

---

## 10. 相关文档

- [ROS2 节点模块使用文档](ros2.md)
- [ROS 接口文档](../protocol/ros_interfaces.md)
- [核心算法库模块说明](core.md)
