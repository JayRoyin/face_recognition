# 文档索引（docs）

本目录是 Face Recognition Node 的**全量文档归档**。按用途分为以下几类。

---

## 快速入口

| 我想…… | 去看 |
|---|---|
| 把项目跑起来 | [setup.md](setup.md) · [module/quick_start.md](module/quick_start.md) |
| 了解整体设计 | [architecture.md](architecture.md) |
| 用某个模块 | [module/](module/) |
| 查接口 / 协议 / 配置 | [protocol/](protocol/) |
| 查核心算法接口 | [services/](services/) |
| 接入第三方依赖 / 换平台 | [peripheral/third_party.md](peripheral/third_party.md) |
| 排查故障 | [FAQ/troubleshooting.md](FAQ/troubleshooting.md) |
| 发布版本 | [release_guide.md](release_guide.md) |

---

## 1. 入门与总览

| 文档 | 内容 |
|---|---|
| [setup.md](setup.md) | 源码开发快速开始：依赖、ONNX Runtime、模型、一键构建、跨平台编译 |
| [architecture.md](architecture.md) | 架构说明：分层、数据流、存储模型、并发模型、关键设计决策 |

## 2. 专项模块文档（module/）

| 文档 | 内容 |
|---|---|
| [module/quick_start.md](module/quick_start.md) | 5 个模块的最短跑通路径 + 组合工作流 |
| [module/core.md](module/core.md) | 核心算法库：目录、依赖、数据类型、公共 API |
| [module/standalone.md](module/standalone.md) | 非 ROS 实时识别 CLI：命令、输入源、参数、性能调优、backfill |
| [module/ros2.md](module/ros2.md) | ROS2 节点 + viewer + MJPEG 推流：编译、启动、参数、验证 |
| [module/ros1.md](module/ros1.md) | ROS1 节点：编译、启动、参数、接口、示例 |
| [module/web.md](module/web.md) | Web 人脸库后台：编译、启动、使用教程、安全性 |

## 3. 核心业务模块文档（services/）

| 文档 | 内容 |
|---|---|
| [services/face_detector.md](services/face_detector.md) | 人脸检测：backend 判定、RetinaFace/YOLOv8 后处理、NMS |
| [services/face_recognizer.md](services/face_recognizer.md) | 人脸识别：预处理、512 维 embedding、相似度语义 |
| [services/face_database.md](services/face_database.md) | 人脸库：CRUD、1:N 检索、回填、并发与文件布局 |

## 4. 接口、协议与配置（protocol/）

| 文档 | 内容 |
|---|---|
| [protocol/README.md](protocol/README.md) | 接口总览、默认端口与路径 |
| [protocol/ros_interfaces.md](protocol/ros_interfaces.md) | ROS Topic / Service / Message（ROS1 与 ROS2） |
| [protocol/web_api.md](protocol/web_api.md) | 人脸库 HTTP API 与 MJPEG 推流接口 |
| [protocol/database_schema.md](protocol/database_schema.md) | SQLite schema、数据字典、运维 SQL |
| [protocol/config.md](protocol/config.md) | 全项目配置项、参数默认值与路径约定 |

## 5. 外设与依赖适配（peripheral/）

| 文档 | 内容 |
|---|---|
| [peripheral/third_party.md](peripheral/third_party.md) | vendored 依赖、系统依赖、硬件平台、模型清单 |

## 6. 问题排查与发布

| 文档 | 内容 |
|---|---|
| [FAQ/troubleshooting.md](FAQ/troubleshooting.md) | 构建 / 运行 / 数据一致性问题排查 |
| [release_guide.md](release_guide.md) | 版本规则、发布流程、检查清单、Release Notes 模板 |

## 7. 历史归档

| 文档 | 说明 |
|---|---|
| [USAGE_AND_TESTING.md](USAGE_AND_TESTING.md) | 早期使用与测试记录（部分命令路径已变化，内容已并入上述文档） |
| [TODO.md](TODO.md) | 实现过程 TODO 与代码审查整改记录 |
| [superpowers/](superpowers/) | 设计文档与实现计划（plans / specs） |
| [img/](img/) | 文档与 README 使用的图片资源 |

---

## 维护约定

1. 新增文档时**必须**更新本索引与根目录 `README.md` 的链接
2. 修改代码行为后，同步更新 `protocol/` 下对应契约文档
3. 命令示例应可直接复制执行，路径使用仓库根目录相对路径
