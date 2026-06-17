#include "pages/ModelToolsPage.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"

#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

ModelToolsPage::ModelToolsPage(QWidget* parent)
    : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* container = new QWidget();
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(14);

    // -- Card 1: 模型加密 --
    auto* encryptCard = new CardWidget(QStringLiteral("模型加密"), QStringLiteral("lock"), container);
    auto* ec = encryptCard->contentLayout();

    // File path line edit + browse button packed into one control for fieldRow.
    auto* pathControl = new QWidget();
    auto* pathRow = new QHBoxLayout(pathControl);
    pathRow->setContentsMargins(0, 0, 0, 0);
    pathRow->setSpacing(8);

    m_modelPath = new QLineEdit();
    m_modelPath->setPlaceholderText(QStringLiteral("模型文件路径"));
    pathRow->addWidget(m_modelPath, 1);

    auto* browseBtn = new QPushButton(QStringLiteral("浏览..."));
    pathRow->addWidget(browseBtn);

    ec->addWidget(FormKit::fieldRow(QStringLiteral("文件"), pathControl));

    m_encryptBtn = new QPushButton(QStringLiteral("加密模型"));
    m_encryptBtn->setProperty("class", "primary");
    m_encryptBtn->setFixedWidth(120);
    m_encryptBtn->setCursor(Qt::PointingHandCursor);
    {
        auto* row = new QHBoxLayout;
        row->setContentsMargins(0, 4, 0, 0);
        row->addWidget(m_encryptBtn);
        row->addStretch();
        ec->addLayout(row);
    }

    m_encryptProgress = new QProgressBar();
    m_encryptProgress->setRange(0, 100);
    m_encryptProgress->setValue(0);
    m_encryptProgress->setVisible(false);
    ec->addWidget(m_encryptProgress);

    m_encryptStatus = new QLabel();
    m_encryptStatus->setProperty("class", "secondary");
    ec->addWidget(m_encryptStatus);

    layout->addWidget(encryptCard);

    // -- Card 2: 模型授权 --
    auto* authCard = new CardWidget(QStringLiteral("模型授权"), QStringLiteral("key"), container);
    auto* ac = authCard->contentLayout();

    m_authAccount = new QLineEdit();
    m_authAccount->setPlaceholderText(QStringLiteral("账户名"));
    ac->addWidget(FormKit::fieldRow(QStringLiteral("账户"), m_authAccount));

    m_authModelId = new QLineEdit();
    m_authModelId->setPlaceholderText(QStringLiteral("模型标识符"));
    ac->addWidget(FormKit::fieldRow(QStringLiteral("模型"), m_authModelId));

    auto* authBtn = new QPushButton(QStringLiteral("授权"));
    authBtn->setProperty("class", "primary");
    authBtn->setFixedWidth(120);
    authBtn->setCursor(Qt::PointingHandCursor);
    {
        auto* row = new QHBoxLayout;
        row->setContentsMargins(0, 4, 0, 0);
        row->addWidget(authBtn);
        row->addStretch();
        ac->addLayout(row);
    }

    m_authStatus = new QLabel();
    m_authStatus->setProperty("class", "secondary");
    ac->addWidget(m_authStatus);

    layout->addWidget(authCard);

    layout->addStretch();

    scroll->setWidget(container);
    root->addWidget(scroll);

    connect(browseBtn, &QPushButton::clicked, this, &ModelToolsPage::onBrowseModel);
    connect(m_encryptBtn, &QPushButton::clicked, this, &ModelToolsPage::onEncrypt);
    connect(authBtn, &QPushButton::clicked, this, &ModelToolsPage::onAuthorize);
}

void ModelToolsPage::onBrowseModel() {
    auto path = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("选择模型文件"),
        QString(),
        QStringLiteral("模型文件 (*.onnx *.engine *.trt);;所有文件 (*)"));

    if (!path.isEmpty()) {
        m_modelPath->setText(path);
    }
}

void ModelToolsPage::onEncrypt() {
    if (m_modelPath->text().trimmed().isEmpty()) {
        m_encryptStatus->setText(QStringLiteral("请选择模型文件"));
        return;
    }
    m_encryptProgress->setVisible(true);
    m_encryptProgress->setValue(0);
    m_encryptStatus->setText(QStringLiteral("加密中..."));
}

void ModelToolsPage::onAuthorize() {
    if (m_authAccount->text().trimmed().isEmpty() || m_authModelId->text().trimmed().isEmpty()) {
        m_authStatus->setText(QStringLiteral("请填写账户名和模型标识符"));
        return;
    }
    m_authStatus->setText(QStringLiteral("授权中..."));
}
