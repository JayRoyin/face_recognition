# 非 ROS 实时识别模块使用文档（face_recognition_standalone）

**零 ROS 依赖**的桌面级实时人脸识别 CLI。底层复用与 ROS1/ROS2 完全相同的
`face_recognition_core`，因此检测/识别精度、模型权重、SQLite 数据库与 ROS 版本
**100% 兼容**。

- 源码：`src/face_recognition_standalone/`
- 依赖：OpenCV + ONNX Runtime + SQLite3 + pthread
- 产物：`install/bin/face_recognition_app`

---

## 1. 编译

```bash
./build.sh STANDALONE          # 或 make -C build/standalone -j4
```

产物：

```
install/bin/face_recognition_app                 # 主程序
install/lib/libface_recognition_core.so*         # 共享核心库（与 web / ROS 同一份）
```

> `STANDALONE` 目标不需要任何 ROS 包，也不需要 `source install/ros2/setup.bash`。

---

## 2. 命令总览

```bash
APP=./install/bin/face_recognition_app
$APP help
```

| 命令 | 作用 |
|---|---|
| `run` | 实时识别循环（摄像头 / RTSP / HTTP / 视频文件 / 图片目录） |
| `cameras` | 列出本机摄像头（`/dev/video*`）：编号、设备名、当前采集模式；`--probe` 还会试出各档分辨率。多摄像头时先用它确认编号 |
| `add` | 从单张图片录入一张人脸 |
| `add-bulk` | 整个目录批量录入（文件名去后缀即人名） |
| `add-template` | 给已存在的身份**补一枪**照片（多模板，解决戴/不戴眼镜等问题） |
| `verify` | 把一张图对全库打分，打印排名、队列统计与最终判定原因 |
| `backfill` | 为库内已有图片补写 embedding（`--all` 强制全部重建） |
| `list` | 列出全部人脸（含 `emb` / `templates` / `img` 状态） |
| `remove` | 按 ID 删除 |
| `clear` | 清空人脸库 |
| `web` | 拉起 `face_db_web` 后台，共享同一个 DB |

---

## 3. 三步上手

```bash
# ① 录入人脸 —— 推荐用 Web 后台传图（可视化、自动检测 + 提取特征）
$APP web --port 8080
#   浏览器打开 http://localhost:8080/ ：
#     · 在 “Add Face” 表单填 Name（必填），可选 Title / Scene / Map Location
#     · “Or Upload” 选本地照片，或在 “Image URL” 填图片链接
#     · 点 “Add Face” → 提示 Face added with embedding 即成功

# ② 确认库内记录（emb=yes）
$APP list

# ③ 实时识别（默认 /dev/video0）
$APP run --source 0
```

弹出 OpenCV 窗口，实时框出人脸并标注姓名 / 相似度。按 `q` / `ESC` 退出。

**批量 / 脚本化录入**（无需打开浏览器）用 CLI：

```bash
# 单张
$APP add --image ~/photos/alice.jpg --name "张三" --title "工程师" --scene office
# 整个目录，文件名去后缀即人名
$APP add-bulk --dir ./photos/ --scene office
# 给已有身份补一枪（Web 页面暂不支持）
$APP add-template --id <uuid> --image ~/photos/alice_no_glasses.jpg
```

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

### 4.1 多摄像头：先 `cameras`，再 `--camera`

`--source 0` 这种"猜编号"的方式在多摄像头机器上很容易开错设备，原因是有两个坑：

1. **一台摄像头会占用多个 `/dev/videoN`**（UVC 除了采集节点还会发布若干 metadata 节点）；
2. 编号按枚举顺序分配，**插拔后可能变化**。

所以提供两个配套能力：

```bash
# ① 列出本机所有 video 节点：编号 / 设备名 / 当前采集模式 / 是否可打开
$APP cameras

# ② 想进一步知道每台设备支持哪些分辨率（会逐档重开设备，较慢）
$APP cameras --probe

# ③ 用编号显式指定（等价于 --source，但参数写错会立刻报错并提示可用编号）
$APP run --camera 0
```

