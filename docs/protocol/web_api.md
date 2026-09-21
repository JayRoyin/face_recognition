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
| POST | `/api/faces/update` | `application/json; charset=utf-8` | 编辑一条人脸（可选重建特征） |
| POST | `/api/faces/add-template` | `application/json; charset=utf-8` | 追加为同一人的额外模板（多枪） |
| GET | `/api/config` | `application/json; charset=utf-8` | 服务端启用了哪些可选行为（前端提示用） |
| GET | `/api/faces/pending` | `application/json; charset=utf-8` | 待人工确认的入库请求 |
| GET | `/api/pending/image/<token>` | `image/png` / `image/jpeg` | 待确认图片的预览 |
| POST | `/api/faces/resolve` | `application/json; charset=utf-8` | **提交人工决定**（唯一写入高相似度人脸的入口） |
| POST | `/api/faces/import-archive` | `multipart/form-data` 或二进制 | **批量导入：上传压缩包** |
| POST | `/api/faces/import-batch` | `application/json` | **批量导入：宫格编辑后的图片列表** |

**路由匹配规则**

- 完全匹配优先
- 以 `/` 结尾且长度 > 1 的路径作为**前缀路由**（`/api/image/` → `/api/image/<id>`）
- 未命中 → `404` + `<h1>404 Not Found</h1>`

**POST 请求体**

- `add` / `remove` / `clear` / `update`：`application/x-www-form-urlencoded`
- 分隔符兼容 `&` 与 `\n`（可选 `\r`）
- 字段值需 URL 编码；`image_data` 为 base64（需 URL 编码）
- `import-batch`：`application/json`
- `import-archive`：`multipart/form-data`（或直接 POST 二进制压缩包）
- 大小上限：由 `--max-upload-mb` 控制，默认 **512 MiB**，超限断开连接

**批量导入的命名规范**

压缩包内与宫格导入的图片均按文件名自动填充关键字：

| 文件名 | title | name |
|---|---|---|
| `工程师_张三.png` | `工程师` | `张三` |
| `资深_工程师_张三.jpg` | `资深_工程师` | `张三`（按**最后一个**下划线切分） |
| `01工程师_张三.png` | `工程师` | `张三` |
| `张三.png` | （空） | `张三` |

- 图片格式仅接受 **png / jpg / jpeg**，其余条目在压缩包导入中被忽略并计入 `skipped`
- 压缩包支持 `.zip`（store/deflate，含 Zip64）、`.tar`、`.tar.gz` / `.tgz`
- 自动跳过隐藏文件、`__MACOSX/`、`._*`（AppleDouble）等杂项

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
      "image_path": "/data/hhqs_data/face_db/faces/550e8400-....jpg"
    }
  ]
}
```

| 字段 | 类型 | 说明 |
|---|---|---|
| `count` | number | 记录总数 |
| `faces[].id` | string | UUID |
| `faces[].name` / `title` / `scene` / `map_location` | string | 业务字段（JSON 已转义 `" \ \n \r \t`） |
| `faces[].gender` | string | `unknown` / `male` / `female` |
| `faces[].image_path` | string | 服务端缩略图绝对路径（可能为空串） |

> **不含 `embedding`**：特征向量不会通过 HTTP 暴露。
> **不含 `uid`**：自增序号是数据库内部排序键，接口不返回（见
> [database_schema.md](database_schema.md)）。列表默认按 `uid` 升序返回，即入库顺序。

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
| `gender` | 否 | `unknown` / `male` / `female`，默认 `unknown`（写入数据库字段） |
| `image_data` | 否 | base64 图片（**不含** `data:image/jpeg;base64,` 前缀） |

