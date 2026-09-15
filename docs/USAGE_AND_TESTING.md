# 人脸识别项目使用与测试

## 模型

ROS2 默认使用：

- `models/det_10g.onnx`：RetinaFace 检测模型
- `models/w600k_r50.onnx`：ArcFace 识别模型，输出 512 维 embedding

## 构建

```bash
./build.sh CORE
source /opt/ros/humble/setup.bash
colcon build --packages-select face_recognition_core face_recognition_ros2_interfaces face_recognition_ros2
source install/setup.bash
```

## 数据库

默认路径为 `/tmp/face_db/faces.db`，图片目录为 `/tmp/face_db/faces`。录入图片后，ROS2 节点启动时会为缺少 embedding 的历史记录自动提取特征。

## 启动实时识别

已有图像话题时：

```bash
ros2 launch face_recognition_ros2 face_recognition.launch.py \
  detection_model:=$PWD/models/det_10g.onnx \
  recognition_model:=$PWD/models/w600k_r50.onnx \
  launch_viewer:=false enable_stream_server:=false
```

USB 摄像头：

```bash
ros2 launch face_recognition_ros2 usb_cam_face.launch.py \
  detection_model:=$PWD/models/det_10g.onnx \
  recognition_model:=$PWD/models/w600k_r50.onnx
```

识别结果话题：`/face/recognition_result`。可用 `ros2 topic echo /face/recognition_result` 查看。

## 离线测试

使用测试图片 `test_2.png`：

```bash
g++ -std=c++17 test_runner.cpp -Isrc/face_recognition_core/include \
  -I/home/qstdc/anaconda3/pkgs/onnxruntime-cpp-1.16.3-h7b6976b_4_cuda/include/onnxruntime \
  $(pkg-config --cflags --libs opencv4) \
  -L/home/qstdc/.local/onnxruntime/lib -lonnxruntime \
  -Lsrc/face_recognition_core/build -lface_recognition_core \
  -o /tmp/test_runner
/tmp/test_runner
```

测试应输出 2 张检测人脸、512 维 embedding，并显示 `db_match=test_person`。

实时测试时应确认：输入图像话题有数据、节点日志显示两个模型初始化成功、识别结果话题持续发布。ROS2 日志目录不可写时可设置 `ROS_LOG_DIR=/tmp/roslog`。
