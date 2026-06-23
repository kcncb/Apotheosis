#include "pages/AiModelPage.h"
#include "config/ConfigManager.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"
#include "widgets/ToggleSwitch.h"

#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

AiModelPage::AiModelPage(QWidget* parent)
    : QWidget(parent) {
    auto& cfg = ConfigManager::instance();

    auto* outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outerLayout->addWidget(scroll);

    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(14);
    scroll->setWidget(content);

    // ── Card 1: 模型 ──
    auto* modelCard = new CardWidget(QStringLiteral("模型"),
                                     QStringLiteral("brain"));

    // Model file combo - list available models from models/ directory
    m_modelCombo = new QComboBox;
    {
        QDir modelsDir(QStringLiteral("models"));
        if (modelsDir.exists()) {
            QStringList filters;
            filters << QStringLiteral("*.onnx")
                    << QStringLiteral("*.engine")
                    << QStringLiteral("*.oliver");
            auto entries = modelsDir.entryList(filters, QDir::Files, QDir::Name);
            m_modelCombo->addItems(entries);
        }
        QString current = cfg.aiModel();
        int idx = m_modelCombo->findText(current);
        if (idx >= 0)
            m_modelCombo->setCurrentIndex(idx);
    }
    m_modelCombo->setToolTip(
        tr("models/ 目录下的模型文件列表。支持 .onnx / .engine / .oliver 格式。\n"
           "切换后需要重启推理会话才能生效。"));

    auto* modelComboRow = FormKit::fieldRow(QStringLiteral("模型文件"), m_modelCombo);
    modelCard->contentLayout()->addWidget(modelComboRow);

    // Fixed input size info label (read-only, next to model combo)
    m_fixedInputLabel = new QLabel;
    m_fixedInputLabel->setStyleSheet(QStringLiteral("color: #888; font-size: 11px;"));
    modelCard->contentLayout()->addWidget(m_fixedInputLabel);

    // Browse button row
    auto* browseWidget = new QWidget;
    auto* browseRow = new QHBoxLayout(browseWidget);
    browseRow->setContentsMargins(0, 0, 0, 0);
    browseRow->setSpacing(8);
    m_modelPath = new QLineEdit;
    m_modelPath->setPlaceholderText(QStringLiteral("选择模型文件..."));
    m_modelPath->setReadOnly(true);
    browseRow->addWidget(m_modelPath, 1);
    auto* browseBtn = new QPushButton(QStringLiteral("浏览..."));
    browseBtn->setToolTip(
        tr("从文件系统选择一个模型文件,自动拷贝到 models/ 目录下。"));
    browseRow->addWidget(browseBtn);
    auto* importLabel = new QLabel(QStringLiteral("<span style='color:#888;'>(导入到 models/)</span>"));
    browseRow->addWidget(importLabel);
    modelCard->contentLayout()->addWidget(browseWidget);

    layout->addWidget(modelCard);

    connect(browseBtn, &QPushButton::clicked, this, &AiModelPage::browseModel);
    connect(m_modelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, &cfg](int index) {
        if (index >= 0) {
            QString model = m_modelCombo->currentText();
            cfg.setAiModel(model);
            emit cfg.configChanged();
        }
    });

    // ── Card 2: 推理后端 ──
    auto* backendCard = new CardWidget(QStringLiteral("推理后端"),
                                       QStringLiteral("cpu"));

    m_backendCombo = new QComboBox;
    m_backendCombo->addItems({
        QStringLiteral("TensorRT (CUDA)"),
        QStringLiteral("DirectML (CPU/GPU)"),
    });
    m_backendCombo->setToolTip(
        tr("TensorRT(CUDA): N 卡专用,延迟最低,需要 CUDA + TensorRT 运行时。\n"
           "DirectML: 通用后端,A 卡/Intel 卡也能跑,精度一致但延迟略高。"));
    {
        QString currentBackend = cfg.backend();
        m_backendCombo->setCurrentIndex(currentBackend == QStringLiteral("DML") ? 1 : 0);
    }
    backendCard->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("后端"), m_backendCombo));

    m_dmlDeviceId = new QSpinBox;
    m_dmlDeviceId->setRange(0, 15);
    m_dmlDeviceId->setValue(cfg.dmlDeviceId());
    m_dmlRow = FormKit::fieldRow(QStringLiteral("DML 设备 ID"), m_dmlDeviceId);
    backendCard->contentLayout()->addWidget(m_dmlRow);
    m_dmlRow->setVisible(m_backendCombo->currentIndex() == 1);

    // Backend status label (TRT availability info)
    m_backendStatusLabel = new QLabel;
    m_backendStatusLabel->setStyleSheet(QStringLiteral("color: #c0a040; font-size: 11px;"));
    m_backendStatusLabel->setWordWrap(true);
    m_backendStatusLabel->setVisible(false);
    backendCard->contentLayout()->addWidget(m_backendStatusLabel);

    layout->addWidget(backendCard);

    connect(m_backendCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AiModelPage::onBackendChanged);
    connect(m_dmlDeviceId, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [&cfg](int val) {
        cfg.setDmlDeviceId(val);
        emit cfg.configChanged();
    });

    // ── Card 3: 检测参数 ──
    auto* detCard = new CardWidget(QStringLiteral("检测参数"),
                                   QStringLiteral("adjustments-horizontal"));

    auto* confRow = FormKit::sliderRowD(
        QStringLiteral("置信度阈值"), 0.01, 1.00, cfg.confidenceThreshold(), 0.01, 2,
        m_confSlider, m_confSpin);
    m_confSpin->setToolTip(
        tr("低于此置信度的检测结果会被丢弃。值越高漏检越多但误检越少;\n"
           "值越低捡回远/小目标但可能引入假框。常用 0.25~0.50。"));
    m_confSlider->setToolTip(m_confSpin->toolTip());
    detCard->contentLayout()->addWidget(confRow);

    auto* nmsRow = FormKit::sliderRowD(
        QStringLiteral("NMS 阈值"), 0.00, 1.00, cfg.nmsThreshold(), 0.01, 2,
        m_nmsSlider, m_nmsSpin);
    m_nmsSpin->setToolTip(
        tr("非极大值抑制的 IoU 阈值。两框重叠超过此比例时去掉低分框。\n"
           "值低(0.3)= 积极去重;值高(0.7)= 允许更多重叠框共存。"));
    m_nmsSlider->setToolTip(m_nmsSpin->toolTip());
    detCard->contentLayout()->addWidget(nmsRow);

    QSlider* maxDetSlider = nullptr;
    auto* maxDetRow = FormKit::sliderRow(
        QStringLiteral("最大检测数"), 1, 100, cfg.maxDetections(),
        maxDetSlider, m_maxDetections);
    m_maxDetections->setToolTip(
        tr("单帧最多保留的检测框数量。值太大浪费后处理时间,一般 20~50 够用。"));
    if (maxDetSlider)
        maxDetSlider->setToolTip(m_maxDetections->toolTip());
    detCard->contentLayout()->addWidget(maxDetRow);

    layout->addWidget(detCard);

    // Connect detection parameter changes to config
    connect(m_confSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [&cfg](double val) {
        cfg.setConfidenceThreshold(static_cast<float>(val));
        emit cfg.configChanged();
    });
    connect(m_nmsSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [&cfg](double val) {
        cfg.setNmsThreshold(static_cast<float>(val));
        emit cfg.configChanged();
    });
    connect(m_maxDetections, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [&cfg](int val) {
        cfg.setMaxDetections(val);
        emit cfg.configChanged();
    });

    // ── Card 4: 小目标增强 ──
    auto* smallCard = new CardWidget(QStringLiteral("小目标增强"),
                                     QStringLiteral("target"));

    auto* stToggleRow = FormKit::toggleRow(
        QStringLiteral("启用小目标增强"), cfg.smallTargetEnabled(), m_smallTargetEnabled);
    m_smallTargetEnabled->setToolTip(
        tr("远处/小目标用更低的置信度门槛救召回;大目标仍用上面的置信度阈值。\n"
           "小目标置信度应低于上面的置信度阈值才有效果。"));
    smallCard->contentLayout()->addWidget(stToggleRow);

    m_smallTargetConfRow = FormKit::sliderRowD(
        QStringLiteral("小目标置信度"), 0.01, 1.00,
        cfg.smallTargetConfidence(), 0.01, 2,
        m_smallTargetConfSlider, m_smallTargetConfSpin);
    smallCard->contentLayout()->addWidget(m_smallTargetConfRow);

    m_smallTargetAreaRow = FormKit::sliderRowD(
        QStringLiteral("小目标面积比例"), 0.001, 0.100,
        cfg.smallTargetAreaFrac(), 0.001, 3,
        m_smallTargetAreaSlider, m_smallTargetAreaSpin);
    m_smallTargetAreaSpin->setToolTip(
        tr("框面积占检测画面的比例,低于此值视为小目标。0.012 约等于边长约 11% 的框。"));
    if (m_smallTargetAreaSlider)
        m_smallTargetAreaSlider->setToolTip(m_smallTargetAreaSpin->toolTip());
    smallCard->contentLayout()->addWidget(m_smallTargetAreaRow);

    // Disable small target sliders when toggle is off
    bool stEnabled = cfg.smallTargetEnabled();
    m_smallTargetConfRow->setEnabled(stEnabled);
    m_smallTargetAreaRow->setEnabled(stEnabled);

    layout->addWidget(smallCard);

    connect(m_smallTargetEnabled, &ToggleSwitch::toggled,
            this, &AiModelPage::onSmallTargetToggled);
    connect(m_smallTargetConfSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [&cfg](double val) {
        cfg.setSmallTargetConfidence(static_cast<float>(val));
        emit cfg.configChanged();
    });
    connect(m_smallTargetAreaSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [&cfg](double val) {
        cfg.setSmallTargetAreaFrac(static_cast<float>(val));
        emit cfg.configChanged();
    });

    // ── Card 5: 导出选项 (collapsible) ──
    auto* exportCard = new CardWidget(QStringLiteral("导出选项"),
                                      QStringLiteral("file-export"));
    exportCard->setCollapsible(true);

    auto* fp16Row = FormKit::toggleRow(
        QStringLiteral("FP16"), cfg.exportEnableFp16(), m_fp16);
    m_fp16->setToolTip(
        tr("导出引擎时启用 FP16 半精度。推理速度更快,精度损失极小。"));
    exportCard->contentLayout()->addWidget(fp16Row);

    auto* fp8Row = FormKit::toggleRow(
        QStringLiteral("FP8"), cfg.exportEnableFp8(), m_fp8);
    m_fp8->setToolTip(
        tr("导出引擎时启用 FP8 量化。速度最快但精度可能下降,仅 Ada+ 架构支持。"));
    exportCard->contentLayout()->addWidget(fp8Row);

    layout->addWidget(exportCard);

    connect(m_fp16, &ToggleSwitch::toggled, this, [&cfg](bool val) {
        cfg.setExportEnableFp16(val);
        emit cfg.configChanged();
    });
    connect(m_fp8, &ToggleSwitch::toggled, this, [&cfg](bool val) {
        cfg.setExportEnableFp8(val);
        emit cfg.configChanged();
    });

    layout->addStretch();
}

