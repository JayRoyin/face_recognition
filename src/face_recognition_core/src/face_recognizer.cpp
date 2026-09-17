#include "face_recognition_core/face_recognizer.hpp"

#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>

namespace face_recognition {

namespace {

// Canonical ArcFace 112x112 alignment template, in the landmark order that
// RetinaFace emits: left eye, right eye, nose, left mouth corner,
// right mouth corner. Same values as insightface's `arcface_dst`.
const double kArcfaceTemplate[5][2] = {
    {38.2946, 51.6963},
    {73.5318, 51.5014},
    {56.0252, 71.7366},
    {41.5493, 92.3655},
    {70.7299, 92.2041}};

/**
 * Closed-form least-squares 2D similarity transform (uniform scale +
 * rotation + translation, no reflection). This is the estimator insightface /
 * skimage `SimilarityTransform.estimate()` uses.
 *
 * Deliberately NOT `cv::estimateAffinePartial2D`: that runs RANSAC, and with
 * only 5 points a noisy landmark set can let a 2-point consensus win, which
 * yields a wildly wrong scale (observed as severely over-zoomed crops on
 * small faces).
 */
bool similarity_from_points(const std::vector<cv::Point2d>& src,
                            const std::vector<cv::Point2d>& dst,
                            cv::Mat& M, double* rms) {
    const size_t n = src.size();
    if (n < 2 || dst.size() != n) return false;

    cv::Point2d ms(0, 0), md(0, 0);
    for (size_t i = 0; i < n; ++i) { ms += src[i]; md += dst[i]; }
    ms /= static_cast<double>(n);
    md /= static_cast<double>(n);

    double a = 0, b = 0, norm = 0;
    for (size_t i = 0; i < n; ++i) {
        const double sx = src[i].x - ms.x, sy = src[i].y - ms.y;
        const double dx = dst[i].x - md.x, dy = dst[i].y - md.y;
        a += sx * dx + sy * dy;
        b += sx * dy - sy * dx;
        norm += sx * sx + sy * sy;
    }
    if (norm < 1e-9) return false;

    const double sc = a / norm;  // scale * cos(theta)
    const double ss = b / norm;  // scale * sin(theta)

    M = (cv::Mat_<double>(2, 3) <<
         sc, -ss, md.x - (sc * ms.x - ss * ms.y),
         ss,  sc, md.y - (ss * ms.x + sc * ms.y));

    if (rms) {
        double acc = 0;
        for (size_t i = 0; i < n; ++i) {
            const double px = M.at<double>(0, 0) * src[i].x +
                              M.at<double>(0, 1) * src[i].y + M.at<double>(0, 2);
            const double py = M.at<double>(1, 0) * src[i].x +
                              M.at<double>(1, 1) * src[i].y + M.at<double>(1, 2);
            acc += (px - dst[i].x) * (px - dst[i].x) +
                   (py - dst[i].y) * (py - dst[i].y);
        }
        *rms = std::sqrt(acc / static_cast<double>(n));
    }
    return true;
}

}  // namespace

// -----------------------------------------------------------------------------

class FaceRecognizer::Impl {
public:
    cv::dnn::Net net;
    int input_size = 112;
    std::string last_error;
    bool model_loaded = false;
    bool align_enabled = true;
    int aligned_count = 0;
    int fallback_count = 0;

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

    // --- Front-end 1: bounding-box crop (20% margin) -------------------------
    cv::Mat bbox_crop(const cv::Mat& image, const BoundingBox& bbox) const {
        int x = std::max(0, static_cast<int>(bbox.x - bbox.width * 0.2));
        int y = std::max(0, static_cast<int>(bbox.y - bbox.height * 0.2));
        int w = std::min(image.cols - x, static_cast<int>(bbox.width * 1.4));
        int h = std::min(image.rows - y, static_cast<int>(bbox.height * 1.4));
        if (w <= 0 || h <= 0) return {};
        cv::Mat out;
        cv::resize(image(cv::Rect(x, y, w, h)), out, cv::Size(input_size, input_size));
        return out;
    }

