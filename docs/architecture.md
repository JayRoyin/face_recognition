# 架构说明（architecture）

本文档描述 **Face Recognition Node** 的整体分层、数据流与模块依赖关系。

---

## 1. 分层设计

```
┌─────────────────────────────────────────────────────────────────────┐
│                        应用层 Application                            │
│  face_recognition_ros2  face_recognition_ros1  face_recognition_    │
│  (node/viewer/stream)   (node)                 standalone (CLI)     │
│  face_db_web (HTTP UI)                                              │
└──────────────────────────────┬──────────────────────────────────────┘
                               │ 复用同一套 C++ API
┌──────────────────────────────▼──────────────────────────────────────┐
│                   核心算法层 face_recognition_core                    │
│  ┌────────────────┐ ┌──────────────────┐ ┌───────────────────────┐  │
│  │  FaceDetector  │ │  FaceRecognizer  │ │     FaceDatabase      │  │
│  │ SCRFD(det_10g)/│ │ ArcFace w600k_r50│ │  SQLite3 + 缩略图归档  │  │
│  │ YOLOv8 (ORT)   │ │ 512-d embedding  │ │  + 余弦检索 + 多模板   │  │
│  └────────────────┘ └──────────────────┘ └───────────────────────┘  │
└──────────────────────────────┬──────────────────────────────────────┘
                               │
┌──────────────────────────────▼──────────────────────────────────────┐
│                  依赖层 third_party (vendored)                        │
│  sqlite3 (RTREE/GEOPOLY)   spatialite (可选)   libmicrohttpd (可选)   │
└─────────────────────────────────────────────────────────────────────┘
```

**设计要点**

1. **算法与应用解耦**：检测 / 识别 / 存储三类能力全部收敛在
   `face_recognition_core`，四个应用层模块（ROS1、ROS2、standalone、web）只是
   不同的"外壳"，因此**模型权重、特征维度、数据库 schema 100% 兼容**。
2. **依赖可复现**：C/C++ 第三方依赖采用 vendoring，任何模块都不写系统目录，
   可在 CI / 容器 / Jetson 上稳定复现。
3. **存储单点化**：所有通路默认指向 `db_path`（默认 `/data/hhqs_data/face_db/faces.db`），
   在任意一处录入即可被其他通路立刻识别。

---

## 2. 识别数据流

```
                    ┌──────────────┐
   摄像头 / RTSP /   │  VideoSource │   （standalone）
   视频文件 / 图片目录└──────┬───────┘
                            │ cv::Mat(BGR)
   ROS 话题 /image_raw ─────┤
                            ▼
                   ┌──────────────────┐
                   │  FaceDetector    │  letterbox → ONNX → 解码 → NMS
                   │  (SCRFD det_10g) │  输出：bbox + 5 点关键点 + score
                   └────────┬─────────┘
                            │ FaceDetection[]
                            ▼
                   ┌──────────────────┐
                   │ FaceRecognizer   │  5 点对齐裁剪（默认）→ 112×112 → ArcFace
                   │  (ArcFace)       │  输出：512-d L2 归一化 embedding
                   └────────┬─────────┘
                            │ std::vector<float>(512)
                            ▼
                   ┌──────────────────┐
                   │  FaceDatabase    │  与库内每条模板求内积（取最高分）
                   │  rank_faces() /  │  已归一化 ⇒ 内积 = 余弦相似度
                   │  match_face()    │  再做队列归一化 + 阈值判定
                   └────────┬─────────┘
                            │ match_face().accepted == true 时命中
                            ▼
        ┌───────────────────┴────────────────────┐
        ▼                                        ▼
  ROS2: FaceResult 话题                     standalone: 绘制窗口 / 抓拍 / 录像
  viewer: 标注图 → MJPEG 推流
```

---

## 3. 人脸库存储模型

SQLite 文件内两张表（详见 [protocol/database_schema.md](protocol/database_schema.md)）：

`faces` —— 一个身份一行：

| 字段 | 类型 | 说明 |
|---|---|---|
| `id` | TEXT PK | UUID v4 小写 |
| `name` | TEXT | 姓名（必填） |
| `title` | TEXT | 职位 |
| `embedding` | BLOB | **主模板**：512 × float32 = 2048 字节，可空 |
| `image_path` | TEXT | 缩略图绝对路径，可空 |
| `scene` | TEXT | 场景标签 |
| `map_location` | TEXT | 物理位置 |
| `created_at` / `updated_at` | INTEGER | Unix 时间戳 |

`face_templates` —— 同一身份的**附加模板**（multi-shot，一枪一行）：

| 字段 | 类型 | 说明 |
|---|---|---|
| `id` | INTEGER PK AUTOINCREMENT | 自增主键 |
| `face_id` | TEXT | 指向 `faces.id` |
| `embedding` | BLOB | 与主模板同维度、同特征空间 |
| `image_path` | TEXT | 该枪的来源图，供 `backfill --all` 重建 |
| `created_at` | INTEGER | Unix 时间戳 |

匹配时对"主模板 + 全部附加模板"取**最高分**，因此一个人的模板数 =
`1 + COUNT(face_templates)`。给同一人补一枪不同外观（如不戴眼镜、侧脸）能显著
提升召回，见 [FAQ/troubleshooting.md](FAQ/troubleshooting.md)。

**embedding 为空的记录无法参与识别**，需通过 `backfill`（CLI）或启动节点时的
自动补全逻辑修复：

- ROS2 节点：构造时遍历库内图片，重新检测 + 提特征后写回
- standalone：`backfill` 子命令（`--all` 强制重建全部），或 `run --auto-backfill`

