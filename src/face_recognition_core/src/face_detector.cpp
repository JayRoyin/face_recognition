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

            // Detect backend by number of outputs
            if (num_outputs == 9) {
                backend_name = "retinaface";
            } else {
                backend_name = "yolov8";
            }
            model_loaded = true;
            return true;
        } catch (const Ort::Exception& e) {
            last_error = std::string("Failed to load model: ") + e.what();
            model_loaded = false;
            return false;
        }
    }

    cv::Mat preprocessRetinaFace(const cv::Mat& image) {
        cv::Mat rgb;
        if (image.channels() == 3) {
            cv::cvtColor(image, rgb, cv::COLOR_BGR2RGB);
        } else {
            rgb = image;
        }
        cv::Mat resized;
        cv::resize(rgb, resized, cv::Size(input_width, input_height));

        cv::Mat blob;
        cv::dnn::blobFromImage(resized, blob,
                               1.0 / 128.0,
                               cv::Size(input_width, input_height),
                               cv::Scalar(127.5, 127.5, 127.5),
                               false,  // already RGB
                               false);
        return blob;
    }

    cv::Mat preprocessYolo(const cv::Mat& image) {
        cv::Mat blob;
        cv::dnn::blobFromImage(image, blob, 1.0 / 255.0,
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

        const float variance[2] = {0.1f, 0.2f};
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

            if      (cols == 1)  scores[layer]    = data;
            else if (cols == 4)  bboxes[layer]    = data;
            else if (cols == 10) landmarks[layer] = data;
        }

        for (int layer = 0; layer < 3; ++layer) {
            if (!scores[layer] || !bboxes[layer] || !landmarks[layer]) continue;

            int stride = strides[layer];
            int grid_w = input_width / stride;
            int grid_h = input_height / stride;

            for (int gy = 0; gy < grid_h; ++gy) {
                for (int gx = 0; gx < grid_w; ++gx) {
                    for (int anchor_type = 0; anchor_type < 2; ++anchor_type) {
                        int i = (gy * grid_w + gx) * 2 + anchor_type;
                        if (i >= expected[layer]) continue;

                        // score is already a softmax probability (in [0,1])
                        float conf = scores[layer][i];
                        if (conf < confidence_threshold) continue;

                        float cx = (gx + 0.5f) * stride;
                        float cy = (gy + 0.5f) * stride;
                        float aw = stride * 4.0f;
                        float ah = (anchor_type == 0) ? (stride * 4.0f)
                                                     : (stride * 4.0f * 1.5f);

                        float dx1 = bboxes[layer][i * 4 + 0] * variance[0] * aw;
                        float dy1 = bboxes[layer][i * 4 + 1] * variance[0] * ah;
                        float dx2 = bboxes[layer][i * 4 + 2] * variance[1] * aw;
                        float dy2 = bboxes[layer][i * 4 + 3] * variance[1] * ah;
                        float x1 = cx - dx1;
                        float y1 = cy - dy1;
                        float x2 = cx + dx2;
                        float y2 = cy + dy2;

                        float scale_w = img_w / (float)input_width;
                        float scale_h = img_h / (float)input_height;
                        x1 *= scale_w; y1 *= scale_h;
                        x2 *= scale_w; y2 *= scale_h;

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
                            float lx = landmarks[layer][i * 10 + k * 2 + 0] * aw * variance[0] + cx;
                            float ly = landmarks[layer][i * 10 + k * 2 + 1] * ah * variance[0] + cy;
                            det.landmarks.push_back(lx * scale_w);
                            det.landmarks.push_back(ly * scale_h);
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

        for (int i = 0; i < num_detections; ++i) {
            float cx, cy, w, h, conf;
            if (transposed) {
                cx = data[i * stride + 0] * img_w;
                cy = data[i * stride + 1] * img_h;
                w  = data[i * stride + 2] * img_w;
                h  = data[i * stride + 3] * img_h;
                conf = 0.0f;
                for (int c = 4; c < 4 + num_classes; ++c)
                    conf = std::max(conf, data[i * stride + c]);
            } else {
                cx = data[0 * num_detections + i] * img_w;
                cy = data[1 * num_detections + i] * img_h;
                w  = data[2 * num_detections + i] * img_w;
                h  = data[3 * num_detections + i] * img_h;
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
            // Preprocess image to blob
            cv::Mat blob = (backend_name == "retinaface")
                            ? preprocessRetinaFace(image)
                            : preprocessYolo(image);

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

            if (backend_name == "retinaface") {
                return postprocessRetinaFace(outputs, image.cols, image.rows, max_faces);
            } else {
                if (outputs.empty()) return {};
                auto shape_info = outputs[0].GetTensorTypeAndShapeInfo();
                auto shape = shape_info.GetShape();
                std::vector<int> shape_int(shape.size());
                for (size_t i = 0; i < shape.size(); ++i) shape_int[i] = (int)shape[i];
                cv::Mat out_mat((int)shape.size(), shape_int.data(), CV_32F,
                                (void*)outputs[0].GetTensorData<float>());
                return postprocessYolo(out_mat, image.cols, image.rows, max_faces);
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

std::string FaceDetector::backend() const {
    return pImpl->backend_name;
}

std::string FaceDetector::getLastError() const {
    return pImpl->last_error;
}

}  // namespace face_recognition