`cameras` 输出示例（8 个节点里只有 3 个可用）：

```
  index  device        name                    mode               status
  0      /dev/video0   HD Webcam: HD Webcam    640x480@30 MJPG    ok
  1      /dev/video1   HD Webcam: HD Webcam    -                  cannot open (metadata node / busy / no capture)
  4      /dev/video4   Intel(R) RealSense(TM)  640x480@30 UYVY    ok
  6      /dev/video6   Intel(R) RealSense(TM)  640x480@30 YUYV    ok
```

难以打开的节点**不是错误**：它们是同一台摄像头的附属节点。把"不可用"当成异常，
反而会让多摄像头现场无从下手。

### 4.2 分辨率与帧率

`--width` / `--height` / `--fps` 是**请求值**，由 V4L2 就近协商：

- USB 摄像头默认按 `1280x720 @ 30` 打开（可用 `--width 0 --height 0` 让摄像头用自己的默认值）；
- 分辨率 ≥ 720p 时**自动改用 MJPG**。原因是未经压缩的 YUYV 在这个尺寸超出 USB2 带宽，
  摄像头会静默掉到个位数帧率 —— 这看起来像"识别慢"，实际是采集模式问题；
- 摄像头无法满足请求时会打印 `[WARN]`，说明实际协商到的分辨率 / 帧率；
- 部分摄像头只支持某一种格式，可用 `--fourcc MJPG` / `--fourcc YUYV` 强制指定。

```bash
# 720p30 + MJPG（默认组合，人脸细节最多）
$APP run --camera 0 --width 1280 --height 720 --fps 30

# 低算力场景：小尺寸高帧率
$APP run --camera 0 --width 640 --height 480 --fps 30

# 摄像头自己的默认分辨率
$APP run --camera 0 --width 0 --height 0
```

启动时会打印实际生效的采集模式，便于确认：

```
[INFO] Source opened: type=usb_camera uri=0 (1280x720 @ 30.0 fps) MJPG
```

---

## 5. 参数速查

### 5.1 通用参数（所有命令）

