#include "glass_filter.h"

#include <algorithm>
#include <cmath>

namespace crosshair
{

namespace
{

int clamp_byte(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// OR a single HSV band into `out` (CV_8UC1, same size as `hsv`).
void accumulate_band(const cv::Mat& hsv,
                     const CrosshairColorBand& b,
                     cv::Mat& out,
                     cv::Mat& scratch)
{
    if (!b.enabled) return;

    const int h_lo = clamp_byte(b.h_low,  0, 179);
    const int h_hi = clamp_byte(b.h_high, 0, 179);
    const int s_lo = clamp_byte(b.s_min,  0, 255);
    const int s_hi = clamp_byte(b.s_max,  0, 255);
    const int v_lo = clamp_byte(b.v_min,  0, 255);
    const int v_hi = clamp_byte(b.v_max,  0, 255);

    cv::inRange(hsv,
                cv::Scalar(std::min(h_lo, h_hi), std::min(s_lo, s_hi), std::min(v_lo, v_hi)),
                cv::Scalar(std::max(h_lo, h_hi), std::max(s_lo, s_hi), std::max(v_lo, v_hi)),
                scratch);
    cv::bitwise_or(out, scratch, out);
}

} // namespace

std::vector<CrosshairColorBand> default_glass_bands()
{
    CrosshairColorBand blue;
    blue.name = "Glass-Blue";
    blue.enabled = true;
    blue.h_low  = 90;
    blue.h_high = 115;
    blue.s_min  = 5;
    blue.s_max  = 90;
    blue.v_min  = 170;
    blue.v_max  = 255;

    CrosshairColorBand green;
    green.name = "Glass-Green";
    green.enabled = true;
    green.h_low  = 55;
    green.h_high = 85;
    green.s_min  = 5;
    green.s_max  = 90;
    green.v_min  = 170;
    green.v_max  = 255;

    return { blue, green };
}

GlassResult GlassFilter::check(const cv::Mat& bgrFrame,
                               const cv::Rect& bbox,
                               const GlassFilterSettings& settings) const
{
    GlassResult r;

    if (!settings.enabled)
        return r;
    if (bgrFrame.empty() || bgrFrame.type() != CV_8UC3)
        return r;

    const cv::Rect frame_rect(0, 0, bgrFrame.cols, bgrFrame.rows);
    cv::Rect clipped = bbox & frame_rect;
    if (clipped.width < 4 || clipped.height < 4)
        return r;

    const int short_side = std::min(clipped.width, clipped.height);
    if (short_side < std::max(4, settings.min_box_short_side))
        return r;

    // 至少一个色带启用,否则跳过(配置不全)。
    bool any_band = false;
    for (const auto& c : settings.colors)
        if (c.enabled) { any_band = true; break; }
    if (!any_band)
        return r;

    // 环厚度:取短边 × frac,夹到 [1, short/2 - 1] 之间。短/2-1 是物理上限,
    // 再厚环就闭合成全 ROI 了,失去边缘特征。
    const float frac = std::clamp(settings.edge_ring_frac, 0.05f, 0.45f);
    int ring = static_cast<int>(std::lround(short_side * frac));
    ring = std::clamp(ring, 1, short_side / 2 - 1);
    if (ring < 1)
        return r;

    // 单次裁切到 box,再统一 cvtColor + inRange,避免 4 个 strip 各做一次
    // cvtColor 的固定开销。box 像素通常 < 1.5w,< 0.2 ms。
    cv::Mat region = bgrFrame(clipped);
    cv::Mat hsv;
    cv::cvtColor(region, hsv, cv::COLOR_BGR2HSV);

    cv::Mat mask = cv::Mat::zeros(hsv.size(), CV_8UC1);
    cv::Mat scratch;
    for (const auto& band : settings.colors)
        accumulate_band(hsv, band, mask, scratch);

    // 环内像素 = 整框 nonzero − 内孔 nonzero。内孔 = box 收缩 ring 像素后
    // 的中心矩形。一次性算两次 countNonZero,O(N) 不需要再建第二张掩码。
    const int W = clipped.width;
    const int H = clipped.height;
    const int inner_w = W - 2 * ring;
    const int inner_h = H - 2 * ring;

    int ring_pixels   = 0;
    int matched_total = 0;
    int matched_inner = 0;
    matched_total = cv::countNonZero(mask);
    const int total_pixels = W * H;

    if (inner_w > 0 && inner_h > 0)
    {
        cv::Rect inner_rect(ring, ring, inner_w, inner_h);
        cv::Mat inner = mask(inner_rect);
        matched_inner = cv::countNonZero(inner);
        ring_pixels = total_pixels - inner_w * inner_h;
    }
    else
    {
        // 环厚已经吃掉整框,直接用全框算覆盖率。
        ring_pixels = total_pixels;
    }

    const int matched_ring = matched_total - matched_inner;
    const float coverage =
        (ring_pixels > 0)
            ? static_cast<float>(matched_ring) / static_cast<float>(ring_pixels)
            : 0.0f;

    r.evaluated      = true;
    r.coverage       = coverage;
    r.ring_pixels    = ring_pixels;
    r.matched_pixels = matched_ring;
    r.is_behind_glass =
        coverage >= std::clamp(settings.coverage_threshold, 0.05f, 0.95f);
    return r;
}

} // namespace crosshair
