# HTTP 接口文档（web_api）

项目提供**两个独立的 HTTP 服务**：

| 服务 | 默认端口 | 用途 | 实现 |
|---|---|---|---|
| `face_db_web` | `8080` | 人脸库管理 UI + REST API | libmicrohttpd |
| `face_stream_server` | `8090` | 标注图 MJPEG 推流 | libmicrohttpd |

两者均只支持 **GET / POST**，统一返回 `Access-Control-Allow-Origin: *`。

---

# 一、face_db_web（人脸库管理 API）

## 1. 路由表

| 方法 | 路径 | Content-Type | 说明 |
|---|---|---|---|
| GET | `/` | `text/html; charset=utf-8` | Web UI 页面 |
| GET | `/api/faces` | `application/json; charset=utf-8` | 列出全部人脸 |
| GET | `/api/image/<id>` | `image/jpeg` | 取某条人脸的原图 |
| POST | `/api/faces/add` | `application/json; charset=utf-8` | 新增一条人脸 |
| POST | `/api/faces/remove` | `application/json; charset=utf-8` | 删除一条人脸 |
| POST | `/api/faces/clear` | `application/json; charset=utf-8` | 清空人脸库 |

**路由匹配规则**

- 完全匹配优先
- 以 `/` 结尾且长度 > 1 的路径作为**前缀路由**（`/api/image/` → `/api/image/<id>`）
- 未命中 → `404` + `<h1>404 Not Found</h1>`

**POST 请求体**

- 格式：`application/x-www-form-urlencoded`
- 分隔符兼容 `&` 与 `\n`（可选 `\r`）
- 字段值需 URL 编码；`image_data` 为 base64（需 URL 编码）
- 大小上限：**16 MiB**，超限直接断开连接

---

## 2. GET /api/faces

```bash
curl -s http://localhost:8080/api/faces | python3 -m json.tool
```

响应：

```json
{
  "count": 2,
  "faces": [
    {
      "id": "550e8400-e29b-41d4-a716-446655440000",
      "name": "张三",
      "title": "工程师",
      "scene": "office",
      "map_location": "5F-A区",
      "image_path": "/tmp/face_db/faces/550e8400-....jpg"
    }
  ]
}
```

| 字段 | 类型 | 说明 |
|---|---|---|
| `count` | number | 记录总数 |
| `faces[].id` | string | UUID |
| `faces[].name` / `title` / `scene` / `map_location` | string | 业务字段（JSON 已转义 `" \ \n \r \t`） |
| `faces[].image_path` | string | 服务端缩略图绝对路径（可能为空串） |

> **不含 `embedding`**：特征向量不会通过 HTTP 暴露。

---

## 3. GET /api/image/&lt;id&gt;

```bash
curl -o alice.jpg http://localhost:8080/api/image/550e8400-e29b-41d4-a716-446655440000
```

| 情况 | 状态码 | 响应体 |
|---|---|---|
| 正常 | `200` | JPEG 二进制（`image/jpeg`） |
| ID 不存在或 `image_path` 为空 | `404` | `Image not found` |
| 文件无法读取 | `404` | `Cannot read image` |
| URL 末尾无 ID | `404` | `Not Found` |

> 返回的 Content-Type 固定为 `image/jpeg`，即使原文件是 PNG。

---

## 4. POST /api/faces/add

**请求字段（form-urlencoded）**

| 字段 | 必需 | 说明 |
|---|---|---|
| `name` | **是** | 姓名 |
| `title` | 否 | 职位 |
| `scene` | 否 | 默认 `default` |
| `map_location` | 否 | 默认 `unknown` |
| `image_data` | 否 | base64 图片（**不含** `data:image/jpeg;base64,` 前缀） |

**响应**

| 情况 | 状态码 | 响应体 |
|---|---|---|
| 成功 | `200` | `{"success":true,"id":"<UUID>","has_embedding":true,"message":"Face added with embedding"}` |
| 成功但无特征（仅在 `--allow-no-embedding` 下可能） | `200` | `{"success":true,"id":"<UUID>","has_embedding":false,"message":"Face added (without embedding — no face detected or models not loaded)"}` |
| **拒绝：无法提取特征** | **`409`** | `{"success":false,"id":"","has_embedding":false,"message":"Refused: no embedding could be extracted (...)"}` |
| `name` 为空 | `400` | `{"success":false,"message":"Name is required"}` |
| 入库失败 | `200` | `{"success":false,"id":"","has_embedding":false,"message":"Failed to add face"}` |

> **关于 409**：`embedding = NULL` 的记录**永远无法被匹配**，所以服务端默认拒绝写入
> （避免产生"看着录进去了、其实永远认不出"的记录）。要恢复录入，请让服务端能加载
> 模型（命令行 / `FACE_DETECTION_MODEL` 等环境变量 / `./models` / `<exe>/../../models`
> 自动发现），或显式传 `--allow-no-embedding`。详见
> [../FAQ/troubleshooting.md](../FAQ/troubleshooting.md) 的 Q21。

