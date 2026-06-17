#include "pages/DepthPage.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"
#include "widgets/ToggleSwitch.h"

#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

DepthPage::DepthPage(QWidget* parent)
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

    // ── Card 1: 深度推理 ──
    auto* inferCard = new CardWidget(QStringLiteral("深度推理"),
                                     QStringLiteral("stack-2"));

    inferCard->contentLayout()->addWidget(
        FormKit::toggleRow(QStringLiteral("启用深度推理"), true, m_depthEnabled));

    auto* browseWidget = new QWidget;
    auto* browseRow = new QHBoxLayout(browseWidget);
    browseRow->setContentsMargins(0, 0, 0, 0);
    browseRow->setSpacing(8);
    m_modelPath = new QLineEdit;
    m_modelPath->setPlaceholderText(QStringLiteral("选择模型文件..."));
    browseRow->addWidget(m_modelPath, 1);
    auto* browseBtn = new QPushButton(QStringLiteral("浏览..."));
    browseRow->addWidget(browseBtn);
    inferCard->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("模型文件"), browseWidget));

    QSlider* depthFpsSlider = nullptr;
    inferCard->contentLayout()->addWidget(
        FormKit::sliderRow(QStringLiteral("深度帧率"), 1, 240, 100,
                           depthFpsSlider, m_depthFps));

    layout->addWidget(inferCard);

    connect(browseBtn, &QPushButton::clicked, this, &DepthPage::browseModel);

    // ── Card 2: 深度遮罩 ──
    auto* maskCard = new CardWidget(QStringLiteral("深度遮罩"),
                                    QStringLiteral("layers-intersect"));

    maskCard->contentLayout()->addWidget(
        FormKit::toggleRow(QStringLiteral("启用遮罩"), false, m_maskEnabled));

    QSlider* maskFpsSlider = nullptr;
    maskCard->contentLayout()->addWidget(
        FormKit::sliderRow(QStringLiteral("遮罩帧率"), 1, 60, 5,
                           maskFpsSlider, m_maskFps));

    maskCard->contentLayout()->addWidget(
        FormKit::sliderRow(QStringLiteral("近景百分比"), 0, 100, 20,
                           m_nearSlider, m_nearSpin, QStringLiteral(" %")));
    maskCard->contentLayout()->addWidget(
        FormKit::sliderRow(QStringLiteral("扩展像素"), 0, 50, 0,
                           m_expandSlider, m_expandSpin));

    QSlider* holdSlider = nullptr;
    maskCard->contentLayout()->addWidget(
        FormKit::sliderRow(QStringLiteral("保持帧数"), 0, 60, 0,
                           holdSlider, m_holdFrames));

    maskCard->contentLayout()->addWidget(
        FormKit::sliderRow(QStringLiteral("遮罩透明度"), 0, 255, 90,
                           m_alphaSlider, m_alphaSpin));

    maskCard->contentLayout()->addWidget(
        FormKit::toggleRow(QStringLiteral("反转遮罩"), false, m_invertMask));

    maskCard->contentLayout()->addWidget(
        FormKit::sliderRowD(QStringLiteral("抑制比例"), 0.0, 1.0, 0.30, 0.01, 2,
                            m_suppressSlider, m_suppressSpin));

    layout->addWidget(maskCard);
    layout->addStretch();
}

void DepthPage::browseModel() {
    auto path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择深度模型文件"), {},
        QStringLiteral("模型文件 (*.onnx *.engine *.trt);;所有文件 (*)"));
    if (!path.isEmpty()) {
        m_modelPath->setText(path);
    }
}
