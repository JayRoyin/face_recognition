# 源码开发快速开始（setup）

本文档说明 **Face Recognition Node** 的开发环境搭建、依赖安装、模型准备与首次构建验证流程。

- 适用平台：Ubuntu 20.04（ROS1 Noetic）/ Ubuntu 22.04（ROS2 Humble）/ Jetson Orin 系列（aarch64）
- 语言标准：C++17
- 构建入口：项目根目录 `./build.sh`

> 只想尽快跑起来？直接跳到 [第 5 节 一键构建](#5-一键构建)。

---

## 1. 依赖总览

| 类别 | 组件 | 是否必需 | 说明 |
|---|---|---|---|
| 构建工具 | `cmake` `make` `g++` | 必需 | CMake ≥ 3.10 |
| 视觉库 | `libopencv-dev` | 必需 | OpenCV ≥ 4.5（图像编解码、缩放、绘制） |
| 推理引擎 | ONNX Runtime（C++） | 必需 | RetinaFace 检测模型依赖，CMake 会自动探测常见路径 |
| 数据库 | `libsqlite3-dev` | 必需 | 用于 `pkg-config` 解析传递依赖；实际链接的是 vendored 版本 |
| 唯一 ID | `uuid-dev` | 必需 | 人脸记录 UUID 生成 |
| Web 服务 | `libmicrohttpd-dev` | 仅 `./build.sh WEB` 需要 | HTTP 服务与 MJPEG 推流 |
| ROS2 | `ros-humble-*` | 按需 | 构建 ROS2 节点时必需 |
| ROS1 | `ros-noetic-*` | 按需 | 构建 ROS1 节点时必需 |
| Python | `python3` + `requests` | 仅模型下载 | 模型下载脚本依赖网络 |

---

## 2. 基础系统依赖

```bash
sudo apt-get update
sudo apt-get install -y \
    cmake make g++ git curl wget unzip \
    libopencv-dev \
    libsqlite3-dev \
    uuid-dev \
    pkg-config
```

**仅当要构建 Web 管理后台时**才需要：

```bash
sudo apt-get install -y libmicrohttpd-dev
```

`./build.sh` 会在依赖检查阶段给出明确提示。若希望完全 vendoring（不依赖 apt 的
libmicrohttpd），把源码放进 `third_party/libmicrohttpd/` 并打开
`-DBUILD_VENDORED_LIBMICROHTTPD=ON`，详见 [peripheral/third_party.md](peripheral/third_party.md)。

> **说明**：`libsqlite3-dev` 仍是必需的——`pkg-config` 会用它解析 `SQLite3` 的
> 传递依赖；真正被**链接**的 `libsqlite3` 来自 `third_party/sqlite3/` 的 vendored 产物
> （启用 `SQLITE_ENABLE_RTREE` / `GEOPOLY`）。

---

## 3. ONNX Runtime（C++ 版）

`face_recognition_core` 使用 ONNX Runtime 运行 RetinaFace（`det_10g.onnx`，
输出含动态 `Reshape`，OpenCV DNN 无法执行）。CMake 会按以下顺序自动探测：

```
${HOME}/.local/onnxruntime/{include,lib}
${HOME}/anaconda3/pkgs/onnxruntime-cpp-*/include/onnxruntime
/opt/onnxruntime/{include,lib}
/usr/local/{include,lib}
/usr/{include,lib}
```

若都未命中，则回退 `find_path` / `find_library` 全盘搜索。安装示例：

```bash
# 方式一：官方预编译包
mkdir -p ~/.local/onnxruntime && cd ~/.local/onnxruntime
wget https://github.com/microsoft/onnxruntime/releases/download/v1.16.3/onnxruntime-linux-x64-1.16.3.tgz
tar -xzf onnxruntime-linux-x64-1.16.3.tgz --strip-components=1

# 方式二：conda
conda install -c conda-forge onnxruntime-cpp
```

配置失败时 CMake 会直接报：

```
ONNX Runtime not found. Please install ONNX Runtime C++.
```

---

## 4. ROS 依赖（按版本二选一）

**ROS2 Humble**：

```bash
sudo apt install -y ros-humble-cv-bridge ros-humble-sensor-msgs \
    ros-humble-std-msgs ros-humble-image-transport \
    ros-humble-builtin-interfaces \
    python3-colcon-common-extensions
```

**ROS1 Noetic**：

```bash
sudo apt install -y ros-noetic-cv-bridge ros-noetic-sensor-msgs \
    ros-noetic-std-msgs ros-noetic-std-srvs \
    ros-noetic-image-transport ros-noetic-message-generation \
    ros-noetic-message-runtime
```

USB 摄像头实时演示还需要：

```bash
sudo apt install -y ros-humble-usb-cam     # ROS2
# 或
sudo apt install -y ros-noetic-usb-cam     # ROS1
```

---

## 5. 一键构建

```bash
cd ~/Royin_Project/face_recognition

# ① 下载模型（必须，首次执行）
./build.sh MODELS

# ② 构建核心库 + vendored SQLite3
./build.sh CORE

# ③ 按需选择目标
./build.sh ROS2          # ROS2 节点（含 viewer / stream server）
./build.sh ROS1          # ROS1 节点
./build.sh STANDALONE    # 非 ROS 实时识别 C++ 可执行
./build.sh WEB           # Web 人脸库管理后台
./build.sh ALL           # 全部模块
```

所有可用目标：

| 目标 | 说明 |
|---|---|
| `./build.sh HELP` | 查看帮助 |
| `./build.sh MODELS` | 仅下载模型 |
| `./build.sh CORE` | 仅构建核心库 + vendored SQLite3 |
| `./build.sh ROS2` | CORE + ROS2 节点（默认流程） |
| `./build.sh ROS1` | CORE + ROS1 节点 |
| `./build.sh STANDALONE` | CORE + 非 ROS 实时识别可执行 |
| `./build.sh WEB` | CORE + Web 后台 |
| `./build.sh ALL` | 全部模块 |
| `./build.sh CLEAN` | 清理 `build/` `install/` `log/` |

构建日志统一写入项目根目录 `build.log`，失败时优先查看日志最后一段。

### 构建产物

```
install/
├── vendored/                      # 项目内 SQLite3（含 R-Tree/GEOPOLY）
├── face_recognition_core/         # 核心库
├── face_recognition_ros2/         # ROS2 节点 + viewer + stream server
└── bin/face_db_web                # Web 服务可执行

src/face_recognition_standalone/build/face_recognition_app   # 非 ROS 可执行
```

---

## 6. 环境变量与运行环境

构建完成后，项目内二进制通过 **RPATH（$ORIGIN 相对路径）** 优先加载 vendored
SQLite3；但在 `ros2 run`、外部 viewer 等场景下建议显式加载：

```bash
source scripts/setup_env.sh
```

该脚本会：

1. `source install/setup.bash`（colcon overlay，幂等）
2. 把 `install/vendored/lib` 前置到 `LD_LIBRARY_PATH` 与 `PKG_CONFIG_PATH`
3. 若 `models/` 存在标准模型，自动导出 `FACE_DETECTION_MODEL` /
   `FACE_RECOGNITION_MODEL`，供 `face_db_web` 录入时提取特征

验证 vendored SQLite 生效：

```bash
sqlite3 :memory: "PRAGMA compile_options;" | grep -i rtree
# → ENABLE_RTREE=1
```

---

## 7. 可选的 vendored SpatiaLite

若要让外部 GIS 工具（QGIS / GDAL / `pyspatialite`）也使用项目内的 SQLite3：

```bash
BUILD_SPATIALITE=ON ./build.sh ALL
```

默认关闭，多数使用场景不需要。

---

## 8. 首次跑通验证

```bash
# 1) 确认模型就位
ls -l models/det_10g.onnx models/w600k_r50.onnx

# 2) 启动 ROS2 节点
source scripts/setup_env.sh
ros2 launch face_recognition_ros2 face_recognition.launch.py

# 期望日志
# [INFO] [...] Face detector initialized
# [INFO] [...] Face recognizer initialized
# [INFO] [...] Face database initialized with N faces
```

离线功能验证（不依赖 ROS 与摄像头）见
[module/standalone.md](module/standalone.md) 与 [FAQ/troubleshooting.md](FAQ/troubleshooting.md)。

---

## 9. 跨平台编译（Jetson / aarch64）

`third_party/sqlite3/` 是**纯 C 单文件 amalgamation**，无 autoconf、无外部构建工具，
非常适合交叉编译：

```bash
cmake -S third_party/sqlite3 -B third_party/sqlite3/build \
    -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
    -DCMAKE_INSTALL_PREFIX=$PWD/install/vendored
cmake --build third_party/sqlite3/build -j
cmake --install third_party/sqlite3/build

# 主体（需先安装 aarch64 版 OpenCV / uuid / libmicrohttpd / ONNX Runtime）
./build.sh ALL
```

`face_recognition_core/CMakeLists.txt` 内部以 **walk-up** 方式自动探测
`third_party/`（最多向上 6 层），因此 Jetson 上无需修改任何代码。

---

## 10. 相关文档

- [架构说明](architecture.md)
- [专项模块快速开始](module/quick_start.md)
- [接口与配置文档](protocol/)
- [第三方依赖适配](peripheral/third_party.md)
- [常见问题排查](FAQ/troubleshooting.md)
