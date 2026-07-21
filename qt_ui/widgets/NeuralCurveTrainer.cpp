#include "widgets/NeuralCurveTrainer.h"
#include "widgets/FreehandCurveEditor.h"

#include <QCursor>
#include <QCheckBox>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSpinBox>
#include <QTextStream>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <random>

namespace {
constexpr int kPadding = 24;
constexpr double kHitRadius = 14.0;
// 采集数据保持轻量，训练后的连续网络再高密度导出。
constexpr int kTrainingSamples = 256;
}

NeuralCurveTrainingCanvas::NeuralCurveTrainingCanvas(QWidget* parent)
    : QWidget(parent) {
    setMinimumSize(560, 340);
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);
}

void NeuralCurveTrainingCanvas::startTraining(int rounds, bool append) {
    m_totalRounds = std::clamp(rounds, 1, 200);
    m_completedRounds = 0;
    if (!append) m_samples.clear();
    m_training = true;
    beginRound();
}

void NeuralCurveTrainingCanvas::stopTraining() {
    m_training = false;
    m_stroke.clear();
    update();
}

QPointF NeuralCurveTrainingCanvas::randomTarget(const QPointF& start) const {
    const QRectF area = rect().adjusted(kPadding, kPadding, -kPadding, -kPadding);
    QPointF result = area.center();
    const double minDistance = std::min(area.width(), area.height()) * 0.35;
    for (int attempt = 0; attempt < 32; ++attempt) {
        result.setX(area.left() + QRandomGenerator::global()->generateDouble() * area.width());
        result.setY(area.top() + QRandomGenerator::global()->generateDouble() * area.height());
        if (QLineF(start, result).length() >= minDistance)
            break;
    }
    return result;
}

void NeuralCurveTrainingCanvas::beginRound() {
    m_start = mapFromGlobal(QCursor::pos());
    if (!QRectF(rect()).adjusted(kPadding, kPadding, -kPadding, -kPadding).contains(m_start))
        m_start = rect().center();
    m_target = randomTarget(m_start);
    m_stroke.clear();
    m_stroke.push_back(m_start);
    if (progressChanged) progressChanged(m_completedRounds, m_totalRounds);
    update();
}

QVector<QPointF> NeuralCurveTrainingCanvas::normalizeStroke(
    const QVector<QPointF>& stroke, const QPointF& start, const QPointF& target) const {
    QVector<QPointF> normalized;
    const QPointF delta = target - start;
    const double length = std::hypot(delta.x(), delta.y());
    if (length < 1.0 || stroke.size() < 5) return normalized;
    double travelled = 0.0;
    for (int i = 1; i < stroke.size(); ++i) {
        const double step = QLineF(stroke[i - 1], stroke[i]).length();
        if (step > length * 0.45) return normalized; // 传送/丢帧式异常轨迹
        travelled += step;
    }
    if (travelled > length * 4.0) return normalized;
    const QPointF axis(delta.x() / length, delta.y() / length);
    const QPointF perp(-axis.y(), axis.x());
    double maxProgress = -1.0;
    for (const QPointF& point : stroke) {
        const QPointF rel = point - start;
        const double progress = std::clamp(
            (rel.x() * axis.x() + rel.y() * axis.y()) / length, 0.0, 1.0);
        const double deviation = std::clamp(
            (rel.x() * perp.x() + rel.y() * perp.y()) / length, -1.0, 1.0);
        if (progress > maxProgress + 0.002) {
            normalized.push_back(QPointF(progress, deviation));
            maxProgress = progress;
        }
    }
    if (normalized.size() < 2) return {};
    normalized.front() = QPointF(0.0, 0.0);
    normalized.push_back(QPointF(1.0, 0.0));

    QVector<QPointF> sampled;
    sampled.reserve(kTrainingSamples);
    int segment = 0;
    for (int i = 0; i < kTrainingSamples; ++i) {
        const double x = static_cast<double>(i) / (kTrainingSamples - 1);
        while (segment + 1 < normalized.size() && normalized[segment + 1].x() < x)
            ++segment;
        const int next = std::min(segment + 1, static_cast<int>(normalized.size()) - 1);
        const double x0 = normalized[segment].x();
        const double x1 = normalized[next].x();
        const double f = (x1 > x0 + 1e-9) ? (x - x0) / (x1 - x0) : 0.0;
        const double y = normalized[segment].y()
                       + (normalized[next].y() - normalized[segment].y()) * f;
        sampled.push_back(QPointF(x, std::clamp(y, -1.0, 1.0)));
    }
    sampled.front().setY(0.0);
    sampled.back().setY(0.0);
    return sampled;
}

void NeuralCurveTrainingCanvas::finishRound() {
    QVector<QPointF> normalized = normalizeStroke(m_stroke, m_start, m_target);
    if (!normalized.isEmpty())
        m_samples.push_back(std::move(normalized));
    ++m_completedRounds;
    if (m_completedRounds >= m_totalRounds) {
        m_training = false;
        if (progressChanged) progressChanged(m_completedRounds, m_totalRounds);
        update();
        if (collectionFinished) collectionFinished(m_samples);
        return;
    }
    beginRound();
}

