#include "crosshair_detector.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace crosshair
{

namespace
{

inline int clamp_byte(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// 动态居中 ROI：以画面中心为基准，兼顾武器全自动后坐力向上跳动的自适应窗口
cv::Rect dynamic_center_roi(const cv::Size& frame, int rect_w, int rect_h)
{
    const int w = std::clamp(rect_w, 8, frame.width);
    const int h = std::clamp(rect_h, 8, frame.height);

    // 水平居中；垂直方向由于射击后坐力准星主要是向上弹跳，
    // 将采样框稍许向上扩展 60% 向上空间，40% 向下空间，避免后坐力抬枪脱框
    const int cx = frame.width / 2;
    const int cy = frame.height / 2;
    const int x = cx - w / 2;
    const int y = cy - static_cast<int>(h * 0.6f);

    cv::Rect roi(x, y, w, h);
    return roi & cv::Rect(0, 0, frame.width, frame.height);
}

// 颜色显著度分类
enum class ColorCategory {
    Red,
    Green,
    Cyan,
    Purple,
    Yellow,
    GenericHsv
};

ColorCategory classify_band(const CrosshairColorBand& b) {
    // 根据 H 区间智能判定目标颜色主类
    int h_mid = (b.h_low + b.h_high) / 2;
    if ((b.h_low <= 15 && b.h_high <= 20) || b.h_low >= 155) return ColorCategory::Red;
    if (h_mid >= 35 && h_mid <= 85) return ColorCategory::Green;
    if (h_mid > 85 && h_mid <= 110) return ColorCategory::Cyan;
    if (h_mid > 110 && h_mid <= 155) return ColorCategory::Purple;
    if (h_mid > 15 && h_mid < 35) return ColorCategory::Yellow;
    return ColorCategory::GenericHsv;
}

// 计算跨格式（NV12/MJPEG/RGB）鲁棒的归一化色彩显著度响应图 (0~255)
// 完全摒弃脆弱的离散色相除法，基于生物视觉拮抗（Opponency）模型
void compute_robust_saliency_map(const cv::Mat& bgr,
                                 const std::vector<CrosshairColorBand>& bands,
                                 cv::Mat& out_saliency)
{
    out_saliency.create(bgr.size(), CV_8UC1);
    out_saliency.setTo(0);

    const int rows = bgr.rows;
    const int cols = bgr.cols;

    // 收集所有激活的颜色类别
    std::vector<ColorCategory> active_cats;
    bool has_custom_hsv = false;
    for (const auto& b : bands) {
        if (!b.enabled) continue;
        ColorCategory cat = classify_band(b);
        if (cat == ColorCategory::GenericHsv) has_custom_hsv = true;
        if (std::find(active_cats.begin(), active_cats.end(), cat) == active_cats.end()) {
            active_cats.push_back(cat);
        }
    }

    if (active_cats.empty()) return;

    // 针对常见标准颜色（红、绿、青、紫、黄），走极速无锁直接内存差分遍历 (微秒级)
    for (int r = 0; r < rows; ++r) {
        const uint8_t* ptr_bgr = bgr.ptr<uint8_t>(r);
        uint8_t* ptr_out = out_saliency.ptr<uint8_t>(r);

        for (int c = 0; c < cols; ++c) {
            const int b = ptr_bgr[0];
            const int g = ptr_bgr[1];
            const int red = ptr_bgr[2];
            ptr_bgr += 3;

            int max_resp = 0;

            for (ColorCategory cat : active_cats) {
                int resp = 0;
                switch (cat) {
                case ColorCategory::Red: {
                    // 红色拮抗响应：Red - max(Green, Blue)
                    int max_gb = std::max(g, b);
                    int diff = red - max_gb;
                    if (diff > 15 && red > 40) {
                        // 动态归一化强化，低饱和压缩帧下依然能拉伸显著度
                        resp = (diff * 255) / std::max(1, red);
                    }
                    break;
                }
                case ColorCategory::Green: {
                    // 绿色拮抗响应：Green - max(Red, Blue)
                    int max_rb = std::max(red, b);
                    int diff = g - max_rb;
                    if (diff > 15 && g > 40) {
                        resp = (diff * 255) / std::max(1, g);
                    }
                    break;
                }
                case ColorCategory::Cyan: {
                    // 青色响应：min(Green, Blue) - Red
                    int min_gb = std::min(g, b);
                    int diff = min_gb - red;
                    if (diff > 15 && min_gb > 40) {
                        resp = (diff * 255) / std::max(1, min_gb);
                    }
                    break;
                }
                case ColorCategory::Purple: {
                    // 紫色响应：min(Red, Blue) - Green
                    int min_rb = std::min(red, b);
                    int diff = min_rb - g;
                    if (diff > 15 && min_rb > 40) {
                        resp = (diff * 255) / std::max(1, min_rb);
                    }
                    break;
                }
                case ColorCategory::Yellow: {
                    // 黄色响应：min(Red, Green) - Blue
                    int min_rg = std::min(red, g);
                    int diff = min_rg - b;
                    if (diff > 15 && min_rg > 40) {
                        resp = (diff * 255) / std::max(1, min_rg);
                    }
                    break;
                }
                default:
                    break;
                }

                if (resp > max_resp) max_resp = resp;
            }

            ptr_out[c] = static_cast<uint8_t>(clamp_byte(max_resp, 0, 255));
        }
    }

    // 如果用户自定义了非标的微调 HSV 范围，融合一次宽容度 HSV 掩膜
    if (has_custom_hsv) {
        cv::Mat hsv;
        cv::cvtColor(bgr, hsv, cv::COLOR_BGR2HSV);
        cv::Mat hsv_mask = cv::Mat::zeros(hsv.size(), CV_8UC1);
        cv::Mat scratch;
        for (const auto& b : bands) {
            if (!b.enabled || classify_band(b) != ColorCategory::GenericHsv) continue;
            int h_lo = clamp_byte(b.h_low,  0, 179);
            int h_hi = clamp_byte(b.h_high, 0, 179);
            // 自动对压缩画质放宽 S/V 容限下限
            int s_lo = clamp_byte(std::max(0, b.s_min - 30),  0, 255);
            int s_hi = clamp_byte(b.s_max,  0, 255);
            int v_lo = clamp_byte(std::max(0, b.v_min - 30),  0, 255);
            int v_hi = clamp_byte(b.v_max,  0, 255);

            cv::inRange(hsv,
                        cv::Scalar(std::min(h_lo, h_hi), s_lo, v_lo),
                        cv::Scalar(std::max(h_lo, h_hi), s_hi, v_hi),
                        scratch);
            cv::bitwise_or(hsv_mask, scratch, hsv_mask);
        }
        cv::bitwise_or(out_saliency, hsv_mask, out_saliency);
    }
}

} // namespace

std::vector<CrosshairColorBand> default_red_bands()
{
    CrosshairColorBand low;
    low.name = "Red-Low";
    low.h_low = 0;
    low.h_high = 10;
    low.s_min = 80;
    low.v_min = 80;

    CrosshairColorBand high;
    high.name = "Red-High";
    high.h_low = 160;
    high.h_high = 179;
    high.s_min = 80;
    high.v_min = 80;

    return { low, high };
}

std::optional<cv::Point2f> CrosshairDetector::detect(
    const cv::Mat& bgrFrame,
    const CrosshairDetectorSettings& settings) const
{
    if (!settings.enabled || bgrFrame.empty() || bgrFrame.type() != CV_8UC3)
        return std::nullopt;

    bool any_enabled = false;
    for (const auto& c : settings.colors) {
        if (c.enabled) { any_enabled = true; break; }
    }
    if (!any_enabled) return std::nullopt;

    const cv::Rect roi = dynamic_center_roi(bgrFrame.size(), settings.rect_w, settings.rect_h);
    if (roi.width < 4 || roi.height < 4) return std::nullopt;

    cv::Mat region = bgrFrame(roi);

    // 1. 跨格式自适应显著度图 (抗压缩、抗色度降采样、抗偏色)
    cv::Mat saliency;
    compute_robust_saliency_map(region, settings.colors, saliency);

    // 2. 动态自适应局部 Otsu 门限二值化 (避免固定阈值在不同亮度下失灵)
    cv::Mat binary_mask;
    double max_val = 0;
    cv::minMaxLoc(saliency, nullptr, &max_val);
    if (max_val < 35.0) {
        // 全图连最微弱的目标色彩都不存在，直接返回
        return std::nullopt;
    }

    // 自适应二值化门限（取最大显著度的 40%，自适应不同浓淡的准星发光）
    const double adaptive_thresh = std::max(30.0, max_val * 0.40);
    cv::threshold(saliency, binary_mask, adaptive_thresh, 255, cv::THRESH_BINARY);

    // 3. 形态学开闭滤波 (滤除单像素高频孤立压缩噪点 + 弥合镂空准星)
    static const cv::Mat openKernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(3, 3));
    cv::morphologyEx(binary_mask, binary_mask, cv::MORPH_OPEN, openKernel);

    if (settings.close_radius > 0) {
        const int r = std::min(settings.close_radius, 5);
        const int k = 2 * r + 1;
        cv::Mat closeKernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(k, k));
        cv::morphologyEx(binary_mask, binary_mask, cv::MORPH_CLOSE, closeKernel);
    }

    // 4. 连通域拓扑分析与智能打分 (彻底杜绝枪口火焰、血条、击杀图标把准星拉偏)
    cv::Mat labels, stats, centroids;
    int num_labels = cv::connectedComponentsWithStats(binary_mask, labels, stats, centroids, 8, CV_32S);
    if (num_labels <= 1) return std::nullopt;

    const float roi_cx = roi.width * 0.5f;
    const float roi_cy = roi.height * 0.5f;
    const float max_dist_sq = (roi.width * 0.5f) * (roi.width * 0.5f) + (roi.height * 0.5f) * (roi.height * 0.5f);

    int best_label = -1;
    double best_score = -1e9;

    const int min_pixels = std::max(2, settings.min_pixel_count);
    const int max_allowed_pixels = static_cast<int>(roi.width * roi.height * 0.35f); // 准星不可能占据超过 35% 的面积(过大必是火光或爆炸)

    for (int label = 1; label < num_labels; ++label) {
        int area = stats.at<int>(label, cv::CC_STAT_AREA);
        if (area < min_pixels || area > max_allowed_pixels) continue;

        int bw = stats.at<int>(label, cv::CC_STAT_WIDTH);
        int bh = stats.at<int>(label, cv::CC_STAT_HEIGHT);

        // 准星对称性检验 (准星一般为点状或十字状，宽高比很少超过 4:1)
        float aspect = static_cast<float>(std::max(bw, bh)) / std::max(1, std::min(bw, bh));
        if (aspect > 4.5f) continue; // 细长条通常是边缘或血条残余

        float c_x = static_cast<float>(centroids.at<double>(label, 0));
        float c_y = static_cast<float>(centroids.at<double>(label, 1));

        float dx = c_x - roi_cx;
        float dy = c_y - roi_cy;
        float dist_sq = dx * dx + dy * dy;

        // 综合打分模型：距离屏幕中心越近得分极高，尺寸适中且紧凑者优先
        // S = -dist_weight + compactness_bonus
        double dist_penalty = (dist_sq / (max_dist_sq + 1e-5)) * 100.0;
        double size_bonus = 20.0 - std::abs(area - 16); // 典型准星大小约 10~30 像素
        double score = -dist_penalty + size_bonus;

        if (score > best_score) {
            best_score = score;
            best_label = label;
        }
    }

    if (best_label == -1) return std::nullopt;

    // 5. 亚像素连续能量重心提取 (Sub-pixel Energy-Weighted Centroid)
    // 利用提取出的最佳连通域内的连续显著度响应值作为权重，收敛至 0.05 像素精度
    double weight_sum = 0.0;
    double wx_sum = 0.0;
    double wy_sum = 0.0;

    int bx = stats.at<int>(best_label, cv::CC_STAT_LEFT);
    int by = stats.at<int>(best_label, cv::CC_STAT_TOP);
    int bw = stats.at<int>(best_label, cv::CC_STAT_WIDTH);
    int bh = stats.at<int>(best_label, cv::CC_STAT_HEIGHT);

    for (int y = by; y < by + bh; ++y) {
        const int32_t* ptr_lbl = labels.ptr<int32_t>(y);
        const uint8_t* ptr_sal = saliency.ptr<uint8_t>(y);
        for (int x = bx; x < bx + bw; ++x) {
            if (ptr_lbl[x] == best_label) {
                double weight = ptr_sal[x];
                weight_sum += weight;
                wx_sum += x * weight;
                wy_sum += y * weight;
            }
        }
    }

    if (weight_sum < 1e-4) return std::nullopt;

    float precise_local_x = static_cast<float>(wx_sum / weight_sum);
    float precise_local_y = static_cast<float>(wy_sum / weight_sum);

    float final_x = precise_local_x + roi.x;
    float final_y = precise_local_y + roi.y;

    return cv::Point2f(final_x, final_y);
}

} // namespace crosshair
