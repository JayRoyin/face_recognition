# Web 人脸库后台模块使用文档（face_db_web）

基于 **libmicrohttpd** 的轻量 HTTP 服务，提供人脸库的可视化 **增 / 删 / 查 / 清空**，
并可在录入时自动提取 512 维 ArcFace 特征写入 SQLite。

- 源码：`src/face_db_web/`
- 依赖：core + OpenCV + libmicrohttpd + pthread（**无 ROS 依赖**）
- 产物：`install/bin/face_db_web`

---

## 1. 编译

```bash
./build.sh WEB       # = vendored SQLite3 + CORE + Web 后台
```

构建前需安装 `libmicrohttpd-dev`：

```bash
sudo apt-get install -y libmicrohttpd-dev
```

---

## 2. 启动

```bash
# 先确保人脸图片存档目录存在
mkdir -p /tmp/face_db/faces

./install/bin/face_db_web \
    --port 8080 \
    --db /tmp/face_db/faces.db \
    --faces-dir /tmp/face_db/faces
```

### 2.1 命令行参数

| 参数 | 默认值 | 是否必需 | 说明 |
|---|---|---|---|
| `--port <n>` | `8080` | 否 | HTTP 监听端口 |
| `--db <path>` | — | **是** | SQLite 数据库路径 |
| `--faces-dir <path>` | — | **是** | 人脸缩略图存放目录 |
| `--detection-model <path>` | — | 否 | 传入后启用录入时自动检测人脸 |
| `--recognition-model <path>` | — | 否 | 传入后启用录入时自动提取 embedding（需同时传 `--detection-model`） |
| `--help` / `-h` | — | 否 | 打印帮助 |

### 2.2 环境变量回退

未通过命令行传模型路径时，会读取环境变量：

```bash
export FACE_DETECTION_MODEL=$(pwd)/models/det_10g.onnx
export FACE_RECOGNITION_MODEL=$(pwd)/models/w600k_r50.onnx
./install/bin/face_db_web --port 8080 --db ... --faces-dir ...
```

`scripts/setup_env.sh` 已自动完成上述导出（当 `models/` 存在标准模型时）。

### 2.3 启用录入时自动提取 embedding（推荐）

```bash
./install/bin/face_db_web \
    --port 8080 \
    --db /tmp/face_db/faces.db \
    --faces-dir /tmp/face_db/faces \
    --detection-model "$(pwd)/models/det_10g.onnx" \
    --recognition-model "$(pwd)/models/w600k_r50.onnx"
```

启动日志会显示：

```
Face DB Web running on http://localhost:8080
Database:        /tmp/face_db/faces.db
Faces dir:       /tmp/face_db/faces
Embedding extraction: ENABLED
Press Ctrl+C to stop
```

若未加载模型，则显示：

```
Embedding extraction: DISABLED (records will be stored with NULL embedding)
```

检测不到人脸的图片**仍可保存**（`has_embedding=false`），留待后续 `backfill` 补齐。

---

## 3. 使用教程

浏览器打开 `http://<服务器IP>:8080`，页面分为两块：

```
┌─────────────────────────────────────────────┐
│ Face Database Management                    │
├─────────────────────────────────────────────┤
│ [Add Face]                                  │
│  Name       [_______________]               │
│  Title      [_______________]               │
│  Scene      [default____________]           │
│  Map        [unknown____________]           │
│  Image URL  [http://...]      [Load]        │
│  Or Upload  [选择文件]                      │
│  [预览缩略图]                               │
│  [Add Face 按钮]                            │
├─────────────────────────────────────────────┤
│ Face List (N)            [Refresh]          │
│ ┌──┬──┬──┬──┬──┬──┬──┐                      │
│ │图片│姓名│职│场│位置│ID│删除│               │
│ └──┴──┴──┴──┴──┴──┴──┘                      │
└─────────────────────────────────────────────┘
```

### 入库一张人脸的完整流程

