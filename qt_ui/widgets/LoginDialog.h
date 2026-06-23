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

    static bool tryAutoLogin();

private:
    void setMode(bool registerMode);
    void onPrimary();
    void showError(const QString& msg);
    void showSuccess(const QString& msg);
    void setUiBusy(bool busy);

    bool m_registerMode = false;

    QLabel* m_title{};
    QLabel* m_subtitle{};
    QLineEdit* m_user{};
    QLineEdit* m_pass{};
    QLineEdit* m_confirm{};
    QWidget* m_confirmRow{};
    QLineEdit* m_invite{};
    QWidget* m_inviteRow{};
    QPushButton* m_primaryBtn{};
    QPushButton* m_toggleBtn{};
    QLabel* m_status{};
};
