# Web 人脸库后台模块使用文档（face_db_web）

基于 **libmicrohttpd** 的轻量 HTTP 服务，提供人脸库的可视化 **增 / 删 / 改 / 查 /
批量导入 / 清空**，并可在录入时自动提取 512 维 ArcFace 特征写入 SQLite。

- 源码：`src/face_db_web/`
- 依赖：core + OpenCV + libmicrohttpd + **zlib**（解压导入包）+ pthread（**无 ROS 依赖**）
- 产物：`install/bin/face_db_web`

主要能力：

| 能力 | 说明 |
|---|---|
| 单张新增 | `/api/faces/add`（表单 UI 已移除，接口保留供脚本调用） |
| **压缩包批量导入** | 拖入 `.zip / .tar / .tar.gz / .tgz`，包内 `png/jpg` 按 `title_name` 命名自动解析 |
| **图片批量导入** | 一次拖入大量图片 → 宫格中逐张编辑 → 按选中项重建入库 |
| 重复校验 | 内容哈希（SHA-256）+ 批内同名 + 特征相似度三层判定 |
| **高相似度人工确认** | 相似度 ≥ 阈值（默认 0.80）**不自动入库**，必须人工选择处理方式 |
| 多模板（多枪） | `/api/faces/add-template`，追加同一人的另一张照片 |
| 性别 | 数据库字段，可手填，也可用 `genderage.onnx` 自动预填 |
| 库内管理 | 搜索 / 按场景·性别筛选 / 分页 / 多选批量改场景·性别·删除 |

---

## 1. 编译

```bash
./build.sh WEB       # = vendored SQLite3 + CORE + Web 后台
```

构建前需安装 `libmicrohttpd-dev` 与 `zlib1g-dev`：

```bash
sudo apt-get install -y libmicrohttpd-dev zlib1g-dev
```

---

## 2. 启动

```bash
# 先确保人脸图片存档目录存在
mkdir -p /data/hhqs_data/face_db/faces

./install/bin/face_db_web \
    --port 8080 \
    --db /data/hhqs_data/face_db/faces.db \
    --faces-dir /data/hhqs_data/face_db/faces
```

### 2.1 命令行参数

| 参数 | 默认值 | 是否必需 | 说明 |
|---|---|---|---|
| `--port <n>` | `8080` | 否 | HTTP 监听端口 |
| `--db <path>` | — | **是** | SQLite 数据库路径 |
| `--faces-dir <path>` | — | **是** | 人脸缩略图存放目录 |
| `--detection-model <path>` | **自动发现** | 否 | 人脸检测模型 |
| `--recognition-model <path>` | **自动发现** | 否 | 入录时提取 embedding 的模型 |
| `--det-threshold <f>` | `0.5` | 否 | 检测置信度阈值；证件照漏检时可降到 `0.3` |
| `--max-upload-mb <n>` | `512` | 否 | 单个请求体上限（批量导入必须放大） |
| `--allow-no-embedding` | 关 | 否 | 允许写入**没有特征**的记录（默认拒绝，见下） |
| `--enroll-policy <path>` | — | 否 | 重入库规则 JSON（环境变量 `FACE_ENROLL_POLICY`） |
| `--high-similarity <f>` | `0.80` | 否 | 触发人工确认的相似度阈值 |
| `--auto-cross-scene` | 关 | 否 | 命中记录在**其他场景**时不提示直接入库（同场景仍提示） |
| `--no-confirm` | 关 | 否 | **危险**：关闭全部人工确认，启动即告警 |
| `--print-enroll-policy` | — | 否 | 打印带注释的规则文件模板后退出 |
| `--auto-gender` | 关 | 否 | 用 `genderage.onnx` 预填性别（仅填空值） |
| `--gender-model <path>` | **自动发现** | 否 | 性别模型路径（环境变量 `FACE_GENDER_MODEL`） |
| `--gender-male-index <0\|1>` | `1` | 否 | 两个输出 logit 中哪个代表男性；结果整体相反时改 `0` |
| `--help` / `-h` | — | 否 | 打印帮助 |

#### 模型自动发现（v2 新增）

不再必须手动传模型路径。按以下顺序查找 `det_10g.onnx` / `w600k_r50.onnx`：

```
1. --detection-model / --recognition-model        （命令行，最高优先）
2. FACE_DETECTION_MODEL / FACE_RECOGNITION_MODEL  （环境变量）
3. ./models/                                       （当前工作目录）
4. <exe>/models/   <exe>/../../models/             （install/bin 安装树）
```

因此在项目根目录直接启动即可：

```bash
./install/bin/face_db_web --port 8080 \
    --db /data/hhqs_data/face_db/faces.db --faces-dir /data/hhqs_data/face_db/faces
```

