# 外设与依赖适配文档（third_party）

本目录文档说明项目对外部依赖（第三方库 / SDK / 硬件 / 推理引擎）的适配方式。

- 源码目录：`third_party/`
- 设计原则：**vendoring + 零系统写入 + 可复现**

---

## 1. vendored 依赖清单

| 模块 | 默认 | 用途 |
|---|---|---|
| [`sqlite3/`](../../third_party/sqlite3/) | **ON** | SQLite 官方 amalgamation，启用 R-Tree / GEOPOLY 等编译选项 |
| [`spatialite/`](../../third_party/spatialite/) | OFF | 可选的完整 GIS 栈 |
| [`libmicrohttpd/`](../../third_party/libmicrohttpd/) | OFF | 可选，默认使用系统 `libmicrohttpd-dev` |

构建行为：

- 所有产物安装到 `install/vendored/`（由
  `FACE_RECOGNITION_VENDORED_PREFIX` 指定）
- 通过 **RPATH（`$ORIGIN` 相对路径）** 让项目二进制优先加载 vendored 版本
- **不写 `/usr/lib` / `/usr/local/lib`**，不修改系统任何库

---

## 2. 为什么 vendoring SQLite3

构建主机上曾同时存在两份 SQLite：

```
/usr/lib/x86_64-linux-gnu/libsqlite3.so.0    # apt 安装（带 R-Tree）
/usr/local/lib/libsqlite3.so                  # 历史源码编译（未启用 R-Tree）
```

外部工具（SpatiaLite、GDAL 的 rtree 驱动、`pyspatialite`）需要
`sqlite3_rtree_query_callback` 符号。若它们 `dlopen` 到后者，会报：

```
libspatialite.so.7: undefined symbol: sqlite3_rtree_query_callback
```

### 解决方案

`third_party/sqlite3/` 内置官方 amalgamation 源码，构建时强制开启：

```c
SQLITE_ENABLE_RTREE=1
SQLITE_ENABLE_GEOPOLY=1
SQLITE_ENABLE_FTS5=1
SQLITE_ENABLE_JSON1=1
SQLITE_ENABLE_COLUMN_METADATA=1
SQLITE_THREADSAFE=1
SQLITE_USE_URI=1
```

### 验证

```bash
source scripts/setup_env.sh
sqlite3 :memory: "PRAGMA compile_options;" | grep -i rtree
# → ENABLE_RTREE=1
```

若 amalgamation 源码缺失，`build.sh` 会自动调用
`third_party/sqlite3/download.sh` 拉取。

---

## 3. 系统依赖（非 vendoring）

| 依赖 | 版本要求 | 用途 | 安装 |
|---|---|---|---|
| OpenCV | ≥ 4.5 | 图像解码 / resize / 绘制 / `cv::dnn` | `apt install libopencv-dev` |
| ONNX Runtime (C++) | ≥ 1.16 建议 | RetinaFace 推理 | 见 [../setup.md](../setup.md#3-onnx-runtimec-版) |
| libuuid | — | UUID v4 生成 | `apt install uuid-dev` |
| libmicrohttpd | — | HTTP / MJPEG 服务 | `apt install libmicrohttpd-dev` |
| pthread | — | 线程与互斥 | glibc 自带 |
| cmake / make / g++ | CMake ≥ 3.10，C++17 | 构建 | `apt install cmake make g++` |

> **为什么 ONNX Runtime 不 vendoring**：官方发行包体积大且与 CPU/GPU 架构强相关，
> 交由使用者按目标平台安装更合适。CMake 会按候选路径自动探测。

---

## 4. 可选 vendored SpatiaLite

需要让外部 GIS 工具也使用项目内的 SQLite3 时启用：

```bash
BUILD_SPATIALITE=ON ./build.sh ALL
```

构建细节：

| 项 | 说明 |
|---|---|
| 源码 | `third_party/spatialite/src/libspatialite/`（缺失时自动 `download.sh`） |
| 前置依赖 | 系统的 GEOS / PROJ 头文件与库 |
| sqlite3 | 来自 vendored 构建（`-DCMAKE_PREFIX_PATH=$prefix`） |
| 安装位置 | `install/vendored/` |

默认关闭——多数使用场景不需要。

---

## 5. 可选 vendored libmicrohttpd

默认使用系统包：

```bash
sudo apt-get install -y libmicrohttpd-dev
```

若要完全脱离 apt，把源码放入 `third_party/libmicrohttpd/` 并打开：

```bash
-DBUILD_VENDORED_LIBMICROHTTPD=ON
```

详见 `third_party/libmicrohttpd/README.md`。

---

## 6. 硬件 / 平台适配

| 平台 | 状态 | 说明 |
|---|---|---|
| Ubuntu 22.04 x86_64 | 已验证 | ROS2 Humble 主开发环境 |
| Ubuntu 20.04 x86_64 | 支持 | ROS1 Noetic |
| Jetson Orin Nano / NX（aarch64） | 支持 | 交叉编译流程见下 |

### Jetson / aarch64 交叉编译

`third_party/sqlite3/` 是**纯 C 单文件**，无 autoconf、无外部构建工具：

```bash
export CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++
cmake -S third_party/sqlite3 -B third_party/sqlite3/build \
    -DCMAKE_C_COMPILER=$CC \
    -DCMAKE_INSTALL_PREFIX=$PWD/install/vendored
cmake --build third_party/sqlite3/build -j
cmake --install third_party/sqlite3/build

# 主体（需先准备 aarch64 版 OpenCV / uuid / libmicrohttpd / ONNX Runtime）
./build.sh ALL
```

`face_recognition_core/CMakeLists.txt` 以 **walk-up** 方式自动探测
`third_party/`（最多向上 6 层），因此目标平台无需修改任何代码。

### 摄像头

| 场景 | 说明 |
|---|---|
| USB / V4L2 | ROS2 用 `usb_cam`，standalone 用 OpenCV `VideoCapture(index)` |
| RTSP / HTTP | standalone `--source rtsp://...` / `http://...` |
| 视频文件 / 图片目录 | standalone `--source` 自动识别 |

---

## 7. ONNX 模型清单

| 文件 | 作用 | 是否主流程使用 |
|---|---|---|
| `det_10g.onnx` | RetinaFace 人脸检测（9 输出 + 5 点关键点） | **是** |
| `w600k_r50.onnx` | ArcFace 特征提取（512 维） | **是** |
| `yolov8n.onnx` | YOLOv8 通用检测（备用 backend） | 否（备用） |
| `1k3d68.onnx` | 3D 关键点（68 点） | 否（扩展） |
| `2d106det.onnx` | 2D 关键点（106 点） | 否（扩展） |
| `genderage.onnx` | 性别 / 年龄估计 | 否（扩展） |

下载：`./build.sh MODELS`（或 `python3 scripts/download_models.py`）。
手工下载说明见 `models/README.md`。

---

## 8. 新增第三方模块的规范

1. 创建 `third_party/<name>/`，包含 `CMakeLists.txt`、可选 `download.sh`、`README.md`
2. 暴露 `add_subdirectory()` 友好的 target
3. 在 `build.sh` 与消费方 `CMakeLists.txt` 中接入
4. 在本文件与 `third_party/README.md` 中登记

---

## 9. 相关文档

- [源码开发快速开始](../setup.md)
- [架构说明](../architecture.md)
- [常见问题排查](../FAQ/troubleshooting.md)
