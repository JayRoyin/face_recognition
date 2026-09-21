# 常见问题排查（FAQ / troubleshooting）

按"现象 → 原因 → 处理"组织。排查通用路径：**先看 `build.log`，再看节点/程序运行时日志。
判断"识别为什么不对"时，第一手段永远是 `verify`**——它把每个身份的原始相似度、
队列统计与判定原因都打印出来。

```bash
APP=./install/bin/face_recognition_app
$APP verify --image ./某张图.jpg
```

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

### Q25 ROS2 链接失败：`libgdal.so: undefined reference to curl_*@CURL_OPENSSL_4`

**现象**（`./build.sh ROS2`：colcon 已完成配置与编译，卡在最后链接）

```
/usr/bin/ld: /lib/libgdal.so.30: undefined reference to `curl_easy_cleanup@CURL_OPENSSL_4'
/usr/bin/ld: /lib/x86_64-linux-gnu/libnetcdf.so.19: undefined reference to `curl_easy_strerror@CURL_OPENSSL_4'
collect2: error: ld returned 1 exit status
Failed   <<< face_recognition_ros2
```

**这不是宿主库的问题**：宿主 `libcurl4 7.81.0` / `libtiff5 4.3.0` / `libgdal30 3.4.1`
同属 jammy，版本自洽。真正原因是 **conda 前缀污染了链接搜索路径**：

```
conda base 处于激活状态（CONDA_PREFIX=<conda>）
  → CMake 从 conda 解析到 Python / spdlog / fmt
  → 链接 conda 的 libpython3.13.so，并把 $CONDA_PREFIX/lib 写进链接行：
        -Wl,-rpath,...,$CONDA_PREFIX/lib
        -Wl,-rpath-link,$CONDA_PREFIX/lib        ← 关键
  → ld 用 -rpath-link 目录优先解析「链接行上各库的 DT_NEEDED」
  → 系统 libgdal.so（经 OpenCV 的 imgcodecs / highgui / videoio 引入）
    需要 libcurl.so.4 与 libtiff.so.5
  → 在 conda 目录里先命中 conda 的 libcurl.so.4.8.0（libcurl 8.x，
    **不提供 CURL_OPENSSL_4 这个符号版本**）→ 符号解析失败
```

`objdump -T` 佐证：系统 `libcurl.so.4` 提供 **88** 个 `CURL_OPENSSL_4` 符号，
conda 那份提供 **0** 个。

**为什么只有 ROS2 出问题**：只有 ament / rosidl 这条链会链接 libpython，也就只有它会把
`$CONDA_PREFIX/lib` 引入链接搜索路径。`face_recognition_core`、`face_recognition_app`、
`face_db_web` 的链接行里没有该目录，所以一直是好的。

**处理**

`build.sh` 的 ROS 分支已固化三道防线，**不需要改动系统库**：

1. 记录并 `unset CONDA_PREFIX`；
2. 传 `-DCMAKE_IGNORE_PREFIX_PATH=$CONDA_PREFIX`（需 CMake ≥ 3.23）；
3. 显式钉住系统 Python —— 注意 `rosidl_generator_py` 走的是 `python_cmake_module`，
   即**遗留变量** `PYTHON_EXECUTABLE` / `PYTHON_LIBRARY` / `PYTHON_INCLUDE_DIR`，
   而不是 `Python3_*`；只钉 `Python3_*` 会漏掉它。

在别的机器上遇到同样报错时，先确认是否处于 conda / pyenv 之类的激活环境，
或直接 `conda deactivate` 后重试。

**自查命令**

```bash
L=build/ros2/face_recognition_ros2/CMakeFiles/face_recognition_node.dir/link.txt
grep -c anaconda3 "$L"                                    # 期望 0
objdump -T /lib/x86_64-linux-gnu/libcurl.so.4 | grep -c CURL_OPENSSL_4   # 88
ldd install/ros2/face_recognition_ros2/lib/face_recognition_ros2/face_recognition_node \
  | grep -E "face_recognition_core|curl|tiff|gdal"        # 期望全部指向系统库
```

