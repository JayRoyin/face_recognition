# 人脸库模块（FaceDatabase）

`face_recognition::FaceDatabase` 负责人脸记录的持久化（SQLite3）与 1:N 检索，
是 Web / ROS1 / ROS2 / standalone 四路数据互通的**唯一存储单点**。

- 头文件：`src/face_recognition_core/include/face_recognition_core/face_database.hpp`
- 实现：`src/face_recognition_core/src/face_database.cpp`
- 存储：SQLite3（优先使用 `third_party/sqlite3/` 的 vendored 版本）
- 线程安全：全部公开方法由 `std::recursive_mutex` 保护
- UUID：`libuuid`（`uuid_generate_random` + `uuid_unparse_lower`）

---

## 1. 接口

```cpp
class FaceDatabase {
public:
    bool initialize(const std::string& db_path, const std::string& faces_dir);

    // --- 身份 ---------------------------------------------------------------
    std::string add_face(const std::string& name,
                         const std::vector<float>& embedding,
                         const std::vector<uint8_t>& image_data = {},
                         const std::string& title        = "",
                         const std::string& scene        = "default",
                         const std::string& map_location = "unknown",
                         const std::string& gender       = "unknown",
                         const std::string& image_hash   = "");

    bool remove_face(const std::string& face_id);
    std::shared_ptr<FaceRecord> get_face(const std::string& face_id);
    std::vector<FaceRecord>     list_faces();   // 按 uid 升序（入库顺序）
    int  clear_all();
    int  get_face_count();

    // 编辑元数据：空串表示"保持原值"（name 为空不会清空 NOT NULL 列）
    bool update_face(const std::string& face_id,
                     const std::string& name,
                     const std::string& title,
                     const std::string& scene,
                     const std::string& map_location,
                     const std::string& image_path = "",
                     const std::string& gender     = "",
                     const std::string& image_hash = "");

    // 导入去重探针：按图片内容哈希查已存在记录（无则返回 nullptr）
    std::shared_ptr<FaceRecord> find_by_image_hash(const std::string& image_hash);

    // --- 附加模板（multi-shot）---------------------------------------------
    bool add_template(const std::string& face_id,
                      const std::vector<float>& embedding,
                      const std::string& image_path = "");
    std::vector<FaceTemplate> list_templates(const std::string& face_id);
    int  template_count(const std::string& face_id);
    bool remove_templates(const std::string& face_id);

    // --- 匹配 ---------------------------------------------------------------
    std::vector<MatchCandidate> rank_faces(const std::vector<float>& embedding);

    MatchResult match_face(const std::vector<float>& embedding,
                           float raw_threshold,
                           float z_threshold = 3.0f,
                           int   min_cohort  = 3,
                           bool  normalize   = true);

    // 兼容旧调用（ROS 节点），内部走 rank_faces + 原始阈值
    std::shared_ptr<FaceRecord> find_matching_face(const std::vector<float>& embedding,
                                                   float threshold = 0.7f);

    // --- 队列归一化 ---------------------------------------------------------
    void setCohortEmbeddings(std::vector<std::vector<float>> cohort);

    // --- 库级元数据（当前只有特征指纹 feature_space_id）---------------------
    // metadata 表记录"库内特征是用哪套配方算出来的"，启动时比对，不一致即告警。
    // 没有它，换前端 / 归一化 / 模型后的静默失效无法与"模型变差"区分。
    // 详见 [../module/core.md](../module/core.md) 的「特征指纹」。
    bool set_meta(const std::string& key, const std::string& value);
    std::string get_meta(const std::string& key) const;
    void setNormalizationDefaults(float z_threshold, int min_cohort, bool enabled);
    int  cohortSize() const;

    // --- 图片 / 特征 ---------------------------------------------------------
    bool save_image(const std::string& face_id,
                    const std::vector<uint8_t>& data,
                    std::string& out_path);
    bool update_embedding(const std::string& face_id,
                          const std::vector<float>& embedding);
};
```

> `find_matching_face` 的 `0.7f` 只是**函数签名里的历史默认值**，
> 应用层（CLI / ROS 节点 / Web）都会显式传入自己的 `recognition_threshold`
> （当前默认 `0.5`）。直接用 C++ API 时请显式传阈值。

---

## 2. initialize —— 初始化

```cpp
database.initialize("/data/hhqs_data/face_db/faces.db", "/data/hhqs_data/face_db/faces");
```

执行动作：

1. 递归创建 `faces_dir`
2. 递归创建 `db_path` 的父目录
3. `sqlite3_open()` 打开（不存在则创建）+ `busy_timeout(5000)` + `PRAGMA journal_mode=WAL`
4. `CREATE TABLE IF NOT EXISTS faces / face_templates / metadata`
   + `CREATE INDEX IF NOT EXISTS idx_face_templates_face_id`
