#include "widgets/LoginDialog.h"
#include "widgets/IconFont.h"

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QApplication>

#include "auth_state.h"

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

    m_subtitle = new QLabel(QStringLiteral("\xe7\x99\xbb\xe5\xbd\x95\xe4\xbb\xa5\xe7\xbb\xa7\xe7\xbb\xad"));
    m_subtitle->setAlignment(Qt::AlignCenter);
    m_subtitle->setStyleSheet("color:#71717A; font-size:13px;");
    lay->addWidget(m_subtitle);

    lay->addSpacing(26);

    m_user = new QLineEdit;
    m_user->setPlaceholderText(QStringLiteral("\xe7\x94\xa8\xe6\x88\xb7\xe5\x90\x8d"));
    m_user->setMinimumHeight(40);
    lay->addWidget(m_user);
    lay->addSpacing(10);

    m_pass = new QLineEdit;
    m_pass->setPlaceholderText(QStringLiteral("\xe5\xaf\x86\xe7\xa0\x81"));
    m_pass->setEchoMode(QLineEdit::Password);
    m_pass->setMinimumHeight(40);
    lay->addWidget(m_pass);

    m_confirmRow = new QWidget;
    auto* confirmLay = new QVBoxLayout(m_confirmRow);
    confirmLay->setContentsMargins(0, 10, 0, 0);
    confirmLay->setSpacing(0);
    m_confirm = new QLineEdit;
    m_confirm->setPlaceholderText(QStringLiteral("\xe7\xa1\xae\xe8\xae\xa4\xe5\xaf\x86\xe7\xa0\x81"));
    m_confirm->setEchoMode(QLineEdit::Password);
    m_confirm->setMinimumHeight(40);
    confirmLay->addWidget(m_confirm);
    lay->addWidget(m_confirmRow);

    m_inviteRow = new QWidget;
    auto* inviteLay = new QVBoxLayout(m_inviteRow);
    inviteLay->setContentsMargins(0, 10, 0, 0);
    inviteLay->setSpacing(0);
    m_invite = new QLineEdit;
    m_invite->setPlaceholderText(QStringLiteral("\xe9\x82\x80\xe8\xaf\xb7\xe7\xa0\x81"));
    m_invite->setMinimumHeight(40);
    inviteLay->addWidget(m_invite);
    lay->addWidget(m_inviteRow);

    m_status = new QLabel;
    m_status->setAlignment(Qt::AlignCenter);
    m_status->setStyleSheet("color:#E5484D; font-size:12px;");
    m_status->setMinimumHeight(18);
    m_status->setWordWrap(true);
    lay->addSpacing(8);
    lay->addWidget(m_status);

    lay->addSpacing(6);
    m_primaryBtn = new QPushButton(QStringLiteral("\xe7\x99\xbb\xe5\xbd\x95"));
    m_primaryBtn->setProperty("class", "primary");
    m_primaryBtn->setMinimumHeight(44);
    m_primaryBtn->setCursor(Qt::PointingHandCursor);
    lay->addWidget(m_primaryBtn);

    lay->addSpacing(10);
    m_toggleBtn = new QPushButton(QStringLiteral("\xe6\xb2\xa1\xe6\x9c\x89\xe8\xb4\xa6\xe5\x8f\xb7\xef\xbc\x9f\xe6\xb3\xa8\xe5\x86\x8c"));
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
    connect(m_invite, &QLineEdit::returnPressed, this, &LoginDialog::onPrimary);

    setMode(false);
}

bool LoginDialog::tryAutoLogin() {
    return auth::state().is_authorized();
}

