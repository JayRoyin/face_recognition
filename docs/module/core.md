# 核心算法库模块说明（face_recognition_core）

`face_recognition_core` 是全项目**唯一的算法实现层**，被 ROS1 / ROS2 / standalone /
web 四个应用层模块共同复用。它对外暴露三个类 + 一套数据类型。

- 源码：`src/face_recognition_core/`
- 语言标准：C++17
- 产物：`libface_recognition_core.so.1`（`VERSION 1.0.0`，`SOVERSION 1`）

---

## 1. 目录结构

```
src/face_recognition_core/
├── CMakeLists.txt
├── include/face_recognition_core/
│   ├── types.hpp              # 通用数据类型 + 相似度函数
│   ├── face_detector.hpp      # 人脸检测接口
│   ├── face_recognizer.hpp    # 人脸特征提取接口
│   └── face_database.hpp      # 人脸库（SQLite3）接口
└── src/
    ├── face_detector.cpp      # RetinaFace / YOLOv8 双 backend 后处理
    ├── face_recognizer.cpp    # ArcFace 推理 + 512-d embedding
    └── face_database.cpp      # SQLite3 CRUD + 余弦相似度检索
```

---

## 2. 依赖

| 依赖 | 类型 | 用途 |
|---|---|---|
| OpenCV ≥ 4.5 | 系统 | 图像解码、缩放（resize）、绘制、`cv::dnn` |
| ONNX Runtime（C++） | 外部 | RetinaFace 推理（含动态 Reshape，OpenCV DNN 不支持） |
| SQLite3 | vendored 优先 | 人脸库存储 |
| libuuid | 系统 | UUID v4 生成 |
| pthread | 系统 | `FaceDatabase` 互斥保护 |

CMake 中 SQLite3 的解析优先级：

1. `install/vendored/{include,lib}` 存在 vendored 产物 → 使用 vendored
2. 否则回退 `pkg_check_modules(sqlite3)`

ONNX Runtime 的探测路径见 [../setup.md](../setup.md#3-onnx-runtimec-版)。

---

## 3. 数据类型（types.hpp）

```cpp
struct BoundingBox { uint32_t x, y, width, height; };

struct FaceDetection {
    BoundingBox bbox;
    float confidence;
    std::vector<float> landmarks;   // 5 点关键点 → 10 个 float，RetinaFace 填充
};

struct FaceInfo {                   // 一次识别命中的业务信息
    std::string id, name, title, scene, map_location;
    float confidence;
    time_t timestamp;
    BoundingBox bbox;
};

struct FaceRecord {                 // 人脸库中的一条记录
    std::string id, name, title, image_path, scene, map_location;
    std::vector<float> embedding;
    time_t created_at, updated_at;
};

float compute_similarity(const std::vector<float>& a, const std::vector<float>& b);
```

`compute_similarity` 计算两个 **已 L2 归一化** 向量的内积，并截断到 `[0, 1]`，
即余弦相似度。

---

## 4. 公共 API

### 4.1 FaceDetector

```cpp
bool initialize(const std::string& model_path,
                float confidence_threshold = 0.5f,
                float nms_threshold        = 0.5f,
                int   input_size           = 640);
std::vector<FaceDetection> detect(const cv::Mat& image, int max_faces = 10);
void        setInputSize(int width, int height);
std::string backend() const;        // "retinaface" | "yolov8" | "unknown"
std::string getLastError() const;
```

- **backend 自动选择**：按输出张量 shape 判定，无需手动配置
  - RetinaFace：9 个输出 `(conf, bbox, landmarks) × 3 层`
  - YOLOv8：单输出 `[1, 84, N]`
- **日志策略**：shape 不匹配时仅打印**一次**完整 dims/size，避免刷屏。

### 4.2 FaceRecognizer

```cpp
bool initialize(const std::string& model_path);
std::vector<float> extract_embedding(const cv::Mat& image, const BoundingBox& bbox);
float compute_similarity(const std::vector<float>& a, const std::vector<float>& b);
void  setInputSize(int width, int height);
std::string getLastError() const;
```

- 输入：与 `detect()` 同一张原图 + 检测框
- 内部流程：按 bbox 裁剪 → resize 112×112 → 归一化 → ArcFace 推理 → L2 归一化
- 输出：512 维 `float` 向量（对应 BMP 存储为 2048 字节 BLOB）

### 4.3 FaceDatabase

```cpp
bool initialize(const std::string& db_path, const std::string& faces_dir);

std::string add_face(const std::string& name,
                     const std::vector<float>& embedding,
                     const std::vector<uint8_t>& image_data = {},
                     const std::string& title        = "",
                     const std::string& scene        = "default",
                     const std::string& map_location = "unknown");

bool remove_face(const std::string& face_id);
std::shared_ptr<FaceRecord> get_face(const std::string& face_id);
std::vector<FaceRecord>     list_faces();
std::shared_ptr<FaceRecord> find_matching_face(const std::vector<float>& embedding,
                                               float threshold = 0.7f);
int  clear_all();
int  get_face_count();
bool save_image(const std::string& face_id, const std::vector<uint8_t>& data,
                std::string& out_path);
bool update_embedding(const std::string& face_id, const std::vector<float>& embedding);
```

行为约定：

| 行为 | 说明 |
|---|---|
| 目录自动创建 | `initialize()` 会递归创建 `faces_dir` 与 `db_path` 的父目录 |
| 表自动创建 | `CREATE TABLE IF NOT EXISTS faces (...)` |
| 返回 `""` | `add_face` 失败（`name` 为空或写入失败） |
| `embedding` 为空 | 允许入库（`NULL`），但无法被识别；需 `backfill` 补全 |
| `remove_face` | 同时删除记录与对应缩略图文件 |
| `clear_all` | 逐条删除记录与图片，返回删除条数 |
| 阈值语义 | `find_matching_face` 要求相似度 **严格大于** `threshold` |
| 线程安全 | 全部公开方法由 `std::recursive_mutex` 保护 |

---

## 5. 构建与安装

```bash
./build.sh CORE
```

等价的手工流程：

```bash
cmake -S src/face_recognition_core -B src/face_recognition_core/build \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=$PWD/install
cmake --build src/face_recognition_core/build -j
cmake --install src/face_recognition_core/build
```

安装内容：

```
install/lib/libface_recognition_core.so.1.0.0
install/include/face_recognition_core/*.hpp
```

**RPATH**：构建时会把 `install/vendored/lib`（或 in-tree
`third_party/sqlite3/build`）写入 `RUNPATH`，使 `.so` 优先加载 vendored SQLite3。
消费者无需额外设置 `LD_LIBRARY_PATH`。

---

## 6. 被谁使用

| 消费方 | 使用方式 |
|---|---|
| `face_recognition_ros2` | `add_subdirectory(../face_recognition_core)` 直接链接源码 |
| `face_recognition_ros1` | 同上（catkin） |
| `face_recognition_standalone` | 链接 `install/` 中的 `libface_recognition_core.so` |
| `face_db_web` | 同上，并设置 `INSTALL_RPATH=$ORIGIN/../lib` |

---

## 7. 相关文档

- [人脸检测模块 face_detector](../services/face_detector.md)
- [人脸识别模块 face_recognizer](../services/face_recognizer.md)
- [人脸库模块 face_database](../services/face_database.md)
- [数据库 schema](../protocol/database_schema.md)
