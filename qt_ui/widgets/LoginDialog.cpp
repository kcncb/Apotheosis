#include "widgets/LoginDialog.h"
#include "widgets/IconFont.h"

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

LoginDialog::LoginDialog(QWidget* parent)
    : QDialog(parent) {
    setObjectName("loginDialog");
    setWindowTitle(QStringLiteral("Apotheosis"));
    setFixedWidth(400);
    setAttribute(Qt::WA_StyledBackground, true);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(44, 40, 44, 32);
    lay->setSpacing(0);

    if (IconFont::available()) {
        auto* icon = new QLabel(QString(IconFont::glyph("user-circle")));
        icon->setAlignment(Qt::AlignCenter);
        icon->setStyleSheet("font-family:\"tabler-icons\"; font-size:40px; color:#5E6AD2;");
        lay->addWidget(icon);
        lay->addSpacing(14);
    }

    m_title = new QLabel(QStringLiteral("Apotheosis"));
    m_title->setAlignment(Qt::AlignCenter);
    m_title->setStyleSheet("font-size:20px; font-weight:500; color:#1A1A1F;");
    lay->addWidget(m_title);

    m_subtitle = new QLabel(QStringLiteral("登录以继续"));
    m_subtitle->setAlignment(Qt::AlignCenter);
    m_subtitle->setStyleSheet("color:#8E8E96; font-size:13px;");
    lay->addWidget(m_subtitle);

    lay->addSpacing(26);

    m_user = new QLineEdit;
    m_user->setPlaceholderText(QStringLiteral("用户名"));
    m_user->setMinimumHeight(40);
    lay->addWidget(m_user);
    lay->addSpacing(10);

    m_pass = new QLineEdit;
    m_pass->setPlaceholderText(QStringLiteral("密码"));
    m_pass->setEchoMode(QLineEdit::Password);
    m_pass->setMinimumHeight(40);
    lay->addWidget(m_pass);

    m_confirmRow = new QWidget;
    auto* confirmLay = new QVBoxLayout(m_confirmRow);
    confirmLay->setContentsMargins(0, 10, 0, 0);
    confirmLay->setSpacing(0);
    m_confirm = new QLineEdit;
    m_confirm->setPlaceholderText(QStringLiteral("确认密码"));
    m_confirm->setEchoMode(QLineEdit::Password);
    m_confirm->setMinimumHeight(40);
    confirmLay->addWidget(m_confirm);
    lay->addWidget(m_confirmRow);

    m_status = new QLabel;
    m_status->setAlignment(Qt::AlignCenter);
    m_status->setStyleSheet("color:#E5484D; font-size:12px;");
    m_status->setMinimumHeight(18);
    lay->addSpacing(8);
    lay->addWidget(m_status);

    lay->addSpacing(6);
    m_primaryBtn = new QPushButton(QStringLiteral("登录"));
    m_primaryBtn->setProperty("class", "primary");
    m_primaryBtn->setMinimumHeight(44);
    m_primaryBtn->setCursor(Qt::PointingHandCursor);
    lay->addWidget(m_primaryBtn);

    lay->addSpacing(10);
    m_toggleBtn = new QPushButton(QStringLiteral("没有账号？注册"));
    m_toggleBtn->setCursor(Qt::PointingHandCursor);
    m_toggleBtn->setStyleSheet(
        "QPushButton{border:none; background:transparent; color:#5E6AD2; font-size:13px;}"
        "QPushButton:hover{color:#4A55C8;}");
    lay->addWidget(m_toggleBtn, 0, Qt::AlignCenter);

    connect(m_primaryBtn, &QPushButton::clicked, this, &LoginDialog::onPrimary);
    connect(m_toggleBtn, &QPushButton::clicked, this,
            [this] { setMode(!m_registerMode); });
    connect(m_pass, &QLineEdit::returnPressed, this, &LoginDialog::onPrimary);
    connect(m_confirm, &QLineEdit::returnPressed, this, &LoginDialog::onPrimary);

    setMode(false);
}

void LoginDialog::setMode(bool registerMode) {
    m_registerMode = registerMode;
    m_confirmRow->setVisible(registerMode);
    m_status->clear();
    m_subtitle->setText(registerMode ? QStringLiteral("创建一个新账号")
                                     : QStringLiteral("登录以继续"));
    m_primaryBtn->setText(registerMode ? QStringLiteral("注册")
                                       : QStringLiteral("登录"));
    m_toggleBtn->setText(registerMode ? QStringLiteral("已有账号？登录")
                                      : QStringLiteral("没有账号？注册"));
    adjustSize();
}

void LoginDialog::showError(const QString& msg) {
    m_status->setStyleSheet("color:#E5484D; font-size:12px;");
    m_status->setText(msg);
}

void LoginDialog::onPrimary() {
    const QString user = m_user->text().trimmed();
    const QString pass = m_pass->text();

    if (user.isEmpty() || pass.isEmpty()) {
        showError(QStringLiteral("请填写用户名和密码"));
        return;
    }

    if (m_registerMode) {
        if (m_confirm->text() != pass) {
            showError(QStringLiteral("两次输入的密码不一致"));
            return;
        }
        m_status->setStyleSheet("color:#1D9E75; font-size:12px;");
        m_status->setText(QStringLiteral("注册成功，请登录"));
        m_pass->clear();
        setMode(false);
        m_status->setStyleSheet("color:#1D9E75; font-size:12px;");
        m_status->setText(QStringLiteral("注册成功，请登录"));
        return;
    }

    accept();
}