#### 为什么默认拒绝"无特征"录入

若模型加载失败，历史版本的 Web 后台会把记录以 `embedding = NULL` 写入 ——
**这种记录永远无法被识别**，症状是"页面里明明有人，识别却怎么都认不出"。

现在：

- 启动时会打印醒目警告（含修复方法）；
- 录入时若拿不到 embedding，返回 **HTTP 409** 并说明原因，**不再产生不可用记录**；
- 确实只想存文字信息时才用 `--allow-no-embedding`。

已经存在的 `NULL` 记录可以直接重建（缩略图已存档，无需重新上传）：

```bash
./install/bin/face_recognition_app backfill --all
```

### 2.2 环境变量回退

未通过命令行传模型路径时，会读取环境变量：

```bash
export FACE_DETECTION_MODEL=$(pwd)/models/det_10g.onnx
export FACE_RECOGNITION_MODEL=$(pwd)/models/w600k_r50.onnx
export FACE_ENROLL_POLICY=$(pwd)/enroll_policy.json   # 可选：重入库规则
export FACE_GENDER_MODEL=$(pwd)/models/genderage.onnx # 可选：性别模型
./install/bin/face_db_web --port 8080 --db ... --faces-dir ...
```

`scripts/setup_env.sh` 已自动完成模型相关的导出（当 `models/` 存在标准模型时）。

页面模板查找顺序（`index.html`）：

```
1. FACE_DB_WEB_TEMPLATES/index.html        （环境变量指定目录）
2. ./templates/index.html
3. <exe>/templates/index.html
4. <exe>/../share/face_db_web/templates/index.html
5. <exe>/../../share/face_db_web/templates/index.html
6. 编译期内嵌副本（CMake 从 templates/index.html 生成）
```

即：直接改 `src/face_db_web/templates/index.html` 无需重编译；即使模板目录
丢失，二进制里也有一份可用页面。

### 2.3 启用录入时自动提取 embedding（推荐）

```bash
./install/bin/face_db_web \
    --port 8080 \
    --db /data/hhqs_data/face_db/faces.db \
    --faces-dir /data/hhqs_data/face_db/faces \
    --detection-model "$(pwd)/models/det_10g.onnx" \
    --recognition-model "$(pwd)/models/w600k_r50.onnx"
```

启动日志会显示：

```
Detection model:   .../models/det_10g.onnx
Recognition model: .../models/w600k_r50.onnx
Embedding extraction: ENABLED
Max request body:    512 MB
Re-enrolment:       confirm at similarity >= 0.8 (same-scene: ask, cross-scene: ask)
Gender model:       .../models/genderage.onnx          # 仅 --auto-gender 时
Press Ctrl+C to stop
```

若未加载模型，则打印醒目警告块，且 `Embedding extraction: DISABLED (see warning above)`。

> **检测不到人脸 / 提不出特征的图片默认被拒绝（HTTP 409）**，不会写入
> `embedding = NULL` 的"永远认不出"的记录。只有显式加 `--allow-no-embedding`
> 才会保存这类记录（并写入 `image_path` 供后续 `backfill` 补齐）。

---

## 3. 使用教程

浏览器打开 `http://<服务器IP>:8080`。页面是**单页宫格**布局，自上而下四块：

```
┌──────────────────────────────────────────────────────┐
│ 人脸库管理                              [N 条记录]    │
├──────────────────────────────────────────────────────┤
│ 方式一 · 上传压缩包       │ 方式二 · 拖入图片           │
│  [拖拽 zip/tar.gz]        │  [拖入多张 png/jpg]        │
│  默认场景/位置/性别        │                            │
│  [导入压缩包]             │                            │
├──────────────────────────────────────────────────────┤
│ 归档编辑（共 N 张，已勾选 M 张）                       │
│  批量填充场景/位置/性别 [应用到全部] [按文件名重新解析] │
│  [全选] [删除选中] [按勾选项重建入库]                  │
│  ┌──────┐┌──────┐┌──────┐  点击卡片=选中(蓝框+「已选」)│
│  │ 图片 ││ 图片 ││ 图片 │  卡片内输入框正常编辑        │
│  │姓名..││姓名..││姓名..│                            │
│  └──────┘└──────┘└──────┘                            │
├──────────────────────────────────────────────────────┤
│ 待确认入库（相似度≥阈值，未确认不写库）                 │
│  左：本次上传  中：相似度%  右：库中匹配                │
│  (入库/追加为模板/替换/跳过)  [提交确认]               │
├──────────────────────────────────────────────────────┤
│ 已录入人脸（共 N 条）                                  │
│  搜索[   ] 场景[▾] 性别[▾] 批量设场景/性别 [应用到选中]│
│  [全选][批量删除] [刷新][清空人脸库]                   │
│  ┌──────┐┌──────┐┌──────┐  [加载更多]                 │
│  │ 图片 ││ 图片 ││ 图片 │  点击卡片=选中               │
│  │姓名..││姓名..││姓名..│  [编辑][加模板][删除]        │
│  └──────┘└──────┘└──────┘                            │
└──────────────────────────────────────────────────────┘
```

