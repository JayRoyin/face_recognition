# ROS 接口文档（ros_interfaces）

ROS1（Noetic）与 ROS2（Humble）两个版本**话题名、服务名与字段语义完全一致**，
仅消息定义的书写格式不同（ROS1 用 `.msg` + `Header`，ROS2 用 `.msg` +
`std_msgs/Header` + `builtin_interfaces/Time`）。

接口定义位置：

```
src/face_recognition_ros2_interfaces/{msg,srv}/
src/face_recognition_ros1_interfaces/{msg,srv}/
```

---

## 1. Topic

| 话题 | 类型 | 方向 | 队列 | 说明 |
|---|---|---|---|---|
| `/image_raw` | `sensor_msgs/Image` | 订阅 | 10（ROS2）/ 1（ROS1） | 输入视频流，`cv_bridge` 转 `bgr8` |
| `/face/recognition_result` | `FaceResult` | 发布 | 10 | 每帧识别结果（无人脸命中时不发布） |
| `/face/annotated` | `sensor_msgs/Image` | 发布 | — | **仅 ROS2**，`face_viewer_node` 输出的标注图 |

> 话题名均可通过参数覆盖：`image_topic` / `result_topic` / `annotated_topic`。

---

## 2. Message 定义

### 2.1 FaceInfo

**ROS2**（`face_recognition_ros2_interfaces/msg/FaceInfo.msg`）：

```
string id
string name
string title
float32 confidence
string scene
string map_location
builtin_interfaces/Time timestamp
uint32 x
uint32 y
uint32 width
uint32 height
```

**ROS1**（`face_recognition_ros1_interfaces/msg/FaceInfo.msg`）：字段同构，
时间类型由 `Header` 承载。

| 字段 | 含义 |
|---|---|
| `id` | 人脸库记录 UUID |
| `name` | 姓名 |
| `title` | 职位 |
| `confidence` | 与库内记录的相似度（0~1），由 `compute_similarity` 计算 |
| `scene` | 场景标签 |
| `map_location` | 物理位置 |
| `timestamp` | 识别时间 |
| `x` `y` `width` `height` | 人脸框（原图像素坐标，左上角原点） |

### 2.2 FaceResult

**ROS2**：

```
std_msgs/Header header
FaceInfo[] faces
```

**ROS1**：

```
Header header
FaceInfo[] faces
```

`header` 直接沿用输入图像的 header（时间戳与 `frame_id` 与源帧对齐）。

### 2.3 结果示例

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

> **发布策略**：`imageCallback` 内先检测，无检测框直接 `return`；识别后若
> `faces` 为空同样**不发布**。即只有"至少命中一张已登记人脸"的帧才会出现在话题上。

---

## 3. Service

| Service | 类型 | 用途 |
|---|---|---|
| `/face_db/add` | `AddFace` | 新增人脸记录 |
| `/face_db/remove` | `RemoveFace` | 按 ID 删除 |
| `/face_db/list` | `ListFaces` | 列出全部记录 |
| `/face_db/clear` | `ClearFaces` | 清空人脸库 |

### 3.1 AddFace.srv

```
string name          # 姓名（必填）
string title         # 职位
string image_data    # base64 编码的人脸图片（可选；为空则只录文字信息）
string scene         # 场景标签
string map_location  # 物理位置
---
string id            # 新记录 UUID
bool   success
string message
```

**服务端行为**：

1. `image_data` 非空 → base64 解码
2. `name` 为空 → `success=false`，`message="Name is required"`
3. 解码成功且 detector / recognizer 可用 → 检测最大人脸并提取 512 维 embedding
4. `scene` 为空 → `"default"`；`map_location` 为空 → `"unknown"`
5. 入库成功 → `success=true`，`message="Face added successfully"`

> **注意**：即使检测不到人脸，记录**仍会入库**（embedding 为 `NULL`），
> 只是无法被识别，需要后续 backfill 补全。

### 3.2 RemoveFace.srv

```
string id
---
bool   success
string message
```

`id` 为空 → `success=false`，`message="Face ID is required"`；
记录不存在 → `message="Face not found"`；成功 → `"Face removed"`。

### 3.3 ListFaces.srv

```
---
FaceInfo[] faces
int32      count
```

请求为空。`count` 为记录总数。返回的 `FaceInfo` 仅填充
`id` / `name` / `title` / `scene` / `map_location`（不含 bbox 与置信度）。

### 3.4 ClearFaces.srv

```
---
int32 deleted_count
bool  success
```

请求为空，`success` 恒为 `true`。

---

## 4. 调用示例

### 4.1 ROS2

```bash
ros2 service call /face_db/add face_recognition_ros2_interfaces/srv/AddFace \
  "{name: '张三', title: '工程师', image_data: '<BASE64>', \
    scene: 'office', map_location: '5F-A区'}"

ros2 service call /face_db/list face_recognition_ros2_interfaces/srv/ListFaces
ros2 service call /face_db/remove face_recognition_ros2_interfaces/srv/RemoveFace \
  "{id: '<UUID>'}"
ros2 service call /face_db/clear face_recognition_ros2_interfaces/srv/ClearFaces

ros2 topic echo /face/recognition_result
```

### 4.2 ROS1

```bash
rosservice call /face_db/add "name: '张三'
title: '工程师'
image_data: '<BASE64>'
scene: 'office'
map_location: '5F-A区'"

rosservice call /face_db/list
rosservice call /face_db/remove "id: '<UUID>'"
rosservice call /face_db/clear

rostopic echo /face/recognition_result
```

---

## 5. 生成 base64 图片

```bash
# Linux
base64 -w0 alice.jpg

# 或 Python
python3 -c "import base64,sys; print(base64.b64encode(open('alice.jpg','rb').read()).decode())"
```

**不要**带 `data:image/jpeg;base64,` 前缀，仅传纯 base64 字符串。

---

## 6. 相关文档

- [ROS2 节点模块使用文档](../module/ros2.md)
- [ROS1 节点模块使用文档](../module/ros1.md)
- [数据库 schema](database_schema.md)
