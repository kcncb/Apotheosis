#pragma once

#include <QDialog>
#include <QPointF>
#include <QVector>
#include <QWidget>

#include <array>
#include <functional>

class QLabel;
class QPushButton;
class QSpinBox;
class QCheckBox;
class FreehandCurveEditor;

class NeuralCurveTrainingCanvas final : public QWidget {
public:
    explicit NeuralCurveTrainingCanvas(QWidget* parent = nullptr);

    void startTraining(int rounds, bool append = false);
    void stopTraining();
    bool isTraining() const { return m_training; }

    std::function<void(int, int)> progressChanged;
    std::function<void(const QVector<QVector<QPointF>>&)> collectionFinished;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

private:
    void beginRound();
    void finishRound();
    QPointF randomTarget(const QPointF& start) const;
    QVector<QPointF> normalizeStroke(const QVector<QPointF>& stroke,
                                     const QPointF& start,
                                     const QPointF& target) const;

    bool m_training = false;
    int m_totalRounds = 0;
    int m_completedRounds = 0;
    QPointF m_start;
    QPointF m_target;
    QVector<QPointF> m_stroke;
    QVector<QVector<QPointF>> m_samples;
};

class NeuralCurveTrainerDialog final : public QDialog {
    Q_OBJECT

public:
    static constexpr int kSampleCount = 32768;

    explicit NeuralCurveTrainerDialog(QWidget* parent = nullptr);

signals:
    void curveTrained(const std::array<float, kSampleCount>& samples,
                      const std::array<float, 25>& weights);

private:
    std::array<float, kSampleCount> trainNetwork(
        const QVector<QVector<QPointF>>& trajectories, double* validationMse,
        std::array<float, 25>* weights) const;
    void setRunning(bool running);

    NeuralCurveTrainingCanvas* m_canvas{};
    QSpinBox* m_rounds{};
    QPushButton* m_start{};
    QPushButton* m_apply{};
    QPushButton* m_discard{};
    QPushButton* m_export{};
    QCheckBox* m_append{};
    QLabel* m_status{};
    FreehandCurveEditor* m_preview{};
    std::array<float, kSampleCount> m_trained{};
    std::array<float, 25> m_weights{};
    bool m_haveTrained = false;
    QVector<QVector<QPointF>> m_trajectories;
};
