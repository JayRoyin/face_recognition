# 协议 / 接口 / 配置文档（protocol）

本目录收录 Face Recognition Node 的全部**对外契约**：

| 文档 | 内容 |
|---|---|
| [ros_interfaces.md](ros_interfaces.md) | ROS Topic / Service / Message 定义（ROS1 与 ROS2） |
| [web_api.md](web_api.md) | Web 后台 HTTP API 与 MJPEG 推流接口 |
| [database_schema.md](database_schema.md) | SQLite 数据库 schema 与数据字典 |
| [config.md](config.md) | 全项目配置项、参数默认值与路径约定 |

---

## 接口总览

```
┌────────────────────────────────────────────────────────────────┐
│  ROS 接口（ROS1 / ROS2 语义一致）                                │
│   订阅  /image_raw                    sensor_msgs/Image        │
│   发布  /face/recognition_result      FaceResult               │
│   发布  /face/annotated               sensor_msgs/Image (ROS2) │
│   服务  /face_db/{add,remove,list,clear}                        │
├────────────────────────────────────────────────────────────────┤
│  HTTP 接口                                                      │
│   face_db_web        :8080                                      │
│     /                       管理页面（宫格：导入/编辑/待确认/库）│
│     /api/config             服务端启用的可选行为                 │
│     /api/faces              列表（按 uid 升序）                  │
│     /api/image/<id>         原图                                 │
│     /api/faces/pending      待人工确认的入库请求                 │
│     /api/pending/image/<t>  待确认图片预览                       │
│     /api/faces/add | update | add-template                      │
│     /api/faces/remove | clear                                   │
│     /api/faces/import-archive   压缩包批量导入                   │
│     /api/faces/import-batch     宫格图片批量导入                 │
│     /api/faces/resolve          提交人工决定（唯一写入入口）      │
│   face_stream_server :8090   /  /stream  /latest.jpg  /healthz  │
├────────────────────────────────────────────────────────────────┤
│  存储接口                                                       │
│   SQLite  table: faces / face_templates / metadata              │
│   （Web / ROS / standalone 三方共享同一文件）                    │
└────────────────────────────────────────────────────────────────┘
```

## 默认端口与路径

| 项 | 默认值 |
|---|---|
| Web 后台 HTTP 端口 | `8080` |
| MJPEG 推流端口 | `8090` |
| SQLite 数据库 | `/data/hhqs_data/face_db/faces.db` |
| 人脸缩略图目录 | `/data/hhqs_data/face_db/faces` |
| 输入图像话题 | `/image_raw` |
| 识别结果话题 | `/face/recognition_result` |
| 标注图话题 | `/face/annotated` |