### 3.1 方式一：压缩包批量导入

1. 把图片打成 `.zip` / `.tar` / `.tar.gz` / `.tgz`（包内层级不限，会自动跳过隐藏
   文件、`__MACOSX/`、`._*`）
2. 拖到「方式一」的拖放区（或点击选择文件）
3. 按需填默认 **场景 / 位置 / 性别**（包内文件名不含这些信息）
4. 点「导入压缩包」→ 返回逐张结果报告

文件名即元数据（**仅接受 png / jpg / jpeg**）：

| 文件名 | title | name |
|---|---|---|
| `工程师_张三.png` | 工程师 | 张三 |
| `资深_工程师_张三.jpg` | 资深_工程师 | 张三（最后一个下划线切分） |
| `01匡洁.png` | （空） | 匡洁（开头序号被去掉） |
| `张三.png` | （空） | 张三 |

### 3.2 方式二：拖入图片 → 宫格编辑 → 入库

1. 一次拖入大量图片，前端会压缩到最长边 1280 的 JPEG 再上传
2. 图片进入「归档编辑」宫格，**姓名/职位已按文件名自动填好**
3. 逐张修改（也可用工具栏「批量填充场景/位置/性别」+「应用到全部」）
4. **点击卡片选中**（再次点击取消，选中显示蓝框与「已选」角标）
5. 点「**按勾选项重建入库**」提交；提交按 20 张一片进行，按钮实时显示 `已处理 n/N`
6. 入库成功的卡片自动从宫格移除；失败 / 重复 / 待确认的留在原处

> 未选中任何卡片时"入库"按钮置灰；已选数量显示在标题上（不在按钮里，
> 避免按钮宽度变化导致误点）。卡片内的输入框、下拉框、按钮不会误触发选中。

### 3.3 待确认入库（高相似度人工确认）

当上传的人脸与库中某人相似度 **≥ 阈值（默认 0.80）** 时，**不会自动写库**：

- 卡片左右并排显示「本次上传」与「库中匹配」，中间标注相似度百分比，
  并标明是**同场景**还是**跨场景（xxx）**
- 每张四选一（默认**跳过**，即不选就不写）：
  - **入库(新增)**：新增一条独立记录
  - **追加为模板**：把这张作为该人的额外模板（多枪），保留原特征 —— 同一人的
    第二张照片推荐用这个
  - **替换原记录**：用新照片与新特征覆盖该记录
  - **跳过**：丢弃本次上传
- 顶部支持「全部入库 / 全部替换 / 全部跳过」，最后点「提交确认」
- 提交后调 `/api/faces/resolve`，令牌一次性消费，重复提交不会写出第二条

规则可通过 `--enroll-policy` 文件或 `--high-similarity` / `--auto-cross-scene`
调整，见 [../protocol/web_api.md §4.6](../protocol/web_api.md)。

### 3.4 人脸库管理

- **搜索**：按姓名 / 职位关键字实时过滤
- **筛选**：场景下拉（选项从库内数据自动生成）+ 性别下拉
- **分页**：默认渲染 60 张，「加载更多」每次 +60
- **多选批量**：点击卡片选中 → 批量设场景 / 批量设性别 / 批量删除
- **加模板**：卡片上「加模板」按钮，选一张该人的照片追加为额外模板
- **编辑**：卡片上「编辑」就地修改姓名 / 职位 / 场景 / 位置 / 性别
- **删除单条**：卡片上「删除」
- **清空全部**：「清空人脸库」按钮（JS 函数 `clearAll()`，也可在控制台调用）

> 页面顶部的「N 条记录」与「已录入人脸（共 N 条）」会随操作自动刷新；
> 另有「刷新」按钮可手动重新拉取。

> **图片要求**：建议清晰、正面、单人脸。系统自动检测人脸、提取 512 维 ArcFace
> 特征并写入 SQLite；提不出特征时按上面的规则拒绝或进入待确认。

---

## 4. HTTP API 速查

