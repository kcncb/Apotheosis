#include "pages/AiModelPage.h"
#include "config.h"              // kFixedMaxDetections
#include "config/ConfigManager.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"
#include "widgets/ToggleSwitch.h"
#include "detector/model_inspector.h"

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
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

#include <cmath>

namespace {

// FormKit::sliderRowD 里 slider 的值域是 round((v - min)/step), 所以还原时
// 必须用同一个映射; 而且 spin 的信号被屏蔽后 slider 不会跟着走, 得手工同步。
void setSliderValue(QDoubleSpinBox* spin, QSlider* slider,
                    double value, double min, double step) {
    const QSignalBlocker blockSpin(spin);
    spin->setValue(value);
    if (!slider)
        return;
    const QSignalBlocker blockSlider(slider);
    slider->setValue(static_cast<int>(std::lround((value - min) / step)));
}

}  // namespace

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
            updateModelInfo();
        }
    });

    // ── Card 2: 推理后端 ──
    // ★ 2026-09-17: DirectML 后端整条移除, 所以这里不再有"后端下拉框"和
    //   "DML 设备 ID" —— TensorRT 是唯一后端。卡片保留, 因为下面的状态标签
    //   仍然要显示 TensorRT 的可用性(它是有用的诊断信息)。
    auto* backendCard = new CardWidget(QStringLiteral("推理后端"),
                                       QStringLiteral("cpu"));

    auto* backendLabel = new QLabel(QStringLiteral("TensorRT (CUDA)"));
    backendLabel->setToolTip(
        tr("TensorRT(CUDA): N 卡专用,延迟最低,需要 CUDA + TensorRT 运行时。\n"
           "DirectML 后端已于 2026-09-17 整条移除, 本程序现在只有这一个后端。"));
    backendCard->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("后端"), backendLabel));

    // Backend status label (TRT availability info)
    m_backendStatusLabel = new QLabel;
    m_backendStatusLabel->setStyleSheet(QStringLiteral("color: #c0a040; font-size: 11px;"));
    m_backendStatusLabel->setWordWrap(true);
    m_backendStatusLabel->setVisible(true);
    backendCard->contentLayout()->addWidget(m_backendStatusLabel);

    layout->addWidget(backendCard);

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

    // ★ 2026-09-17: "最大检测数" 固定为 kFixedMaxDetections (=20), 不再可调。
    //   原来是 1~100 的滑块, 但那会让人误以为"调大能检出更多" —— 模型是
    //   end2end 形态, 每帧成品框本来就少, 多出来的框在下游全被丢弃。
    //   这里改成只读展示, 说明为什么不可调。
    auto* maxDetLabel = new QLabel(QString::number(kFixedMaxDetections));
    maxDetLabel->setToolTip(
        tr("单帧最多保留的检测框数量, 固定为 %1 不可修改。\n"
           "模型为 end2end 形态, 每帧输出的成品框本来就很少; 下游(预览/选靶)\n"
           "也只需要极少数目标, 调大不会检出更多, 只会增加后处理开销。")
            .arg(kFixedMaxDetections));
    detCard->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("最大检测数"), maxDetLabel));

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

    layout->addStretch();

    updateModelInfo();
    updateBackendStatus();

    // 切换配置方案后, 本页所有控件都要按新方案重读。
    connect(&cfg, &ConfigManager::configLoaded, this, &AiModelPage::reloadFromConfig);
}