**示例**

```bash
curl -X POST http://localhost:8080/api/faces/add \
     --data-urlencode "name=张三" \
     --data-urlencode "title=工程师" \
     --data-urlencode "scene=office" \
     --data-urlencode "map_location=5F-A区" \
     --data-urlencode "image_data=<BASE64_STRING>"
```

`has_embedding` 表示该记录是否携带可用于匹配的 512 维 ArcFace 特征；
未传图片、未加载模型或未检测到人脸时为 `false`（**不会阻断保存**）。

---

## 5. POST /api/faces/remove

| 字段 | 必需 | 说明 |
|---|---|---|
| `id` | 是 | 要删除的记录 UUID |

```bash
curl -X POST http://localhost:8080/api/faces/remove --data-urlencode "id=<UUID>"
```

响应（始终 `200`）：

```json
{"success":true,"message":"Face removed"}
{"success":false,"message":"Face not found"}
```

---

## 6. POST /api/faces/clear

无请求字段。

```bash
curl -X POST http://localhost:8080/api/faces/clear
```

响应（始终 `200`）：

```json
{"success":true,"deleted_count":12}
```

> 会同时删除数据库记录与对应的缩略图文件。

---

## 7. 前端行为说明

内置页面（`templates/index.html`，可用环境变量 `FACE_DB_WEB_TEMPLATES` 覆盖目录，
未找到时回退到内嵌 HTML）：

- `loadFaces()` 页面加载即调用 `/api/faces` 渲染表格
- `loadImage(url)` 通过 `<canvas>` 转 base64（`crossOrigin="anonymous"`，跨域图片需服务端允许）
- `clearAll()` 提供确认弹窗后调用 `/api/faces/clear`

---

# 二、face_stream_server（MJPEG 推流）

## 8. 路由表

| 方法 | 路径 | 状态码 | Content-Type | 说明 |
|---|---|---|---|---|
| GET | `/` | `200` | `text/html; charset=utf-8` | 自动刷新页面，内嵌 `/stream` |
| GET | `/stream` | `200` | `multipart/x-mixed-replace; boundary=frame` | MJPEG 流 |
| GET | `/latest.jpg` | `200` / `503` | `image/jpeg` / `text/plain` | 最新单帧 |
| GET | `/healthz` | `200` / `503` | `text/plain` | 健康检查 |
| 其他方法 | `*` | `405` | `text/plain` | `405 Method Not Allowed` |
| 其他路径 | `GET` | `404` | `text/plain` | `404 Not Found` |

响应头统一包含 `Cache-Control: no-store` 与 `Access-Control-Allow-Origin: *`。

## 9. GET /healthz

```bash
curl -i http://localhost:8090/healthz
```

| 情况 | 状态码 | 响应体 |
|---|---|---|
| 已收到至少一帧 | `200` | `ok\n` |
| 尚未收到帧 | `503` | `no_frame_yet\n` |

## 10. GET /latest.jpg

```bash
curl -o frame.jpg http://localhost:8090/latest.jpg
file frame.jpg
# → frame.jpg: JPEG image data, ..., 640x480, components 3
```

无帧时返回 `503` + `no frame yet`。

## 11. GET /stream

标准 MJPEG：

```
Content-Type: multipart/x-mixed-replace; boundary=frame

--frame
Content-Type: image/jpeg
Content-Length: <N>

<JPEG bytes>
--frame
...
```

**实现要点**

- 服务端维护"最新一帧 JPEG + 序号 `frame_seq`"，**只保留最新帧**，天然丢帧不背压
- 流式回调**从不阻塞**：无新帧时返回 0，由 MHD 内部轮询线程再次调用
- 同一 `frame_seq` 不会被重复发送
- 订阅使用 `rmw_qos_profile_sensor_data`（`depth=1`，BEST_EFFORT），避免堆积旧帧

## 12. 浏览器访问

| URL | 用途 |
|---|---|
| `http://<host>:8090/` | 推荐的查看入口（带导航栏） |
| `http://<host>:8090/stream` | 直接嵌入 `<img src>` 使用 |

---

## 13. 错误码汇总

| 状态码 | 含义 |
|---|---|
| `200` | 成功 |
| `400` | 请求参数缺失（仅 `add` 缺 `name`） |
| `404` | 路由或资源不存在 |
| `405` | 仅 `face_stream_server`，非 GET 方法 |
| `503` | 仅 `face_stream_server`，尚无可用帧 |

---

## 14. 安全提示

`face_db_web` **未内置**认证 / CSRF / HTTPS，仅建议部署在受信任内网；
对外暴露请前置反向代理并启用 TLS 与访问控制。详见
[../module/web.md](../module/web.md#6-安全性说明)。

---

## 15. 相关文档

- [Web 后台模块使用文档](../module/web.md)
- [ROS2 节点模块使用文档](../module/ros2.md)
- [数据库 schema](database_schema.md)
