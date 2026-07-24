#include "pages/ModelToolsPage.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"
#include "widgets/LoginDialog.h"
#include "auth/auth_state.h"
#include "model_crypto/model_crypto.h"

#include <memory>
#include <string>

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QThread>
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

    m_authBtn = new QPushButton(QStringLiteral("授权"));
    m_authBtn->setProperty("class", "primary");
    m_authBtn->setFixedWidth(120);
    m_authBtn->setCursor(Qt::PointingHandCursor);
    {
        auto* row = new QHBoxLayout;
        row->setContentsMargins(0, 4, 0, 0);
        row->addWidget(m_authBtn);
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
    connect(m_authBtn, &QPushButton::clicked, this, &ModelToolsPage::onAuthorize);
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
    const QString inputPath = m_modelPath->text().trimmed();
    if (inputPath.isEmpty()) {
        m_encryptStatus->setText(QStringLiteral("请选择模型文件"));
        return;
    }
    if (!ensureAuthorized())
        return;

    const QFileInfo inputInfo(inputPath);
    const QString defaultOutput = inputInfo.dir().filePath(inputInfo.completeBaseName() + QStringLiteral(".oliver"));
    const QString outputPath = QFileDialog::getSaveFileName(
        this, QStringLiteral("保存加密模型"), defaultOutput,
        QStringLiteral("Oliver 加密模型 (*.oliver)"));
    if (outputPath.isEmpty())
        return;

    const QByteArray inputUtf8 = QDir::toNativeSeparators(inputPath).toUtf8();
    const QByteArray outputUtf8 = QDir::toNativeSeparators(outputPath).toUtf8();
    const auto payloadType = oliver::payload_type_from_extension(inputUtf8.constData());
    if (payloadType == oliver::PayloadType::Unknown) {
        m_encryptStatus->setText(QStringLiteral("仅支持 ONNX 或 TensorRT Engine 模型"));
        return;
    }

    struct EncryptResult {
        bool ok = false;
        std::string error;
        std::string modelId;
    };
    auto result = std::make_shared<EncryptResult>();
    const QByteArray modelNameUtf8 = inputInfo.completeBaseName().toUtf8();
    m_encryptProgress->setVisible(true);
    m_encryptProgress->setRange(0, 0);
    m_encryptStatus->setText(QStringLiteral("加密中..."));
    m_encryptBtn->setEnabled(false);

    auto* worker = QThread::create([result, inputUtf8, outputUtf8, modelNameUtf8, payloadType] {
        std::vector<uint8_t> key;
        if (!auth::state().create_model_key(modelNameUtf8.constData(), result->modelId, key)) {
            result->error = auth::state().last_error();
            if (result->error.empty()) result->error = u8"创建模型密钥失败";
            return;
        }
        oliver::set_runtime_key(key);
        result->ok = oliver::encrypt_file(inputUtf8.constData(), outputUtf8.constData(),
                                          payloadType, result->modelId, result->error);
    });
    connect(worker, &QThread::finished, this, [this, result, outputPath] {
        m_encryptBtn->setEnabled(true);
        m_encryptProgress->setRange(0, 100);
        m_encryptProgress->setValue(result->ok ? 100 : 0);
        if (result->ok) {
            m_encryptStatus->setText(QStringLiteral("加密完成：%1").arg(QDir::toNativeSeparators(outputPath)));
            m_authModelId->setText(QString::fromStdString(result->modelId));
        } else {
            m_encryptStatus->setText(result->error.empty()
                ? QStringLiteral("加密失败")
                : QString::fromUtf8(result->error.c_str()));
        }
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void ModelToolsPage::onAuthorize() {
    if (m_authAccount->text().trimmed().isEmpty() || m_authModelId->text().trimmed().isEmpty()) {
        m_authStatus->setText(QStringLiteral("请填写账户名和模型标识符"));
        return;
    }
    if (!ensureAuthorized())
        return;

    const QByteArray account = m_authAccount->text().trimmed().toUtf8();
    const QByteArray modelId = m_authModelId->text().trimmed().toUtf8();
    struct GrantResult { bool ok = false; std::string error; };
    auto result = std::make_shared<GrantResult>();
    m_authStatus->setText(QStringLiteral("授权中..."));
    m_authBtn->setEnabled(false);

    auto* worker = QThread::create([result, account, modelId] {
        result->ok = auth::state().grant_model(modelId.constData(), account.constData());
        if (!result->ok)
            result->error = auth::state().last_error();
    });
    connect(worker, &QThread::finished, this, [this, result] {
        m_authBtn->setEnabled(true);
        m_authStatus->setText(result->ok
            ? QStringLiteral("授权完成")
            : (result->error.empty() ? QStringLiteral("授权失败")
                                     : QString::fromUtf8(result->error.c_str())));
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

bool ModelToolsPage::ensureAuthorized() {
    if (auth::state().is_authorized())
        return true;
    if (auth::state().try_restore_session())
        return true;
    LoginDialog dialog(this);
    if (dialog.exec() == QDialog::Accepted)
        return true;
    m_encryptStatus->setText(QStringLiteral("需要先登录才能创建或授权模型密钥"));
    m_authStatus->setText(QStringLiteral("需要先登录才能创建或授权模型密钥"));
    return false;
}