void AiModelPage::reloadFromConfig() {
    auto& cfg = ConfigManager::instance();

    // 整段还原期间屏蔽控件信号: 否则 setValue/setChecked 会走回写路径, 把
    // 中间态当成用户改动塞进新方案。
    const QSignalBlocker blockCombo(m_modelCombo);
    const QSignalBlocker blockStToggle(m_smallTargetEnabled);

    const QString model = cfg.aiModel();
    int modelIdx = m_modelCombo->findText(model);
    if (modelIdx < 0 && !model.isEmpty()) {
        // 方案里的模型不在 models/ 列表里 (被删了/拷走了): 补一项让用户看见真实值,
        // 而不是让下拉悄悄留在上一个方案的文件名上。
        m_modelCombo->addItem(model);
        modelIdx = m_modelCombo->findText(model);
    }
    if (modelIdx >= 0)
        m_modelCombo->setCurrentIndex(modelIdx);

    // ★ 2026-09-17: 后端恒为 TRT(下拉框与 DML 设备 ID 已随 DirectML 后端删除),
    //   "最大检测数" 固定为 kFixedMaxDetections —— 两者都不再需要还原控件状态。
    setSliderValue(m_confSpin, m_confSlider, cfg.confidenceThreshold(), 0.01, 0.01);
    setSliderValue(m_nmsSpin, m_nmsSlider, cfg.nmsThreshold(), 0.00, 0.01);

    m_smallTargetEnabled->setChecked(cfg.smallTargetEnabled());
    setSliderValue(m_smallTargetConfSpin, m_smallTargetConfSlider,
                   cfg.smallTargetConfidence(), 0.01, 0.01);
    setSliderValue(m_smallTargetAreaSpin, m_smallTargetAreaSlider,
                   cfg.smallTargetAreaFrac(), 0.001, 0.001);
    const bool stEnabled = cfg.smallTargetEnabled();
    m_smallTargetConfRow->setEnabled(stEnabled);
    m_smallTargetAreaRow->setEnabled(stEnabled);

    updateModelInfo();
    updateBackendStatus();
}

// ★ 2026-09-17: onBackendChanged 已删除 —— DirectML 后端整条移除后没有"后端"可选,
//   TensorRT 是唯一后端, 所以不存在"切换后端"这个动作。

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
        updateModelInfo();
    }
}

void AiModelPage::updateModelInfo() {
    const QString fileName = m_modelCombo->currentText().trimmed();
    if (fileName.isEmpty()) {
        m_fixedInputLabel->setText(QStringLiteral("尚未选择模型"));
        return;
    }

    const QString path = QDir(QStringLiteral("models")).filePath(fileName);
    const QFileInfo info(path);
    if (!info.exists()) {
        m_fixedInputLabel->setText(QStringLiteral("模型文件不存在，将在启动时重新检查"));
        return;
    }

    if (info.suffix().compare(QStringLiteral("onnx"), Qt::CaseInsensitive) == 0) {
        const QByteArray utf8Path = QDir::toNativeSeparators(info.absoluteFilePath()).toUtf8();
        const auto metadata = detector::inspect_onnx_model(std::string(utf8Path.constData()), false);
        if (metadata.fixed_input_size_known) {
            m_fixedInputLabel->setText(metadata.fixed_input_size
                ? QStringLiteral("输入尺寸：固定 · 类别数：%1").arg(metadata.class_count)
                : QStringLiteral("输入尺寸：动态 · 类别数：%1").arg(metadata.class_count));
        } else {
            m_fixedInputLabel->setText(QStringLiteral("输入尺寸：启动推理时自动检测"));
        }
        return;
    }

    m_fixedInputLabel->setText(QStringLiteral("输入尺寸：启动推理时自动检测"));
}

void AiModelPage::updateBackendStatus() {
    // ★ 2026-09-17: 后端只剩 TensorRT, 所以这里不再有分支。
    //   顺带把"必须是 end2end [1,N,6] 模型"这条硬要求写在界面上 ——
    //   加载非 end2end 模型现在会直接报错退出。
    m_backendStatusLabel->setText(QStringLiteral(
        "TensorRT 引擎固定使用 FP16 I/O；ONNX 首次启动时自动构建并缓存引擎。\n"
        "只支持 end2end 形态的模型(输出 [1,N,6]，即 NMS/解码已烘进图内)。"));
}