void LoginDialog::setMode(bool registerMode) {
    m_registerMode = registerMode;
    m_confirmRow->setVisible(registerMode);
    m_inviteRow->setVisible(registerMode);
    m_status->clear();
    m_subtitle->setText(registerMode
        ? QStringLiteral("\xe5\x88\x9b\xe5\xbb\xba\xe4\xb8\x80\xe4\xb8\xaa\xe6\x96\xb0\xe8\xb4\xa6\xe5\x8f\xb7")
        : QStringLiteral("\xe7\x99\xbb\xe5\xbd\x95\xe4\xbb\xa5\xe7\xbb\xa7\xe7\xbb\xad"));
    m_primaryBtn->setText(registerMode
        ? QStringLiteral("\xe6\xb3\xa8\xe5\x86\x8c")
        : QStringLiteral("\xe7\x99\xbb\xe5\xbd\x95"));
    m_toggleBtn->setText(registerMode
        ? QStringLiteral("\xe5\xb7\xb2\xe6\x9c\x89\xe8\xb4\xa6\xe5\x8f\xb7\xef\xbc\x9f\xe7\x99\xbb\xe5\xbd\x95")
        : QStringLiteral("\xe6\xb2\xa1\xe6\x9c\x89\xe8\xb4\xa6\xe5\x8f\xb7\xef\xbc\x9f\xe6\xb3\xa8\xe5\x86\x8c"));
    adjustSize();
}

void LoginDialog::showError(const QString& msg) {
    m_status->setStyleSheet("color:#E5484D; font-size:12px;");
    m_status->setText(msg);
}

void LoginDialog::showSuccess(const QString& msg) {
    m_status->setStyleSheet("color:#1D9E75; font-size:12px;");
    m_status->setText(msg);
}

void LoginDialog::setUiBusy(bool busy) {
    m_primaryBtn->setEnabled(!busy);
    m_toggleBtn->setEnabled(!busy);
    m_user->setEnabled(!busy);
    m_pass->setEnabled(!busy);
    m_confirm->setEnabled(!busy);
    m_invite->setEnabled(!busy);
    if (busy)
        QApplication::setOverrideCursor(Qt::WaitCursor);
    else
        QApplication::restoreOverrideCursor();
}

void LoginDialog::onPrimary() {
    const QString user = m_user->text().trimmed();
    const QString pass = m_pass->text();

    if (user.isEmpty() || pass.isEmpty()) {
        showError(QStringLiteral("\xe8\xaf\xb7\xe5\xa1\xab\xe5\x86\x99\xe7\x94\xa8\xe6\x88\xb7\xe5\x90\x8d\xe5\x92\x8c\xe5\xaf\x86\xe7\xa0\x81"));
        return;
    }

    if (m_registerMode) {
        if (m_confirm->text() != pass) {
            showError(QStringLiteral("\xe4\xb8\xa4\xe6\xac\xa1\xe8\xbe\x93\xe5\x85\xa5\xe7\x9a\x84\xe5\xaf\x86\xe7\xa0\x81\xe4\xb8\x8d\xe4\xb8\x80\xe8\x87\xb4"));
            return;
        }
        const QString invite = m_invite->text().trimmed();
        if (invite.isEmpty()) {
            showError(QStringLiteral("\xe8\xaf\xb7\xe8\xbe\x93\xe5\x85\xa5\xe9\x82\x80\xe8\xaf\xb7\xe7\xa0\x81"));
            return;
        }

        setUiBusy(true);
        QApplication::processEvents();

        bool ok = auth::state().register_user(
            user.toStdString(), pass.toStdString(), invite.toStdString());

        setUiBusy(false);

        if (!ok) {
            std::string err = auth::state().last_error();
            showError(err.empty()
                ? QStringLiteral("\xe6\xb3\xa8\xe5\x86\x8c\xe5\xa4\xb1\xe8\xb4\xa5")
                : QString::fromUtf8(err.c_str()));
            return;
        }

        m_pass->clear();
        m_confirm->clear();
        m_invite->clear();
        setMode(false);
        showSuccess(QStringLiteral("\xe6\xb3\xa8\xe5\x86\x8c\xe6\x88\x90\xe5\x8a\x9f\xef\xbc\x8c\xe8\xaf\xb7\xe7\x99\xbb\xe5\xbd\x95"));
        return;
    }

    setUiBusy(true);
    QApplication::processEvents();

    bool ok = auth::state().login(user.toStdString(), pass.toStdString());

    setUiBusy(false);

    if (!ok) {
        std::string err = auth::state().last_error();
        showError(err.empty()
            ? QStringLiteral("\xe7\x99\xbb\xe5\xbd\x95\xe5\xa4\xb1\xe8\xb4\xa5")
            : QString::fromUtf8(err.c_str()));
        return;
    }

    accept();
}
