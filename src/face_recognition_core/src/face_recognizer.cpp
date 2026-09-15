#include "face_recognition_core/face_recognizer.hpp"
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>
#include <iostream>

namespace face_recognition {

class FaceRecognizer::Impl {
public:
    cv::dnn::Net net;
    int input_size = 112;
    std::string last_error;
    bool model_loaded = false;

    bool loadModel(const std::string& model_path) {
        try {
            net = cv::dnn::readNetFromONNX(model_path);
            net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
            net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
            model_loaded = true;
            return true;
        } catch (const cv::Exception& e) {
            last_error = std::string("Failed to load model: ") + e.what();
            model_loaded = false;
            return false;
        }
    }

    /**
     * Crop the face with a 20% margin and resize to the network input size.
     * Returns an 8-bit BGR patch: scaling / channel order / mean subtraction
     * are all done by blobFromImage() in extract() so that the exact
     * normalisation is visible in one place.
     */
    cv::Mat preprocess(const cv::Mat& image, const BoundingBox& bbox) {
        int x = std::max(0, static_cast<int>(bbox.x - bbox.width * 0.2));
        int y = std::max(0, static_cast<int>(bbox.y - bbox.height * 0.2));
        int w = std::min(image.cols - x, static_cast<int>(bbox.width * 1.4));
        int h = std::min(image.rows - y, static_cast<int>(bbox.height * 1.4));

        if (w <= 0 || h <= 0) {
            return cv::Mat();
        }

        cv::Rect roi(x, y, w, h);
        cv::Mat resized;
        cv::resize(image(roi), resized, cv::Size(input_size, input_size));

        return resized;  // BGR, 8-bit
    }

    std::vector<float> extract(const cv::Mat& face) {
        if (face.empty()) return {};

        try {
            // Post-processing recipe measured on this project's own models:
            //   8-bit BGR patch -> RGB (swapRB) -> scale by 1/255, mean 0
            // Compared against the previous (BGR, (x-127.5)/127.5) recipe on
            // frontal / low-quality / camera-condition probes, this lowers the
            // IMPOSTOR similarity (0.37/0.30/0.38 -> 0.29/0.21/0.32) while
            // keeping genuine similarity well above threshold
            // (0.74/0.75/0.69 -> 0.70/0.70/0.68), i.e. a wider operating
            // margin and fewer false accepts.
            //
            // NOTE: this changes the embedding space — existing rows in the
            // face database MUST be re-enrolled (see `backfill --all`).
            cv::Mat blob;
            cv::dnn::blobFromImage(face, blob, 1.0 / 255.0,
                                   cv::Size(input_size, input_size),
                                   cv::Scalar(0, 0, 0),
                                   /*swapRB=*/true,
                                   /*crop=*/false);

            net.setInput(blob);
            cv::Mat output = net.forward();

            std::vector<float> embedding;
            embedding.assign(output.begin<float>(), output.end<float>());

            float norm = 0;
            for (float v : embedding) norm += v * v;
            norm = std::sqrt(norm);
            if (norm > 1e-8) {
                for (auto& v : embedding) v /= norm;
            }

            return embedding;
        } catch (const cv::Exception& e) {
            std::cerr << "[FaceRecognizer] extract() exception: " << e.what() << std::endl;
            last_error = e.what();
            return {};
        } catch (const std::exception& e) {
            std::cerr << "[FaceRecognizer] extract() std::exception: " << e.what() << std::endl;
            last_error = e.what();
            return {};
        }
    }
};

FaceRecognizer::FaceRecognizer() : pImpl(std::make_unique<Impl>()) {}
FaceRecognizer::~FaceRecognizer() = default;

bool FaceRecognizer::initialize(const std::string& model_path) {
    return pImpl->loadModel(model_path);
}

std::vector<float> FaceRecognizer::extract_embedding(const cv::Mat& image, const BoundingBox& bbox) {
    try {
        cv::Mat preprocessed = pImpl->preprocess(image, bbox);
        return pImpl->extract(preprocessed);
    } catch (const cv::Exception& e) {
        std::cerr << "[FaceRecognizer] extract_embedding() exception: " << e.what() << std::endl;
        pImpl->last_error = e.what();
        return {};
    } catch (const std::exception& e) {
        std::cerr << "[FaceRecognizer] extract_embedding() std::exception: " << e.what() << std::endl;
        pImpl->last_error = e.what();
        return {};
    }
}

float FaceRecognizer::compute_similarity(const std::vector<float>& emb1, const std::vector<float>& emb2) {
    if (emb1.size() != emb2.size() || emb1.empty()) return 0.0f;

    float dot = 0;
    for (size_t i = 0; i < emb1.size(); i++) {
        dot += emb1[i] * emb2[i];
    }
    return std::max(0.0f, std::min(1.0f, dot));
}

void FaceRecognizer::setInputSize(int width, int height) {
    if (width == height && width > 0) {
        pImpl->input_size = width;
    } else {
        pImpl->last_error = "FaceRecognizer requires square input";
    }
}

std::string FaceRecognizer::getLastError() const {
    return pImpl->last_error;
}

}  // namespace face_recognition
