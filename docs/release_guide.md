# 发布指南（release_guide）

本文档说明 Face Recognition Node 的版本管理规则、发布流程与发布前校验要求。

---

## 1. 版本管理规则

采用 **语义化版本** `MAJOR.MINOR.PATCH`：

| 位 | 递增条件 |
|---|---|
| `MAJOR` | 不兼容变更（数据库 schema、ROS 接口字段、CLI 参数语义） |
| `MINOR` | 向后兼容的新功能（新子命令、新话题、新模型支持） |
| `PATCH` | 向后兼容的缺陷修复与性能优化 |

当前版本：**1.0.0**

### 版本号来源（需同步修改）

| 文件 | 位置 |
|---|---|
| `src/face_recognition_ros2/package.xml` | `<version>1.0.0</version>` |
| `src/face_recognition_ros1/package.xml` | `<version>...</version>` |
| `src/face_recognition_core/CMakeLists.txt` | `VERSION "1.0.0" SOVERSION 1` |
| `src/face_recognition_ros2_interfaces/package.xml` | `<version>...</version>` |
| `src/face_recognition_ros1_interfaces/package.xml` | `<version>...</version>` |

> 修改 `face_recognition_core` 的 ABI（如增删虚函数、改结构体布局）时，
> 必须同步递增 `SOVERSION`。

---

## 2. 分支模型

| 分支 | 用途 |
|---|---|
| `main` | 集成分支，始终保持可构建 |
| `feature/*` | 功能开发 |
| `fix/*` | 缺陷修复 |
| `release/*` | 发布冻结分支（可选） |

合并到 `main` 前必须保证：**全模块构建通过 + 离线验证通过**。

---

## 3. 发布前检查清单

### 3.1 构建校验

```bash
./build.sh CLEAN
./build.sh MODELS
./build.sh ALL
```

确认产物齐备：

```
install/vendored/lib/libsqlite3.so*
install/lib/libface_recognition_core.so*
install/bin/face_db_web
install/face_recognition_ros2/lib/face_recognition_ros2/face_recognition_node
install/face_recognition_ros2/lib/face_recognition_ros2/face_viewer_node
install/face_recognition_ros2/lib/face_recognition_ros2/face_stream_server
src/face_recognition_standalone/build/face_recognition_app
src/face_recognition_ros1/devel/lib/face_recognition_ros1/face_recognition_node
```

### 3.2 功能校验

```bash
export ROS_LOG_DIR=/tmp/roslog

# ① ROS2 节点初始化
source scripts/setup_env.sh
ros2 launch face_recognition_ros2 face_recognition.launch.py \
    launch_viewer:=false enable_stream_server:=false
# 期望：detector / recognizer / database 均 initialized

# ② MJPEG 推流
ros2 launch face_recognition_ros2 usb_cam_face.launch.py
curl -s http://localhost:8090/healthz     # → ok

# ③ CLI 全流程
APP=./src/face_recognition_standalone/build/face_recognition_app
$APP add --image alice.jpg --name alice
$APP list                                  # emb=yes
$APP remove --id <uuid>
$APP clear

# ④ Web API
curl -s http://localhost:8080/api/faces | python3 -m json.tool
```

### 3.3 数据校验

```bash
sqlite3 /tmp/face_db/faces.db \
  "SELECT COUNT(*) AS total,
          SUM(CASE WHEN embedding IS NULL THEN 1 ELSE 0 END) AS no_emb
   FROM faces;"
```

`no_emb` 应等于 `0`（或已确认可通过 backfill 修复）。

### 3.4 依赖校验

```bash
# vendored SQLite3 生效
source scripts/setup_env.sh
sqlite3 :memory: "PRAGMA compile_options;" | grep -i rtree   # → ENABLE_RTREE=1

# 无系统写入（应无输出）
ls -l /usr/local/lib/libsqlite3.so* 2>/dev/null
```

---

## 4. 发布流程

```bash
# 1) 同步 main
git checkout main
git pull --rebase

# 2) 修改各处版本号（见第 1 节表格）
#    package.xml / CMakeLists.txt project VERSION

# 3) 提交版本变更
git add -A
git commit -m "chore(release): bump version to v1.0.0"

# 4) 打带注释的标签
git tag -a v1.0.0 -m "Face Recognition Node v1.0.0"

# 5) 推送分支与标签
git push origin main
git push origin v1.0.0
```

### 回滚

```bash
git tag -d v1.0.0                    # 删除本地标签
git push origin :refs/tags/v1.0.0    # 删除远端标签（谨慎）
```

> 标签一经发布并被下游引用，**不要**重新指向其它 commit；应发布 `PATCH` 版本。

---

## 5. 发布产物

| 产物 | 位置 | 说明 |
|---|---|---|
| 核心库 | `install/lib/libface_recognition_core.so.1.0.0` | 含 `SOVERSION 1` |
| vendored SQLite3 | `install/vendored/lib/libsqlite3.so*` | 含 R-Tree / GEOPOLY |
| ROS2 节点 | `install/face_recognition_ros2/lib/...` | 含 viewer / stream server |
| ROS1 节点 | `src/face_recognition_ros1/devel/lib/...` | catkin 产物 |
| standalone | `src/face_recognition_standalone/build/face_recognition_app` | 零 ROS 依赖 |
| Web 后台 | `install/bin/face_db_web` | 含 `share/face_db_web/templates` |
| 模型 | `models/*.onnx` | 由 `./build.sh MODELS` 下载，**不纳入版本控制** |

> 目标平台与编译环境需在发布说明中明确（Ubuntu 版本、架构、ONNX Runtime 版本）。

---

## 6. 自动化建议（CI/CD）

当前仓库**未内置** CI 配置。建议流水线至少包含：

1. `./build.sh MODELS && ./build.sh ALL`（构建门禁）
2. `./build.sh CLEAN` 后复现构建（可复现性门禁）
3. `source scripts/setup_env.sh && sqlite3 :memory: "PRAGMA compile_options;" | grep -i rtree`（vendored 依赖门禁）
4. 打标签后自动生成 Release Notes（列出 `MAJOR/MINOR/PATCH` 变更分类）

已知待办项（历史遗留）见 `docs/TODO.md`，例如
"修复构建脚本 ALL 部分失败仍报告成功"、"增加核心、ROS、Web 自动化回归测试"。

---

## 7. 发布说明（Release Notes）模板

```markdown
## Face Recognition Node v1.0.0

### 新增
- ...

### 修复
- ...

### 不兼容变更
- 数据库 schema：无
- ROS 接口：无
- CLI 参数：无

### 环境要求
- Ubuntu 22.04 / ROS2 Humble（或 Ubuntu 20.04 / ROS1 Noetic）
- OpenCV >= 4.5，ONNX Runtime C++ >= 1.16
- 模型：det_10g.onnx + w600k_r50.onnx（./build.sh MODELS）

### 校验结果
- [x] ./build.sh ALL 通过
- [x] 离线功能验证通过
- [x] vendored SQLite3 ENABLE_RTREE=1
```

---

## 8. 相关文档

- [源码开发快速开始](setup.md)
- [专项模块快速开始](module/quick_start.md)
- [常见问题排查](FAQ/troubleshooting.md)