    // --- Front-end 2: 5-point landmark alignment ----------------------------
    // Returns an empty Mat when landmarks are missing / unreliable, in which
    // case the caller falls back to bbox_crop().
    cv::Mat align_crop(const cv::Mat& image, const std::vector<float>& landmarks) {
        if (!align_enabled || landmarks.size() != 10) return {};

        std::vector<cv::Point2d> src, dst;
        for (int i = 0; i < 5; ++i) {
            const double px = landmarks[2 * i];
            const double py = landmarks[2 * i + 1];
            if (!std::isfinite(px) || !std::isfinite(py)) return {};
            // Allow a little slack outside the frame but reject nonsense.
            if (px < -0.2 * image.cols || px > 1.2 * image.cols ||
                py < -0.2 * image.rows || py > 1.2 * image.rows) {
                return {};
            }
            src.emplace_back(px, py);
            dst.emplace_back(kArcfaceTemplate[i][0], kArcfaceTemplate[i][1]);
        }

        // Inter-ocular distance is the scale reference.
        //
        // The threshold is deliberately high. At eye=8 px the warp would
        // magnify the face ~4.4x, turning landmark noise into a geometrically
        // meaningless "face" — and such degraded embeddings behave like hubs,
        // matching many different people. Requiring >=18 px caps the
        // magnification at ~2x so the crop still carries real identity detail.
        const double eye = cv::norm(src[0] - src[1]);
        if (eye < 18.0) return {};

        cv::Mat M;
        double rms = 0.0;
        if (!similarity_from_points(src, dst, M, &rms)) return {};

        const double sc = M.at<double>(0, 0), ss = M.at<double>(0, 1);
        const double scale = std::sqrt(sc * sc + ss * ss);
        // Only the UPPER bound matters: scale > 2 means the source face is
        // smaller than half the 112x112 template, so warping it would upsample
        // and fabricate detail that is not in the source.
        //
        // The lower bound must stay far from 1. A face LARGER than the template
        // is not a problem: downscaling preserves identity information. The
        // earlier bound of 0.3 rejected perfectly ordinary photos — a 215 px
        // inter-ocular distance (any normal selfie) needs scale 0.16 — and each
        // rejection silently pushed that face onto the bbox-crop front-end.
        // Mixed galleries are unusable: the enrolled selfie was rejected
        // (scale 0.16) while the query photo was aligned (scale 0.32), and the
        // owner then scored only 0.41 against their own face.
        if (scale <= 0.05 || scale > 2.0) return {};

        // Poor fit => at least one landmark is off (very common on profiles,
        // where one eye is occluded and the 5-point geometry no longer matches
        // the frontal template). Prefer the safe fallback.
        if (rms > std::max(2.5, 0.10 * eye)) return {};

        cv::Mat out;
        cv::warpAffine(image, out, M, cv::Size(input_size, input_size),
                       cv::INTER_LINEAR, cv::BORDER_REPLICATE);
        return out;
    }

    cv::Mat prepare(const cv::Mat& image, const BoundingBox& bbox,
                    const std::vector<float>& landmarks) {
        cv::Mat aligned = align_crop(image, landmarks);
        if (!aligned.empty()) {
            ++aligned_count;
            return aligned;
        }
        ++fallback_count;
        return bbox_crop(image, bbox);
    }

    std::vector<float> embed(const cv::Mat& patch) {
        if (patch.empty()) return {};
        try {
            // Normalisation measured to give the widest genuine/impostor
            // margin on this project's models: BGR -> RGB, scaled to [0,1].
            cv::Mat blob;
            cv::dnn::blobFromImage(patch, blob, 1.0 / 255.0,
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

std::vector<float> FaceRecognizer::extract_embedding(const cv::Mat& image,
                                                    const BoundingBox& bbox) {
    return extract_embedding(image, bbox, std::vector<float>{}, nullptr);
}

std::vector<float> FaceRecognizer::extract_embedding(const cv::Mat& image,
                                                    const BoundingBox& bbox,
                                                    const std::vector<float>& landmarks,
                                                    bool* used_alignment) {
    if (used_alignment) *used_alignment = false;
    if (image.empty()) return {};
    try {
        cv::Mat aligned = pImpl->align_crop(image, landmarks);
        const bool used = !aligned.empty();
        if (used_alignment) *used_alignment = used;
        cv::Mat patch = used ? aligned : pImpl->bbox_crop(image, bbox);
        if (used) ++pImpl->aligned_count; else ++pImpl->fallback_count;
        return pImpl->embed(patch);
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

float FaceRecognizer::compute_similarity(const std::vector<float>& emb1,
                                        const std::vector<float>& emb2) {
    // Single source of truth: types.hpp. Negative cosines are meaningful and
    // must not be clamped, otherwise unrelated identities all collapse onto 0.
    return face_recognition::compute_similarity(emb1, emb2);
}

void FaceRecognizer::setInputSize(int width, int height) {
    if (width == height && width > 0) {
        pImpl->input_size = width;
    } else {
        pImpl->last_error = "FaceRecognizer requires square input";
    }
}

void FaceRecognizer::setAlignmentEnabled(bool enabled) {
    pImpl->align_enabled = enabled;
}

bool FaceRecognizer::alignmentEnabled() const {
    return pImpl->align_enabled;
}

void FaceRecognizer::alignmentStats(int* aligned, int* fallback) const {
    if (aligned)  *aligned  = pImpl->aligned_count;
    if (fallback) *fallback = pImpl->fallback_count;
}

std::string FaceRecognizer::getLastError() const {
    return pImpl->last_error;
}

}  // namespace face_recognition
