# 文档索引（docs）

本目录是 Face Recognition Node 的**全量文档归档**。所有内容以**当前代码实现**为准，
不记录历史运行结果与测试数值（这类信息随机器与数据变化，写进文档必然过期）。

---

## 快速入口

| 我想…… | 去看 |
|---|---|
| 把项目跑起来（含首次离线验证） | [setup.md](setup.md) · [module/quick_start.md](module/quick_start.md) |
| 了解整体设计与目录结构 | [architecture.md](architecture.md) · [根 README](../README.md) |
| 用某个模块 | [module/](module/) |
| 用 CLI 做录入 / 校验 / 回填 | [module/standalone.md](module/standalone.md) |
| 查接口 / 协议 / 配置 / 数据格式 | [protocol/](protocol/) |
| 查核心算法接口（检测 / 识别 / 人脸库） | [services/](services/) |
| 接入第三方依赖 / 换平台 | [peripheral/third_party.md](peripheral/third_party.md) |
| 排查识别不准的问题 | [FAQ/troubleshooting.md](FAQ/troubleshooting.md) |
| 发布版本 | [release_guide.md](release_guide.md) |

---

## 1. 入门与总览

| 文档 | 内容 |
|---|---|
| [setup.md](setup.md) | 依赖、ONNX Runtime、模型、一键构建、跨平台编译、**首次跑通验证（build → add → backfill --all → verify → run）** |
| [architecture.md](architecture.md) | 分层设计、识别数据流、人脸库存储模型、模块依赖矩阵、并发模型、关键设计决策 |

## 2. 专项模块文档（module/）

| 文档 | 内容 |
|---|---|
| [module/quick_start.md](module/quick_start.md) | 5 个模块的最短跑通路径 + 组合工作流 |
| [module/core.md](module/core.md) | 核心算法库：目录、依赖、数据类型、公共 API（含多模板与队列归一化） |
| [module/standalone.md](module/standalone.md) | 非 ROS 实时识别 CLI：命令、输入源、参数、**verify 判定可视化**、阈值/前端/帧率调优、批量入库与 `backfill --all` |
| [module/ros2.md](module/ros2.md) | ROS2 节点 + viewer + MJPEG 推流：编译、启动、参数 |
| [module/ros1.md](module/ros1.md) | ROS1 节点：编译、启动、参数、接口 |
| [module/web.md](module/web.md) | Web 人脸库后台：编译、启动、使用、安全性 |

## 3. 核心业务模块文档（services/）

| 文档 | 内容 |
|---|---|
| [services/face_detector.md](services/face_detector.md) | 人脸检测：backend 判定、letterbox 预处理、**SCRFD 解码公式**、NMS |
| [services/face_recognizer.md](services/face_recognizer.md) | 人脸识别：**两个取脸前端（对齐默认启用）**、预处理、512 维 embedding、**余弦语义与阈值取值** |
| [services/face_database.md](services/face_database.md) | 人脸库：CRUD、`rank_faces` / `match_face`、多模板、队列归一化、并发与文件布局 |

## 4. 接口、协议与配置（protocol/）

| 文档 | 内容 |
|---|---|
| [protocol/README.md](protocol/README.md) | 接口总览、默认端口与路径 |
| [protocol/ros_interfaces.md](protocol/ros_interfaces.md) | ROS Topic / Service / Message（ROS1 与 ROS2） |
| [protocol/web_api.md](protocol/web_api.md) | 人脸库 HTTP API 与 MJPEG 推流接口 |
| [protocol/database_schema.md](protocol/database_schema.md) | SQLite schema（`faces` + `face_templates`）、数据字典、**512 维 embedding 布局**、`id` / `name` 字段语义、运维 SQL、版本兼容 |
| [protocol/config.md](protocol/config.md) | 全项目配置项、参数默认值与路径约定 |

## 5. 外设与依赖适配（peripheral/）

| 文档 | 内容 |
|---|---|
| [peripheral/third_party.md](peripheral/third_party.md) | vendored 依赖、系统依赖、硬件平台、模型清单 |

## 6. 问题排查与发布

| 文档 | 内容 |
|---|---|
| [FAQ/troubleshooting.md](FAQ/troubleshooting.md) | 构建 / 运行 / 数据一致性排查；**Q23 专门讲"不同人之间相似度异常偏高"的检测解码缺陷** |
| [release_guide.md](release_guide.md) | 版本规则、发布流程、检查清单、Release Notes 模板 |

## 7. 已废弃文档（保留占位，勿参考）

以下文件的内容**与当前实现不一致**，已替换为废弃说明并指向替代文档。
确认无需保留时可直接删除：

| 文件 | 原因 | 替代 |
|---|---|---|
| [USAGE_AND_TESTING.md](USAGE_AND_TESTING.md) | 早期使用与测试记录，命令路径与默认值均已变化 | setup / module/standalone / FAQ |
| [TODO.md](TODO.md) | 实现过程的临时 TODO 与审查记录 | architecture / protocol/database_schema / release_guide |
| [superpowers/plans/2026-09-09-...plan.md](superpowers/plans/2026-09-09-face-recognition-node-plan.md) | 立项阶段的实现计划 | architecture / module / services |
| [superpowers/specs/2026-09-09-...design.md](superpowers/specs/2026-09-09-face-recognition-node-design.md) | 立项阶段的设计稿 | architecture / services |

```bash
# 如确认删除
rm -f docs/USAGE_AND_TESTING.md docs/TODO.md
rm -rf docs/superpowers
```

> [img/](img/) 是文档与根 `README.md` 引用的图片资源（`logo.jpg`），**不要删除**。

---

## 维护约定

1. **文档必须与代码一致**。改动行为、默认值、接口、数据格式后，同步更新
   `protocol/`（契约类）与对应的 `services/` / `module/` 文档。
2. **不写运行结果与测试数值**。文档只描述"行为、默认值、判定语义、排查方法"，
   不记录某次实测分数、FPS、耗时——这些会随机器与数据变化而过期。需要量化时，
   把可复现的命令写清楚，让读者自己测。
3. **不写失效内容**。过期的章节要么合并进正确位置，要么按第 7 节的方式标记废弃；
   不要留两处互相矛盾的说明。
4. **两个前端 / 特征空间的变更属于破坏性变更**。任何影响取脸的改动
   （对齐开关、检测框或关键点解码、预处理配方、更换模型）都必须同时说明
   **"需执行 `backfill --all` 重建全部特征"**，否则会出现
   "本人识别不出、他人也能识别"。
5. 新增文档时更新本索引与根目录 `README.md` 的链接。
6. 命令示例应可直接复制执行；路径使用仓库根目录相对路径。
