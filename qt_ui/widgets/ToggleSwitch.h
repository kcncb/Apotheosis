#pragma once

#include <QAbstractButton>

class ToggleSwitch : public QAbstractButton {
    Q_OBJECT
    Q_PROPERTY(qreal pos READ pos WRITE setPos)

public:
    explicit ToggleSwitch(QWidget* parent = nullptr);

    QSize sizeHint() const override;
    qreal pos() const { return m_pos; }
    void setPos(qreal pos);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    qreal m_pos = 0.0;
};
