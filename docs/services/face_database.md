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

    std::string add_face(const std::string& name,
                         const std::vector<float>& embedding,
                         const std::vector<uint8_t>& image_data = {},
                         const std::string& title        = "",
                         const std::string& scene        = "default",
                         const std::string& map_location = "unknown");

    bool remove_face(const std::string& face_id);
    std::shared_ptr<FaceRecord> get_face(const std::string& face_id);
    std::vector<FaceRecord>     list_faces();
    std::shared_ptr<FaceRecord> find_matching_face(const std::vector<float>& embedding,
                                                   float threshold = 0.7f);

    int  clear_all();
    int  get_face_count();

    bool save_image(const std::string& face_id,
                    const std::vector<uint8_t>& data,
                    std::string& out_path);
    bool update_embedding(const std::string& face_id,
                          const std::vector<float>& embedding);
};
```

---

## 2. initialize —— 初始化

```cpp
database.initialize("/tmp/face_db/faces.db", "/tmp/face_db/faces");
```

执行动作：

1. 递归创建 `faces_dir`
2. 递归创建 `db_path` 的父目录
3. `sqlite3_open()` 打开（不存在则创建）
4. `CREATE TABLE IF NOT EXISTS faces (...)`

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

## 4. find_matching_face —— 1:N 检索

```cpp
auto match = database.find_matching_face(embedding, 0.7f);
```

算法：全表扫描，对每条记录计算 `compute_similarity`（内积，双方均已 L2 归一化），
保留**严格大于阈值**的最大者。

```
best_sim = threshold
for record in all:
    sim = dot(query, record.embedding)
    if sim > best_sim: best_sim, best_match = sim, record
return best_match      // 无命中返回 nullptr
```

**复杂度** O(N)。当前实现未使用向量索引；人脸库规模在数千条以内时完全可用。

---

## 5. remove_face / clear_all —— 删除

| 方法 | 行为 | 返回 |
|---|---|---|
| `remove_face(id)` | 先查 `image_path` 并 `std::remove()` 删除文件，再 `DELETE FROM faces WHERE id = ?` | `bool` |
| `clear_all()` | 遍历全部记录，逐条删文件 + 删行 | 删除条数 `int` |

**注意**：删除文件与删行**不在同一事务内**。若删行失败，文件已被删除，属于
已知的遗留项（见 `docs/TODO.md` 中"完善删除失败回滚与孤立文件清理"）。

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
└── table: faces
      id            ←→  文件名（去掉 .jpg）
      embedding     BLOB(512 × float32 = 2048B)
      image_path    →    缩略图绝对路径
```

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
db.initialize("/tmp/face_db/faces.db", "/tmp/face_db/faces");

// 新增（带缩略图）
std::vector<uint8_t> jpeg = /* ... */;
std::string id = db.add_face("张三", embedding, jpeg, "工程师", "office", "5F-A区");

// 检索
auto match = db.find_matching_face(embedding, 0.7f);

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
