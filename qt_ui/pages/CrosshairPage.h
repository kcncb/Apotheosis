#pragma once

#include <QWidget>

class QSpinBox;
class QTableWidget;
class QPushButton;

class CrosshairPage : public QWidget {
    Q_OBJECT

public:
    explicit CrosshairPage(QWidget* parent = nullptr);

private:
    void addColorRow();
    void removeSelectedRows();

    QSpinBox* m_width{};
    QSpinBox* m_height{};
    QSpinBox* m_minPixels{};
    QSpinBox* m_closeRadius{};

    QTableWidget* m_colorTable{};
    QPushButton* m_addBtn{};
    QPushButton* m_removeBtn{};
};
