#pragma once

#include <QWidget>

class QComboBox;
class QStackedWidget;

class HardwarePage : public QWidget {
    Q_OBJECT

public:
    explicit HardwarePage(QWidget* parent = nullptr);

private:
    void onInputMethodChanged(int index);

    QComboBox* m_inputMethodCombo{};
    QStackedWidget* m_deviceStack{};
};
