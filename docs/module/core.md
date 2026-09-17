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
├── cmake/face_recognition_coreConfig.cmake.in   # 导出的 CMake package 模板
├── include/face_recognition_core/
│   ├── types.hpp              # 通用数据类型 + 相似度函数
│   ├── face_detector.hpp      # 人脸检测接口
│   ├── face_recognizer.hpp    # 单张人脸的取脸 + 特征提取
│   ├── embedding_policy.hpp   # ★ 统一的"检测 → 特征"质量门控（唯一入口）
│   ├── feature_space.hpp      # ★ 特征指纹：记录 / 校验库内特征所属的配方
│   └── face_database.hpp      # 人脸库（SQLite3）接口
└── src/
    ├── face_detector.cpp      # SCRFD(RetinaFace 系) / YOLOv8 双 backend 后处理
    ├── face_recognizer.cpp    # 取脸（对齐 / bbox 裁剪）+ ArcFace 推理 + 512-d embedding
    └── face_database.cpp      # SQLite3 CRUD + 余弦检索 + 队列归一化判定
```

---

## 2. 依赖

| 依赖 | 类型 | 用途 |
|---|---|---|
| OpenCV ≥ 4.5 | 系统 | 图像解码、缩放、warpAffine、绘制、`cv::dnn`（识别推理） |
| ONNX Runtime（C++） | 外部 | 检测推理（输出含动态 `Reshape`，OpenCV DNN 不支持） |
| SQLite3 | vendored 优先 | 人脸库存储 |
| libuuid | 系统 | UUID v4 生成 |
| pthread | 系统 | `FaceDatabase` 互斥保护 |

> **两个推理引擎并存是刻意的**：检测必须用 ONNX Runtime；识别用哪个都行
> （同一模型两者输出一致），因此识别保留 OpenCV DNN 以减少运行时依赖。
> 详见 [../services/face_detector.md](../services/face_detector.md)。

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
    std::vector<float> landmarks;   // 5 点关键点 → 10 个 float（x0,y0,x1,y1,...）
};

struct FaceTemplate {               // 一个人的一"枪"附加模板
    long long          id = 0;
    std::string        face_id;
    std::vector<float> embedding;
    std::string        image_path;
    time_t             created_at = 0;
};

struct MatchCandidate {             // 一个身份的排名项
    std::string face_id;
    std::string name;
    std::string title;
    float       similarity     = 0.0f;  // 该身份所有模板的最高余弦
    long long   template_id    = -1;    // -1 表示命中的是主模板 faces.embedding
    int         template_count = 0;
};

struct MatchResult {                // 最终判定
    bool        accepted = false;
    std::string face_id;
    std::string name;
    std::string title;
    float       raw_similarity = 0.0f;
    float       z_score        = 0.0f; // 队列归一化后的 z（未归一化时为 0）
    float       cohort_median  = 0.0f;
    float       cohort_mad     = 0.0f;
    int         cohort_size    = 0;
    bool        normalized     = false; // 队列样本是否足够
    std::string reason;                 // 未通过的原因
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

`compute_similarity` 计算两个**已 L2 归一化**向量的内积，即余弦相似度：

- 只把上界截断到 `1.0`（吸收浮点误差）；
- **不截断负值**——负数表示"明显不相似"，是有效信息，也会参与队列统计。

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

- **backend 自动选择**：按**输出张量数量**判定，无需手动配置
  - 9 个输出 → `retinaface`（`det_10g.onnx`，实际是 SCRFD 系列）
  - 其它 → `yolov8`（单输出 `[1, 84, N]`）
- **关键点**：`retinaface` backend 填充 `landmarks`（10 个 float）；`yolov8` backend 不填充
- **预处理**：始终 letterbox（等比缩放 + 补边），不拉伸人脸
- **日志策略**：shape 不匹配时仅打印**一次**完整 dims/size，避免刷屏

### 4.2 FaceRecognizer

```cpp
bool initialize(const std::string& model_path);

std::vector<float> extract_embedding(const cv::Mat& image, const BoundingBox& bbox);
std::vector<float> extract_embedding(const cv::Mat& image, const BoundingBox& bbox,
                                     const std::vector<float>& landmarks,
                                     bool* used_alignment = nullptr);