| 选项 | 默认值 | 说明 |
|---|---|---|
| `--detection-model` | `models/det_10g.onnx` | 检测模型 |
| `--recognition-model` | `models/w600k_r50.onnx` | 识别模型 |
| `--db` | `/data/hhqs_data/face_db/faces.db` | SQLite 数据库 |
| `--faces-dir` | `/data/hhqs_data/face_db/faces` | 缩略图目录 |
| `--detection-threshold` | `0.5` | 检测置信度阈值 |
| `--recognition-threshold` | `0.5` | 识别相似度阈值，取值依据见 [§7.1](#71-识别阈值) |
| `--nms-threshold` | `0.5` | NMS IoU 阈值 |
| `--input-size` | `640` | 检测器输入尺寸（帧等比缩放后贴到 N×N） |
| `--max-faces` | `10` | 单帧最大人脸数 |
| `--min-face-size` | `80` | 人脸框短边低于此值只检测、**不识别** |
| `--align` / `--no-align` | **对齐开** | 是否用 5 点关键点对齐前端，见 [§7.2](#72-取脸前端与对齐) |
| `--allow-unaligned` | 关 | 对齐不可用时是否允许回退 bbox 前端（**不建议**） |
| `--no-cohort-norm` | 关 | 关闭队列 z-score 门控（**默认开启**） |
| `--z-threshold` | `3.0` | 队列归一化的 z 门限 |
| `--min-cohort` | `3` | 队列样本数不足时退化为纯原始门限 |
| `--cohort-db` | 空 | 外部"他人"队列库路径 |

> 所有路径**相对于当前工作目录**，建议先 `cd` 到仓库根目录，或直接传绝对路径。

### 5.2 `run` 专有参数

| 选项 | 说明 |
|---|---|
| `--source <uri>` | 输入源，见第 4 节 |
| `--camera <n\|node>` | **显式选择本机摄像头**：编号（`0`）或设备节点（`/dev/video2`）。会用 `/dev/video*` 校验，设备不存在时立刻报错并提示 `cameras` 命令；与 `--source` 同时给出时 `--camera` 优先 |
| `--width` / `--height` | 请求采集分辨率（默认 `1280x720`；传 `0` 用摄像头默认值） |
| `--fps` | 请求采集帧率 |
| `--fourcc <FMT>` | 强制 V4L2 像素格式（`MJPG` / `YUYV`）。缺省行为：分辨率 ≥ 720p 时自动用 MJPG（YUYV 在该尺寸会超出 USB2 带宽而掉到几帧） |
| `--loop` | 视频文件循环播放 |
| `--no-display` | headless 模式（不打开窗口） |
| `--detect-every-n <n>` | 每 n 帧检测一次，其余帧复用上次 bbox（默认 `2`） |
| `--downscale <n>` | 送入检测器前把帧最长边缩到 ≤ n |
| `--no-recognition` | 仅检测，不做特征提取与比对 |
| `--target-fps <n>` | 识别循环帧率上限（默认 `24`，`0` = 不限制） |
| `--auto-backfill` | 启动时自动补齐缺失的 embedding |
| `--save-video <path>` | 把标注后画面保存为 MP4 |
| `--snapshot-dir <path>` | 每识别到一张人脸自动抓拍存档 |

### 5.3 其他命令专有参数

| 命令 | 选项 |
|---|---|
| `add` | `--image`（必需）、`--name`（必需）、`--title`、`--scene`、`--map-location` |
| `add-bulk` | `--dir`（必需）、`--scene`、`--map-location`、`--recursive` |
| `add-template` | `--id`（必需）、`--image`（必需） |
| `verify` | `--image`（必需） |
| `backfill` | `--all`（重算全部记录；缺省只补 `embedding` 为空的） |
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

## 7. 识别效果调优

### 7.1 识别阈值

`--recognition-threshold` 决定「相似度 ≥ 多少才算同一个人」。相似度是 512 维
embedding 的余弦值，同一人的不同照片落在 `0.5 ~ 0.8` 区间，"不同的人"通常落在
`0 ~ 0.2` 甚至负值。因此：

| 取值 | 效果 |
|---|---|
| `0.45` | 宽松，远距离 / 小脸召回更高，误识别风险上升 |
| **`0.5`** | **默认**，位于本人簇与"不同人"簇之间 |
| `0.6` | 严格：进一步压低误识，遮挡 / 侧脸易漏 |
| `0.7` | 过严：会漏判大量现场画面（画面上显示 `Unknown`） |

```bash
# 更严格（误识别更少，遮挡/侧脸易漏）
$APP run --source 0 --recognition-threshold 0.6

# 更宽松（远距离/小脸更容易命中）
$APP run --source 0 --recognition-threshold 0.45
```

> **阈值解决不了的问题**：如果"本人"分数低于"不同的人"上界，说明特征或取脸前端
> 出了问题（前端混用、检测框 / 关键点异常、库内特征过期），此时调阈值只会同时
> 放大误识与漏识。先用 `verify` 把分数打出来定位。

### 7.2 取脸前端与对齐

识别器有两个取脸前端，**特征空间不互通**：

| 前端 | 说明 |
|---|---|
| **对齐（默认）** | 用检测器输出的 5 点关键点做相似变换，warp 到 ArcFace 标准 112×112 模板 |
| bbox 裁剪 | 框外扩 20% 后取 `1.4×` 区域，`resize` 到 112×112 |

`--align` 默认开启。当关键点不可用（太小 / 侧脸 / 坐标异常）时：

- 默认（`allow_unaligned=false`）：该人脸**不参与识别**，避免污染库；
- 加 `--allow-unaligned`：回退到 bbox 裁剪——**会导致库内前端混用，不建议**。

> ⚠️ 切换前端后必须 `backfill --all` 重建全部特征，详见 §8.2。

### 7.3 帧率调优

CPU 上检测是主要瓶颈。**默认已开启 `--detect-every-n 2`**，即检测成本按帧摊薄
一半，识别仍在每个处理帧上运行；`--target-fps 24` 只是循环帧率上限，在慢于
24 fps 的机器上不起作用。

| 优化项 | 默认 | 推荐值 | 原理 | 副作用 |
|---|---|---|---|---|
| `--detect-every-n` | `2` | `3` | 每 N 帧检测一次，跳过帧复用上次 bbox | 快速运动时 bbox 可能滞后 |
| `--input-size` | `640` | `320` / `240` | 检测器输入缩到 N×N，**N 减半 ≈ 4× 提速** | 小脸 / 远脸召回下降，关键点精度下降 |
| `--downscale` | 关 | `480` / `320` | 检测前先把帧最长边缩小 | 同上 |
| `--no-recognition` | 关 | — | 只画检测框，不做 embedding 与比对 | 不识别身份 |
| `--target-fps` | `24` | `0` | 取消循环帧率上限，跑满硬件 | CPU 占用升高 |

> `--input-size` 控制检测器的 N×N 输入尺寸；`--downscale` 是预处理阶段的一次
> resize，二者**语义不同、可叠加**。
>
> `--input-size` 同时作用于 `add` / `add-bulk` / `backfill`，**批量入库时建议保持
> `640`** 以保证特征质量，只在 `run` 时调小。
>
> **任意 `--input-size` 均受支持**（后处理按输入尺寸推导各层 anchor 数）。

> **识别本身是硬下限**：每张脸的 embedding 推理与 `--input-size` 无关（内部固定
> 112×112），因此画面里人脸越多、帧率越低。纯 CPU 上很难达到 `--target-fps 24`，
> 此时该参数只是一道上限；要更高帧率需换 GPU 版 ONNX Runtime，或用
> `--no-recognition` 去掉识别。

**推荐生产配置**：

```bash
$APP run --source 0 --input-size 320      # --detect-every-n 已是默认 2
```

---

## 8. `verify`：判定过程可视化

`verify` 对**单张图片**跑完整的「检测 → 取脸 → 提特征 → 全库比对 → 判定」链路，
并把中间的每个分数打印出来。它是排查"为什么识别不出 / 为什么认错人"的第一手段，
也可以用来对一批图片做离线校验。

```bash
$APP verify --image ./test.jpg
```

输出包含：

| 行 | 含义 |
|---|---|
| `faces detected` | 检出的脸数 |
| 排名列表 | 每个身份的 `raw similarity`、模板数、`id` |
| `raw similarity` | 最佳身份的原始余弦值，与 `--recognition-threshold` 对照 |
| `cohort` | 队列归一化统计（`n` / `median` / `mad` / `z`）；样本不足时退化为纯原始门限 |
| `decision` | `ACCEPT` / `REJECT` 及原因（低于阈值 / 低于 z 门限 / 前端不可用等） |
| `=> <id>` | 命中时给出的记录 ID |

批量校验（对目录里所有图片逐一打分）：

```bash
for f in ./test_image/*.png; do
  printf '%-24s ' "$f"
  $APP verify --image "$f" | grep -E 'raw similarity|decision' | tr '\n' ' '
  echo
done
```

> `verify` 与实时循环、录入路径共用同一个 `prepare_embedding()` 质量门控
> （前端一致性 + `--min-face-size`），所以它报告的判定**就是运行时真实的判定**。

---

## 9. 批量入库与 embedding 回填

SQLite 表内**每行都存了 `image_path`**，这带来两条实用能力。

### 9.1 `add-bulk` —— 目录批量入库

```bash
# 文件名去后缀即人名
$APP add-bulk --dir ./photos/ --scene office --map-location "B座3F"

# 递归子目录
$APP add-bulk --dir ./photos/ --recursive
```

支持的图片扩展名：`.jpg` `.jpeg` `.png` `.bmp` `.webp`

### 9.2 `add-template` —— 给同一个人补多枪

一个人可以存**任意多张模板**（主模板在 `faces.embedding`，其余在 `face_templates`）。
匹配时取所有模板的最高分，因此补一枪"不戴眼镜的正脸"能同时覆盖戴 / 不戴眼镜的场景：

```bash
# 先用 list 拿到 id
$APP list
$APP add-template --id <uuid> --image ~/photos/alice_no_glasses.jpg
# OK template added to id=<uuid> (stored /data/hhqs_data/face_db/faces/<uuid>_t1.jpg)
#     total templates for this id: 2
```

模板越多越稳，但也会抬高"误识"的概率（任一枪命中即算命中），建议每人 1~3 枪、
覆盖真实使用时的姿态 / 光照 / 配饰差异。

### 9.3 `backfill` —— 补写 embedding

若导入了**老的 SQLite**，或用 Web 录入时未启用自动 embedding，库内只有
`image_path`、`embedding` 为空，此时直接 `run` **所有脸都识别不出**。执行一次
`backfill` 即可：

```bash
$APP backfill
# [backfill] updated id=xxx name=张三 (512-d embedding)
# backfill done: total=12 updated=10 already_embedded=2 failed=0
```

#### `backfill --all` —— 绑定取脸/预处理变更的迁移手段

`backfill` 默认**只处理 `embedding` 为空的记录**。但下面任何一项变更都会让
**所有已存特征失效**（新旧特征空间不兼容）：

- 识别器预处理配方变更（归一化 / 通道顺序）；
- **取脸前端切换**（对齐 ↔ bbox 裁剪）；
- **检测器取框 / 关键点解码修正**（框或关键点位置变化 ⇒ 裁剪区域变化）；
- 更换识别模型。

此时必须强制重算全部记录：

```bash
$APP backfill --all
# [backfill] updated id=xxx name=1 (512-d embedding)
# backfill done: total=1 updated=1 already_embedded=0 failed=0  (--all: every record re-extracted)
```

它直接复用库内已存档的缩略图（`image_path`），**不需要重新上传照片**。
凡是升级过二进制、或混用了新旧构建（例如 standalone 已升级但 ROS 节点未重编），
都应对同一个 `--db` 执行一次 `backfill --all`。

`list` 会显示每条记录的 embedding 状态，便于定位：

```bash
$APP list
# Faces in DB: 12
#   id=xxx name=张三  title=工程师  scene=office map=B座3F  emb=yes  templates=1 img=/data/hhqs_data/face_db/faces/xxx.jpg
#   id=yyy name=李四  title=       scene=office map=B座3F  emb=NO   templates=0 img=(none)
```

---

## 10. 共享数据库（Web / ROS / standalone 三向互通）

三方使用**同一套 schema**，且默认都指向 `/data/hhqs_data/face_db/faces.db`：

| 通路 | 默认 DB 路径 | 指定方式 |
|---|---|---|
| `face_recognition_standalone` | `/data/hhqs_data/face_db/faces.db` | `--db` |
| `face_db_web` | — | `--db` 显式传入 |
| ROS2 节点 | `/data/hhqs_data/face_db/faces.db` | parameter `db_path` |

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

## 11. 日志与退出

- 每 2 秒打印一次平滑 FPS 与耗时拆解：

  ```
  [INFO] ~2.9 FPS | last frame: 341.3 ms (det=215.6, rec=125.6)
  ```

- 启动时打印生效配置，便于确认前端与阈值：

  ```
  [INFO] recognition: threshold=0.50 detect-every-n=2 input-size=640 align=on(strict) min-face=80px frame-cap=24 fps
  [INFO] cohort norm: on z>=3.00 min-cohort=3 (external cohort=0)
  ```

- 窗口模式按 `q` / `ESC` 退出；终端模式 `Ctrl+C` 退出
- 若 `cv::namedWindow` 抛异常（无 GUI 环境），自动降级为 headless 并打印 `[WARN]`

---

## 12. 相关文档

- [核心算法库模块说明](core.md)
- [Web 后台模块使用文档](web.md)
- [接口、协议与配置](../protocol/)
- [常见问题排查](../FAQ/troubleshooting.md)