5. **schema 迁移**（`migrate()`，每次启动都会跑，幂等）：
   - `PRAGMA table_info` 检查后 `ALTER TABLE ADD COLUMN` 补齐
     `gender` / `image_hash` / `uid`
   - 老数据 `uid` 按 `rowid` 回填（即历史插入顺序）
   - 建触发器 `faces_uid_ai`（`AFTER INSERT` 赋 `MAX(uid)+1`）与索引
     `idx_faces_image_hash`、`idx_faces_uid`
   - `gender` 的空值归一化为 `unknown`

> 老库升级**无需手工迁移**，启动即自动完成。列与触发器的完整定义见
> [../protocol/database_schema.md](../protocol/database_schema.md)。

任一步失败返回 `false`（并打印 `Cannot open database: <errmsg>`）。

---

## 3. add_face —— 新增

| 步骤 | 说明 |
|---|---|
| 1 | `name` 为空 → 直接返回 `""`（失败） |
| 2 | 生成 UUID v4 小写字符串作为 `id` |
| 3 | `image_data` 非空 → 写入 `<faces_dir>/<id>.jpg`，回填 `image_path` |
| 4 | `INSERT INTO faces (...)` |
| 5 | 成功返回 `id`，失败返回 `""` |

**重要语义**

- `embedding` 允许为空 → 数据库写入 `NULL`。该记录**可被列出、可显示图片，但
  无法参与识别比对**（`embedding` 为空时相似度恒为 0）。
- `created_at` 与 `updated_at` 均置为当前 `time(nullptr)`。
- 缩略图写入失败**不会**阻断数据库插入（仅 `image_path` 为空）。

---

## 4. rank_faces / match_face —— 1:N 检索与归一化

### 4.1 多模板（multi-shot）

一个人的外观会变（**戴/不戴眼镜**、姿态、光照）。只存一个 embedding 时，整个身份
就是特征空间里的**一个点**，任何外观变化都会让查询点跑偏——这正是"戴眼镜才能识别、
摘眼镜就认不出"的根因。

因此除 `faces.embedding`（主模板）外，新增 `face_templates` 表存**任意多个额外模板**：

```sql
CREATE TABLE IF NOT EXISTS face_templates (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    face_id TEXT NOT NULL,       -- -> faces.id
    embedding BLOB,
    image_path TEXT,             -- 该模板来源图，供 backfill --all 重建
    created_at INTEGER
);
```

```cpp
database.add_template(face_id, embedding, image_path);   // 加一"枪"
database.list_templates(face_id);
database.template_count(face_id);                        // 不含主模板
```

`face_recognition_app add-template --id <face_id> --image <照片>` 是它的 CLI 入口。
**推荐做法：同一个人的"戴眼镜"和"不戴眼镜"各存一张。**

### 4.2 rank_faces —— 排序

对查询向量，与**所有身份的所有模板**求余弦，每个身份取最高分，按分数降序返回：

```cpp
struct MatchCandidate {
    std::string face_id, name, title;
    float similarity;      // 该身份的最佳模板得分
    long long template_id; // -1 表示主模板
    int template_count;
};
std::vector<MatchCandidate> rank_faces(const std::vector<float>& embedding);
```

复杂度 O(N × T)，全内存暴力扫描；数千模板量级完全够用。

### 4.3 match_face —— 判定（含队列归一化）

```cpp
MatchResult match_face(const std::vector<float>& embedding,
                       float raw_threshold,
                       float z_threshold = 3.0f,
                       int   min_cohort  = 3,
                       bool  normalize   = true);
```

判定分两级：

```
1) 原始门限：  raw_similarity  >= raw_threshold           （否则 REJECT）
2) 队列门限：  z_score         >= z_threshold             （队列足够大时）
```

`z_score` 的定义：

```
cohort   = 其它身份的模板得分（每个身份取最佳）+ 外部冒充者队列
median   = median(cohort)
mad      = median(|cohort - median|)            # 稳健尺度
z_score  = (raw_similarity - median) / (1.4826 * mad)
```

**为什么需要它**：低质量模板（模糊、低分辨率、反光）容易成为**枢纽点（hub）**——
它对**所有**人都给出偏高的相似度，于是"是个人都能识别"。队列归一化把判定从
"绝对分数"变成"相对于这张查询对**无关系的人**有多高分"：枢纽模板会把 `median`
一起抬高，因此它必须给出**高得多的分数**才能通过。这也是说话人识别里
Z-norm / T-norm 的同一思路。

**队列来源与回退**：

| 情况 | 行为 |
|---|---|
| 库里有 ≥2 个身份 | 队列 = 其它身份的最佳模板得分 |
| 配置了 `--cohort-db <另一个库>` | 队列额外并入该库的全部模板 |
| 队列样本数 < `min_cohort`（默认 3） | **跳过归一化，退化为纯原始门限**（`normalized=false`） |
| 库为空 / 全部模板为空 | `accepted=false`，`reason="gallery empty"` |

