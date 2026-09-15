# 非 ROS 实时识别模块使用文档（face_recognition_standalone）

**零 ROS 依赖**的桌面级实时人脸识别 CLI。底层复用与 ROS1/ROS2 完全相同的
`face_recognition_core`，因此检测/识别精度、模型权重、SQLite 数据库与 ROS 版本
**100% 兼容**。

- 源码：`src/face_recognition_standalone/`
- 依赖：OpenCV + ONNX Runtime + SQLite3 + pthread
- 产物：`src/face_recognition_standalone/build/face_recognition_app`

---

## 1. 编译

```bash
./build.sh STANDALONE
```

产物：

```
src/face_recognition_standalone/build/face_recognition_app         # 主程序
src/face_recognition_standalone/build/libface_recognition_core.so* # 核心库（同目录）
```

> `STANDALONE` 目标不需要任何 ROS 包，也不需要 `source install/setup.bash`。

---

## 2. 命令总览

```bash
APP=./src/face_recognition_standalone/build/face_recognition_app
$APP help
```

| 命令 | 作用 |
|---|---|
| `run` | 实时识别循环（摄像头 / RTSP / HTTP / 视频文件 / 图片目录） |
| `add` | 从单张图片录入一张人脸 |
| `add-bulk` | 整个目录批量录入（文件名去后缀即人名） |
| `backfill` | 为库内已有图片补写 embedding |
| `list` | 列出全部人脸（含 `emb` / `img` 状态） |
| `remove` | 按 ID 删除 |
| `clear` | 清空人脸库 |
| `web` | 拉起 `face_db_web` 后台，共享同一个 DB |

---

## 3. 三步上手

```bash
# ① 录入人脸（自动检测 + 提取特征入库）
$APP add --image ~/photos/alice.jpg \
         --name "张三" --title "工程师" --scene office

# ② 查看人脸库
$APP list

# ③ 实时识别（默认 /dev/video0）
$APP run --source 0
```

弹出一个 OpenCV 窗口，实时框出人脸并标注姓名 / 相似度。按 `q` / `ESC` 退出。

---

## 4. 输入源

`--source` 自动识别 URI 类型：

| 场景 | `--source` 写法 |
|---|---|
| 默认 USB 摄像头（`/dev/video0`） | `0`（可省略） |
| 第二个 USB 摄像头 | `1` |
| 指定设备节点 | `/dev/video1` |
| RTSP 网络摄像头 | `rtsp://user:pass@192.168.1.10/stream1` |
| HTTP / MJPEG 流 | `http://host:port/video` |
| 本地视频文件 | `./input.mp4`（加 `--loop` 可循环） |
| 单张图片（处理一次即退出） | `./face.jpg` |
| 整个目录的图片（顺序处理） | `./frames/` |

---

## 5. 参数速查

### 5.1 通用参数（所有命令）

