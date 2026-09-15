# 常见问题排查（FAQ / troubleshooting）

按"现象 → 原因 → 处理"组织。排查通用路径：**先看 `build.log`，再看节点/程序运行时日志。**

---

## 一、构建阶段

### Q1 `./build.sh` 报 `[ERROR] Build had errors`

**处理**

1. 查看项目根目录 `build.log` 最后一段（脚本把所有输出都写进这里）
2. 常见原因是 `apt` 包未装齐；脚本会在依赖检查阶段列出缺失项，例如：
   `Missing dependencies: libmicrohttpd-dev  # required by WEB`
3. 补齐后重跑对应目标；必要时先 `./build.sh CLEAN`

### Q2 `ONNX Runtime not found. Please install ONNX Runtime C++.`

**原因**：`face_recognition_core/CMakeLists.txt` 在候选路径中都没找到
`onnxruntime_cxx_api.h` / `libonnxruntime.so`。

**处理**

```bash
# 检查是否已安装
ls ~/.local/onnxruntime/lib/libonnxruntime.so
ls ~/.local/onnxruntime/include/onnxruntime/core/session/onnxruntime_cxx_api.h

# 未安装则部署到候选路径
mkdir -p ~/.local/onnxruntime && cd ~/.local/onnxruntime
wget https://github.com/microsoft/onnxruntime/releases/download/v1.16.3/onnxruntime-linux-x64-1.16.3.tgz
tar -xzf onnxruntime-linux-x64-1.16.3.tgz --strip-components=1
```

### Q3 `Failed to initialize detector: cannot open model`

**原因**：模型文件缺失或路径错误。

**处理**

```bash
ls -l models/det_10g.onnx models/w600k_r50.onnx
./build.sh MODELS        # 重新下载
```

确认 `detection_model` 实际指向的绝对路径正确（ROS1 的 launch 默认值是
`/models/det_10g.onnx`，**必须覆盖**）。

### Q4 Web 目标编译失败：`libmicrohttpd` 找不到

```bash
sudo apt-get install -y libmicrohttpd-dev
```