> ⚠️ 单身份库 + 无外部队列时**归一化不生效**（队列为空），此时系统行为与旧版
> 完全一致。要让归一化生效，请至少再录入 1~2 个人，或准备一个冒充者库：
>
> ```bash
> # 用任意人脸图片目录建一个冒充者库（只用于归一化，不参与识别）
> APP=./install/bin/face_recognition_app
> $APP add-bulk --dir ./other_faces --db /data/hhqs_data/face_db/cohort.db
> # 之后运行识别时挂上它
> $APP run --source 0 --cohort-db /data/hhqs_data/face_db/cohort.db
> ```

### 4.4 find_matching_face —— 兼容封装

```cpp
std::shared_ptr<FaceRecord> find_matching_face(const std::vector<float>& embedding,
                                               float threshold = 0.7f);
```

内部走 `match_face()`（沿用 `setNormalizationDefaults()` 设置的默认参数），
供 ROS1 / ROS2 节点直接调用。返回 `nullptr` 表示拒绝。

> 签名里的 `0.7f` 是历史默认值；应用层都会显式传自己的阈值（当前默认 `0.5`）。

> **匹配单元是模板而不是身份**：`rank_faces()` 返回的每个身份分数已经是该身份
> **所有模板的最高分**，因此调用方不需要自己遍历模板（详见 §4.1 / §4.2）。

---

## 5. remove_face / clear_all —— 删除

| 方法 | 行为 | 返回 |
|---|---|---|
| `remove_face(id)` | 先查 `image_path` 并 `std::remove()` 删除文件，再 `DELETE FROM faces WHERE id = ?` | `bool` |
| `clear_all()` | 遍历全部记录，逐条删文件 + 删行 | 删除条数 `int` |

**注意**：删除文件与删行**不在同一事务内**。若删行失败，文件已被删除，属于
已知遗留项，按
[../protocol/database_schema.md §5 一致性与维护](../protocol/database_schema.md#5-一致性与维护)
手工清理孤立文件即可。

---

## 6. update_embedding —— 补写特征

```cpp
bool ok = database.update_embedding(face_id, embedding);
```

```sql
UPDATE faces SET embedding = ?, updated_at = ? WHERE id = ?
```

成功条件：`sqlite3_step() == SQLITE_DONE` **且** `sqlite3_changes() == 1`
（即确实有一条记录被更新）。`face_id` / `embedding` 为空时直接返回 `false`。

这是 **embedding 回填（backfill）** 的底层实现，被以下路径复用：

| 路径 | 入口 |
|---|---|
| standalone CLI | `face_recognition_app backfill` |
| standalone 运行时 | `run --auto-backfill` |
| ROS1 / ROS2 节点 | 节点构造时自动遍历补全 |

---

## 7. 数据流与文件布局

```
faces_dir/
├── 550e8400-e29b-41d4-a716-446655440000.jpg
├── 6ba7b810-9dad-11d1-80b4-00c04fd430c8.jpg
└── ...

db_path (SQLite)
├── table: faces              # 一个身份一行
│     id            ←→  文件名（去掉 .jpg）
│     embedding     BLOB(512 × float32 = 2048B)   # 主模板
│     image_path    →    缩略图绝对路径
└── table: face_templates     # 同一身份的附加模板，一枪一行
      face_id       →    faces.id
      embedding     BLOB(512 × float32 = 2048B)   # 同维度、同特征空间
      image_path    →    该枪的来源图（供 backfill --all 重建）
```

> 附加模板的图片文件名形如 `<id>_t1.jpg`、`<id>_t2.jpg`，与主缩略图
> `<id>.jpg` 并列存放在同一个 `faces_dir` 下。

字段完整定义见 [../protocol/database_schema.md](../protocol/database_schema.md)。

---

## 8. 并发与多进程

| 场景 | 结论 |
|---|---|
| 同一进程内多线程 | 安全，`recursive_mutex` 串行化 |
| 多进程**并发读** | 安全 |
| 多进程**并发写** | **需调用方串行化** |

> 不要让 standalone `run` / `add` 与 ROS2 节点同时向同一个 SQLite 文件写入。
> `face_db_web` 使用短事务，一般无影响。

---

## 9. 使用示例

```cpp
#include "face_recognition_core/face_database.hpp"

face_recognition::FaceDatabase db;
db.initialize("/data/hhqs_data/face_db/faces.db", "/data/hhqs_data/face_db/faces");

// 新增（带缩略图）
std::vector<uint8_t> jpeg = /* ... */;
std::string id = db.add_face("张三", embedding, jpeg, "工程师", "office", "5F-A区");

// 追加一枪（同一人的另一张照片）
db.add_template(id, embedding_from_another_photo, stored_path);

// 检索：排名 + 完整判定（含队列归一化）
auto ranked = db.rank_faces(embedding);
auto result = db.match_face(embedding, /*raw_threshold=*/0.5f);
if (result.accepted) { /* 命中 result 对应的身份 */ }

// 回填
db.update_embedding(id, embedding);

// 统计 / 清理
int n = db.get_face_count();
db.remove_face(id);
db.clear_all();
```

---

## 10. 相关文档

- [人脸识别模块](face_recognizer.md)
- [数据库 schema](../protocol/database_schema.md)
- [核心算法库模块说明](../module/core.md)
