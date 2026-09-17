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

`FaceDetection` 含 `bbox`、`confidence` 与 `landmarks`（5 组 `(x, y)`，共 10 个 float）。

---

## 2. Backend 自动判定

`initialize()` 加载模型后读取输出张量**数量**：

| 输出数量 | backend | 对应模型 |
|---|---|---|
| `9` | `retinaface` | `models/det_10g.onnx` |
| 其他 | `yolov8` | 单输出 `[1, 84, N]`，如 `models/yolov8n.onnx` |

调用 `backend()` 可查询当前生效的实现，便于排障。

---

## 3. RetinaFace / SCRFD 分支

`det_10g.onnx` 属于 **SCRFD** 系列。它与经典 RetinaFace 共用 9 输出布局，但**解码
公式不同**，实现时必须区分（见 §3.3）。

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

### 3.2 预处理：letterbox（等比缩放 + 补边）

```cpp
BGR → 等比缩放 s = min(W/w, H/h) → 居中贴到 W×H 黑色画布 → RGB
    → blobFromImage(scale=1/128, mean=127.5)
```

**为什么必须 letterbox**：网络输入是正方形。若用普通 `resize` 把非正方形帧硬拉成
正方形，人会被拉伸——1280×720 的帧横向压 4×、纵向只压 2.25×，**畸变 1.78×**。
关键点回归是在**无畸变**人脸上训练的，畸变会直接劣化 5 点精度，而下游的对齐
（默认启用，见 [face_recognizer.md §2.1](face_recognizer.md)）完全依赖关键点精度。

坐标反算（`LetterboxInfo`）：

```
x_img = (x_net - pad_x) / scale
y_img = (y_net - pad_y) / scale
```

> **推论（对选型很重要）**：`--input-size` 越小，人脸在网络输入里占的像素越少。
> 1280×720 采集 + `--input-size 320` ⇒ 人脸被缩到 **1/4**，关键点精度明显下降。
> 16:9 采集建议配 `--input-size 640`（此时帧恰好是输入的 2 倍）。

### 3.3 解码（SCRFD 约定）

```
strides[3]  = {8, 16, 32}
grid        = input_size / stride，每格 2 个 anchor（anchor_type = 0 / 1）
cx, cy      = gx * stride, gy * stride          // 无 +0.5 单元格偏移

x1 = cx - bbox[i][0] * stride
y1 = cy - bbox[i][1] * stride
x2 = cx + bbox[i][2] * stride
y2 = cy + bbox[i][3] * stride

kps_x[k] = cx + land[i][2k]   * stride
kps_y[k] = cy + land[i][2k+1] * stride          // k = 0..4
```

随后：

1. 用 letterbox 参数把坐标映射回原图（`x_img = (x_net - pad_x) / scale`）；
2. clamp 到图像范围；
3. 过滤：`conf ≥ confidence_threshold` 且框宽高 `≥ 4`。

> ⚠️ **不要套用经典 RetinaFace 的 variance / anchor-size 公式**
> （`x1 = cx - raw[0] * variance[0] * (stride*4)` 这类）。SCRFD 的回归头输出的距离
> 已经按特征 stride 归一化，乘 `stride` 即可；再叠一层 `variance(0.1/0.2) × anchor宽高`
> 会把解码出的距离整体缩到 **0.4 倍（y 方向 0.8 倍）**，后果是：
>
> | 受影响对象 | 现象 |
> |---|---|
> | 边界框 | 缩到真值约 60%，**下巴与额头被切掉** |
> | 5 点关键点 | 被压向 anchor 中心（双眼间距只剩真值约 0.55） |
> | 下游对齐 | 相似变换为了把小点距放大到模板尺寸而**放大画面**，裁剪里只剩眼睛和鼻子 |
> | 最终表现 | 特征区分度崩塌，**不同的人之间相似度高达 0.6~0.7，而本人只有 0.6** |
>
> 该缺陷已在 `postprocessRetinaFace()` 中修复，排查过程见
> [FAQ/troubleshooting.md](../FAQ/troubleshooting.md)。

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
该分支**不输出关键点**，因此调用方只能走 bbox 裁剪前端。

---

## 5. 参数影响

| 参数 | 默认 | 影响 |
|---|---|---|
| `confidence_threshold` | `0.5` | 调高 → 误检更少、召回更低 |
| `nms_threshold` | `0.5` | 调低 → 重叠框抑制更激进 |
| `input_size` | `640` | **性能主开关**：N 减半约 4× 提速，小脸召回下降 |

> 预处理与推理**始终走 letterbox**（等比 + 补边），不会拉伸人脸；
> `setInputSize()` 只影响网络输入边长，不改变这一行为。

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
if (!detector.initialize("models/det_10g.onnx", 0.5f, 0.5f, 640)) {
    std::cerr << detector.getLastError() << std::endl;
}
std::cout << "backend = " << detector.backend() << std::endl;   // retinaface

cv::Mat image = cv::imread("alice.jpg");
for (const auto& det : detector.detect(image, 10)) {
    // det.bbox / det.confidence / det.landmarks (10 floats)
}
```

---

## 8. 相关文档

- [人脸识别模块](face_recognizer.md)
- [人脸库模块](face_database.md)
- [核心算法库模块说明](../module/core.md)
