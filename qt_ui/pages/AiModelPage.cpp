#include "pages/AiModelPage.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"
#include "widgets/ToggleSwitch.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

AiModelPage::AiModelPage(QWidget* parent)
    : QWidget(parent) {
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

    // ── Card 1: 模型设置 ──
    auto* modelCard = new CardWidget(QStringLiteral("模型设置"),
                                     QStringLiteral("brain"));

    auto* browseWidget = new QWidget;
    auto* browseRow = new QHBoxLayout(browseWidget);
    browseRow->setContentsMargins(0, 0, 0, 0);
    browseRow->setSpacing(8);
    m_modelPath = new QLineEdit;
    m_modelPath->setPlaceholderText(QStringLiteral("选择模型文件..."));
    browseRow->addWidget(m_modelPath, 1);
    auto* browseBtn = new QPushButton(QStringLiteral("浏览..."));
    browseRow->addWidget(browseBtn);
    modelCard->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("模型文件"), browseWidget));

    m_backendCombo = new QComboBox;
    m_backendCombo->addItems({
        QStringLiteral("TensorRT"),
        QStringLiteral("DirectML"),
    });
    modelCard->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("推理后端"), m_backendCombo));

    m_dmlDeviceId = new QSpinBox;
    m_dmlDeviceId->setRange(0, 15);
    m_dmlRow = FormKit::fieldRow(QStringLiteral("DML 设备 ID"), m_dmlDeviceId);
    modelCard->contentLayout()->addWidget(m_dmlRow);
    m_dmlRow->setVisible(false);

    layout->addWidget(modelCard);

    connect(browseBtn, &QPushButton::clicked, this, &AiModelPage::browseModel);
    connect(m_backendCombo, &QComboBox::currentIndexChanged,
            this, &AiModelPage::onBackendChanged);

    // ── Card 2: 检测参数 ──
    auto* detCard = new CardWidget(QStringLiteral("检测参数"),
                                   QStringLiteral("adjustments-horizontal"));

    detCard->contentLayout()->addWidget(
        FormKit::sliderRowD(QStringLiteral("置信度"), 0.0, 1.0, 0.15, 0.01, 2,
                            m_confSlider, m_confSpin));
    detCard->contentLayout()->addWidget(
        FormKit::sliderRowD(QStringLiteral("NMS"), 0.0, 1.0, 0.50, 0.01, 2,
                            m_nmsSlider, m_nmsSpin));

    QSlider* maxDetSlider = nullptr;
    detCard->contentLayout()->addWidget(
        FormKit::sliderRow(QStringLiteral("最大检测"), 1, 100, 20,
                           maxDetSlider, m_maxDetections));

    layout->addWidget(detCard);

    // ── Card 3: 导出选项 (collapsible) ──
    auto* exportCard = new CardWidget(QStringLiteral("导出选项"),
                                      QStringLiteral("file-export"));
    exportCard->setCollapsible(true);

    exportCard->contentLayout()->addWidget(
        FormKit::toggleRow(QStringLiteral("FP16"), true, m_fp16));
    exportCard->contentLayout()->addWidget(
        FormKit::toggleRow(QStringLiteral("FP8"), false, m_fp8));
    exportCard->contentLayout()->addWidget(
        FormKit::toggleRow(QStringLiteral("固定输入尺寸"), false, m_fixedInput));

    layout->addWidget(exportCard);
    layout->addStretch();
}

void AiModelPage::onBackendChanged(int index) {
    m_dmlRow->setVisible(index == 1);
}

void AiModelPage::browseModel() {
    auto path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择模型文件"), {},
        QStringLiteral("模型文件 (*.onnx *.engine *.trt);;所有文件 (*)"));
    if (!path.isEmpty()) {
        m_modelPath->setText(path);
    }
}