void NeuralCurveTrainingCanvas::mouseMoveEvent(QMouseEvent* event) {
    if (!m_training) return;
    const QPointF point = event->position();
    if (m_stroke.isEmpty() || QLineF(m_stroke.back(), point).length() >= 1.0)
        m_stroke.push_back(point);
    if (QLineF(point, m_target).length() <= kHitRadius)
        finishRound();
    else
        update();
}

void NeuralCurveTrainingCanvas::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), QColor("#F7F8FA"));
    painter.setPen(QPen(QColor("#D8DBE2"), 1));
    painter.setBrush(QColor("#FFFFFF"));
    painter.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 10, 10);

    if (!m_training) {
        painter.setPen(QColor("#71717A"));
        painter.drawText(rect(), Qt::AlignCenter,
                         QString::fromUtf8(u8"设置训练轮数后点击开始\n自然移动鼠标命中随机目标点"));
        return;
    }

    if (m_stroke.size() >= 2) {
        QPainterPath path(m_stroke.front());
        for (int i = 1; i < m_stroke.size(); ++i) path.lineTo(m_stroke[i]);
        painter.setPen(QPen(QColor(74, 127, 229, 150), 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
    }

    painter.setPen(QPen(QColor("#16A34A"), 2));
    painter.setBrush(QColor("#22C55E"));
    painter.drawEllipse(m_target, kHitRadius, kHitRadius);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor("#FFFFFF"));
    painter.drawEllipse(m_target, 4, 4);

    painter.setPen(QColor("#52525B"));
    painter.drawText(QRectF(12, 8, width() - 24, 24), Qt::AlignLeft,
                     QString::fromUtf8(u8"第 %1 / %2 轮").arg(m_completedRounds + 1).arg(m_totalRounds));
}

NeuralCurveTrainerDialog::NeuralCurveTrainerDialog(QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(QString::fromUtf8(u8"神经网络曲线训练"));
    setModal(false);
    resize(620, 460);

    auto* root = new QVBoxLayout(this);
    auto* description = new QLabel(QString::fromUtf8(
        u8"每轮将随机出现一个绿色目标点。请按平时操作习惯移动鼠标命中它，"
        u8"完成后由轻量神经网络学习你的平均运动轨迹。"));
    description->setWordWrap(true);
    root->addWidget(description);

    auto* controls = new QHBoxLayout;
    controls->addWidget(new QLabel(QString::fromUtf8(u8"训练轮数")));
    m_rounds = new QSpinBox;
    m_rounds->setRange(3, 200);
    m_rounds->setValue(20);
    controls->addWidget(m_rounds);
    m_append = new QCheckBox(QString::fromUtf8(u8"追加训练"));
    controls->addWidget(m_append);
    m_start = new QPushButton(QString::fromUtf8(u8"开始训练"));
    controls->addWidget(m_start);
    controls->addStretch();
    m_status = new QLabel(QString::fromUtf8(u8"等待开始"));
    controls->addWidget(m_status);
    root->addLayout(controls);

    m_canvas = new NeuralCurveTrainingCanvas;
    root->addWidget(m_canvas, 1);

    m_preview = new FreehandCurveEditor;
    m_preview->setEnabled(false);
    m_preview->hide();
    root->addWidget(m_preview, 0, Qt::AlignHCenter);

    auto* decisions = new QHBoxLayout;
    decisions->addStretch();
    m_discard = new QPushButton(QString::fromUtf8(u8"放弃"));
    m_export = new QPushButton(QString::fromUtf8(u8"保存原始训练集"));
    m_apply = new QPushButton(QString::fromUtf8(u8"应用到当前热键"));
    m_discard->setEnabled(false);
    m_export->setEnabled(false);
    m_apply->setEnabled(false);
    decisions->addWidget(m_export);
    decisions->addWidget(m_discard);
    decisions->addWidget(m_apply);
    root->addLayout(decisions);

    connect(m_start, &QPushButton::clicked, this, [this] {
        if (m_canvas->isTraining()) {
            m_canvas->stopTraining();
            setRunning(false);
            m_status->setText(QString::fromUtf8(u8"已停止"));
        } else {
            setRunning(true);
            m_canvas->startTraining(m_rounds->value(), m_append->isChecked());
        }
    });
    m_canvas->progressChanged = [this](int done, int total) {
        m_status->setText(QString::fromUtf8(u8"采集 %1 / %2").arg(done).arg(total));
    };
    m_canvas->collectionFinished = [this](const QVector<QVector<QPointF>>& trajectories) {
        m_trajectories = trajectories;
        m_status->setText(QString::fromUtf8(u8"正在训练轻量网络…"));
        double validationMse = 0.0;
        m_trained = trainNetwork(trajectories, &validationMse, &m_weights);
        m_haveTrained = true;
        m_preview->setSamples(m_trained);
        m_preview->show();
        m_apply->setEnabled(true);
        m_discard->setEnabled(true);
        m_export->setEnabled(true);
        setRunning(false);
        m_status->setText(QString::fromUtf8(u8"训练完成，验证误差 %1")
                          .arg(validationMse, 0, 'f', 6));
    };
    connect(m_apply, &QPushButton::clicked, this, [this] {
        if (!m_haveTrained) return;
        emit curveTrained(m_trained, m_weights);
        m_status->setText(QString::fromUtf8(u8"已应用到当前热键"));
        m_apply->setEnabled(false);
    });
    connect(m_discard, &QPushButton::clicked, this, [this] { reject(); });
    connect(m_export, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(
            this, QString::fromUtf8(u8"保存原始训练集"),
            QStringLiteral("curve_training.csv"), QStringLiteral("CSV (*.csv)"));
        if (path.isEmpty()) return;
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return;
        QTextStream stream(&file);
        stream << "trajectory,progress,deviation\n";
        for (int ti = 0; ti < m_trajectories.size(); ++ti)
            for (const QPointF& point : m_trajectories[ti])
                stream << ti << ',' << point.x() << ',' << point.y() << '\n';
    });
}

