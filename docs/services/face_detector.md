# 人脸检测模块（FaceDetector）

`face_recognition::FaceDetector` 负责在图像中定位人脸，输出边界框、置信度与 5 点关键点。

- 头文件：`src/face_recognition_core/include/face_recognition_core/face_detector.hpp`
- 实现：`src/face_recognition_core/src/face_detector.cpp`
- 推理引擎：**ONNX Runtime（C++）**

> 之所以用 ONNX Runtime 而不是 OpenCV DNN：`det_10g.onnx` 的输出包含动态
> `Reshape`，OpenCV 4.5 的 DNN 模块无法执行。OpenCV 仅用于预处理与 blob 构造。

---

## 1. 接口

```cpp
class FaceDetector {
public:
    bool initialize(const std::string& model_path,
                    float confidence_threshold = 0.5f,
                    float nms_threshold        = 0.5f,
                    int   input_size           = 640);

    std::vector<FaceDetection> detect(const cv::Mat& image, int max_faces = 10);
    void        setInputSize(int width, int height);   // 仅接受正方形
    std::string backend() const;                       // "retinaface" | "yolov8" | "unknown"
    std::string getLastError() const;
};
```

---

## 2. Backend 自动判定

`initialize()` 加载模型后读取输出张量**数量**：

| 输出数量 | backend | 对应模型 |
|---|---|---|
| `9` | `retinaface` | `models/det_10g.onnx` |
| 其他 | `yolov8` | 单输出 `[1, 84, N]`，如 `models/yolov8n.onnx` |

调用 `backend()` 可查询当前生效的实现，便于排障。

---

## 3. RetinaFace 分支

### 3.1 输入张量布局

ONNX Runtime 按图输出声明顺序返回 9 个张量。**模型对 640×640 输入声明的是静态
形状**，实际形状随 `input_size` 变化——anchor 数 = `(W/stride) × (H/stride) × 2`：

| 索引 | 张量 | 640×640 时 | 320×320 时 | 含义 |
|---|---|---|---|---|
| 0 / 1 / 2 | `conf_0` `conf_1` `conf_2` | `[12800,1]` `[3200,1]` `[800,1]` | `[3200,1]` `[800,1]` `[200,1]` | 置信度（**已是 softmax 概率**） |
| 3 / 4 / 5 | `bbox_0` `bbox_1` `bbox_2` | `[12800,4]` `[3200,4]` `[800,4]` | `[3200,4]` `[800,4]` `[200,4]` | 框回归偏移 |
| 6 / 7 / 8 | `land_0` `land_1` `land_2` | `[12800,10]` `[3200,10]` `[800,10]` | `[3200,10]` `[800,10]` `[200,10]` | 5 点关键点 |

因此实现**不硬编码 12800 / 3200 / 800**，而是：

1. 先按当前 `input_width` / `input_height` 与 `strides = {8, 16, 32}` 算出每层
   期望的 anchor 数 `expected[l]`；
2. 用 `rows` 匹配 layer，用 `cols`（`1` / `4` / `10`）匹配张量种类；
3. 仅当都匹配不上时才回退到 640 的固定布局（兼容固定形状的导出）。

> **历史缺陷（已修复）**：早期实现按固定行数 `12800 / 3200 / 800` 判定 layer。
> 当 `--input-size` 取 320 时实际行数为 `3200 / 800 / 200`，于是 layer 被整体错位
> 且最小一层被丢弃，**检测结果恒为空**（表现为"完全检测不到人脸"），同时
> ONNX Runtime 会为每个输出打印 `VerifyOutputSizes` 警告。

### 3.2 预处理

```cpp
BGR → RGB → resize(input_width × input_height) → blobFromImage(scale=1/128, mean=127.5)
```

### 3.3 解码

```
stride[3]         = {8, 16, 32}
variance[2]       = {0.1, 0.2}
anchor 宽高        = stride*4（anchor_type 1 高度 ×1.5）
grid              = input_size / stride，每格 2 个 anchor
cx, cy            = (gx + 0.5) * stride, (gy + 0.5) * stride
x1 = cx - dx1*variance[0]*aw      x2 = cx + dx2*variance[1]*aw
y1 = cy - dy1*variance[0]*ah      y2 = cy + dy2*variance[1]*ah
```

坐标按 `img_w/input_w`、`img_h/input_h` 缩放回原图，并 clamp 到图像范围。
过滤条件：`conf ≥ confidence_threshold` 且框的 `w ≥ 4` 且 `h ≥ 4`。

关键点同样按 anchor + variance 反算，共 5 组 `(x, y)`。

### 3.4 NMS 与截断

自定义贪心 NMS，按置信度降序，`IoU > nms_threshold` 即抑制；
最后按置信度截断到 `max_faces` 条。

---

## 4. YOLOv8 分支

支持两种内存布局：

| 布局 | 判定 | 解码 |
|---|---|---|
| `[1, 84, N]`（转置） | `rows < cols` | `data[i*stride + c]` |
| `[1, N, 84]` | `rows >= cols` | `data[c*num_detections + i]` |
| 2 维 | `dims == 2` | 按 `rows/cols` 解析 |

置信度取所有类别的最大值（YOLOv8-face 通常仅 1 类），随后同样做 NMS 与截断。

---

## 5. 参数影响

| 参数 | 默认 | 影响 |
|---|---|---|
| `confidence_threshold` | `0.5` | 调高 → 误检更少、召回更低 |
| `nms_threshold` | `0.5` | 调低 → 重叠框抑制更激进 |
| `input_size` | `640` | **性能主开关**：N 减半约 4× 提速，小脸召回下降 |

> `detect()` 内部使用 `resize` 而非 letterbox，长宽比会被拉伸。
> 若需保持比例，应在调用前对原图做预处理。

---

## 6. 异常与日志策略

| 场景 | 行为 |
|---|---|
| 模型加载失败 | `initialize()` 返回 `false`，`getLastError()` 给出原因 |
| 推理抛异常 | 捕获 `Ort::Exception` / `std::exception`，返回空结果 |
| 重复同类异常 | 仅当错误消息**变化**时打印，避免日志刷屏 |
| `outputs.size() < 9` | 记录 `"RetinaFace: expected 9 outputs, got N"` 并返回空 |

**ONNX Runtime 日志级别**：`Ort::Env` 与 `SessionOptions::SetLogSeverityLevel`
均设为 `ORT_LOGGING_LEVEL_ERROR`。原因：`det_10g.onnx` 声明的是静态输出形状，
只要 `input_size ≠ 640`，ORT 就会为每个输出打印一条 `VerifyOutputSizes` 警告——
每帧 9~21 条、完全无害但会淹没控制台。真正的推理错误仍会通过异常路径上报。

---

## 7. 使用示例

```cpp
#include "face_recognition_core/face_detector.hpp"
#include <opencv2/imgcodecs.hpp>

face_recognition::FaceDetector detector;
if (!detector.initialize("models/det_10g.onnx", 0.5f, 0.5f, 320)) {
    std::cerr << detector.getLastError() << std::endl;
}
std::cout << "backend = " << detector.backend() << std::endl;   // retinaface

cv::Mat image = cv::imread("test_2.png");
for (const auto& det : detector.detect(image, 10)) {
    // det.bbox / det.confidence / det.landmarks (10 floats)
}
```

---

## 8. 相关文档

- [人脸识别模块](face_recognizer.md)
- [人脸库模块](face_database.md)
- [核心算法库模块说明](../module/core.md)
