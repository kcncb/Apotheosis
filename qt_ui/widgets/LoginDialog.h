#pragma once

#include <QDialog>

class QLabel;
class QLineEdit;
class QPushButton;
class QWidget;

class LoginDialog : public QDialog {
    Q_OBJECT

public:
    explicit LoginDialog(QWidget* parent = nullptr);

private:
    void setMode(bool registerMode);
    void onPrimary();
    void showError(const QString& msg);

    bool m_registerMode = false;

    QLabel* m_title{};
    QLabel* m_subtitle{};
    QLineEdit* m_user{};
    QLineEdit* m_pass{};
    QLineEdit* m_confirm{};
    QWidget* m_confirmRow{};
    QPushButton* m_primaryBtn{};
    QPushButton* m_toggleBtn{};
    QLabel* m_status{};
};