### Q26 启动时提示 `gallery feature space mismatch`

**现象**

```
[WARN] gallery feature space mismatch
         stored : fs1|det=scrfd-stride|front=arcface112-align|norm=rgb-pm127.5|model=174383860:1632137312
         current: fs1|det=scrfd-stride|front=faceswap-latest|norm=rgb-pm127.5|model=174383860:1632137312
```

**含义**：库里的特征是用**另一套配方**算出来的，与当前代码产出的特征不在同一子空间，
因此逐项比对毫无意义 —— 这正是"本人识别不出、他人却很像"的成因。它不是数据损坏，
而是**升级后的正常反应**：指纹就是用来把这种静默失效变成一条明确告警。

**处理**

```bash
./install/bin/face_recognition_app backfill --all
```

重算全部特征后指纹会更新为当前配方。

**常见触发改动**：切换对齐 / bbox 裁剪前端、修改检测框或关键点解码、调整归一化常量、
更换识别模型、改动模板尺寸 —— 任何一项都会改变特征子空间。

> 指纹串各字段含义见 [../module/core.md](../module/core.md) 的「特征指纹」。
> 若 `stored` 为空，说明是本次之前创建的旧库，程序会自动补写（状态 `STAMPED`），
> 无需任何操作；首次 `backfill --all` 之后它才真正成立。

---

## 二、运行阶段

### Q7 `Failed to initialize database`

**原因**：`db_path` 目录无写权限。

**处理**

```bash
mkdir -p /data/hhqs_data/face_db/faces
chmod -R u+rwX /data/hhqs_data/face_db
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

**原因**：模型输出 layout 既不是 RetinaFace/SCRFD（9 输出）也不是 YOLOv8（`[1,84,N]`）。

**处理**：检查模型文件是否为预期版本。

> 正常情况下，模型 shape 不匹配只会打印**一次**（含完整 dims/size），
> 后续不会刷屏。若持续刷屏，说明错误消息在变化，需逐条分析。

### Q10 Web 打开后看不到列表 / 列表为空

**原因**：`face_db_web` 的 `--db` 与识别节点 `db_path` 不一致。

**处理**

```bash
# 用 standalone 直接查看同一个库
./install/bin/face_recognition_app \
    --db /data/hhqs_data/face_db/faces.db list
```

确保三方（Web / ROS / standalone）使用**同一个** `--db`。

### Q11 所有已录入的人脸都识别不出来（`emb=NO`）

**原因**：录入时未加载识别模型，或导入了缺少 embedding 的老库。

**处理**

```bash
APP=./install/bin/face_recognition_app
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

### Q15 FPS 太低

**原因**：CPU 上检测是瓶颈，识别也有固定开销（每张脸一次 112×112 推理）。

**现状**：`--detect-every-n` 默认已为 `2`（检测成本按帧摊薄一半），
`--target-fps` 默认 `24` 只是循环上限——**机器跑不到 24 fps 时它不生效**。

**处理**（按收益排序）

```bash
# ① 降低检测输入尺寸：最有效的一档
./install/bin/face_recognition_app run --source 0 \
    --input-size 320

# ② 再降一档 / 加大跳帧
./install/bin/face_recognition_app run --source 0 \
    --input-size 240 --detect-every-n 3

# ③ 只看有没有人脸（不识别身份）
./install/bin/face_recognition_app run --source 0 \
    --no-recognition --input-size 240
```

> 画面里**人越多越慢**（每张脸都要单独提一次特征）。
> 需要高帧率且要识别身份，只能降低 `--input-size`、减少人脸数，或改用 GPU 版
> ONNX Runtime。