| 选项 | 默认值 | 说明 |
|---|---|---|
| `--detection-model` | `models/det_10g.onnx` | 检测模型 |
| `--recognition-model` | `models/w600k_r50.onnx` | 识别模型 |
| `--db` | `/tmp/face_db/faces.db` | SQLite 数据库 |
| `--faces-dir` | `/tmp/face_db/faces` | 缩略图目录 |
| `--detection-threshold` | `0.5` | 检测置信度阈值 |
| `--recognition-threshold` | `0.5` | 识别相似度阈值，取值依据见 [§7.1](#71-识别阈值为什么默认-05) |
| `--nms-threshold` | `0.5` | NMS IoU 阈值 |
| `--input-size` | `640` | 检测器输入尺寸（帧被缩放到 N×N） |
| `--max-faces` | `10` | 单帧最大人脸数 |

> 所有路径**相对于当前工作目录**，建议先 `cd` 到仓库根目录，或直接传绝对路径。

### 5.2 `run` 专有参数

| 选项 | 说明 |
|---|---|
| `--source <uri>` | 输入源，见第 4 节 |
| `--width` / `--height` | 请求采集分辨率 |
| `--fps` | 请求采集帧率 |
| `--loop` | 视频文件循环播放 |
| `--no-display` | headless 模式（不打开窗口） |
| `--detect-every-n <n>` | 每 n 帧检测一次，其余帧复用上次 bbox（默认 `2`） |
| `--downscale <n>` | 送入检测器前把帧最长边缩到 ≤ n |
| `--no-recognition` | 仅检测，不做特征提取与比对 |
| `--target-fps <n>` | 识别循环帧率上限（默认 `24`，`0` = 不限制） |
| `--auto-backfill` | 启动时自动补齐缺失的 embedding |
| `--save-video <path>` | 把标注后画面保存为 MP4 |
| `--snapshot-dir <path>` | 每识别到一张人脸自动抓拍存档 |

### 5.3 `add` / `add-bulk` / `web` 专有参数

| 命令 | 选项 |
|---|---|
| `add` | `--image`（必需）、`--name`（必需）、`--title`、`--scene`、`--map-location` |
| `add-bulk` | `--dir`（必需）、`--scene`、`--map-location`、`--recursive` |
| `web` | `--port`、`--web-binary`（默认 `./install/bin/face_db_web`） |

---

## 6. 常用示例

```bash
# 指定分辨率 / 帧率
$APP run --source 0 --width 1280 --height 720 --fps 30

# headless（远程服务器 / 录制）
$APP run --source rtsp://... --no-display

# 保存标注视频
$APP run --source 0 --save-video ./annotated.mp4

# 自动抓拍
$APP run --source 0 --snapshot-dir ./snapshots

# 无窗口下调整阈值
$APP run --source 0 --recognition-threshold 0.6 --detection-threshold 0.4

# 删除 / 清空
$APP remove --id <uuid>
$APP clear
```

---

## 7. 性能调优（提升 FPS）

CPU 上 RetinaFace 检测是主要瓶颈（`--input-size 640` 时约 215 ms/帧）。
**默认已开启 `--detect-every-n 2`**，即检测成本按帧摊薄一半，识别仍在每个处理帧
上运行；`--target-fps 24` 只是循环帧率上限，在慢于 24 fps 的机器上不起作用。

| 优化项 | 默认 | 推荐值 | 原理 | 副作用 |
|---|---|---|---|---|
| `--detect-every-n` | `2` | `3` | 每 N 帧检测一次，跳过帧复用上次 bbox | 快速运动时 bbox 可能滞后 |
| `--input-size` | `640` | `320` / `240` | 检测器输入缩到 N×N，**N 减半 ≈ 4× 提速** | 小脸/远脸召回略降 |
| `--downscale` | 关 | `480` / `320` | 检测前先把帧最长边缩小 | 同上 |
| `--no-recognition` | 关 | — | 只画检测框，不做 embedding 与比对 | 不识别身份 |
| `--target-fps` | `24` | `0` | 取消循环帧率上限，跑满硬件 | CPU 占用升高 |

> `--input-size` 控制检测器的 N×N 输入尺寸；`--downscale` 是预处理阶段的一次
> resize，二者**语义不同、可叠加**。
> `--input-size` 同时作用于 `add` / `add-bulk` / `backfill`，**批量入库时建议保持
> `640`** 以保证特征质量，只在 `run` 时调小。
>
> **任意 `--input-size` 均受支持**（后处理按输入尺寸推导各层 anchor 数）。
> 若你用的是修复前的旧二进制，`--input-size` 非 640 会导致**完全检测不到人脸**，
> 详见 [../FAQ/troubleshooting.md](../FAQ/troubleshooting.md) 的 Q18。

实测（`det_10g.onnx` + `w600k_r50.onnx`，640×479 摄像头条件图，画面 3 张脸）：

| `--input-size` | 检测耗时 | 识别耗时/人脸 | 单人脸帧率（`detect-every-n 2`） | 同一人相似度 |
|---|---|---|---|---|
| `640`（默认） | 224 ms | 64.7 ms | ≈ 3.6 FPS | 0.6936 |
| `512` | 159 ms | ~65 ms | ≈ 4.9 FPS | — |
| `416` | 104 ms | ~65 ms | ≈ 7.1 FPS | — |
| **`320`** | **58.9 ms** | 67.4 ms | **≈ 10.6 FPS** | 0.5918 |
| `240` | 33.6 ms | 66.9 ms | ≈ 12.2 FPS | 0.5800 |

> **识别是硬下限**：约 65 ms/人脸，与 `--input-size` 无关（内部固定 112×112）。
> 因此单人脸的理论天花板约 **15 FPS**；画面 3 张脸时光识别就要 195 ms
> （≈ 4~5 FPS）。**在纯 CPU 上无法达到 24 FPS**，`--target-fps 24` 只作为上限，
> 要真 24 FPS 需换 GPU 版 ONNX Runtime，或去掉识别（`--no-recognition`）。

**推荐生产配置**（兼顾召回率与帧率）：

```bash
$APP run --source 0 --input-size 320      # --detect-every-n 已是默认 2
```

### 7.1 识别阈值：为什么默认 0.5

`--recognition-threshold` 决定「相似度 ≥ 多少才算同一个人」。用本项目自带模型
（`det_10g` + `w600k_r50`）实测：

| 场景 | 相似度 | 0.5 判定 | 旧默认 0.7 判定 |
|---|---|---|---|
| 同图自比对 | 0.9997 | 命中 | 命中 |
| 同一人，两张清晰证件照 | 0.7539 | 命中 | 命中（勉强） |
| 同一人，640×480 + JPEG q75（≈ 摄像头条件） | 0.6936 | 命中 | **漏判** |
| 同一人，480×360 + JPEG q55 | 0.6669 | 命中 | **漏判** |
| 同一人，320×240 + JPEG q55 | 0.6236 | 命中 | **漏判** |
| 同一人，缩放 50% + 旋转 12° | 0.4982 | 命中 | **漏判** |
| 不同人（背景路人） | 0.23 ~ 0.40 | 拒绝 | 拒绝 |

结论：`0.7` 会把**几乎所有现场摄像头画面**判为 `Unknown`（画面上是红框 +
`Unknown` 标签）。默认值因此下调为 **0.5**：与"不同人"的上界（0.40）仍留有
0.1 的间隔，同时能容忍摄像头侧的降质。

调参建议：

```bash
# 更严格（误识别更少，遮挡/侧脸易漏）
$APP run --source 0 --recognition-threshold 0.6

# 更宽松（远距离/小脸更容易命中）
$APP run --source 0 --recognition-threshold 0.45
```

---

## 8. 批量入库与 embedding 回填

SQLite 表内**每行都存了 `image_path`**，这带来两条实用能力。

### 8.1 `add-bulk` —— 目录批量入库

```bash
# 文件名去后缀即人名
$APP add-bulk --dir ./photos/ --scene office --map-location "B座3F"

# 递归子目录
$APP add-bulk --dir ./photos/ --recursive
```

支持的图片扩展名：`.jpg` `.jpeg` `.png` `.bmp` `.webp`

### 8.2 `backfill` —— 补写 embedding

若导入了**老的 SQLite**，或用 Web 录入时未启用自动 embedding，库内只有
`image_path`、`embedding` 为空，此时直接 `run` **所有脸都识别不出**。执行一次
`backfill` 即可：

```bash
$APP backfill
# [backfill] updated id=xxx name=张三 (512-d embedding)
# backfill done: total=12 updated=10 already_embedded=2 failed=0
```

`list` 会显示每条记录的 embedding 状态，便于定位：

```bash
$APP list
# Faces in DB: 12
#   id=xxx name=张三  title=工程师  scene=office map=B座3F  emb=yes  img=/tmp/face_db/faces/xxx.jpg
#   id=yyy name=李四  title=       scene=office map=B座3F  emb=NO   img=/tmp/face_db/faces/yyy.jpg
#   id=zzz name=(no image) title=  scene=office map=B座3F  emb=NO   img=(none)
```

---

## 9. 共享数据库（Web / ROS / standalone 三向互通）

三方使用**同一套 schema**，且默认都指向 `/tmp/face_db/faces.db`：

| 通路 | 默认 DB 路径 | 指定方式 |
|---|---|---|
| `face_recognition_standalone` | `/tmp/face_db/faces.db` | `--db` |
| `face_db_web` | — | `--db` 显式传入 |
| ROS2 节点 | `/tmp/face_db/faces.db` | parameter `db_path` |

只要三方使用同一个 `--db`，在一处录入 / 删除，另两处**立刻可见**。

### 推荐工作流：Web 录入 → standalone 实时识别

```bash
# 0) 构建
./build.sh WEB
./build.sh STANDALONE

# 1) 启动 Web 后台（绑定同一 DB，等价于直接用 face_db_web）
$APP web --port 8080

# 2) 浏览器打开 http://localhost:8080/ 录入人脸（Web 端自动提取 embedding）

# 3) 启动实时识别
$APP run --source 0 --input-size 320

# 若 Web 启动时未加载识别模型（emb=NO），加 --auto-backfill 自动补
$APP run --source 0 --auto-backfill --input-size 320
```

> **注意**：不要让 standalone `run` / `add` 与 ROS2 节点**同时**向同一个 SQLite
> 文件写入。WAL 可并发读，但并发写仍需串行化；Web 后台使用短事务，一般无影响。

---

## 10. 日志与退出

- 每 2 秒打印一次平滑 FPS 与耗时拆解：

  ```
  [INFO] ~24.3 FPS | last frame: 59.1 ms (det=41.2, rec=17.9)
  ```

- 窗口模式按 `q` / `ESC` 退出；终端模式 `Ctrl+C` 退出
- 若 `cv::namedWindow` 抛异常（无 GUI 环境），自动降级为 headless 并打印 `[WARN]`

---

## 11. 相关文档

- [核心算法库模块说明](core.md)
- [Web 后台模块使用文档](web.md)
- [接口、协议与配置](../protocol/)
- [常见问题排查](../FAQ/troubleshooting.md)