| 方法 | 路径 | 用途 |
|---|---|---|
| GET | `/` | Web UI 页面 |
| GET | `/api/config` | 服务端启用了哪些可选行为（`auto_gender` / 入库策略） |
| GET | `/api/faces` | 列出所有人脸（JSON，按 `uid` 升序） |
| GET | `/api/image/<id>` | 取某条人脸的原图 |
| GET | `/api/faces/pending` | 待人工确认的入库请求 |
| GET | `/api/pending/image/<token>` | 待确认图片预览 |
| POST | `/api/faces/add` | 新增一条人脸（form-urlencoded） |
| POST | `/api/faces/update` | 编辑一条人脸（可选换照片并重建特征） |
| POST | `/api/faces/add-template` | 追加为同一人的额外模板（多枪） |
| POST | `/api/faces/remove` | 删除一条人脸（按 id） |
| POST | `/api/faces/clear` | 清空库 |
| POST | `/api/faces/import-archive` | 压缩包批量导入（multipart / 裸二进制） |
| POST | `/api/faces/import-batch` | 宫格图片批量导入（JSON + base64） |
| POST | `/api/faces/resolve` | 提交人工决定（唯一写入高相似度人脸的入口） |

```bash
# 列出所有人脸
curl -s http://localhost:8080/api/faces | python3 -m json.tool

# 压缩包批量导入（包内文件名 = title_name.png）
curl -X POST http://localhost:8080/api/faces/import-archive \
     -F "archive=@公司人脸库.zip" -F "scene=office" -F "map_location=公司总部"

# 追加模板 / 编辑 / 删除 / 清空
curl -X POST http://localhost:8080/api/faces/add-template \
     --data-urlencode "id=<UUID>" --data-urlencode "image_data=<BASE64>"
curl -X POST http://localhost:8080/api/faces/update \
     --data-urlencode "id=<UUID>" --data-urlencode "scene=lab" --data-urlencode "gender=female"
curl -X POST http://localhost:8080/api/faces/remove --data-urlencode "id=<UUID>"
curl -X POST http://localhost:8080/api/faces/clear

# 高相似度：先看队列，再人工决定
curl -s http://localhost:8080/api/faces/pending | python3 -m json.tool
curl -X POST http://localhost:8080/api/faces/resolve \
     -H 'Content-Type: application/json' \
     -d '{"decisions":[{"token":"<TOKEN>","action":"template"}]}'
```

完整字段、状态码与错误语义见 [../protocol/web_api.md](../protocol/web_api.md)。

---

## 5. 与 standalone CLI 的等价入口

`face_recognition_app web` 子命令会**自动转发** `--db` / `--faces-dir` /
`--detection-model` / `--recognition-model` 给 `face_db_web`：

```bash
./install/bin/face_recognition_app web --port 8080
# 等价于：
#   ./install/bin/face_db_web --port 8080 \
#       --db /data/hhqs_data/face_db/faces.db --faces-dir /data/hhqs_data/face_db/faces \
#       --detection-model models/det_10g.onnx \
#       --recognition-model models/w600k_r50.onnx
```

若 `face_db_web` 不在默认位置，可用 `--web-binary /abs/path` 指定。

> **注意**：该转发**只覆盖上面 4 个参数**（外加 `--port`）。批量导入上限
> `--max-upload-mb`、重入库规则 `--enroll-policy` / `--high-similarity` /
> `--auto-cross-scene` / `--no-confirm`、性别 `--auto-gender` /
> `--gender-model` / `--gender-male-index`、检测阈值 `--det-threshold`
> **都不会被转发**。需要这些能力时请直接启动 `face_db_web`，
> 或用环境变量（`FACE_ENROLL_POLICY` / `FACE_GENDER_MODEL`）间接生效。

---

## 6. 安全性说明

当前实现**未内置**认证 / CSRF / HTTPS。因此：

- 仅建议部署在**受信任内网**
- 需要对外暴露时，请前置反向代理（Nginx）并启用 Basic Auth / TLS
- 服务默认只监听 **`127.0.0.1`**（`FACE_WEB_BIND` 可改为 `0.0.0.0` 等）；
  由于 `/api/faces/clear` 会**连图片文件一起删**，对外暴露前务必加访问控制
- 请求体上限由 `--max-upload-mb` 控制（默认 **512 MiB**，为批量导入放大）。
  上限越大，单个请求可占用的内存越多，公网/多用户场景建议下调
- 压缩包解压有独立限制（4000 个条目、总解压 512 MiB、单文件 64 MiB），
  用于防"压缩炸弹"
- 前端 DOM 写入使用 `textContent` / 显式转义，JSON 已转义 `" \ \n \r \t`
- 服务使用**短事务**写库，与 ROS / standalone 并发时影响一般可接受

---

## 7. 相关文档

- [Web HTTP API 文档](../protocol/web_api.md)
- [数据库 schema](../protocol/database_schema.md)
- [非 ROS 实时识别模块](standalone.md)
- [常见问题排查](../FAQ/troubleshooting.md)