> 同一张图片已入库时返回 **409** + `{"duplicate":true,"id":"<已存在记录的UUID>"}`，
> 不会重复写入（判定依据见 [4.4 重复校验](#44-重复性校验)）。

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

## 4.1 POST /api/faces/update

编辑一条已有记录；换照片时**同步重建特征**（保留旧特征会导致记录继续匹配旧人脸）。

**请求字段（form-urlencoded）**

| 字段 | 必需 | 说明 |
|---|---|---|
| `id` | **是** | 记录 UUID |
| `name` / `title` / `scene` / `map_location` | 否 | 留空表示**保持原值** |
| `gender` | 否 | `unknown` / `male` / `female`（写入数据库字段） |
| `image_data` | 否 | base64 新照片；提供时重做 检测→提特征→更新 embedding，并刷新 `image_hash` |

**响应**

| 情况 | 状态码 | 说明 |
|---|---|---|
| 成功 | `200` | `{"success":true,"id":"...","has_embedding":true,"message":"Face updated"}` |
| `id` 缺失 | `400` | `id is required` |
| 记录不存在 | `404` | `Face not found` |
| 新照片提不出特征 | **`409`** | 记录保持不变（仅在默认拒绝模式下） |

---

## 4.2 POST /api/faces/import-archive（压缩包批量导入）

一次上传整个压缩包，服务端解压后逐张执行 `解码 → 检测 → 提特征 → 入库`，
并返回**逐张结果**。单张失败不会中断整批。

**请求**

- 推荐 `multipart/form-data`：字段 `archive`（压缩包文件）、可选 `scene`、
  `map_location`、`gender`（默认 `unknown`）、`similar_threshold`（默认 `0.70`）、
  `on_similar`（`skip` 时把"疑似同一人"也跳过）
- 也接受直接 POST 二进制（格式按文件头识别，无需文件名）

```bash
curl -X POST http://localhost:8080/api/faces/import-archive \
     -F "archive=@公司人脸库.zip" \
     -F "scene=office" \
     -F "map_location=公司总部"
```

**响应**

```json
{
  "success": true,
  "archive": "公司人脸库.zip",
  "total": 30, "succeeded": 30, "failed": 0, "skipped": 2,
  "results": [
    {"file": "01张三.png", "name": "张三", "title": "",
     "success": true, "id": "<UUID>", "has_embedding": true, "reason": ""}
  ]
}
```

| 字段 | 说明 |
|---|---|
| `skipped` | 非 png/jpg、隐藏文件、`__MACOSX` 等被忽略的条目数 |
| `duplicates` | 被判定为重复而**跳过**的条目数 |
| `results[].duplicate` | true 表示该张被当作重复跳过 |
| `results[].existing_name` | 与之重复的记录姓名 |
| `results[].similar_name` / `similarity` | 疑似同一人（不同照片）及其余弦相似度 |
| `results[].reason` | 失败 / 跳过原因（`no face detected` / `too small (…< 80 px)` / `库里已存在…` 等） |
| `success` | `failed == 0 && succeeded > 0` |

| 情况 | 状态码 |
|---|---|
| 压缩包无法解析 / 无有效文件 | `400` |
| 模型未加载且未开 `--allow-no-embedding` | `503` |

> 部分失败仍是 `200`：看 `results[]` 逐条处理。检测环节带一次低阈值（0.1）补救
> 重试——证件照里"人脸占满整图"时 SCRFD 常给出偏低的分数，补救后仍需通过
> 质量闸门（最小脸 80 px、必须对齐）才会入库。

---

## 4.3 POST /api/faces/import-batch（宫格图片批量导入）

配合前端"拖入图片 → 宫格编辑 → 保存"流程：图片在前端已完成命名预填与人工修订，
一次性提交到此接口完成 `提特征 + 入库`。

**请求（`application/json`）**

```json
{
  "scene": "default",
  "map_location": "unknown",
  "gender": "unknown",
  "similar_threshold": 0.70,
  "skip_similar": false,
  "items": [
    {"file": "01张三.png", "name": "张三", "title": "",
     "scene": "office", "map_location": "3F", "gender": "male",
     "image_data": "<BASE64>"}
  ]
}
```

- `name` 为空时按 `file` 的文件名自动解析（命名规范见上文）
- `scene` / `map_location` / `gender` 为空时回落到顶层默认值，再回落
  `default` / `unknown` / `unknown`
- `image_data` 允许带 `data:image/png;base64,` 前缀
- 条目上限 `2000`（`kMaxBatchItems`）

**响应**：与 `import-archive` 相同（`archive` 字段为空串）。

---

## 4.4 重复性校验

导入（压缩包 / 宫格 / 单张）都会先做重复校验，**依据由强到弱三层**：

| 层级 | 依据 | 比对范围 | 默认动作 |
|---|---|---|---|
| 1 | **内容哈希** SHA-256（图片字节） | 本次请求内 + 库内 `image_hash` 列 | **跳过** |
| 2 | **文件名** basename | 仅本次请求内 | **跳过** |
| 3 | **人脸特征** 512 维余弦 ≥ `similar_threshold`（默认 0.70） | 库内全部记录 | **入库 + 提示** |

判定说明：

- **为什么用内容哈希而不是文件名**：文件名可以随便改，同一张照片重新导出后
  字节也可能变；而像素内容相同就一定"是不是同一张"。哈希存进
  `faces.image_hash` 并建索引，比对是 O(1) 查表，不随库大小变慢。
- **为什么文件名只在批内比对**：库内同名是合法的（同一个人的多张照片），
  不能据此判重；但在一次导入里出现两个同名文件，几乎一定是重复拖入。
- **为什么特征相似默认不跳过**：多模板（一个人的多张照片）是受支持的工作流，
  两张三角度不同的照片本就应该都入库。因此只给出提示（"疑似与库中 X 同一人，
  相似度 0.998"），由操作者决定；需要严格去重用 `skip_similar=true` /
  `on_similar=skip`。
- 被跳过的条目计入 `duplicates`，**不计入 `failed`**：重复不是错误。
- 单张 `/api/faces/add` 命中内容哈希时返回 **409** + `duplicate:true`。

---

## 4.5 高相似度：人工确认流程

**相似度 ≥ `high_similarity`（默认 0.80）时，绝不自动写入。** 该次上传被放入
"待确认队列"，只有 `/api/faces/resolve` 收到人工决定后才会落库。

```
上传图片
  │
  ├─ 内容哈希已存在 ─────────────► 直接跳过（完全重复）
  │
  ├─ 相似度 ≥ 0.80 ─┬─ 规则要求确认 ─► 挂起（返回 token，未写库）
  │                 └─ 规则允许放行 ─► 自动入库（日志会记录原因）
  │
  └─ 相似度 < 0.80 ─────────────► 直接入库
                                        │
  挂起 ◄───────────────────────────────┘
    │  GET  /api/faces/pending        列出待确认项（含相似度、是否同场景）
    │  GET  /api/pending/image/<token> 预览待确认图片
    │  POST /api/faces/resolve        人工决定：enroll / replace / skip
    ▼
  写库（仅此处）
```

### 三种处理方式

| action | 行为 |
|---|---|
| `enroll` | **强制入库**：新增一条独立记录（不影响库中原有记录） |
| `template` | **追加为模板**：保留原记录与原照片，把这张作为该人的**额外模板（多枪）**，识别时取最高分 |
| `replace` | **重入库**：用新照片与新特征覆盖匹配到的那条记录（记录数不变） |
| `skip` | 丢弃本次上传（默认选项：不选就不写） |

> 同一人的第二张照片建议用 `template` 而不是 `replace`：`replace` 会把原特征
> 覆盖掉，只留下最新一张；`template` 保留多枪，对姿态/光照变化更鲁棒。

### 接口

**GET /api/faces/pending**

```json
{"count":1,"pending":[{
  "token":"7c451afc90d1…","file":"01张三.png","name":"01张三","scene":"office",
  "match_id":"<UUID>","match_name":"张三","match_scene":"office",
  "same_scene":true,"similarity":0.997,"allow_replace":true,"created_at":1768…}]}
```

**POST /api/faces/resolve**

```bash
curl -X POST http://localhost:8080/api/faces/resolve \
     -H 'Content-Type: application/json' \
     -d '{"decisions":[{"token":"7c451afc90d1…","action":"replace"}]}'
```

```json
{"results":[{"token":"…","action":"replace","success":true,
             "id":"<UUID>","message":"已替换库中该记录的特征与照片"}],
 "enrolled":0,"replaced":1,"skipped":0,"failed":0}
```

- `take()` 语义：一个 token 只能消费一次，重复提交只会得到
  `token 不存在或已处理`，**不可能写出第二条记录**
- 队列在内存中（默认上限 500 条、TTL 30 分钟）：待确认项是"决策"而不是数据，
  重启后重新导入即可，避免在共享库里留下半成品行

### 单张新增

`/api/faces/add` 命中高相似度时返回 **409**：

```json
{"success":false,"needs_confirm":true,"token":"…","similar_name":"张三",
 "match_id":"<UUID>","same_scene":true,"similarity":0.997,
 "message":"与库中「张三」相似度 0.998（同场景），等待人工确认后才会入库"}
```

前端会把该 token 放进「待确认入库」宫格，由用户选择处理方式。

---

## 4.6 入库模式配置

配置文件（`--enroll-policy <file>` 或环境变量 `FACE_ENROLL_POLICY`），
`--print-enroll-policy` 可打印带注释的模板：

```json
{
  "high_similarity": 0.80,
  "require_confirmation": true,
  "confirm_on_same_scene": true,
  "confirm_on_cross_scene": true,
  "allow_replace": true,
  "scene_rules": {
    "office":  {"threshold": 0.80, "confirm_required": true},
    "visitor": {"threshold": 0.85, "confirm_required": false}
  }
}
```

| 配置项 | 含义 |
|---|---|
| `high_similarity` | 触发确认的相似度阈值，默认 **0.80** |
| `require_confirmation` | 总开关；false = 不拦截任何入库（启动时打印警告） |
| `confirm_on_same_scene` | 同场景是否必须确认（**默认必须**） |
| `confirm_on_cross_scene` | 跨场景是否必须确认（默认必须；可放宽为非同一场景不提示） |
| `allow_replace` | 是否允许"替换库中记录" |
| `scene_rules[<scene>]` | 按场景覆盖：`threshold` / `confirm_required` / `allow_auto_cross_scene` / `allow_replace` |

命令行快捷开关（优先级高于文件）：

| 参数 | 效果 |
|---|---|
| `--high-similarity F` | 覆盖阈值 |
| `--auto-cross-scene` | 跨场景直接入库不提示（**同场景仍然提示**） |
| `--no-confirm` | 关闭全部人工确认（危险，启动即告警） |

**判定顺序**：`require_confirmation=false` → 不确认；否则场景规则里有
`confirm_required` 就按它；再看 `allow_auto_cross_scene`；最后按
同/跨场景的全局开关。默认配置下**同场景与跨场景都需要人工确认**，
只有显式配置才会放行，且放行原因会写进 `results[].reason`。

---

## 4.7 追加模板（多枪）

`POST /api/faces/add-template`：给**已存在**的人再喂一张照片，作为额外模板。

| 字段 | 必需 | 说明 |
|---|---|---|
| `id` | 是 | 目标记录 UUID |
| `image_data` | 是 | base64 照片 |

```bash
curl -X POST http://localhost:8080/api/faces/add-template \
     --data-urlencode "id=<UUID>" --data-urlencode "image_data=<BASE64>"
```

```json
{"success":true,"id":"<UUID>","templates":2,"message":"已追加为该人的额外模板（多枪）"}
```

- 同样走 `make_embedding()` 质量闸门：提不出特征时返回 **409**，不会写入半成品模板
- `templates` = 该人当前模板总数（含主模板），匹配时取所有模板的最高分
- 前端入口：人脸卡片上的「加模板」按钮；待确认卡片里的「追加为模板」选项

---

## 4.8 性别自动识别（可选）

用 `models/genderage.onnx` 在导入时**预填**性别，仅当调用方填的是
`unknown` 时才生效，人工可随时修改。

```bash
face_db_web ... --auto-gender                # 开启
              --gender-model <path>          # 默认自动发现 models/genderage.onnx
              --gender-male-index 0|1        # 默认 1，结果整体相反时改 0
```

`GET /api/config` 会返回 `auto_gender`，前端据此提示"性别自动识别已开启"。

**实现要点（踩过的坑）**：该模型的图内部自带 `Sub(127.5)` + `Mul(1/128)`
（两个常量在 .onnx initializer 里紧邻），因此输入必须是**原始 0~255 像素**。
若按常见做法再归一化一次，等于双重归一化，激活值塌缩，输出会变成对任何人
都几乎相同的 `[-v, +v, age]`，表现为"所有人都判成同一类"却不报任何错。
另外它训练用的是 **5 点对齐人脸**，喂松散 bbox 裁剪同样会退化，所以这里复用了
识别器的对齐结果（`FaceRecognizer::alignedFace`）。

**准确率**：在 30 张真实证件照上约 83%（明显偏向女性名/男性名的样本均判对，
中性名偶有误判）。因此它只作为**预填建议**——库里的值始终可编辑，且不会覆盖
任何显式传入的性别。

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
未找到时使用编译期内嵌副本）分为两个标签页：

**人脸库**（宫格）：

- **搜索**：按姓名 / 职位关键字实时过滤
- **筛选**：场景下拉（从库内数据自动生成）+ 性别下拉
- **分页**：默认渲染 60 张，「加载更多」每次 +60（避免几百张时一次性渲染卡顿）
- **多选批量**：卡片勾选 → 批量设场景 / 批量设性别 / 批量删除（逐个调
  `/api/faces/update`、`/api/faces/remove`）
- **加模板**：卡片上的「加模板」按钮，选一张该人的照片调
  `/api/faces/add-template`，不覆盖原特征
- 单条编辑（调 `/api/faces/update`）/ 删除

**归档编辑**：

- 勾选需要入库的图片，点「按勾选项重建入库」；**未勾选时按钮置灰**，
  已勾选数量显示在标题（不在按钮上，避免按钮宽度变化导致误点）
- 提交按 **20 张一片**分片进行，按钮实时显示 `已处理 n/N`：
  整批一次提交时任何网络抖动都会让整批白跑，分片后已完成的不会重来
- 性别留空时，若服务端开了 `--auto-gender` 会用模型预填并回填到卡片

**批量导入**：

- *压缩包导入*：拖拽或选择 `.zip/.tar/.tar.gz/.tgz` → `POST /api/faces/import-archive`
  → 展示逐张结果报告
- *图片批量导入*：一次拖入大量 png/jpg → 前端按命名规范自动填入 title/name →
  宫格中逐张可改（姓名/职位/场景/位置/性别，支持批量填充、按文件名重解析、
  删除、勾选）→ 「保存入库并重建特征」调 `POST /api/faces/import-batch`
- *待确认入库*：导入结果里相似度达阈值的图片会进入该宫格，**左右并排**
  显示「本次上传 vs 库中匹配」并标注相似度与同/跨场景，每张三选一
  （入库 / 替换 / 跳过，默认跳过），确认后调 `POST /api/faces/resolve`；
  未经选择不会写入任何数据
- 前端会把图片压缩到最长边 1280 的 JPEG（质量 0.92）再上传，减小请求体积

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
| `400` | 请求参数缺失（`add` 缺 `name`、`update` 缺 `id`、压缩包/JSON 无法解析） |
| `404` | 路由或资源不存在（含 `update` 的未知 `id`、失效的 `token`） |
| `405` | 仅 `face_stream_server`，非 GET 方法 |
| `409` | 拒绝写入：提不出可用特征 / 内容完全重复 / **相似度达阈值需人工确认** |
| `503` | 模型未加载且未开 `--allow-no-embedding`；或 `face_stream_server` 尚无可用帧 |

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
