# 人脸识别节点实现 TODO

> 创建时间: 2026-09-10
> 项目: face_recognition_node
> 目标: ROS1/ROS2 双版本 C++ 人脸识别节点

---

## 阶段 0: 模型准备

- [x] 0.1 创建 models 目录结构
- [x] 0.2 下载 YOLOv8-Face ONNX 模型 (脚本已创建)
- [x] 0.3 下载 ArcFace buffalo_l 模型 (脚本已创建)
- [x] 0.4 创建模型下载脚本

## 阶段 1: 核心算法库 (C++)

- [x] 1.1 创建项目结构
  - [x] face_recognition_core/include/
  - [x] face_recognition_core/src/

- [x] 1.2 实现 types.hpp 类型定义
- [x] 1.3 实现 face_detector.hpp/.cpp (YOLO OpenCV DNN)
- [x] 1.4 实现 face_recognizer.hpp/.cpp (ArcFace OpenCV DNN)
- [x] 1.5 实现 face_database.hpp/.cpp (SQLite3)
- [x] 1.6 创建 face_recognition_core/CMakeLists.txt
- [x] 1.7 创建模型转换脚本

## 阶段 2: ROS2 软件包

- [x] 2.1 创建 ROS2 包配置 (package.xml, CMakeLists.txt)
- [x] 2.2 创建消息和服务类型 (msg/, srv/)
- [x] 2.3 实现 ROS2 节点 (node.hpp, node.cpp, main.cpp)
- [x] 2.4 创建启动文件和配置
- [ ] 2.5 编译测试

## 阶段 3: ROS1 软件包

- [x] 3.1 创建 ROS1 包配置
- [x] 3.2 创建消息和服务类型
- [x] 3.3 实现 ROS1 节点
- [x] 3.4 创建启动文件和配置
- [ ] 3.5 编译测试

## 阶段 4: Web 管理界面

- [x] 4.1 创建 HTTP 服务器 (server.hpp, server.cpp)
- [x] 4.2 创建 Web 前端 (index.html)
- [x] 4.3 实现 REST API (GET/POST/DELETE)
- [ ] 4.4 编译测试

## 阶段 5: 测试与文档

- [x] 5.1 创建测试脚本 (模型下载脚本)
- [x] 5.2 编写 README.md
- [ ] 5.3 创建部署文档
- [ ] 5.4 性能基准测试

---

## 里程碑

| 阶段 | 状态 |
|------|------|
| Phase 0 | ✅ 完成 |
| Phase 1 | ✅ 完成 |
| Phase 2 | ⚠️ 待编译测试 |
| Phase 3 | ⚠️ 待编译测试 |
| Phase 4 | ⚠️ 待编译测试 |
| Phase 5 | ⚠️ 进行中 |

---

## 依赖项

### 系统依赖
```bash
sudo apt-get install libopencv-dev libsqlite3-dev uuid-dev
```

### Python 依赖 (模型转换)
```bash
pip install ultralytics insightface onnx
```

### ROS2 依赖
```bash
sudo apt install ros-humble-cv-bridge ros-humble-sensor-msgs ros-humble-std-msgs ros-humble-std-srvs
```

---

## 待完成事项

- [ ] 编译测试 ROS2 软件包
- [ ] 编译测试 ROS1 软件包
- [ ] 编译测试 Web 服务器
- [ ] 下载实际 ONNX 模型文件
- [ ] 创建部署文档

## 代码审查整改（2026-09-11）

- [x] 修复 ONNX Runtime `Session::Run` 参数类型导致的核心库编译失败
- [x] ROS1/ROS2 AddFace 从图片提取 embedding
- [x] 检查 RetinaFace 输出 tensor rank
- [x] 递归创建数据库和图片目录
- [x] Web 请求体大小限制
- [x] Web JSON 转义
- [x] Web 前端 DOM 安全写入
- [ ] Web API 认证、CSRF 和 HTTPS
- [ ] FaceDatabase 并发互斥、事务和索引优化
- [ ] 完善删除失败回滚与孤立文件清理
- [x] 下载脚本不再强制覆盖代理
- [ ] 修复构建脚本 ALL 部分失败仍报告成功
- [ ] 增加核心、ROS、Web 自动化回归测试

### ROS 离线验证记录

- [x] 使用 `models/det_10g.onnx`（RetinaFace）检测数据库图片：1 张人脸
- [x] 使用 `models/w600k_r50.onnx`（ArcFace）生成 512 维 embedding
- [x] 为现有 `Royin` 数据库记录补写 embedding（2048 字节）
- [x] 数据库同图匹配相似度达到 0.999999
- [x] 非人脸测试图检测结果为 0 张
- [x] ROS2 节点启动并完成模型、数据库初始化
