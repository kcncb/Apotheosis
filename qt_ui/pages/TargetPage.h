#pragma once

#include <QWidget>

class QTableWidget;
class QPushButton;

class TargetPage : public QWidget {
    Q_OBJECT

public:
    explicit TargetPage(QWidget* parent = nullptr);

private:
    void addRow();
    void removeSelectedRows();

    QTableWidget* m_table{};
    QPushButton* m_addBtn{};
    QPushButton* m_removeBtn{};
};