详见 [../module/standalone.md](../module/standalone.md#73-帧率调优)。

### Q16 Jetson 上 OpenCV 找不到

```bash
sudo apt install libopencv-dev      # arm64
# 或自行编译 OpenCV4 与 ONNX Runtime（aarch64）
```

### Q17 检测到人脸，但一直显示 `Unknown`（不识别）

**现象**：画面上有框，但标签是 `Unknown` / `rf.recognized == false`。

**第一步：用 `verify` 拿到真实分数**

```bash
APP=./install/bin/face_recognition_app
$APP verify --image <库内同一人的已知照片>
```

它会把排名、`raw similarity`、队列统计与 `decision` 原因都打出来。据此分三种情况：

| `decision` 原因 | 含义 | 处理 |
|---|---|---|
| `below raw threshold` | 分数低于 `--recognition-threshold` | 见第二步 |
| 队列 z 门限未过 | 原始分数够、但比"随机陌生人"没高多少 | 该模板是"枢纽"，见 Q22 / 重建特征 |
| 前端不可用 / 人脸过小 | 被质量门控拦下 | 见第三步 |

**第二步：确认阈值是否过严**

standalone 默认阈值已是 `0.5`。若你在用旧二进制或显式传了 `0.7`：

```bash
./install/bin/face_recognition_app run --source 0 \
    --recognition-threshold 0.5
```

**ROS1 / ROS2 节点** 的 `confidence_threshold` 默认已统一为 `0.5`（与 standalone /
Web 一致）。注意它**同时被当作检测置信度**传给 `FaceDetector`（见 `node.cpp` 中
`detector_->initialize(path, confidence_threshold_)`），所以把它调高会连带丢掉检测
置信度低于该值的人脸 —— 这是"阈值越调越高、框反而越来越少"的原因。

需要临时覆盖时：

```bash
ros2 launch face_recognition_ros2 usb_cam_face.launch.py confidence_threshold:=0.5
```

**第三步：确认取脸前端与人脸尺寸**

- 日志里 `align=on(strict)`：对齐不可用的人脸会**直接不参与识别**。
  若你的画面普遍是小脸或大侧脸，可临时用 `--allow-unaligned` 观察（**注意会污染库，
  见 Q20**），或提高采集分辨率 / 缩短拍摄距离。
- 人脸框短边 < `--min-face-size`（默认 `80`）时只画框不识别。这不是 bug：小脸要被
  放大塞进 112×112 输入，产生的退化特征会"匹配所有人"。

**第四步：确认镜头前的人就是库里的人**

换一张**库内同一人的已知照片**用 `verify` 打分，或把当前画面截图另存后入库，
确认是"人不对"还是"条件太差"。

**第五步：其余可能原因**

| 原因 | 排查 |
|---|---|
| 库里录的是另一张脸（例如误传了背景路人） | `list` + 打开 `faces/<id>.jpg` 核对 |
| 光线过暗 / 强逆光 / 大角度侧脸 | 改善拍摄条件，或重新录入该人 |
| `db_path` 与录入时不一致（库为空） | `list` 查看记录条数 |
| 库内特征由旧前端产生 | `backfill --all`，见 Q20 |

### Q18 `--input-size` 相关：`VerifyOutputSizes` 警告 / 检测不到人脸

**现象**

```bash
face_recognition_app run --source 0 --input-size 320
# [INFO] recognition: threshold=0.50 detect-every-n=2 input-size=320 align=on(strict) ...
# [W:onnxruntime:, execution_frame.cc:857 VerifyOutputSizes]
#   Expected shape from model of {800,10} does not match actual shape of {200,10} for output 500
```

**原因**

`det_10g.onnx` 对 **640×640** 输入声明的是**静态**输出形状，实际形状随输入尺寸变化
（anchor 数 = `(W/stride) × (H/stride) × 2`）：

| `--input-size` | 三层 anchor 数 | 旧实现的 layer 判定 |
|---|---|---|
| `640` | 12800 / 3200 / 800 | 正确 |
| `320` | 3200 / 800 / 200 | **整体错位，最小一层被丢弃 → 结果恒为空** |

旧代码写死了 `if (rows == 12800) layer = 0; else if (rows == 3200) layer = 1; ...`，
于是 320 输入下 `3200` 被当成 layer 1、`800` 被当成 layer 2、`200` 直接 `continue`，
解码出的框全部无效 → 检测为空。

**修复（已合入）**

后处理改为按当前输入尺寸推导 anchor 数，并对 `rows` / `cols` 做形状匹配
（见 [../services/face_detector.md](../services/face_detector.md#31-输入张量布局)）；
ORT 日志级别降为 `ERROR`，不再打印上述无害警告。

```bash
# 重新编译核心库与 standalone 即可
make -C build/standalone -j$(nproc)
# 或
./build.sh STANDALONE
```

**排查口诀**

| 日志特征 | 结论 |
|---|---|
| 出现 `VerifyOutputSizes` 且**一个人脸框都没有** | layer 判定问题，重新编译 |
| **有框但显示 `Unknown`** | 识别侧问题，见 [Q17](#q17-检测到人脸但一直显示-unknown不识别) |

### Q19 侧脸 / 大角度时识别率下降明显

**原因**：侧脸会同时影响三件事——检测框质量、5 点关键点精度、以及特征本身
（ArcFace 在正脸模板上判别力最强）。

**处理**

1. **保持默认的 `--align`（关键点对齐）**。对齐会把头姿归一化到标准正脸模板，
   是当前版本应对大角度的主要手段；用 `--no-align` 退回 bbox 裁剪只会更差。
2. **录入时用正脸**照片 —— 收益最大的一步（`add --image <正脸照>`）。
3. 给同一个人补一枪**侧脸照**（`add-template`），让匹配覆盖真实使用姿态。
4. 现场尽量正对摄像头；若已知业务以大角度为主，可适度放宽阈值，但要盯住
   `verify` 里"不同的人"的分数，别把误识放进来。

> 如果对齐经常不可用（日志显示大量人脸落在 `not aligned`），说明关键点质量差
> ——多数是**人脸太小**：提高采集分辨率或缩短距离，而不是关闭对齐。

### Q20 本人识别不出 / 是个人都能识别（误识）

**这是本项目最典型的"看起来像识别坏了"的现象。** 现象的共同特征是：
**本人分数不高，而某些"不同的人"分数很高，两者区间重叠。**

按以下顺序排查。

#### 原因一：库内特征与当前前端 / 预处理不匹配

识别器有**两个取脸前端**（关键点对齐 / bbox 裁剪），且它们**不在同一个特征子空间**。
只要库里的特征是用另一个前端算出来的，就会出现"本人比陌生人还难识别"。

其它同样会导致特征失效的变更：

- 预处理配方（归一化、通道顺序）变更；
- 检测器**取框 / 关键点解码**修正（裁剪区域随之变化）；
- 更换识别模型。

**确认**：这类问题的特征是**分数整体被压缩**——本人和陌生人都往中间挤，
`verify` 里排名靠前的身份之间差距很小。

**修复**（用库内存档的缩略图重建，不需要重新上传）：

```bash
./install/bin/face_recognition_app backfill --all
```

> ⚠️ **务必保证所有通路使用同一次构建**。`face_recognition_core` 在
> standalone / ROS1 / ROS2 / web 各自的 build 目录里是**各编译一份**的：
> ```bash
> ./build.sh ROS2      # 用 ROS2 就重编
> ./build.sh WEB       # 用 Web 后台就重编
> ```
> 只要有一个旧二进制还在往同一个 `--db` 写特征，就会重新引入不匹配。

#### 原因二：检测框 / 关键点解码缺陷（已修复，见 [Q23](#q23-不同的人之间相似度异常偏高本人反而更低)）

若你手上是修复前的构建，**不同的人之间相似度会高达 0.6~0.7**，甚至超过本人。
先确认已重新编译，再执行 `backfill --all`。

#### 原因三：模板太少 / 质量太差

- 一个人只有一枪，且那一枪是低质量（模糊、大侧脸、强逆光、戴眼镜而现场不戴）
  ⇒ 该模板在特征空间里远离本人真实分布。
- **处理**：用 `add-template` 补 1~2 枪真实场景照片（见
  [Q22](#q22-戴上眼镜能识别摘掉就认不出而别人戴着眼镜却被认成我)）。

#### 自查三步

```bash
APP=./install/bin/face_recognition_app

# ① 库里到底有几个人？（只有 1 个人还被"他人"命中，就一定是误识或前端问题）
$APP list

# ② 用已知照片打分，看排名与决策原因
$APP verify --image <本人已知照片>
$APP verify --image <他人照片>

# ③ 若分数区间重叠，重建特征后再测
$APP backfill --all
```

**健康的分数形态**：本人明显高于所有人，且"不同的人"普遍落在 `0 ~ 0.2`。
只要"不同的人"能逼近甚至超过本人，就是特征/前端问题，**调阈值无效**。

### Q21 Web 后台录入的人脸，识别时永远匹配不到（特征为空）

**现象**（两种，先分清是哪一种）

**A. 录入时就被拒绝**（现在的默认行为）

```bash
./install/bin/face_db_web --port 8080 --db ... --faces-dir ...
# ============================================================
#  WARNING: embedding extraction is DISABLED
# ============================================================
#  ...
#  Enrolment requests will be REJECTED until this is fixed.
# ============================================================
# Embedding extraction: DISABLED (see warning above)
```

此时浏览器里录入会得到 **HTTP 409**，提示
`Refused: no embedding could be extracted (...)`，或批量导入时该张计入"失败"、
原因写 `detection/recognition models are not loaded`。**这是保护行为**：
不会产生永远认不出的记录。

**B. 老库里已经存在的空特征记录**

历史版本（在默认拒绝之前）写进的记录，`embedding` 为空：页面里能看到记录、也能
看到缩略图，但 `run` / ROS 节点**怎么也认不出这个人**。

**原因**

两种情况都是启动时**没有加载检测/识别模型**：

- 现在会**直接拒绝**这次录入（A），避免继续产生不可用记录；
- 但**已经存在**的空特征记录不会自己变好，必须重建（B）。

**确认**

```bash
sqlite3 /data/hhqs_data/face_db/faces.db \
  "SELECT name, title, length(embedding) AS emb_bytes FROM faces;"
# emb_bytes 为空 ⇒ 就是这个问题
```

**修复**

1. **重建已有记录**（缩略图已存档，不需要重新上传）：

   ```bash
   ./install/bin/face_recognition_app backfill --all
   ```

2. **让 Web 后台以后能自动找到模型**。现在它会按以下顺序查找
   `det_10g.onnx` / `w600k_r50.onnx`：

   ```
   --detection-model / --recognition-model   （命令行，最高优先）
   FACE_DETECTION_MODEL / FACE_RECOGNITION_MODEL（环境变量）
   ./models/                                  （当前工作目录）
   <exe>/models/  <exe>/../../models/          （install/bin 安装树）
   ```

   所以**在项目根目录启动、或显式传模型路径**都可以：

   ```bash
   ./install/bin/face_db_web --port 8080 \
       --db /data/hhqs_data/face_db/faces.db --faces-dir /data/hhqs_data/face_db/faces \
       --detection-model "$(pwd)/models/det_10g.onnx" \
       --recognition-model "$(pwd)/models/w600k_r50.onnx"
   ```

3. **现在默认会拒绝无特征的录入**（HTTP 409），避免再产生不可用记录。
   确实需要"只存文字信息"时才用 `--allow-no-embedding`。

> **另外一个常见陷阱**：仓库里还有一个 `data/faces.db`（空的）。三个模块的默认库都是
> `/data/hhqs_data/face_db/faces.db`，只有显式传 `--db data/faces.db` 才会用到它。如果两处混用，
> 表现就是"我录入的人脸查不到了"。用 `find / -name faces.db` 确认只有一个在用。

### Q22 戴上眼镜能识别、摘掉就认不出；而别人戴着眼镜却被认成我

**这是同一个机制的两面**，根因是"**模板覆盖的外观范围太窄**"：

- 入库照是**戴眼镜**的 ⇒ 这个人的身份信息只覆盖"戴眼镜"那一片特征区域。
- **摘掉眼镜**：眼镜 + 眼周外观剧变，而眼 / 眉区域恰好是 ArcFace 判别力最强的区域
  ⇒ 查询点远离已有模板 ⇒ **本人漏判**。
- **别的戴眼镜的人**：镜框、镜腿、镀膜反光构成了一个**与身份无关的共享外观方向**，
  把不同的人一起拉向"戴眼镜"的区域 ⇒ **误识**。

**修复（按收益排序）**

1. **给同一人补一枪"不戴眼镜"的照片**（多模板，最直接有效）：

   ```bash
   APP=./install/bin/face_recognition_app
   $APP list                       # 取到该人的 <face_id>
   $APP add-template --id <face_id> --image ~/photos/me_no_glasses.jpg
   $APP list                       # templates 变成 2
   ```

   匹配时取该身份**所有模板的最高分**，因此两种外观都能命中。
   建议每人 1~3 枪，覆盖真实使用时的姿态 / 光照 / 配饰差异；枪数过多会抬高误识概率。

2. **保持默认的 `--align`**。对齐会把脸归一到标准正脸模板，对"姿态 / 配饰带来的
   几何差异"最有效。

   > ⚠️ 若你切换过前端（`--align` / `--no-align` / `--allow-unaligned`），
   > **必须** `$APP backfill --all`：两个前端产生的特征**不可互换**，混用会让
   > 陌生人比本人更容易被识别。

3. **确保人脸够大**。人脸框短边 < `--min-face-size`（默认 80 px）时系统**只画框不识别**。
   这不是 bug 而是保护：小脸要被放大塞进 112×112 输入，产生的退化特征会
   "匹配所有人"，是"是个人都能识别"的主要来源。

4. **看真实数字再决定要不要调阈值**，不要凭感觉：

   ```bash
   $APP verify --image ~/photos/me_no_glasses.jpg
   # faces detected : 1
   #    0.6xxx  name=2   templates=2  id=895bd341-...
   # raw similarity : 0.6xxx (threshold 0.50)
   # cohort         : n=0 (needs >= 3 -> normalisation skipped)
   # decision       : ACCEPT -- accepted (raw only)
   ```

**如果已经有多个人**，队列归一化（`--cohort-db` 或库内 ≥2 个身份）会自动生效，
进一步压制"枢纽模板乱认人"，见 [../services/face_database.md](../services/face_database.md)
的 §4.3 队列归一化。

### Q23 不同的人之间相似度异常偏高（本人反而更低）

**现象**（最容易被误判成"模型不行"）：

- `verify` 里**两个毫不相关的身份**互相有很高的分数；
- **本人**的分数反而不高，排名被陌生人压过；
- 表现随裁剪尺度、归一化、通道顺序怎么调都**不改善**（说明不在预处理层）。

**原因：检测器把 5 点关键点按错误的公式解码**（已修复）。

`det_10g.onnx` 是 **SCRFD**，它的回归头输出已经按特征 stride 归一化，解码只需乘
`stride`。早期实现却套用了经典 RetinaFace 的
`variance(0.1/0.2) × anchor宽高(stride*4 / stride*6)` 公式，于是：

| 受影响对象 | 后果 |
|---|---|
| 边界框 | 只有真值约 60%，**下巴与额头被切掉** |
| 5 点关键点 | 被压向 anchor 中心，双眼间距只剩真值约一半 |
| 关键点对齐 | 相似变换为把小点距放大到模板尺寸而**放大画面**，裁剪里只剩眼睛和鼻子 |
| 最终表现 | 半张脸的裁剪导致 **embedding 区分度崩塌**：不同的人之间有 0.6~0.7 的相似度 |

**如何自行确认**（不依赖相似度分数，直接看几何）：

1. 检测框应当**覆盖整张脸**（含额头与下巴），而不是只框住眼睛到嘴；
2. 5 点关键点应落在真实的眼 / 鼻 / 嘴上，其中**眼睛→嘴巴的纵向跨度约占框高的 40%**；
   若只占 25%~30%，就是点被压缩了；
3. 把对齐后的 112×112 裁剪图导出来看：应当是一张**完整正脸**，而不是眼睛 + 鼻子特写。

**修复**：把检测器后处理改成 SCRFD 约定（`cx = gx * stride`，
`x1 = cx - raw[0] * stride`，关键点同理），详见
[../services/face_detector.md §3.3](../services/face_detector.md#33-解码scrfd-约定)。

```bash
make -C build/standalone -j$(nproc)
./install/bin/face_recognition_app backfill --all   # 必须重建
```

> 这条修复**改变了取脸区域**，因此等价于更换前端：**不执行 `backfill --all`
> 就会出现"评分还是乱的"**。

**修复后的健康形态**：本人明显最高，"不同的人"普遍落在 `0 ~ 0.2`（不少为负值）。

### Q24 接了多个摄像头，`--source 0` 开的不是我想用的那台 / 设了分辨率帧率却不生效

**原因（两件事，都属于"采集模式"，不是识别问题）**

1. **一台摄像头会占用多个 `/dev/videoN`**：UVC 除了采集节点，还会发布若干 metadata
   节点；RealSense 这类多流设备更是一次占用好几个。编号又按枚举顺序分配，插拔后会变。
   所以"猜编号"必然不靠谱。
2. **`--width/--height/--fps` 只是请求值**，由 V4L2 就近协商。若选了未经压缩的 YUYV
   且分辨率 ≥ 720p，就超出 USB2 带宽，摄像头会静默掉到个位数帧率 —— 表现为"识别很慢"。

**处理**

```bash
APP=./install/bin/face_recognition_app

# ① 先看这台机器上到底有哪些节点、哪台是什么、各自当前模式
$APP cameras
#    index  device        name                    mode               status
#    0      /dev/video0   HD Webcam: HD Webcam    640x480@30 MJPG    ok
#    1      /dev/video1   HD Webcam: HD Webcam    -                  cannot open (metadata node / ...)
#    4      /dev/video4   Intel(R) RealSense(TM)  640x480@30 UYVY    ok
#
#    "cannot open" 的多半是同一台摄像头的附属节点，属正常现象。

# ② 想知道各档分辨率能不能用，加 --probe（会逐档重开设备，较慢）
$APP cameras --probe

# ③ 用编号显式指定；写错时立刻报错并提示可用编号
$APP run --camera 0
$APP run --camera 0 --width 1280 --height 720 --fps 30
$APP run --camera 0 --fourcc MJPG
$APP run --camera 0 --width 0 --height 0        # 用摄像头自己的默认分辨率
```

**确认实际生效的模式**：启动日志会打印协商结果，降级时会给出 `[WARN]`：

```
[INFO] Source opened: type=usb_camera uri=0 (1280x720 @ 30.0 fps) MJPG
[WARN] camera delivers 5 fps instead of the requested 30 fps -- try --fourcc MJPG, or a smaller --width/--height
```

**判读**

| 现象 | 含义 |
|---|---|
| `Source opened` 里的分辨率/帧率 = 你请求的值 | 设置生效 |
| 出现 `[WARN] camera does not support ...` | 摄像头不支持该分辨率，已就近降级 |
| 出现 `[WARN] camera delivers N fps instead of ...` | 采集模式带宽不足 → 加 `--fourcc MJPG` 或降低 `--width/--height` |
| 帧率低但日志无 `[WARN]` | 瓶颈在识别侧（每张脸一次 112×112 推理），见 [Q15](#q15-fps-太低) |

详见 [../module/standalone.md §4](../module/standalone.md#41-多摄像头先-cameras再---camera)。

### Q25 批量导入后"少了几个人"：图片没进库

**先看导入报告**。`/api/faces/import-archive` 与 `/api/faces/import-batch` 都会返回
逐张结果，每张只会有一种归属：

| 归属 | 字段 | 含义与处理 |
|---|---|---|
| 成功 | `success=true` | 已入库 |
| 重复跳过 | `duplicate=true` | 内容哈希/批内同名命中，**属正常**，不是错误 |
| **待人工确认** | `needs_confirm=true` | 相似度 ≥ 阈值（默认 0.80），**未写库**，需在「待确认入库」宫格选择处理方式 |
| 失败 | `success=false` + `reason` | 见下表 |

失败原因对照：

| `reason` | 原因 | 处理 |
|---|---|---|
| `no face detected` | 检测不到人脸（含低阈值补救重试后仍无） | 换清晰正面照；或启动时 `--det-threshold 0.3` |
| `too small (WxH < 80 px)` | 人脸框短边 < 80px | 换更大分辨率的照片 |
| `no landmarks` | 关键点质量不达标（大侧脸等） | 换正面照 |
| `cannot decode image` | 不是有效 png/jpg | 检查文件本身 |
| `detection/recognition models are not loaded` | 服务端没加载模型 | 见 [Q21](#q21-web-后台录入的人脸识别时永远匹配不到特征为空) |

> 压缩包导入还会把非 png/jpg、隐藏文件、`__MACOSX/`、`._*` 计入 `skipped`。

### Q26 自动性别识别结果不对 / 想关掉

**现象**：开启了 `--auto-gender`，个别记录性别判反。

**原因**：`genderage.onnx` 是轻量属性模型，在真实证件照上准确率约 83%，
中性名字/光线差的样本容易错。

**处理**

1. **它就是"预填建议"**：在人脸卡片上点「编辑」直接改即可（不会覆盖你手填的值）；
2. 若**整体**男女判反了，说明该导出模型的输出顺序不同：
   改 `--gender-male-index 0` 重启；
3. 不需要就**不要加 `--auto-gender`**（默认关闭），此时性别全部为 `unknown`，
   完全由人工填写。

诊断：`GET /api/config` 返回的 `auto_gender` 表示当前是否开启；
启动日志会打印 `Gender model shape: input dims=[-1x3x96x96] (NCHW) ... outputs: fc1[1x3]`；
设环境变量 `FACE_GENDER_DEBUG=1` 可打印每次推理的原始输出。

### Q27 同一个人有两张照片：该用「追加为模板」还是「替换原记录」？

| 选择 | 行为 | 适用 |
|---|---|---|
| **追加为模板** | 保留原记录与原照片，新照片作为**额外模板（多枪）** | **推荐**：同一人的另一角度 / 戴不戴眼镜 / 不同光照 |
| 替换原记录 | 用新照片与新特征**覆盖**原记录 | 原照片录错了、或要用新照完全取代旧照 |
| 入库(新增) | 新增一条**独立**记录 | 确认是**另一个人**（同脸不同人），或就是要把同一个人拆成两条 |

> 匹配时取一个身份下**所有模板的最高分**，因此多枪能显著提升召回
> （见 [Q22](#q22-戴上眼镜能识别摘掉就认不出而别人戴着眼镜却被认成我)）。
> 「替换」会把旧模板丢掉，只在确认旧照片质量差时才用。

CLI 等价入口：`face_recognition_app add-template --id <uuid> --image <照片>`。

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
APP=./install/bin/face_recognition_app

# ① 录入测试图片并查看
$APP add --image ./test.jpg --name test_person --scene test
$APP list                       # 期望 emb=yes

# ② 对同一张已知照片打分，检查排名与判定
$APP verify --image ./test.jpg

# ③ 对"另一个人"的照片打分，确认被拒绝
$APP verify --image ./other.jpg
```

**判读要点**：同一人的分数应明显高于其他人，且其他人的分数应落在低位区间
（接近 0 或负值）。若两者区间重叠，按 [Q20](#q20-本人识别不出--是个人都能识别误识) /
[Q23](#q23-不同的人之间相似度异常偏高本人反而更低) 排查，**不要靠调阈值救**。

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
