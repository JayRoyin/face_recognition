# 数据库 schema 文档（database_schema）

所有人脸数据存储在**单个 SQLite 文件**中，表结构由 `FaceDatabase::initialize()`
用 `CREATE TABLE IF NOT EXISTS` 自动创建，无需手工建库。

- 默认路径：`/data/hhqs_data/face_db/faces.db`
- 缩略图目录：`/data/hhqs_data/face_db/faces`
- 建表语句：`src/face_recognition_core/src/face_database.cpp`

---

## 1. 表结构

```sql
CREATE TABLE IF NOT EXISTS faces (
    id           TEXT PRIMARY KEY,
    name         TEXT NOT NULL,
    title        TEXT,
    embedding    BLOB,
    image_path   TEXT,
    scene        TEXT,
    map_location TEXT,
    created_at   INTEGER,
    updated_at   INTEGER,
    gender       TEXT DEFAULT 'unknown',  -- unknown / male / female
    image_hash   TEXT,                    -- SHA-256(图片字节)，导入去重依据
    uid          INTEGER                  -- 数据库自动维护的自增序号（排序键）
);

-- uid 由触发器维护：任何写入方（CLI / Web / ROS）都共用同一个计数器
CREATE TRIGGER IF NOT EXISTS faces_uid_ai
AFTER INSERT ON faces WHEN NEW.uid IS NULL
BEGIN
  UPDATE faces SET uid = (SELECT COALESCE(MAX(uid), 0) + 1 FROM faces)
  WHERE id = NEW.id;
END;
CREATE INDEX IF NOT EXISTS idx_faces_image_hash ON faces(image_hash);
CREATE UNIQUE INDEX IF NOT EXISTS idx_faces_uid ON faces(uid);

-- 额外模板（multi-shot）：一个人可以有任意多"枪"
CREATE TABLE IF NOT EXISTS face_templates (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    face_id    TEXT NOT NULL,   -- -> faces.id
    embedding  BLOB,            -- 与 faces.embedding 同维度、同空间
    image_path TEXT,            -- 该模板的来源图，供 backfill --all 重建
    created_at INTEGER
);
CREATE INDEX IF NOT EXISTS idx_face_templates_face_id
    ON face_templates(face_id);
```

> `faces.embedding` 是**主模板**（等价于第 0 枪），额外模板放在 `face_templates`。
> 匹配时取一个身份下所有模板的最高分，因此一个人的模板数 = `1 + COUNT(face_templates)`。
> 老库升级时该表由 `initialize()` 自动创建，**无需手工迁移**。

---

## 2. 数据字典

| 字段 | 类型 | 可空 | 说明 |
|---|---|---|---|
| `id` | TEXT | 否（主键） | UUID v4 **小写**字符串（36 字符，含 4 个 `-`） |
| `name` | TEXT | 否 | 姓名；写入时为空会被 `add_face` 拒绝 |
| `title` | TEXT | 是 | 职位 / 头衔，默认空串 |
| `embedding` | BLOB | 是 | **512 × float32 = 2048 字节**；为空表示该记录不可识别 |
| `image_path` | TEXT | 是 | 缩略图**绝对路径**，`<faces_dir>/<id>.jpg` |
| `scene` | TEXT | 是 | 场景标签，默认 `default` |
| `map_location` | TEXT | 是 | 物理位置，默认 `unknown` |
| `created_at` | INTEGER | 是 | Unix 时间戳（秒），`time(nullptr)` |
| `updated_at` | INTEGER | 是 | 新增时等于 `created_at`；`update_embedding` / `update_face` 时刷新 |
| `gender` | TEXT | 是 | 性别，`unknown` / `male` / `female`，默认 `unknown`（**数据库字段**，非前端私有） |
| `image_hash` | TEXT | 是 | 存储图片字节的 SHA-256（64 位小写 hex），导入去重的**唯一内容依据** |
| `uid` | INTEGER | 是 | 数据库自动维护的自增序号：列表**排序标准**与库内编号；触发器赋值，不对外暴露 |

### uid 的维护方式

- 新增行不写 `uid`（`INSERT` 语句里没有这一列），由 `faces_uid_ai` 触发器
  `AFTER INSERT` 补上 `MAX(uid)+1`
- 这样 CLI、Web、ROS1/ROS2 四个写入方共用同一个计数器；若由某个进程自己
  维护自增，多进程并发写入必然撞号
- 老库升级时按 `rowid` 回填（即历史插入顺序），因此序号稳定、不跳变
- **不出现在 HTTP API 的响应里**：`/api/faces` 只返回 UUID `id`，`uid` 仅用于
  库内排序与运维

### image_hash 与去重

同一张照片即使被改名、被重新导出（EXIF/压缩参数变化），像素内容仍然一致；
反之同一个人不同照片**不算重复**（多模板是合法场景）。因此：

| 判定 | 依据 | 处理 |
|---|---|---|
| 完全重复 | `image_hash` 相同（批内 或 库内） | **跳过**，返回友好提示 |
| 批次内同名 | 文件名相同 | **跳过**（仅同批次内比对） |
| 疑似同一人 | 512 维特征余弦 ≥ 0.70 | 默认**入库 + 提示**，可 `on_similar=skip` 改为跳过 |

### embedding 的二进制布局

```
偏移        内容
0   ~ 3     float32 第 0 维
4   ~ 7     float32 第 1 维
...
2044~ 2047  float32 第 511 维
```

