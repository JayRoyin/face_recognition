#include "face_recognition_core/face_detector.hpp"

#include <onnxruntime/core/session/onnxruntime_cxx_api.h>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/dnn.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <numeric>
#include <unordered_map>
#include <vector>

namespace face_recognition {

class FaceDetector::Impl {
public:
    std::unique_ptr<Ort::Env> ort_env;
    std::unique_ptr<Ort::Session> session;
    Ort::AllocatorWithDefaultOptions allocator;

    std::vector<std::string> input_names;
    std::vector<std::string> output_names;
    std::vector<int64_t> input_shape;

    float confidence_threshold = 0.5f;
    float nms_threshold = 0.5f;
    int input_width = 640;
    int input_height = 640;
    std::string last_error;
    std::string backend_name = "unknown";
    bool model_loaded = false;

    bool loadModel(const std::string& model_path) {
        try {
            if (!ort_env) {
                // Only surface real errors. det_10g.onnx declares STATIC output
                // shapes for a 640x640 input, so every Run() with another input
                // size makes ORT emit a batch of "VerifyOutputSizes" warnings.
                // They are harmless (the shapes are read dynamically below) but
                // would flood the console once per frame.
                ort_env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_ERROR, "face_detector");
            }

            Ort::SessionOptions session_options;
            session_options.SetIntraOpNumThreads(1);
            session_options.SetLogSeverityLevel(ORT_LOGGING_LEVEL_ERROR);
            session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);

            session = std::make_unique<Ort::Session>(*ort_env, model_path.c_str(), session_options);

            // Input info
            size_t num_inputs = session->GetInputCount();
            input_names.reserve(num_inputs);
            for (size_t i = 0; i < num_inputs; ++i) {
                auto name = session->GetInputNameAllocated(i, allocator);
                input_names.push_back(name.get());
            }

            // Get input shape
            auto input_type_info = session->GetInputTypeInfo(0);
            auto input_tensor_info = input_type_info.GetTensorTypeAndShapeInfo();
            input_shape = input_tensor_info.GetShape();

            // Output info
            size_t num_outputs = session->GetOutputCount();
            output_names.reserve(num_outputs);
            for (size_t i = 0; i < num_outputs; ++i) {
                auto name = session->GetOutputNameAllocated(i, allocator);
                output_names.push_back(name.get());
            }

