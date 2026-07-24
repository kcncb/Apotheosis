#pragma once

#include <QAbstractButton>

class QPropertyAnimation;

class ToggleSwitch : public QAbstractButton {
    Q_OBJECT
    Q_PROPERTY(qreal pos READ pos WRITE setPos)

public:
    explicit ToggleSwitch(QWidget* parent = nullptr);

    QSize sizeHint() const override;
    qreal pos() const { return m_pos; }
    void setPos(qreal pos);

protected:
    void checkStateSet() override;
    void paintEvent(QPaintEvent* event) override;

private:
    QPropertyAnimation* m_animation{};
    qreal m_pos = 0.0;
};