或改用 vendoring，见 [../peripheral/third_party.md](../peripheral/third_party.md#5-可选-vendored-libmicrohttpd)。

### Q5 `undefined symbol: sqlite3_rtree_query_callback`（外部 viewer / rqt）

**原因**：外部进程 `dlopen` 到了未启用 R-Tree 的系统 SQLite。

**处理**

```bash
source scripts/setup_env.sh
# 验证
sqlite3 :memory: "PRAGMA compile_options;" | grep -i rtree
# → ENABLE_RTREE=1
```

### Q6 ROS2 日志目录不可写

```bash
export ROS_LOG_DIR=/tmp/roslog
mkdir -p "$ROS_LOG_DIR"
```

---

## 二、运行阶段

### Q7 `Failed to initialize database`

**原因**：`db_path` 目录无写权限。

**处理**

```bash
mkdir -p /tmp/face_db/faces
chmod -R u+rwX /tmp/face_db
```

### Q8 ROS2 节点启动后无人脸检出

**排查顺序**

```bash
ros2 topic hz /image_raw        # ① 输入流是否有数据
ros2 topic echo /face/recognition_result   # ② 是否有人脸命中
ros2 node list | grep face      # ③ 节点是否存活
```

常见原因：

| 原因 | 处理 |
|---|---|
| `/image_raw` 无数据 | 启动 `usb_cam` 或其他图像源 |
| 库内记录 `embedding` 为空 | `face_recognition_app backfill` 或重启节点触发自动补全 |
| 阈值过高 | 降低 `confidence_threshold`（如 `0.6`） |
| 图像编码非 `bgr8` | `cv_bridge` 转换失败，检查发布端编码 |

### Q9 日志持续刷 `[FaceDetector] detect() exception: ...`

**原因**：模型输出 layout 既不是 RetinaFace（9 输出）也不是 YOLOv8（`[1,84,N]`）。

**处理**：检查模型文件是否为预期版本。

> 正常情况下，模型 shape 不匹配只会打印**一次**（含完整 dims/size），
> 后续不会刷屏。若持续刷屏，说明错误消息在变化，需逐条分析。

### Q10 Web 打开后看不到列表 / 列表为空

**原因**：`face_db_web` 的 `--db` 与识别节点 `db_path` 不一致。

**处理**

```bash
# 用 standalone 直接查看同一个库
./src/face_recognition_standalone/build/face_recognition_app \
    --db /tmp/face_db/faces.db list
```

确保三方（Web / ROS / standalone）使用**同一个** `--db`。

### Q11 所有已录入的人脸都识别不出来（`emb=NO`）

**原因**：录入时未加载识别模型，或导入了缺少 embedding 的老库。

**处理**

```bash
APP=./src/face_recognition_standalone/build/face_recognition_app
$APP list                  # 确认哪些记录 emb=NO
$APP backfill              # 一次性补全

# 或运行时自动补全
$APP run --source 0 --auto-backfill
```

ROS 节点在**启动时**会自动执行同样的补全逻辑。

### Q12 浏览器打不开 `http://localhost:8090/`

**排查**

| 检查 | 命令 |
|---|---|
| 服务是否拉起 | `ros2 node list \| grep stream` |
| 是否有帧 | `curl -i http://localhost:8090/healthz` → `ok` / `no_frame_yet` |
| 端口是否被占用 | `ss -ltnp \| grep 8090` |
| 是否被禁用 | 启动参数 `enable_stream_server:=true` |

`no_frame_yet` 表示服务正常但还没收到图像 —— 说明上游（`face_viewer_node` /
`/face/annotated`）没有产出，检查图像源。

### Q13 `cv::imshow` / rqt 启动即崩溃（`__libc_pthread_init`）

**原因**：snap 应用（VS Code 等）导致 GIO 从 snap 路径加载 GTK 模块，
拉入 snap-core20 的 `libpthread.so.0`（glibc 2.31），与系统 glibc 2.35 ABI 冲突。

```
symbol lookup error: __libc_pthread_init, version GLIBC_PRIVATE
```

**处理**：改用 HTTP MJPEG 方案：

```bash
ros2 launch face_recognition_ros2 usb_cam_face.launch.py   # 浏览器看 8090
```

`face_stream_server` 只依赖 libmicrohttpd，headless 安全。

### Q14 standalone 窗口打不开，提示 `OpenCV GUI unavailable`

**原因**：无 GUI 环境（SSH / 容器 / 无 X11）。

**处理**：程序会自动降级为 headless（打印 `[WARN]`），无需干预。
如需显式关闭窗口：`--no-display`。

### Q15 FPS 太低（约 3 FPS）

**原因**：CPU 上 RetinaFace 检测是瓶颈（`--input-size 640` 时约 215 ms/帧），
识别约 60 ms/人脸。

**现状**：`--detect-every-n` 默认已为 `2`（检测成本按帧摊薄一半），
`--target-fps` 默认 `24` 只是循环上限——**机器跑不到 24 fps 时它不生效**。

**处理**（按收益排序）

```bash
# ① 降低检测输入尺寸：这是最有效的一档（≈4× 提速）
./src/face_recognition_standalone/build/face_recognition_app run --source 0 \
    --input-size 320                        # ≈24 FPS（单人脸，参考机实测）

# ② 再降一档 / 加大跳帧
./src/face_recognition_standalone/build/face_recognition_app run --source 0 \
    --input-size 240 --detect-every-n 3     # ≈42 FPS

# ③ 只看有没有人脸（不识别身份）
./src/face_recognition_standalone/build/face_recognition_app run --source 0 \
    --no-recognition --input-size 240       # ≈50+ FPS
```

> 画面里**人越多越慢**：识别约 60 ms/人，3 张脸时 24 fps 会掉到约 8 fps。
> 需要真 24 fps 且要识别身份，只能降低 `--input-size` 或改用 GPU 版 ONNX Runtime。

详见 [../module/standalone.md](../module/standalone.md#7-性能调优提升-fps)。

### Q16 Jetson 上 OpenCV 找不到

```bash
sudo apt install libopencv-dev      # arm64
# 或自行编译 OpenCV4 与 ONNX Runtime（aarch64）
```

### Q17 检测到人脸，但一直显示 `Unknown`（不识别）

**现象**：画面上有框，但标签是 `Unknown` / `rf.recognized == false`。

**第一步：确认库与比对逻辑本身没问题**

```bash
# standalone：确认识别模型加载 + 每条记录 embedding 已生成
./src/face_recognition_standalone/build/face_recognition_app list
# 期望：emb=yes

# 若为 emb=NO
./src/face_recognition_standalone/build/face_recognition_app backfill
```

库内 embedding 与比对逻辑的正确性可以这样自证：把**已入库的那张缩略图**再喂给
识别流程，同图自比对相似度应接近 `1.0`（实测 `0.9997`）。如果这一点都不成立，
说明是模型/预处理层面的问题，而不是阈值问题。

**第二步：几乎可以肯定是阈值过严**

实测同一人、不同照片的相似度：

| 场景 | 相似度 |
|---|---|
| 两张清晰照片 | 0.7539 |
| 640×480 + JPEG（≈ 摄像头条件） | **0.6936** |
| 320×240 + JPEG | 0.6236 |
| 缩放 + 旋转 12° | 0.4982 |
| 不同人 | 0.23 ~ 0.40 |

旧默认阈值 `0.7` 会把上面**除第一行以外的全部**判为 `Unknown`。
standalone 的默认值已下调为 `0.5`；若你在用旧二进制或显式传了 `0.7`：

```bash
./src/face_recognition_standalone/build/face_recognition_app run --source 0 \
    --recognition-threshold 0.5
```

**ROS1 / ROS2 节点** 的 `confidence_threshold` 默认仍是 `0.7`，而且它**同时被当作
检测置信度**传给 `FaceDetector`（见 `node.cpp` 中
`detector_->initialize(path, confidence_threshold_)`），会连带把检测置信度低于
0.7 的人脸直接丢掉。现场建议把该参数下调到 `0.5`：

```bash
ros2 launch face_recognition_ros2 usb_cam_face.launch.py confidence_threshold:=0.5
```

**第三步：确认镜头前的人就是库里的人**

如果阈值已降到 `0.45` 仍显示 `Unknown`，请直接换一张**库内同一人的已知照片**
对着摄像头比对，或把当前画面截图另存后入库，确认是"人不对"还是"条件太差"。

**第四步：其余可能原因**

| 原因 | 排查 |
|---|---|
| 库里录的是另一张脸（例如误传了背景路人） | `list` + 打开 `faces/<id>.jpg` 核对 |
| 光线过暗 / 强逆光 / 侧脸大角度 | 改善拍摄条件，或重新录入该人 |
| `db_path` 与录入时不一致（库为空） | `list` 查看记录条数 |

更多背景见
[../module/standalone.md#71-识别阈值为什么默认-05](../module/standalone.md#71-识别阈值为什么默认-05)
与 [../services/face_recognizer.md](../services/face_recognizer.md)。

### Q18 `--input-size` 非 640 时完全检测不到人脸

**现象**

```bash
face_recognition_app run --source 0 --input-size 320
# [INFO] recognition: threshold=0.50 detect-every-n=2 input-size=320 frame-cap=24 fps
# 2026-09-15 ... [W:onnxruntime:, execution_frame.cc:857 VerifyOutputSizes]
#   Expected shape from model of {800,10} does not match actual shape of {200,10} for output 500
#   ... (每帧刷 9 条，画面里一个框都没有)
```

**原因**（旧二进制存在，已修复）

`det_10g.onnx` 对 **640×640** 输入声明的是**静态**输出形状，实际形状随输入尺寸变化
（anchor 数 = `(W/stride) × (H/stride) × 2`）：

| `--input-size` | 三层 anchor 数 | 旧实现的 layer 判定 |
|---|---|---|
| `640` | 12800 / 3200 / 800 | 正确 |
| `320` | 3200 / 800 / 200 | **整体错位，最小一层被丢弃 → 结果恒为空** |

旧代码写死了 `if (rows == 12800) layer = 0; else if (rows == 3200) layer = 1; ...`，
于是 320 输入下 `3200` 被当成 layer 1、`800` 被当成 layer 2、`200` 直接 `continue`，
解码出的框全部无效 → 检测为空。同时 ONNX Runtime 的 `VerifyOutputSizes` 警告刷屏。

**修复**

后处理改为按当前输入尺寸推导 anchor 数，并对 `rows` / `cols` 做形状匹配
（见 [../services/face_detector.md](../services/face_detector.md#31-输入张量布局)）；
ORT 日志级别降为 `ERROR`，不再打印上述无害警告。

```bash
# 重新编译核心库与 standalone 即可
make -C src/face_recognition_standalone/build -j$(nproc)
# 或
./build.sh STANDALONE
```

修复后各输入尺寸实测（640×479 摄像头条件图，画面 3 张脸）：

| `--input-size` | 检测 | 人脸数 | 同一人相似度 |
|---|---|---|---|
| `640` | 224 ms | 3 | 0.6936 |
| `320` | 58.9 ms | 3 | 0.5918 |
| `240` | 33.6 ms | 2 | 0.5800 |

**排查口诀**：只要日志里出现 `VerifyOutputSizes` 且**一个人脸框都没有**，
就是这个问题（重新编译即可）；如果**有框但显示 `Unknown`**，那是阈值问题，
见 [Q17](#q17-检测到人脸但一直显示-unknown不识别)。

### Q19 侧脸 / 大角度时识别率下降明显

**原因**：识别器的输入是**不含关键点对齐**的 bbox 裁剪（仅外扩 1.2 倍后 resize 到
112×112），因此头部旋转会直接降低特征质量。

**实测**（同一人）：

| 姿态 | 相似度 |
|---|---|
| 正脸（两张清晰照片） | 0.7539 |
| 旋转 12° | 0.4982 |
| 45° 侧脸 | 明显更低，常跌破 0.5 |

**处理**

1. **录入时用正脸**照片 —— 这是收益最大的一步（`add --image <正脸照>`）；
2. 现场尽量正对摄像头，避免 45° 以上侧身；
3. 需要容忍侧脸时放宽阈值，但要留意"不同人"的上界是 0.40：

   ```bash
   ./src/face_recognition_standalone/build/face_recognition_app run \
       --source 0 --input-size 320 --recognition-threshold 0.45
   ```

4. 若必须支持大角度识别，需要把 `FaceRecognizer::preprocess` 升级为
   **基于 5 点关键点的仿射对齐**（RetinaFace 已输出 `det.landmarks`，目前未被使用）。
   注意：改变预处理会让**现有数据库中的特征全部失效**，必须清库重新录入。

---

## 三、数据一致性

| 现象 | 原因 | 处理 |
|---|---|---|
| 列表有记录但缩略图 404 | `image_path` 文件丢失 | 重新上传该记录 |
| 磁盘有孤立 `.jpg` | 手工 `DELETE` 或删除失败回滚不完整 | 手工清理 `faces/` 下无对应记录的图片 |
| 删除了记录但图片还在 | 同上 | 同上 |
| 同时写入导致 `database is locked` | 多进程并发写 | 串行化写入，见 [../services/face_database.md](../services/face_database.md#8-并发与多进程) |

诊断 SQL 见 [../protocol/database_schema.md](../protocol/database_schema.md#4-常用运维-sql)。

---

## 四、离线功能验证

不依赖 ROS 与摄像头的最小验证路径：

```bash
APP=./src/face_recognition_standalone/build/face_recognition_app

# ① 用测试图片录入 / 查看
$APP add --image ./test_2.png --name test_person --scene test
$APP list

# ② 确认 embedding 已生成（emb=yes）

# ③ 对同一张图做比对
$APP run --source ./test_2.png --no-display
```

期望：检测到人脸、生成 512 维 embedding、库内同图匹配相似度接近 `1.0`。

> 旧的离线验证记录见 `docs/USAGE_AND_TESTING.md`（历史文档，部分命令路径已变化）。

---

## 五、问题反馈

提交问题时请附上：

1. 使用的模块与版本（`git rev-parse --short HEAD`）
2. 完整复现命令
3. `build.log` 相关片段
4. 节点 / 程序完整日志（含模型路径与数据库路径）
5. `uname -a`、`lsb_release -a`、ROS 版本

---

## 六、相关文档

- [源码开发快速开始](../setup.md)
- [专项模块快速开始](../module/quick_start.md)
- [接口、协议与配置](../protocol/)
- [第三方依赖适配](../peripheral/third_party.md)