            // The backend is decided from the ACTUAL output tensors on the first
            // inference — see detectFaces(). Not here, and not from the output
            // count:
            //   * Session::GetOutputTypeInfo(i).GetTensorTypeAndShapeInfo() is
            //     unusable in this ONNX Runtime build: GetDimensionsCount()
            //     returns a bogus value and GetShape() then throws
            //     "cannot create std::vector larger than max_size()", which
            //     takes down every command that loads the detector.
            //     Ort::Value::GetTensorTypeAndShapeInfo() (used by the
            //     post-processing and by the classifier below) is fine.
            //   * "num_outputs == 9" on its own is not a signature: any 9-output
            //     YOLO variant matches it and would then be decoded with the
            //     SCRFD decoder, silently producing garbage boxes.
            backend_name = "auto";
            model_loaded = true;
            return true;
        } catch (const Ort::Exception& e) {
            last_error = std::string("Failed to load model: ") + e.what();
            model_loaded = false;
            return false;
        }
    }

    /**
     * Geometry of the last preprocess(): how the original frame was scaled and
     * padded to reach the square network input. Needed to map decoded
     * coordinates back to image space.
     */
    struct LetterboxInfo {
        double scale = 1.0;   // resize factor applied to the original frame
        int    pad_x = 0;     // left padding, in network-input pixels
        int    pad_y = 0;     // top padding
        bool   valid = false;
    };

    /**
     * Aspect-preserving resize + letterbox padding onto a square canvas.
     *
     * The network input is square (640x640 / 320x320). Feeding a 16:9 or 4:3
     * frame by plain `resize` squashes it (a 1280x720 frame is compressed 4x
     * horizontally but only 2.25x vertically), which distorts every face and
     * measurably degrades the 5-point landmark regression — and alignment
     * quality depends directly on landmark accuracy. Padding instead keeps the
     * geometry the model was trained on.
     */
    cv::Mat letterbox(const cv::Mat& image, LetterboxInfo& out_lb) const {
        cv::Mat src;
        if (image.channels() == 1) {
            cv::cvtColor(image, src, cv::COLOR_GRAY2BGR);
        } else if (image.channels() == 4) {
            cv::cvtColor(image, src, cv::COLOR_BGRA2BGR);
        } else {
            src = image;
        }
        if (src.empty()) return {};

        const double s = std::min(static_cast<double>(input_width) / src.cols,
                                  static_cast<double>(input_height) / src.rows);
        const int new_w = std::max(1, static_cast<int>(std::lround(src.cols * s)));
        const int new_h = std::max(1, static_cast<int>(std::lround(src.rows * s)));

        out_lb.scale = s;
        out_lb.pad_x = (input_width  - new_w) / 2;
        out_lb.pad_y = (input_height - new_h) / 2;
        out_lb.valid = true;

        cv::Mat resized;
        cv::resize(src, resized, cv::Size(new_w, new_h), 0, 0, cv::INTER_LINEAR);

        cv::Mat canvas(input_height, input_width, src.type(), cv::Scalar(0, 0, 0));
        resized.copyTo(canvas(cv::Rect(out_lb.pad_x, out_lb.pad_y, new_w, new_h)));
        return canvas;
    }

    cv::Mat preprocessRetinaFace(const cv::Mat& image, LetterboxInfo& lb) {
        cv::Mat canvas = letterbox(image, lb);
        if (canvas.empty()) return {};
        cv::Mat rgb;
        cv::cvtColor(canvas, rgb, cv::COLOR_BGR2RGB);

        cv::Mat blob;
        cv::dnn::blobFromImage(rgb, blob,
                               1.0 / 128.0,
                               cv::Size(input_width, input_height),
                               cv::Scalar(127.5, 127.5, 127.5),
                               false,  // already RGB
                               false);
        return blob;
    }

    cv::Mat preprocessYolo(const cv::Mat& image, LetterboxInfo& lb) {
        cv::Mat canvas = letterbox(image, lb);
        if (canvas.empty()) return {};
        cv::Mat blob;
        cv::dnn::blobFromImage(canvas, blob, 1.0 / 255.0,
                               cv::Size(input_width, input_height),
                               cv::Scalar(0, 0, 0),
                               true,
                               false);
        return blob;
    }

    static std::vector<FaceDetection> nms(const std::vector<FaceDetection>& dets,
                                          float iou_threshold) {
        if (dets.empty()) return {};
        std::vector<FaceDetection> sorted = dets;
        std::sort(sorted.begin(), sorted.end(),
                  [](const FaceDetection& a, const FaceDetection& b) {
                      return a.confidence > b.confidence;
                  });

        std::vector<FaceDetection> kept;
        std::vector<bool> suppressed(sorted.size(), false);

        for (size_t i = 0; i < sorted.size(); ++i) {
            if (suppressed[i]) continue;
            kept.push_back(sorted[i]);
            float a_area = sorted[i].bbox.width * sorted[i].bbox.height;
            for (size_t j = i + 1; j < sorted.size(); ++j) {
                if (suppressed[j]) continue;
                float inter_w = std::max(0.0f, std::min((float)(sorted[i].bbox.x + sorted[i].bbox.width),
                                                         (float)(sorted[j].bbox.x + sorted[j].bbox.width))
                                              - std::max((float)sorted[i].bbox.x, (float)sorted[j].bbox.x));
                float inter_h = std::max(0.0f, std::min((float)(sorted[i].bbox.y + sorted[i].bbox.height),
                                                         (float)(sorted[j].bbox.y + sorted[j].bbox.height))
                                              - std::max((float)sorted[i].bbox.y, (float)sorted[j].bbox.y));
                float inter = inter_w * inter_h;
                float b_area = sorted[j].bbox.width * sorted[j].bbox.height;
                float union_area = a_area + b_area - inter;
                if (union_area > 0 && inter / union_area > iou_threshold) {
                    suppressed[j] = true;
                }
            }
        }
        return kept;
    }

    std::vector<FaceDetection> postprocessRetinaFace(
            const std::vector<Ort::Value>& outputs,
            float img_w, float img_h,
            const LetterboxInfo& lb,
            int max_faces) {
        std::vector<FaceDetection> candidates;

        if (outputs.size() < 9) {
            last_error = "RetinaFace: expected 9 outputs, got " +
                         std::to_string(outputs.size());
            return candidates;
        }

        // outputs are sorted by cv::dnn order: conf_0, conf_1, conf_2, bbox_0...
        // In ORT runtime, the order matches graph output declaration:
        // [0] 448 conf_0 [12800,1]
        // [1] 471 conf_1 [3200,1]
        // [2] 494 conf_2 [800,1]
        // [3] 451 bbox_0 [12800,4]
        // [4] 474 bbox_1 [3200,4]
        // [5] 497 bbox_2 [800,4]
        // [6] 454 land_0 [12800,10]
        // [7] 477 land_1 [3200,10]
        // [8] 500 land_2 [800,10]

        // SCRFD decoding constants. NOTE: det_10g is an SCRFD model, NOT a
        // classic RetinaFace one. SCRFD regression heads predict distances that
        // are already normalised by the feature stride, so the decoder must be
        // a plain `anchor_center +/- raw * stride`:
        //
        //     center_x = gx * stride          (no +0.5 cell offset)
        //     x1 = center_x - raw[0] * stride
        //     x2 = center_x + raw[2] * stride
        //     kps_x = center_x + raw_kps[2k] * stride
        //
        // The RetinaFace convention (variance 0.1/0.2 applied to the raw
        // offsets, then multiplied by an anchor box of `stride*4`) does NOT
        // apply here. Applying it scaled every decoded distance by 0.4 (x1/y1)
        // or 0.8 (x2/y2) instead of 1.0, which shrank the boxes to ~60% of the
        // true face extent (cropping the chin and forehead away) and squeezed
        // the 5 landmarks towards the anchor centre. The landmark compression
        // made the similarity transform zoom into the eyes/nose, so the aligned
        // crop contained no mouth or chin at all — and both front-ends produced
        // embeddings that no longer separated different people.
        const int strides[3] = {8, 16, 32};

        // Anchor count per feature layer for the CURRENT input size:
        //     (W / stride) * (H / stride) * 2 anchors
        // det_10g.onnx declares STATIC output shapes for a 640x640 input
        // (12800 / 3200 / 800). Any other input size changes the actual tensor
        // shapes (e.g. 320 -> 3200 / 800 / 200), so the layer MUST be derived
        // from the configured input size. Hardcoding the 640 layout mis-assigns
        // every layer — and silently drops the smallest one — which makes
        // detection return nothing at all for non-640 inputs.
        int expected[3];
        for (int l = 0; l < 3; ++l) {
            expected[l] = (input_width / strides[l]) * (input_height / strides[l]) * 2;
        }

        const float* scores[3] = {nullptr, nullptr, nullptr};
        const float* bboxes[3] = {nullptr, nullptr, nullptr};
        const float* landmarks[3] = {nullptr, nullptr, nullptr};

        // Real anchor count of each bound tensor. `expected[l]` is derived from
        // the configured input size, so the two only agree for the shipped
        // model/input combination; indexing up to expected[l] anyway would read
        // past the tensor whenever they disagree.
        int actual_rows[3] = {0, 0, 0};

        // Match outputs by shape: `rows` identifies the layer, `cols` the kind
        // (1 = score, 4 = bbox offset, 10 = 5 landmarks).
        for (size_t i = 0; i < outputs.size() && i < 9; ++i) {
            auto shape_info = outputs[i].GetTensorTypeAndShapeInfo();
            auto shape = shape_info.GetShape();
            if (shape.size() != 2 || shape[0] <= 0 || shape[1] <= 0) continue;
            const int rows = (int)shape[0];
            const int cols = (int)shape[1];
            const float* data = outputs[i].GetTensorData<float>();

            int layer = -1;
            for (int l = 0; l < 3; ++l) {
                if (rows == expected[l]) { layer = l; break; }
            }
            if (layer < 0) {
                // Fallback: fixed-shape exports built for a 640x640 input.
                if      (rows == 12800) layer = 0;
                else if (rows == 3200)  layer = 1;
                else if (rows == 800)   layer = 2;
                else continue;
            }

            if      (cols == 1)  { scores[layer]    = data; actual_rows[layer] = rows; }
            else if (cols == 4)  { bboxes[layer]    = data; actual_rows[layer] = rows; }
            else if (cols == 10) { landmarks[layer] = data; actual_rows[layer] = rows; }
        }

        for (int layer = 0; layer < 3; ++layer) {
            if (!scores[layer] || !bboxes[layer] || !landmarks[layer]) continue;

            // Bound every access by the smaller of "what the input size implies"
            // and "what the tensor actually holds".
            const int limit = std::min(expected[layer], actual_rows[layer]);

            int stride = strides[layer];
            int grid_w = input_width / stride;
            int grid_h = input_height / stride;

            for (int gy = 0; gy < grid_h; ++gy) {
                for (int gx = 0; gx < grid_w; ++gx) {
                    for (int anchor_type = 0; anchor_type < 2; ++anchor_type) {
                        int i = (gy * grid_w + gx) * 2 + anchor_type;
                        if (i >= limit) continue;

                        // score is already a softmax probability (in [0,1])
                        float conf = scores[layer][i];
                        if (conf < confidence_threshold) continue;

                        float cx = gx * stride;
                        float cy = gy * stride;

                        float dx1 = bboxes[layer][i * 4 + 0] * stride;
                        float dy1 = bboxes[layer][i * 4 + 1] * stride;
                        float dx2 = bboxes[layer][i * 4 + 2] * stride;
                        float dy2 = bboxes[layer][i * 4 + 3] * stride;
                        float x1 = cx - dx1;
                        float y1 = cy - dy1;
                        float x2 = cx + dx2;
                        float y2 = cy + dy2;

                        // Network-input px -> original image px (undo letterbox).
                        const float inv_scale = 1.0f / (float)lb.scale;
                        x1 = (x1 - lb.pad_x) * inv_scale;
                        y1 = (y1 - lb.pad_y) * inv_scale;
                        x2 = (x2 - lb.pad_x) * inv_scale;
                        y2 = (y2 - lb.pad_y) * inv_scale;

                        x1 = std::max(0.0f, std::min(img_w - 1.0f, x1));
                        y1 = std::max(0.0f, std::min(img_h - 1.0f, y1));
                        x2 = std::max(0.0f, std::min(img_w - 1.0f, x2));
                        y2 = std::max(0.0f, std::min(img_h - 1.0f, y2));

                        float w = x2 - x1;
                        float h = y2 - y1;
                        if (w < 4.0f || h < 4.0f) continue;

                        FaceDetection det;
                        det.confidence = conf;
                        det.bbox.x = static_cast<uint32_t>(x1);
                        det.bbox.y = static_cast<uint32_t>(y1);
                        det.bbox.width = static_cast<uint32_t>(w);
                        det.bbox.height = static_cast<uint32_t>(h);

                        det.landmarks.clear();
                        for (int k = 0; k < 5; ++k) {
                            float lx = landmarks[layer][i * 10 + k * 2 + 0] * stride + cx;
                            float ly = landmarks[layer][i * 10 + k * 2 + 1] * stride + cy;
                            det.landmarks.push_back((lx - lb.pad_x) * inv_scale);
                            det.landmarks.push_back((ly - lb.pad_y) * inv_scale);
                        }

                        candidates.push_back(det);
                    }
                }
            }
        }

        std::vector<FaceDetection> detections = nms(candidates, nms_threshold);

        if ((int)detections.size() > max_faces) {
            std::sort(detections.begin(), detections.end(),
                      [](const FaceDetection& a, const FaceDetection& b) {
                          return a.confidence > b.confidence;
                      });
            detections.resize(max_faces);
        }
        return detections;
    }

    std::vector<FaceDetection> postprocessYolo(const cv::Mat& output,
                                                float img_w, float img_h,
                                                const LetterboxInfo& lb,
                                                int max_faces) {
        std::vector<FaceDetection> candidates;
        if (output.empty() || output.dims < 2 || output.dims > 3 ||
            (output.dims == 3 && output.size[0] != 1)) {
            return candidates;
        }

        const float* data = (float*)output.data;
        int num_detections = 0, num_classes = 0, stride = 0;
        bool transposed = false;

        if (output.dims == 3 && output.size[0] == 1) {
            int rows = output.size[1], cols = output.size[2];
            if (rows < cols) {
                transposed = true;
                num_detections = rows;
                num_classes = cols - 4;
            } else {
                num_detections = cols;
                num_classes = rows - 4;
            }
            stride = num_classes + 4;
        } else if (output.dims == 2) {
            num_detections = output.rows;
            num_classes = output.cols - 4;
            stride = output.cols;
        }
        if (num_classes <= 0 || stride <= 0 || num_detections <= 0) return candidates;

        // YOLOv8 emits normalised [0,1] coordinates relative to the network
        // input canvas, so unpad + unscale them the same way as RetinaFace.
        const float inv_scale = 1.0f / (float)lb.scale;
        const float sx = input_width  * inv_scale;
        const float sy = input_height * inv_scale;
        auto map_x = [&](float vn) { return vn * sx - (float)lb.pad_x * inv_scale; };
        auto map_y = [&](float vn) { return vn * sy - (float)lb.pad_y * inv_scale; };

        for (int i = 0; i < num_detections; ++i) {
            float cx, cy, w, h, conf;
            if (transposed) {
                cx = map_x(data[i * stride + 0]);
                cy = map_y(data[i * stride + 1]);
                w  = data[i * stride + 2] * sx;
                h  = data[i * stride + 3] * sy;
                conf = 0.0f;
                for (int c = 4; c < 4 + num_classes; ++c)
                    conf = std::max(conf, data[i * stride + c]);
            } else {
                cx = map_x(data[0 * num_detections + i]);
                cy = map_y(data[1 * num_detections + i]);
                w  = data[2 * num_detections + i] * sx;
                h  = data[3 * num_detections + i] * sy;
                conf = 0.0f;
                for (int c = 0; c < num_classes; ++c)
                    conf = std::max(conf, data[(4 + c) * num_detections + i]);
            }
            if (conf < confidence_threshold) continue;

            FaceDetection det;
            det.confidence = conf;
            det.bbox.x = static_cast<uint32_t>(std::max(0.0f, cx - w / 2));
            det.bbox.y = static_cast<uint32_t>(std::max(0.0f, cy - h / 2));
            det.bbox.width  = static_cast<uint32_t>(std::min(img_w - det.bbox.x, w));
            det.bbox.height = static_cast<uint32_t>(std::min(img_h - det.bbox.y, h));
            candidates.push_back(det);
        }

        std::vector<FaceDetection> detections = nms(candidates, nms_threshold);
        if ((int)detections.size() > max_faces) {
            std::sort(detections.begin(), detections.end(),
                      [](const FaceDetection& a, const FaceDetection& b) {
                          return a.confidence > b.confidence;
                      });
            detections.resize(max_faces);
        }
        return detections;
    }

    std::vector<FaceDetection> detectFaces(const cv::Mat& image, int max_faces) {
        if (image.empty() || !model_loaded) return {};

        try {
            // Preprocess image to blob (letterboxed; geometry kept in `lb`)
            LetterboxInfo lb;
            cv::Mat blob = (backend_name == "retinaface")
                            ? preprocessRetinaFace(image, lb)
                            : preprocessYolo(image, lb);
            if (blob.empty() || !lb.valid) return {};

            // Prepare input tensor
            std::vector<int64_t> input_dims = {1, 3, input_height, input_width};
            Ort::MemoryInfo mem_info = Ort::MemoryInfo::CreateCpu(
                OrtAllocatorType::OrtArenaAllocator, OrtMemType::OrtMemTypeDefault);

            size_t input_size = 1 * 3 * input_height * input_width;
            std::vector<float> input_data(input_size);
            std::memcpy(input_data.data(), blob.data, input_size * sizeof(float));

            Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
                mem_info, input_data.data(), input_size,
                input_dims.data(), input_dims.size());

            // Run inference
            Ort::RunOptions run_options;
            std::vector<const char*> input_name_cstrs;
            std::vector<const char*> output_name_cstrs;
            for (const auto& n : input_names) input_name_cstrs.push_back(n.c_str());
            for (const auto& n : output_names) output_name_cstrs.push_back(n.c_str());

            std::vector<Ort::Value> outputs = session->Run(
                run_options,
                input_name_cstrs.data(), &input_tensor, 1,
                output_name_cstrs.data(), output_name_cstrs.size());

            // Classify the backend from the real tensors the first time we see
            // them: SCRFD / det_10g emits three groups of (1 = score, 4 = bbox,
            // 10 = landmarks) tensors, which no YOLO export matches.
            if (backend_name == "auto") {
                int n_score = 0, n_bbox = 0, n_landmark = 0;
                for (const auto& v : outputs) {
                    const auto shape = v.GetTensorTypeAndShapeInfo().GetShape();
                    if (shape.empty()) continue;
                    const int64_t last = shape.back();
                    if      (last == 1)  ++n_score;
                    else if (last == 4)  ++n_bbox;
                    else if (last == 10) ++n_landmark;
                }
                const bool scrfd = (n_score > 0 && n_bbox > 0 && n_landmark > 0 &&
                                    n_score == n_bbox && n_bbox == n_landmark);
                backend_name = scrfd ? "retinaface" : "yolov8";
            }

            if (backend_name == "retinaface") {
                return postprocessRetinaFace(outputs, image.cols, image.rows, lb, max_faces);
            } else {
                if (outputs.empty()) return {};
                auto shape_info = outputs[0].GetTensorTypeAndShapeInfo();
                auto shape = shape_info.GetShape();
                std::vector<int> shape_int(shape.size());
                for (size_t i = 0; i < shape.size(); ++i) shape_int[i] = (int)shape[i];
                cv::Mat out_mat((int)shape.size(), shape_int.data(), CV_32F,
                                (void*)outputs[0].GetTensorData<float>());
                return postprocessYolo(out_mat, image.cols, image.rows, lb, max_faces);
            }
        } catch (const Ort::Exception& e) {
            if (last_error != e.what()) {
                std::cerr << "[FaceDetector] ORT exception: " << e.what() << std::endl;
                last_error = e.what();
            }
            return {};
        } catch (const std::exception& e) {
            if (last_error != e.what()) {
                std::cerr << "[FaceDetector] std::exception: " << e.what() << std::endl;
                last_error = e.what();
            }
            return {};
        }
    }
};