float compute_similarity(const std::vector<float>& a, const std::vector<float>& b);
void  setInputSize(int width, int height);
void  setAlignmentEnabled(bool enabled);       // 默认 true
bool  alignmentEnabled() const;
void  alignmentStats(int* aligned, int* fallback) const;
std::string getLastError() const;
```

- 输入：与 `detect()` 同一张原图 + 检测框（可选带 5 点关键点）
- **两个取脸前端**（默认对齐）：
  1. 关键点对齐：相似变换 warp 到 ArcFace 标准 112×112 模板
  2. bbox 裁剪：框外扩 20% 取 `1.4×` 区域后 resize
- `used_alignment` 回传本次实际用的前端；调用方**必须用它防止库内混用**
- 输出：512 维 `float` 向量（存储为 2048 字节 BLOB）
- 归一化：`RGB` + 缩放到 `[0, 1]`，随后 L2 归一化

> ⚠️ **不要直接调用 `extract_embedding()` 产生入库 / 查询特征**，请走下面的统一门控。

#### 统一门控：`embedding_policy.hpp`

```cpp
struct EmbeddingPolicy {
    int  min_face_size     = 80;    // 0 = 不限制
    bool require_alignment = true;  // 要求对齐前端，拒绝 bbox 回退
    bool allow_unaligned   = false;
};

struct EmbeddingOutcome {
    std::vector<float> embedding;
    bool               used_alignment = false;
    std::string        reject_reason;   // 非空即被拒
    bool ok() const;
};

EmbeddingOutcome make_embedding(FaceRecognizer& recognizer,
                                const cv::Mat& image,
                                const FaceDetection& det,
                                const EmbeddingPolicy& policy);
```

所有**产生**特征的路径（录入、`add-template`、`backfill`）与所有**查询**路径
（实时识别、`verify`）都走它。

> **为什么把它放进 core**：这个门控原先只存在于 standalone 的
> `RecognitionPipeline` 里，于是 `face_db_web` 与两个 ROS 节点直接调用
> `extract_embedding()` 绕过了它 —— 同一个人脸库里因此可以混入不同规则下产生的
> 特征，而数据本身**没有任何标记**能暴露这一点，外部表现就是"模型坏了"。

#### 特征指纹：`feature_space.hpp`

```cpp
/** 由流水线的编译期常量 + 模型文件身份生成，配方一变它就变。 */
std::string feature_space_id(const std::string& recognition_model_path);

enum class FeatureSpaceStatus { OK, STAMPED, MISMATCH };

FeatureSpaceStatus check_feature_space(FaceDatabase& db,
                                       const std::string& current_id,
                                       std::string* stored_id = nullptr);
```

首次使用时把当前指纹写入 `metadata` 表（`feature_space_id`），之后每次启动比对；
不一致就打印 `stored` / `current` 与 `backfill --all` 修复命令。

指纹串形如：

```
fs1|det=scrfd-stride|front=arcface112-align|norm=rgb-pm127.5|model=174383860:1632137312
```

- `det` / `front` / `norm` 是**流水线的编译期常量**，改代码就会变；
- `model` 用**大小 + mtime** 而非内容哈希 —— 每次启动哈希 170 MB 的 ONNX 不值那一秒，
  而换模型必然改变其中之一。

> 存在意义：一个 512 维 float blob **不含任何"我是用哪套配方算出来的"信息**。
> 前端 / 归一化 / 模型任何一项变更后，库里的旧特征与查询特征不再可比，表现与
> "模型变差"完全一致而无法归因。有了指纹，这种静默失效就变成启动时的一条明确告警。

详见 [../services/face_recognizer.md](../services/face_recognizer.md)。

### 4.3 FaceDatabase

```cpp
bool initialize(const std::string& db_path, const std::string& faces_dir);

// --- 身份 ---------------------------------------------------------------
std::string add_face(const std::string& name,
                     const std::vector<float>& embedding,
                     const std::vector<uint8_t>& image_data = {},
                     const std::string& title        = "",
                     const std::string& scene        = "default",
                     const std::string& map_location = "unknown");
bool remove_face(const std::string& face_id);
std::shared_ptr<FaceRecord> get_face(const std::string& face_id);
std::vector<FaceRecord>     list_faces();
int  clear_all();
int  get_face_count();

// --- 附加模板（multi-shot）----------------------------------------------
bool add_template(const std::string& face_id,
                  const std::vector<float>& embedding,
                  const std::string& image_path = "");
std::vector<FaceTemplate> list_templates(const std::string& face_id);
int  template_count(const std::string& face_id);
bool remove_templates(const std::string& face_id);

// --- 匹配 ---------------------------------------------------------------
std::vector<MatchCandidate> rank_faces(const std::vector<float>& embedding);

MatchResult match_face(const std::vector<float>& embedding,
                       float raw_threshold,
                       float z_threshold = 3.0f,
                       int   min_cohort  = 3,
                       bool  normalize   = true);

// 兼容旧调用（ROS 节点使用），内部走 rank_faces + 原始阈值
std::shared_ptr<FaceRecord> find_matching_face(const std::vector<float>& embedding,
                                               float threshold = 0.7f);