1. **填写基本信息**
   - **Name**（必填）：姓名
   - **Title**：职位 / 头衔（可选）
   - **Scene**：业务场景，如 `office` / `gate` / `factory`（默认 `default`）
   - **Map Location**：物理位置，如 `5F-A区`（默认 `unknown`）
2. **准备人脸图片**（二选一）
   - **URL 方式**：填入 `Image URL`，触发 `onchange` 后自动预览
   - **本地上传**：点击 `Or Upload` 选择本地图片
3. **预览确认**：能清楚看到一张正面人脸最佳
4. **点击 `Add Face`**：弹出 `Face added successfully`
5. **列表自动追加新条目**（可点 `Refresh` 手动刷新）

> **图片要求**：建议清晰、正面、单人脸、≥ 200×200 的 JPG/PNG。
> 系统自动检测人脸、提取 512 维 ArcFace 特征并写入 SQLite。

### 删除

- **删除单条**：列表中点击对应行的 `Delete` 按钮
- **清空全部**：页面提供的 JS 函数 `clearAll()`（可在浏览器控制台执行），
  或直接调用 HTTP API，见 [../protocol/web_api.md](../protocol/web_api.md)

---

## 4. HTTP API 速查

| 方法 | 路径 | 用途 |
|---|---|---|
| GET | `/` | Web UI 页面 |
| GET | `/api/faces` | 列出所有人脸（JSON） |
| GET | `/api/image/<id>` | 取某条人脸的原图 |
| POST | `/api/faces/add` | 新增一条人脸（form-urlencoded） |
| POST | `/api/faces/remove` | 删除一条人脸（按 id） |
| POST | `/api/faces/clear` | 清空库 |

```bash
# 列出所有人脸
curl -s http://localhost:8080/api/faces | python3 -m json.tool

# 新增（image_data 为去掉 data:image/jpeg;base64, 前缀的 base64 字符串）
curl -X POST http://localhost:8080/api/faces/add \
     --data-urlencode "name=张三" \
     --data-urlencode "title=工程师" \
     --data-urlencode "scene=office" \
     --data-urlencode "map_location=5F-A区" \
     --data-urlencode "image_data=<BASE64_STRING>"
# → {"success":true,"id":"...","has_embedding":true,"message":"Face added with embedding"}

# 删除
curl -X POST http://localhost:8080/api/faces/remove --data-urlencode "id=<UUID>"

# 清空
curl -X POST http://localhost:8080/api/faces/clear
```

完整字段、状态码与错误语义见 [../protocol/web_api.md](../protocol/web_api.md)。

---

## 5. 与 standalone CLI 的等价入口

`face_recognition_app web` 子命令会**自动转发** `--db` / `--faces-dir` /
`--detection-model` / `--recognition-model` 给 `face_db_web`：

```bash
./src/face_recognition_standalone/build/face_recognition_app web --port 8080
# 等价于：
#   ./install/bin/face_db_web --port 8080 \
#       --db /tmp/face_db/faces.db --faces-dir /tmp/face_db/faces \
#       --detection-model models/det_10g.onnx \
#       --recognition-model models/w600k_r50.onnx
```

若 `face_db_web` 不在默认位置，可用 `--web-binary /abs/path` 指定。

---

## 6. 安全性说明

当前实现**未内置**认证 / CSRF / HTTPS（见 `docs/TODO.md` 待办项）。因此：

- 仅建议部署在**受信任内网**
- 需要对外暴露时，请前置反向代理（Nginx）并启用 Basic Auth / TLS
- 请求体大小已做限制，前端 DOM 写入已做转义处理
- 服务使用**短事务**写库，与 ROS / standalone 并发时影响一般可接受

---

## 7. 相关文档

- [Web HTTP API 文档](../protocol/web_api.md)
- [数据库 schema](../protocol/database_schema.md)
- [非 ROS 实时识别模块](standalone.md)
- [常见问题排查](../FAQ/troubleshooting.md)
