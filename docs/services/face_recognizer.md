# 人脸识别模块（FaceRecognizer）

`face_recognition::FaceRecognizer` 负责把人脸区域转换为 **512 维 L2 归一化
embedding**，并提供余弦相似度计算。

- 头文件：`src/face_recognition_core/include/face_recognition_core/face_recognizer.hpp`
- 实现：`src/face_recognition_core/src/face_recognizer.cpp`
- 推理引擎：**OpenCV DNN**（`cv::dnn::readNetFromONNX`，CPU target）
- 模型：`models/w600k_r50.onnx`（ArcFace / InsightFace buffalo_l）

> 模型 I/O：输入 `input.1`，形状 `[-1, 3, 112, 112]`；输出 `683`，形状 `[1, 512]`。
> 用 OpenCV DNN 与 ONNX Runtime 推理**结果一致**，因此识别侧保留 OpenCV DNN
> （少一个运行时依赖），检测侧因动态 `Reshape` 才必须用 ONNX Runtime。

---

## 1. 接口

```cpp
class FaceRecognizer {
public:
    bool initialize(const std::string& model_path);

    // 不带关键点：直接走 bbox 裁剪前端
    std::vector<float> extract_embedding(const cv::Mat& image, const BoundingBox& bbox);

    // 带关键点：优先走 5 点对齐前端，used_alignment 回传实际用了哪一个
    std::vector<float> extract_embedding(const cv::Mat& image,
                                        const BoundingBox& bbox,
                                        const std::vector<float>& landmarks,
                                        bool* used_alignment = nullptr);

    float compute_similarity(const std::vector<float>& emb1,
                             const std::vector<float>& emb2);

    void        setInputSize(int width, int height);   // 仅接受正方形
    void        setAlignmentEnabled(bool enabled);     // 默认 true
    bool        alignmentEnabled() const;
    void        alignmentStats(int* aligned, int* fallback) const;
    std::string getLastError() const;
};
```

---

## 2. 处理流程

```
原图 + 检测框 bbox + 5 点关键点
      │
      ▼  ① 取脸（两个前端，二选一）
      ├─ 对齐前端（默认）：相似变换 warp 到标准 112×112 模板，见 §2.1
      └─ bbox 前端：x = bbox.x - w*0.2,  y = bbox.y - h*0.2
                    w' = w*1.4,          h' = h*1.4
                    （左上角 clamp 到 0，右下角 clamp 到图像边界）
                    再 resize 到 112×112
      │
      ▼  ② blobFromImage(scale = 1/255, mean = 0, swapRB = true)
        BGR → RGB，并缩放到 [0, 1]
      │
      ▼  ③ ArcFace forward → 512-d 原始输出
      │
      ▼  ④ L2 归一化：v /= ||v||₂   （范数 ≤ 1e-8 时跳过）
      │
      ▼
512 维 float 向量（归一化后）
```

> ⚠️ **两个前端产生的 embedding 不兼容，绝不可混用在一个库里。**
> 前端切换后必须 `face_recognition_app backfill --all` 用存档缩略图重建全部特征，
> 或清库重新录入。混用会让**本人比陌生人更难被识别**。
>
> 上层（`RecognitionPipeline::prepare_embedding()`）通过 `used_alignment`
> 回传值来杜绝混用：`allow_unaligned = false`（默认）时，无法对齐的人脸直接
> **拒绝入库 / 拒绝识别**，而不是悄悄回退到 bbox 前端。

### 2.1 5 点关键点对齐（**默认启用**）

`FaceDetector` 输出 5 点关键点（`FaceDetection::landmarks`，10 个 float）。本模块用
**闭式最小二乘相似变换**把脸 warp 到 ArcFace 的标准 112×112 模板（目标点与
insightface `norm_crop` 的 `arcface_dst` 完全一致）：左眼、右眼、鼻尖、左嘴角、右嘴角。

```
原图 + 5 点关键点
      │  ① 可靠性门控（任一不满足 ⇒ 放弃对齐）
      │     · alignment_enabled 为 true
      │     · landmarks 恰好 10 个 float 且全部有限
      │     · 每个点不超出图像 [-0.2W, 1.2W] × [-0.2H, 1.2H]
      │     · 双眼间距 ≥ 18 px
      │     · 变换尺度 ∈ (0.05, 2.0]
      │     · 拟合残差 RMS ≤ max(2.5, 0.10 × 双眼间距)
      ▼  ② 闭式最小二乘相似变换 → 112×112（BORDER_REPLICATE）
      ▼  ③ 与 bbox 路径完全相同的归一化 + 推理
```

**为什么不用 `cv::estimateAffinePartial2D`**：它内部跑 RANSAC，只有 5 个点时
一个噪声点就可能让"2 点共识"胜出，得到完全错误的尺度（表现为裁剪被严重放大）。
闭式解对全部 5 点做整体最小二乘，与 insightface 行为一致。