void NeuralCurveTrainerDialog::setRunning(bool running) {
    m_rounds->setEnabled(!running);
    m_start->setText(running ? QString::fromUtf8(u8"停止")
                             : QString::fromUtf8(u8"重新训练"));
}

std::array<float, NeuralCurveTrainerDialog::kSampleCount>
NeuralCurveTrainerDialog::trainNetwork(const QVector<QVector<QPointF>>& trajectories,
                                       double* validationMse,
                                       std::array<float, 25>* weights) const {
    // 1 -> 8 -> 1 MLP. The t*(1-t) envelope pins both path endpoints to zero.
    constexpr int hidden = 8;
    std::mt19937 rng(0x41504F54u);
    std::normal_distribution<double> init(0.0, 0.18);
    std::array<double, hidden> w1{}, b1{}, w2{};
    for (int h = 0; h < hidden; ++h) { w1[h] = init(rng); w2[h] = init(rng); }
    double b2 = 0.0;
    const double learningRate = 0.025;

    for (int epoch = 0; epoch < 500; ++epoch) {
        for (int ti = 0; ti < trajectories.size(); ++ti) {
            if (trajectories.size() >= 5 && ti % 5 == 0) continue;
            const auto& trajectory = trajectories[ti];
            for (const QPointF& sample : trajectory) {
                const double x = sample.x() * 2.0 - 1.0;
                const double envelope = 4.0 * sample.x() * (1.0 - sample.x());
                std::array<double, hidden> activation{};
                double raw = b2;
                for (int h = 0; h < hidden; ++h) {
                    activation[h] = std::tanh(w1[h] * x + b1[h]);
                    raw += w2[h] * activation[h];
                }
                const double prediction = std::tanh(raw) * envelope;
                const double dRaw = 2.0 * (prediction - sample.y())
                                  * envelope * (1.0 - std::tanh(raw) * std::tanh(raw));
                const auto oldW2 = w2;
                for (int h = 0; h < hidden; ++h) {
                    w2[h] -= learningRate * dRaw * activation[h];
                    const double dz = dRaw * oldW2[h]
                                    * (1.0 - activation[h] * activation[h]);
                    w1[h] -= learningRate * dz * x;
                    b1[h] -= learningRate * dz;
                }
                b2 -= learningRate * dRaw;
            }
        }
    }

    std::array<float, kSampleCount> result{};
    for (int i = 0; i < kSampleCount; ++i) {
        const double t = static_cast<double>(i) / (kSampleCount - 1);
        const double x = t * 2.0 - 1.0;
        double raw = b2;
        for (int h = 0; h < hidden; ++h)
            raw += w2[h] * std::tanh(w1[h] * x + b1[h]);
        result[i] = static_cast<float>(std::clamp(
            std::tanh(raw) * 4.0 * t * (1.0 - t), -1.0, 1.0));
    }
    result.front() = 0.0f;
    result.back() = 0.0f;
    if (weights) {
        for (int h = 0; h < hidden; ++h) {
            (*weights)[h] = static_cast<float>(w1[h]);
            (*weights)[8 + h] = static_cast<float>(b1[h]);
            (*weights)[16 + h] = static_cast<float>(w2[h]);
        }
        (*weights)[24] = static_cast<float>(b2);
    }

    if (validationMse) {
        double sum = 0.0;
        size_t count = 0;
        for (int ti = 0; ti < trajectories.size(); ++ti) {
            if (trajectories.size() >= 5 && ti % 5 != 0) continue;
            for (const QPointF& sample : trajectories[ti]) {
                const double t = sample.x();
                const double x = t * 2.0 - 1.0;
                double raw = b2;
                for (int h = 0; h < hidden; ++h)
                    raw += w2[h] * std::tanh(w1[h] * x + b1[h]);
                const double predicted = std::tanh(raw) * 4.0 * t * (1.0 - t);
                const double error = predicted - sample.y();
                sum += error * error;
                ++count;
            }
        }
        *validationMse = count > 0 ? sum / static_cast<double>(count) : 0.0;
    }
    return result;
}