void AiModelPage::onBackendChanged(int index) {
    auto& cfg = ConfigManager::instance();
    m_dmlRow->setVisible(index == 1);
    QString backend = (index == 1) ? QStringLiteral("DML") : QStringLiteral("TRT");
    cfg.setBackend(backend);
    emit cfg.configChanged();
}

void AiModelPage::onSmallTargetToggled(bool enabled) {
    auto& cfg = ConfigManager::instance();
    m_smallTargetConfRow->setEnabled(enabled);
    m_smallTargetAreaRow->setEnabled(enabled);
    cfg.setSmallTargetEnabled(enabled);
    emit cfg.configChanged();
}

void AiModelPage::browseModel() {
    auto path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择模型文件"), QStringLiteral("models"),
        QStringLiteral("模型文件 (*.onnx *.engine *.oliver);;所有文件 (*)"));
    if (!path.isEmpty()) {
        // Import: copy to models/ directory if not already there
        QFileInfo fi(path);
        QDir modelsDir(QStringLiteral("models"));
        if (!modelsDir.exists())
            modelsDir.mkpath(QStringLiteral("."));

        QString destName = fi.fileName();
        QString destPath = modelsDir.filePath(destName);

        if (QFileInfo(destPath).absoluteFilePath() != fi.absoluteFilePath()) {
            QFile::copy(path, destPath);
        }

        // Add to combo if not already present
        int idx = m_modelCombo->findText(destName);
        if (idx < 0) {
            m_modelCombo->addItem(destName);
            idx = m_modelCombo->findText(destName);
        }
        m_modelCombo->setCurrentIndex(idx);
        m_modelPath->setText(path);

        auto& cfg = ConfigManager::instance();
        cfg.setAiModel(destName);
        emit cfg.configChanged();
    }
}
