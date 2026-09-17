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
| [`insightface/`](../../third_party/insightface/) | — | **参考实现，不参与构建**：`det_10g.onnx` / `w600k_r50.onnx` 的原始出处 |
| [`opencv_zoo/`](../../third_party/opencv_zoo/) | — | **参考实现，不参与构建**：Apache-2.0 的 YuNet + SFace 替代方案 |

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

## 9. 参考实现（reference-only，不参与构建）

`third_party/` 下有两个**只读参考仓库**，用于比对 `face_recognition_core`
的检测 / 对齐 / 特征前端：

| 模块 | 上游 | 固定 commit | 许可 | 用途 |
|---|---|---|---|---|
| `third_party/insightface/` | [deepinsight/insightface](https://github.com/deepinsight/insightface) | `1480e705…` | 代码 MIT / **模型权重非商业** | 本项目 `det_10g.onnx`（SCRFD-10GF）与 `w600k_r50.onnx`（ArcFace R50）的原始出处 |
| `third_party/opencv_zoo/` | [opencv/opencv_zoo](https://github.com/opencv/opencv_zoo) | `47534e27…` | **Apache-2.0** | YuNet + SFace，商用无授权风险的替代方案 |

关键区别：

- **没有 `CMakeLists.txt`**，`build.sh` 不构建，不被链接进任何目标
- 已在 `.gitignore` 中排除（源码约 80 MB，opencv_zoo 的 LFS 权重约 1.4 GB 被刻意跳过）
- 新机器需先执行一次拉取脚本：

```bash
./third_party/fetch_reference.sh          # 拉取 / 复用已有副本
./third_party/fetch_reference.sh --clean  # 删除本地副本
```

脚本内固化了上述 commit；实际 checkout 与记录值不一致时会打 `WARN`，
提示需要重新核对对照表。

**逐文件的上下游对照表（含已确认的差异）见
[`third_party/REFERENCE.md`](../../third_party/REFERENCE.md)。**
排查检测精度、对齐质量、识别相似度分布时，应首先阅读该文件。

---

## 10. 相关文档

- [源码开发快速开始](../setup.md)
- [架构说明](../architecture.md)
- [常见问题排查](../FAQ/troubleshooting.md)
- [上游参考实现对照表](../../third_party/REFERENCE.md)