**各门控的取值理由**

| 门控 | 取值 | 理由 |
|---|---|---|
| 双眼间距 | `≥ 18 px` | 模板双眼距为 35.24 px，18 px 对应放大 ~2×；再小就会把关键点噪声放大成一张"看起来像脸但不是本人"的几何图形 |
| 上界尺度 | `≤ 2.0` | 同上：源脸不足模板一半时必须上采样，凭空补不出细节 |
| 下界尺度 | `> 0.05` | **仅防御退化值**。人脸**大于**模板（需要缩小）完全正常——任何常规自拍的眼距都有 200+ px、对应尺度 0.15 左右；若把下界设成 0.3 这类"接近 1"的值，绝大多数照片会被踢出对齐路径，导致库内前端混用 |
| 拟合残差 | `≤ max(2.5, 0.10 × 双眼距)` | 残差在 112×112 目标空间计算；侧脸时一侧眼被遮挡、5 点几何已不满足正面模板，此时宁可放弃对齐 |

> **常见误区**：早期版本曾把对齐默认关闭，理由是"检测器 5 点精度不够、对齐反而
> 拉低本人分数"。该结论的实际成因是**检测器关键点解码用错了公式**（被压到真值的
> 约 0.4 倍，见 [face_detector.md §3.3](face_detector.md)）——点挤在中心，变换就会
> 把画面放大到只剩眼睛和鼻子。解码修复后对齐是精度更高的前端。

### 为什么先归一化再算相似度

embedding 已 L2 归一化 ⇒ 内积等于余弦相似度，可直接比较，无需每次重新算模长。

---

## 3. 相似度语义

```cpp
float face_recognition::compute_similarity(const std::vector<float>& a,
                                           const std::vector<float>& b);
```

- 长度不一致或为空 → 返回 `0.0f`
- 否则返回点积（两个输入已 L2 归一化，故即余弦相似度）
- 只做**上界**截断到 `1.0`（吸收浮点误差）
- **负值有意义、不被截断**：不同身份通常落在 `0` 以下。旧实现把结果截断到
  `[0, 1]`，会把"完全不同"和"略微不同"压成同一个值，既丢失判定余量，也会让
  cohort 归一化的中位数 / MAD 退化为 0。

| 取值 | 含义 |
|---|---|
| `≈ 1.0` | 同一张图自比对 |
| `0.5 ~ 0.8` | 同一人的不同照片（正面、光照良好） |
| `0 ~ 0.2` | 不同的人（同一族裔 / 性别 / 年龄段） |
| `< 0` | 差异明显的不同人 |

`FaceRecognizer::compute_similarity()` 与上述自由函数**同一实现**，不再各自维护。

### 阈值取值

`recognition_threshold` / ROS 节点的 `confidence_threshold` 决定「相似度 ≥ 多少才算同一个人」。

| 取值 | 效果 |
|---|---|
| `0.45` | 宽松，远距离 / 小脸召回更高，误识别风险上升 |
| **`0.5`** | **默认**，位于"本人"与"不同人"两簇之间 |
| `0.6` | 严格：进一步压低误识，遮挡 / 侧脸易漏 |
| `0.7` | 过严：会漏判大量现场画面（画面上显示 `Unknown`） |

> 阈值本身不能弥补前端问题。若"本人"分数明显低于"不同人"上界，说明特征或前端
> 出了问题（前端混用、检测框/关键点异常），提高或降低阈值都无法解决。

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
| 对齐门控不通过 | `align_crop()` 返回空，`used_alignment = false`，改走 bbox 前端 |
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
    bool aligned = false;
    auto emb = recognizer.extract_embedding(image, dets.front().bbox,
                                            dets.front().landmarks, &aligned);
    // emb.size() == 512
    float sim = recognizer.compute_similarity(emb, other_emb);
}
```

---

## 7. 与数据库的配合

`FaceDatabase` 对库内**每条模板**（一个人可有多条，见
[face_database.md](face_database.md)）调用同样的相似度计算，取最大值作为该身份的
相似度，再与阈值比较：

```cpp
auto match = database.match_face(emb, /*threshold=*/0.5f, /*z=*/3.0f,
                                 /*min_cohort=*/3, /*cohort_norm=*/true);
if (match.accepted) {
    // match.raw_similarity / match.z_score / match.accepted / match.reason
}
```

**库内记录 embedding 为空时不会命中**，需通过 `backfill` 或节点启动时的
`--auto-backfill` 修复。

---

## 8. 相关文档

- [人脸检测模块](face_detector.md)
- [人脸库模块](face_database.md)
- [数据库 schema](../protocol/database_schema.md)