---

## 4. 模块与依赖矩阵

| 模块 | 依赖 core | 依赖 ROS | 依赖 OpenCV | 依赖 ONNX RT | 依赖 SQLite | 依赖 libmicrohttpd |
|---|---|---|---|---|---|---|
| `face_recognition_core` | — | ✗ | ✓ | ✓ | ✓ | ✗ |
| `face_recognition_ros2` | ✓ | ✓ (Humble) | ✓ | ✓ | ✓ | ✓（stream server） |
| `face_recognition_ros1` | ✓ | ✓ (Noetic) | ✓ | ✓ | ✓ | ✗ |
| `face_recognition_standalone` | ✓ | ✗ | ✓ | ✓ | ✓ | ✗ |
| `face_db_web` | ✓ | ✗ | ✓ | ✓ | ✓ | ✓ |

> `face_recognition_standalone` 是**唯一零 ROS 依赖**的实时识别通路，只需要
> OpenCV + ONNX Runtime + SQLite3 + pthread。

---

## 5. 线程与并发模型

| 模块 | 并发方式 | 说明 |
|---|---|---|
| `FaceDatabase` | `std::recursive_mutex` 保护全部公开方法 | 进程内串行化；跨进程并发写需调用方自行串行 |
| ROS2 节点 | 单线程 executor + 订阅回调 | 图像回调内完成检测/识别/发布 |
| `face_viewer_node` | 订阅 `sensor_msgs/Image` + 结果话题 | 与识别结果做帧级对齐后绘制 |
| `face_stream_server` | 后台线程缓存最新 JPEG，MHD 内部轮询线程拉取 | 只保留最新一帧，天然丢帧不背压 |
| `face_db_web` | libmicrohttpd `MHD_USE_INTERNAL_POLLING_THREAD` | 短事务写库 |

> **注意**：多个进程同时向同一个 SQLite 文件写入时仍需串行化。standalone 的
> `run` / `add` 与 ROS2 节点应避免同时写入；Web 后台使用短事务，一般无影响。

---

## 6. 关键设计决策

### 6.1 为什么检测用 ONNX Runtime、识别用 OpenCV DNN

`det_10g.onnx`（SCRFD 系列）的输出含动态 `Reshape`，OpenCV 4.5 的 DNN 模块无法
执行；ONNX Runtime 支持完整算子集，因此**检测器**走 ONNX Runtime。

**识别器**则保留 OpenCV DNN：`w600k_r50.onnx` 只有常规算子，两条路径实测输出一致
（同一模型、同一输入，两者的 embedding 完全相同），因此选依赖更少的方案，
少引入一个运行时耦合。

### 6.2 为什么 vendoring SQLite3

构建主机上曾同时存在：

```
/usr/lib/x86_64-linux-gnu/libsqlite3.so.0    # apt，带 R-Tree
/usr/local/lib/libsqlite3.so                  # 历史源码编译，未启用 R-Tree
```

外部工具（SpatiaLite、GDAL rtree 驱动、`pyspatialite`）需要
`sqlite3_rtree_query_callback` 符号，若 `dlopen` 到后者则报：

```
libspatialite.so.7: undefined symbol: sqlite3_rtree_query_callback
```

项目通过内置官方 amalgamation 并强制开启 `SQLITE_ENABLE_RTREE` /
`SQLITE_ENABLE_GEOPOLY` / `FTS5` / `JSON1` / `COLUMN_METADATA`，安装到
`install/vendored/lib`，再用 RPATH 让项目二进制优先加载，**对系统零写入**。

### 6.3 为什么用 HTTP MJPEG 而不是 `cv::imshow` / rqt

`cv::imshow` 与 rqt（Qt5）都走 GTK3 / Qt → GIO 模块加载。在装有 snap 应用
（VS Code 等）的机器上，GIO 会从 snap 路径加载 GTK 模块，进而拉入
snap-core20 的 `libpthread.so.0`（glibc 2.31），与系统 glibc 2.35 ABI 不兼容：

```
symbol lookup error: __libc_pthread_init, version GLIBC_PRIVATE
```

`face_stream_server` 只依赖 libmicrohttpd（无 Qt/GTK），headless 安全，
在任何能开浏览器的机器上都能用。

### 6.4 后处理 backend 自动选择

`FaceDetector` 按输出张量**数量**自动选择后处理：

- **9 个输出** → RetinaFace/SCRFD 布局 `(conf, bbox, landmarks) × 3 层` → `det_10g.onnx`
- **其它** → YOLOv8 单输出 `[1, 84, N]`

> 判定依据是**数量**而非 shape：`det_10g.onnx` 声明的是 640×640 的静态输出形状，
> 用其它 `--input-size` 时 shape 会变，按 shape 判定会误判。

模型 shape 不匹配时**只打印一次**完整 dims/size，避免日志刷屏。

### 6.5 检测后处理必须用 SCRFD 的解码公式

`det_10g.onnx` 是 SCRFD，回归头输出已按 stride 归一化，解码只需乘 `stride`。
**不要**套用经典 RetinaFace 的 `variance × anchor宽高` 公式——那会把框缩到真值的
约 60%、把关键点压向中心，进而使对齐裁剪退化、不同人之间相似度虚高。
细节见 [services/face_detector.md §3.3](services/face_detector.md#33-解码scrfd-约定)。

---

## 7. 相关文档

- [源码开发快速开始](setup.md)
- [专项模块快速开始](module/quick_start.md)
- [核心业务模块文档](services/)
- [接口、协议与配置](protocol/)
- [常见问题排查](FAQ/troubleshooting.md)
