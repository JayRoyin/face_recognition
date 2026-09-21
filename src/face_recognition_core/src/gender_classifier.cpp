#include "face_recognition_core/gender_classifier.hpp"

#include <onnxruntime/core/session/onnxruntime_cxx_api.h>

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <vector>

namespace face_recognition {

class GenderClassifier::Impl {
public:
    std::unique_ptr<Ort::Env>     ort_env;
    std::unique_ptr<Ort::Session> session;
    Ort::AllocatorWithDefaultOptions allocator;

    std::vector<std::string> input_names;
    std::vector<std::string> output_names;

    int input_w = 96;
    int input_h = 96;
    bool nhwc = false;
    int male_index = 1;
    std::string input_dims;

    std::string last_error;
    std::string shape_info;
    bool loaded = false;

    bool load(const std::string& model_path, int male_idx) {
        male_index = male_idx;
        try {
            ort_env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_ERROR, "genderage");
            Ort::SessionOptions opts;
            opts.SetIntraOpNumThreads(1);
            opts.SetLogSeverityLevel(ORT_LOGGING_LEVEL_ERROR);
            opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);

            session = std::make_unique<Ort::Session>(*ort_env, model_path.c_str(), opts);

            for (size_t i = 0; i < session->GetInputCount(); ++i) {
                auto name = session->GetInputNameAllocated(i, allocator);
                input_names.push_back(name.get());
                auto shape = session->GetInputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape();

                std::ostringstream raw;
                raw << "input dims=[";
                for (size_t d = 0; d < shape.size(); ++d) raw << (d ? "x" : "") << shape[d];
                raw << "]";
                input_dims = raw.str();

                if (shape.size() == 4) {
                    // [1,3,H,W] (NCHW, the InsightFace export) vs [1,H,W,3].
                    // A square input makes both read "96x96", so the channel
                    // axis has to decide the layout.
                    if (shape[1] == 3) {
                        nhwc = false;
                        if (shape[2] > 0) input_h = static_cast<int>(shape[2]);
                        if (shape[3] > 0) input_w = static_cast<int>(shape[3]);
                    } else if (shape[3] == 3) {
                        nhwc = true;
                        if (shape[1] > 0) input_h = static_cast<int>(shape[1]);
                        if (shape[2] > 0) input_w = static_cast<int>(shape[2]);
                    }
                }
            }
            for (size_t i = 0; i < session->GetOutputCount(); ++i) {
                auto name = session->GetOutputNameAllocated(i, allocator);
                output_names.push_back(name.get());
            }

            std::ostringstream info;
            info << input_dims << (nhwc ? " (NHWC)" : " (NCHW)")
                 << " size=" << input_w << "x" << input_h << " outputs:";
            for (size_t i = 0; i < output_names.size(); ++i) {
                auto shape = session->GetOutputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape();
                info << " " << output_names[i] << "[";
                for (size_t d = 0; d < shape.size(); ++d) {
                    info << (d ? "x" : "") << shape[d];
                }
                info << "]";
            }
            shape_info = info.str();

            loaded = true;
            return true;
        } catch (const std::exception& e) {
            last_error = e.what();
            std::cerr << "[GenderClassifier] load failed: " << last_error << std::endl;
            return false;
        }
    }

    GenderAgeResult run(const cv::Mat& face_bgr) const {
        GenderAgeResult out;
        if (!loaded || face_bgr.empty()) return out;

        cv::Mat resized;
        cv::resize(face_bgr, resized, cv::Size(input_w, input_h), 0, 0, cv::INTER_LINEAR);
        if (resized.channels() == 1) cv::cvtColor(resized, resized, cv::COLOR_GRAY2RGB);
        else if (resized.channels() == 4) cv::cvtColor(resized, resized, cv::COLOR_BGRA2RGB);
        else cv::cvtColor(resized, resized, cv::COLOR_BGR2RGB);

        // RAW 0..255 pixels, NCHW float, RGB order.
        //
        // This is NOT the usual "(x-127.5)/128 at the caller" recipe: the graph
        // itself starts with Sub(127.5) followed by Mul(0.0078125) — the two
        // constants sit next to each other in the .onnx initializer block — so
        // it normalises internally. Feeding it already-normalised values
        // double-normalises: every activation collapses and the output becomes
        // a near-constant [-v, +v, age] for any input, which reads as "all
        // faces are class 0" and silently mislabels the whole gallery.
        // Verified with a probe over several candidate scalings.
        std::vector<float> blob(static_cast<size_t>(3) * input_h * input_w);
        const size_t plane = static_cast<size_t>(input_h) * input_w;
        for (int c = 0; c < 3; ++c) {
            for (int i = 0; i < input_h * input_w; ++i) {
                blob[static_cast<size_t>(c) * plane + i] =
                    static_cast<float>(resized.data[i * 3 + c]);
            }
        }

        try {
            std::vector<int64_t> shape = {1, 3, input_h, input_w};
            Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            Ort::Value tensor = Ort::Value::CreateTensor<float>(
                mem, blob.data(), blob.size(), shape.data(), shape.size());

            std::vector<const char*> in_cstr;
            std::vector<const char*> out_cstr;
            for (const auto& n : input_names)  in_cstr.push_back(n.c_str());
            for (const auto& n : output_names) out_cstr.push_back(n.c_str());

            Ort::RunOptions run_options;
            auto outputs = session->Run(run_options,
                                        in_cstr.data(), &tensor, 1,
                                        out_cstr.data(), out_cstr.size());
            if (outputs.empty()) return out;

            const float* data = outputs[0].GetTensorData<float>();
            const size_t n = outputs[0].GetTensorTypeAndShapeInfo().GetElementCount();

            if (std::getenv("FACE_GENDER_DEBUG") != nullptr) {
                std::cerr << "[GenderClassifier] raw output:";
                for (size_t i = 0; i < n; ++i) std::cerr << " " << data[i];
                std::cerr << std::endl;
            }

            if (n >= 2) {
                const float a = data[0];
                const float b = data[1];
                const int winner = (b > a) ? 1 : 0;
                // Softmax over the two logits -> a comparable confidence.
                const float ma = std::max(a, b);
                const float ea = std::exp(a - ma);
                const float eb = std::exp(b - ma);
                out.confidence = (winner == 0) ? (ea / (ea + eb)) : (eb / (ea + eb));
                out.gender = (winner == male_index) ? "male" : "female";
            }
            if (n >= 3) {
                // The third value is the age head (already in "years * 0.01").
                out.age = data[2] * 100.0f;
            }
        } catch (const std::exception& e) {
            std::cerr << "[GenderClassifier] inference failed: " << e.what() << std::endl;
        }
        return out;
    }
};

GenderClassifier::GenderClassifier() : pImpl(std::make_unique<Impl>()) {}
GenderClassifier::~GenderClassifier() = default;

bool GenderClassifier::initialize(const std::string& model_path, int male_index) {
    return pImpl->load(model_path, male_index);
}

bool GenderClassifier::ready() const { return pImpl->loaded; }
std::string GenderClassifier::getLastError() const { return pImpl->last_error; }
std::string GenderClassifier::shapeInfo() const { return pImpl->shape_info; }

GenderAgeResult GenderClassifier::classify(const cv::Mat& face_bgr) const {
    return pImpl->run(face_bgr);
}

}  // namespace face_recognition