FaceDetector::FaceDetector() : pImpl(std::make_unique<Impl>()) {}
FaceDetector::~FaceDetector() = default;

bool FaceDetector::initialize(const std::string& model_path,
                               float confidence_threshold,
                               float nms_threshold,
                               int input_size) {
    pImpl->confidence_threshold = confidence_threshold;
    pImpl->nms_threshold = nms_threshold;
    pImpl->input_width = input_size;
    pImpl->input_height = input_size;
    return pImpl->loadModel(model_path);
}

std::vector<FaceDetection> FaceDetector::detect(const cv::Mat& image, int max_faces) {
    return pImpl->detectFaces(image, max_faces);
}

void FaceDetector::setInputSize(int width, int height) {
    if (width == height && width > 0) {
        pImpl->input_width = width;
        pImpl->input_height = height;
    }
}

void FaceDetector::setConfidenceThreshold(float threshold) {
    if (threshold < 0.01f) threshold = 0.01f;
    if (threshold > 0.99f) threshold = 0.99f;
    pImpl->confidence_threshold = threshold;
}

float FaceDetector::confidenceThreshold() const {
    return pImpl->confidence_threshold;
}

std::string FaceDetector::backend() const {
    return pImpl->backend_name;
}

std::string FaceDetector::getLastError() const {
    return pImpl->last_error;
}

}  // namespace face_recognition