小端序（x86_64 / aarch64 均为小端）。读取时按
`emb_size / sizeof(float)` 还原维度，因此维度变化（如换成 256 维模型）不会崩溃，
但**新旧特征无法互相匹配**。

---

## 3. 文件与记录的对应关系

```
/data/hhqs_data/face_db/
├── faces.db                              # SQLite 库
└── faces/
    ├── 550e8400-e29b-41d4-a716-446655440000.jpg    ← faces.id 一致
    └── 6ba7b810-9dad-11d1-80b4-00c04fd430c8.jpg
```

- 新增：写入 `<faces_dir>/<id>.jpg`，然后把路径回填 `image_path`
- 删除：`remove_face` / `clear_all` 会**同时**删除文件与记录
- 图片写入失败**不阻断**入库（`image_path` 留空）

---

## 4. 常用运维 SQL

```bash
DB=/data/hhqs_data/face_db/faces.db

# 记录总数
sqlite3 "$DB" "SELECT COUNT(*) FROM faces;"

# 列出全部（含是否有特征）
sqlite3 -header -column "$DB" \
  "SELECT id, name, title, scene, map_location,
          CASE WHEN embedding IS NULL THEN 'NO' ELSE 'yes' END AS emb,
          length(embedding) AS emb_bytes
   FROM faces;"

# 查找缺少 embedding 的记录（需要 backfill）
sqlite3 "$DB" "SELECT id, name, image_path FROM faces \
               WHERE embedding IS NULL OR length(embedding) = 0;"

# 查找缺少缩略图的记录
sqlite3 "$DB" "SELECT id, name FROM faces \
               WHERE image_path IS NULL OR image_path = '';"

# 按场景统计
sqlite3 -header -column "$DB" \
  "SELECT scene, COUNT(*) AS n FROM faces GROUP BY scene;"

# 按性别统计
sqlite3 -header -column "$DB" \
  "SELECT gender, COUNT(*) AS n FROM faces GROUP BY gender;"

# 按入库顺序（uid）列出
sqlite3 -header -column "$DB" \
  "SELECT uid, name, title, gender FROM faces ORDER BY uid;"

# 查重复：同一张图被录了多次
sqlite3 -header -column "$DB" \
  "SELECT image_hash, COUNT(*) AS n, group_concat(name) FROM faces
   WHERE image_hash IS NOT NULL GROUP BY image_hash HAVING n > 1;"

# 删除某条记录（注意：不会删除磁盘上的缩略图）
sqlite3 "$DB" "DELETE FROM faces WHERE id = '<UUID>';"
```

> 手工改库后请用 `face_recognition_app list` 复核，避免出现"有记录无图片"
> 或"有图片无特征"的不一致状态。

---

## 5. 一致性与维护

| 状态 | 现象 | 修复方式 |
|---|---|---|
| `embedding IS NULL` 且 `image_path` 有效 | 该人脸**永远识别不出** | `backfill` / `run --auto-backfill` / 重启 ROS 节点 |
| `image_path` 指向不存在的文件 | Web 列表缩略图 404 | 重新上传该记录，或手工清理 |
| 存在孤立 `.jpg`（无对应记录） | 磁盘占用 | 手工删除文件 |
| 手工 `DELETE` 后图片残留 | 磁盘占用 | 手工删除 `faces/<id>.jpg` |

**已知限制**：`remove_face` 与 `clear_all` 的"删文件"和"删行"不在同一事务内，
失败时可能留下孤立文件。按上表手工清理即可。

---

## 6. 跨模块共享

| 通路 | 默认 DB 路径 | 指定参数 |
|---|---|---|
| `face_recognition_standalone` | `/data/hhqs_data/face_db/faces.db` | `--db` |
| `face_db_web` | 无默认，必填 | `--db` |
| ROS2 节点 | `/data/hhqs_data/face_db/faces.db` | 参数 `db_path` |
| ROS1 节点 | `/data/hhqs_data/face_db/faces.db` | 参数 `db_path` |

使用同一个 `--db` 即可三向互通。并发写需外部串行化，详见
[../services/face_database.md](../services/face_database.md#8-并发与多进程)。

---

## 7. 版本兼容

| 变更 | 兼容性 |
|---|---|
| 新增可空列 | 旧二进制可继续运行（未使用新列） |
| 新增 `gender` / `image_hash` / `uid` 列 | **自动迁移**：`initialize()` 用 `PRAGMA table_info` 检查后 `ALTER TABLE ADD COLUMN`，老库启动即升级，无需手工操作 |
| 修改 `embedding` 维度 | **不兼容**，库内旧特征无法匹配，需重新入库 |
| 修改识别模型 | 需**清空并重建**人脸库，ArcFace 特征空间不通用 |
| 更换识别前端（对齐 ↔ bbox 裁剪） | **不兼容**，两个前端不在同一特征子空间；执行 `backfill --all` 用存档缩略图重建 |
| 更换 SQLite 版本 | 兼容（vendored 与系统库读写同一文件格式） |

> 判断"库里存的到底是不是当前前端的特征"没有标记字段，只能靠**最后一次
> `backfill --all` 的时间**与代码/参数的变更时间对照。任何影响取脸的改动
> （前端、检测框解码、关键点解码）都必须重跑 `backfill --all`。

---

## 8. 相关文档

- [人脸库模块](../services/face_database.md)
- [Web HTTP API](web_api.md)
- [ROS 接口文档](ros_interfaces.md)