// --- 队列归一化 ---------------------------------------------------------
void setCohortEmbeddings(std::vector<std::vector<float>> cohort);
void setNormalizationDefaults(float z_threshold, int min_cohort, bool enabled);
int  cohortSize() const;

// --- 图片 / 特征 ---------------------------------------------------------
bool save_image(const std::string& face_id, const std::vector<uint8_t>& data,
                std::string& out_path);
bool update_embedding(const std::string& face_id, const std::vector<float>& embedding);
```

行为约定：

| 行为 | 说明 |
|---|---|
| 目录自动创建 | `initialize()` 会递归创建 `faces_dir` 与 `db_path` 的父目录 |
| 表自动创建 | `faces` 与 `face_templates` 均 `CREATE TABLE IF NOT EXISTS` |
| 返回 `""` | `add_face` 失败（`name` 为空或写入失败） |
| `embedding` 为空 | 允许入库（`NULL`），但无法被识别；需 `backfill` 补全 |
| 一人多模板 | 主模板在 `faces.embedding`，附加模板在 `face_templates`；匹配取**最高分** |
| `remove_face` | 同时删除记录、附加模板与缩略图文件 |
| `clear_all` | 逐条删除记录与图片，返回删除条数 |
| 阈值语义 | `find_matching_face` / `match_face` 要求相似度 **严格大于** 阈值 |
| 队列归一化 | 用"与他人的相似度分布"（median/MAD → z）抑制"枢纽模板"乱认人；样本不足时退化为纯原始阈值 |
| 线程安全 | 全部公开方法由 `std::recursive_mutex` 保护 |

---

## 5. 构建与安装

```bash
./build.sh CORE
```

等价的手工流程：

```bash
cmake -S src/face_recognition_core -B build/core \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=$PWD/install
cmake --build build/core -j
cmake --install build/core
```

构建树固定放在仓库根的 `build/core/`，安装到 `install/{lib,include}` ——
**`src/` 下不再产生任何构建产物**。

安装内容：

```
install/lib/libface_recognition_core.so.1.0.0
install/include/face_recognition_core/*.hpp
```

**RPATH**：构建时会把 `install/vendored/lib`（或 in-tree
`third_party/sqlite3/build`）写入 `RUNPATH`，使 `.so` 优先加载 vendored SQLite3。
消费者无需额外设置 `LD_LIBRARY_PATH`。

> ⚠️ **改了 `face_recognition_core` 就必须把每个用到的模块重新编译一遍**
> （standalone / ROS1 / ROS2 / web 各自有一份构建产物）。只重编其中一个会导致
> 它用新逻辑写特征、另一个用旧逻辑写特征，同一库内特征空间不一致。改完执行一次
> `face_recognition_app backfill --all` 重建全库特征。

---

## 6. 被谁使用

四个应用层模块**全部**通过 `find_package(face_recognition_core)` 链接
`install/lib/libface_recognition_core.so` 这**同一份**库：

| 消费方 | 关键设置 |
|---|---|
| `face_recognition_standalone` | `find_package` + 安装到 `install/bin/`，`INSTALL_RPATH=$ORIGIN/../lib` |
| `face_db_web` | 同上 |
| `face_recognition_ros2` | `find_package` + `INSTALL_RPATH=$ORIGIN/../../../../lib`（安装空间在 `install/ros2/`） |
| `face_recognition_ros1` | `find_package`（catkin_make，devel 空间构建） |

> **历史与动机**：这四个模块原来各自 `add_subdirectory(../face_recognition_core)`
> 把核心源码编进自己的构建树；colcon 又额外把 core 当独立包编了一次。仓库里最多
> 同时存在 **8 份同源 `.so`**，后果是"改一处只修好其中一份" —— 曾出现
> `face_db_web` 用旧前端写入特征、而 `face_recognition_app` 用新前端查询，
> 同一个库里混着两个特征子空间，且**没有任何机制能发现**。改为链接同一份之后，
> 这类漂移在结构上不再可能发生。

这依赖 core 导出 CMake package（`install/lib/cmake/face_recognition_core/`），
且消费者配置时把 `CMAKE_PREFIX_PATH` 指向 `install/`（`build.sh` 已处理）。
因此**必须先 `./build.sh CORE`**，再构建其它模块。

---

## 7. 相关文档

- [人脸检测模块 face_detector](../services/face_detector.md)
- [人脸识别模块 face_recognizer](../services/face_recognizer.md)
- [人脸库模块 face_database](../services/face_database.md)
- [数据库 schema](../protocol/database_schema.md)
