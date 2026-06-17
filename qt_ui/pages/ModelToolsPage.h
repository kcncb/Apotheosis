#pragma once

#include <QWidget>

class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;

class ModelToolsPage : public QWidget {
    Q_OBJECT

public:
    explicit ModelToolsPage(QWidget* parent = nullptr);

private slots:
    void onBrowseModel();
    void onEncrypt();
    void onAuthorize();

private:
    QLineEdit* m_modelPath{};
    QPushButton* m_encryptBtn{};
    QProgressBar* m_encryptProgress{};
    QLabel* m_encryptStatus{};

    QLineEdit* m_authAccount{};
    QLineEdit* m_authModelId{};
    QLabel* m_authStatus{};
};
