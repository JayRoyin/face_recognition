# 人脸识别模块（FaceRecognizer）

`face_recognition::FaceRecognizer` 负责把人脸区域转换为 **512 维 L2 归一化
embedding**，并提供余弦相似度计算。

- 头文件：`src/face_recognition_core/include/face_recognition_core/face_recognizer.hpp`
- 实现：`src/face_recognition_core/src/face_recognizer.cpp`
- 推理引擎：**OpenCV DNN**（`cv::dnn::readNetFromONNX`，CPU target）
- 模型：`models/w600k_r50.onnx`（ArcFace / InsightFace buffalo_l）

---

## 1. 接口

```cpp
class FaceRecognizer {
public:
    bool initialize(const std::string& model_path);

    std::vector<float> extract_embedding(const cv::Mat& image, const BoundingBox& bbox);
    float compute_similarity(const std::vector<float>& emb1,
                             const std::vector<float>& emb2);

    void        setInputSize(int width, int height);   // 仅接受正方形
    std::string getLastError() const;
};
```

---

## 2. 处理流程

```
原图 + 检测框 bbox
      │
      ▼  ① 扩展裁剪区域
   x = bbox.x - w*0.2 ,  y = bbox.y - h*0.2
   w' = w*1.4          ,  h' = h*1.4
   （左上角 clamp 到 0，右下角 clamp 到图像边界）
      │
      ▼  ② resize 到 112×112
      │
      ▼  ③ 归一化：pixel/255 → (p - 0.5) / 0.5   ⇒ 值域 [-1, 1]
      │
      ▼  ④ blobFromImage（scale=1.0，不做 mean 减除）
      │
      ▼  ⑤ ArcFace forward → 512-d 原始输出
      │
      ▼  ⑥ L2 归一化：v /= ||v||₂   （范数 ≤ 1e-8 时跳过）
      │
      ▼
512 维 float 向量（归一化后）
```

### 为什么先归一化再算相似度

embedding 已 L2 归一化 ⇒ 内积等于余弦相似度，可直接比较，无需每次重新算模长。

---

## 3. 相似度语义

```cpp
float face_recognition::compute_similarity(const std::vector<float>& a,
                                           const std::vector<float>& b);
```

- 长度不一致或为空 → 返回 `0.0f`
- 否则计算点积并截断到 `[0, 1]`
- `1.0` 表示完全一致（同一张图自比对实测可达 `0.999999`）

**阈值经验**（`recognition_threshold` / `confidence_threshold`）。

以下为本项目自带模型（`det_10g` + `w600k_r50`）的实测相似度：

| 场景 | 相似度 |
|---|---|
| 同图自比对 | 0.9997 |
| 同一人，两张清晰照片 | 0.7539 |
| 同一人，640×480 + JPEG q75（≈ 摄像头条件） | 0.6936 |
| 同一人，320×240 + JPEG q55 | 0.6236 |
| 同一人，缩放 50% + 旋转 12° | 0.4982 |
| 不同人（impostor） | 0.23 ~ 0.40 |

据此的取值建议：

| 取值 | 效果 |
|---|---|
| `0.45` | 宽松，远距离 / 小脸召回更高，误识别风险上升 |
| **`0.5`** | **默认**：与"不同人"上界（0.40）留有 0.1 间隔，同时容忍摄像头降质 |
| `0.6` | 严格，误识别更少，遮挡 / 侧脸易漏 |
| `0.7` | 过严：**实测会漏判几乎所有现场摄像头画面**（画面上显示 `Unknown`） |

---

## 4. 输入尺寸

`setInputSize(width, height)` 仅接受 `width == height` 的正方形；非正方形会写入
`last_error = "FaceRecognizer requires square input"` 且不生效。

默认 `112 × 112`，与 ArcFace 训练输入一致，**一般不需要修改**。

---

## 5. 异常处理

| 场景 | 行为 |
|---|---|
| 模型加载失败 | `initialize()` 返回 `false`，`last_error` 含 `Failed to load model: ...` |
| 裁剪区域非法（w/h ≤ 0） | 返回空向量 `{}` |
| 推理异常 | 捕获后打印 `[FaceRecognizer] extract() exception: ...`，返回空向量 |

调用方应判断返回值是否为空，为空表示该人脸**没有可用特征**，不可参与比对。

---

## 6. 使用示例

```cpp
#include "face_recognition_core/face_detector.hpp"
#include "face_recognition_core/face_recognizer.hpp"

face_recognition::FaceDetector   detector;
face_recognition::FaceRecognizer recognizer;
detector.initialize("models/det_10g.onnx");
recognizer.initialize("models/w600k_r50.onnx");

cv::Mat image = cv::imread("alice.jpg");
auto dets = detector.detect(image, 1);
if (!dets.empty()) {
    auto emb = recognizer.extract_embedding(image, dets.front().bbox);
    // emb.size() == 512
    float sim = recognizer.compute_similarity(emb, other_emb);
}
```

---

## 7. 与数据库的配合

`FaceDatabase::find_matching_face(embedding, threshold)` 会对库内每条记录调用
同样的相似度计算，返回**相似度最高且严格大于阈值**的记录：

```cpp
auto match = database.find_matching_face(emb, 0.7f);
if (match) {
    // match->id / match->name / match->title / match->scene / match->map_location
}
```

**库内记录 embedding 为空时不会命中**，需通过 `backfill` 或节点启动时的自动补全修复。

---

## 8. 相关文档

- [人脸检测模块](face_detector.md)
- [人脸库模块](face_database.md)
- [数据库 schema](../protocol/database_schema.md)
