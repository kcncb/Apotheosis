#pragma once

// 通用控制器层 —— 基础类型
//
// ★★ 单位法（docs/generic-controller-layer.md §1）：
//   本层只允许出现两种单位 —— **检测图像素** 与 **整数鼠标计数**。
//   任何"米"、"倍镜倍率"、"游戏机灵敏度"都非法。设计动机可以看那些数字，
//   但代码里不许出现。
//
// ★ 本文件必须【零依赖】：不引 OpenCV、不引 Windows、不引 Qt。
//   理由：控制器是纯数值计算，必须能在 macOS 上编进逻辑测试
//   （主程序 ai 目标在非 Windows 上根本编不了，见 CLAUDE.md）。

#include <cstdint>

namespace control {

// ── 二维点 / 向量（检测图像素） ───────────────────────────────────────────
struct Vec2
{
    double x = 0.0;
    double y = 0.0;

    Vec2() = default;
    Vec2(double x_, double y_) : x(x_), y(y_) {}

    Vec2 operator+(const Vec2& o) const { return { x + o.x, y + o.y }; }
    Vec2 operator-(const Vec2& o) const { return { x - o.x, y - o.y }; }
    Vec2 operator*(double s) const { return { x * s, y * s }; }
    Vec2 operator/(double s) const { return { x / s, y / s }; }
    Vec2& operator+=(const Vec2& o) { x += o.x; y += o.y; return *this; }
    Vec2& operator-=(const Vec2& o) { x -= o.x; y -= o.y; return *this; }

    double dot(const Vec2& o) const { return x * o.x + y * o.y; }
    double norm() const;
    double normSq() const { return x * x + y * y; }
};

// ── 检测框（检测图像素） ─────────────────────────────────────────────────
// 刻意不用 cv::Rect：控制层不依赖 OpenCV。
// 用 double 而不是 int —— 推理输出本身就是浮点框（preciseBox），
// 在这里提前取整会把抖动放大成量化噪声，而那正是稳定器要处理的东西。
struct Box
{
    double x = 0.0;       // 左上角 x
    double y = 0.0;       // 左上角 y
    double w = 0.0;       // 宽
    double h = 0.0;       // 高

    double centerX() const { return x + w * 0.5; }
    double centerY() const { return y + h * 0.5; }
    Vec2 center() const { return { centerX(), centerY() }; }

    double area() const { return w * h; }
    double diagonal() const;   // sqrt(w² + h²)

    bool valid() const { return w > 0.0 && h > 0.0; }
};

// ── 一个候选目标 ─────────────────────────────────────────────────────────
// 这是 L1 的输入单元：推理输出经类别桶筛选后剩下的东西。
struct Candidate
{
    Box box;
    int classId = -1;
    double confidence = 0.0;   // [0,1]
    // ★ 时间戳字段刻意【不加】—— 用户明确决定不做"检测延迟降权"
    //   （"无法区分该帧是否是 200ms 前的，就当所有帧都能及时处理"）。
    //   新鲜度门禁是二值的，由调用方在进 L1 之前判掉，不进本层。
};

// ── 整数鼠标计数（本层的唯一出口量） ─────────────────────────────────────
struct Counts
{
    int x = 0;
    int y = 0;
};

// ── 方向枚举：让"分方向"的代码能统一写 ───────────────────────────────────
enum class Axis { X = 0, Y = 1 };

inline Vec2 axisVec(Axis a, double v)
{
    return (a == Axis::X) ? Vec2{ v, 0.0 } : Vec2{ 0.0, v };
}

} // namespace control
