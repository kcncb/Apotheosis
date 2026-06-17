#include "pages/SessionPage.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"
#include "widgets/ToggleSwitch.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QStyle>
#include <QVBoxLayout>

SessionPage::SessionPage(QWidget* parent)
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

    // -- Card 1: 推理后端 --
    auto* backendCard = new CardWidget(QStringLiteral("推理后端"), QStringLiteral("cpu"), container);
    auto* bc = backendCard->contentLayout();

    m_backendCombo = new QComboBox();
    m_backendCombo->addItem(QStringLiteral("TensorRT (TRT)"), QStringLiteral("trt"));
    m_backendCombo->addItem(QStringLiteral("DirectML (DML)"), QStringLiteral("dml"));
    bc->addWidget(FormKit::fieldRow(QStringLiteral("后端"), m_backendCombo));

    m_dmlDeviceId = new QSpinBox();
    m_dmlDeviceId->setRange(0, 15);
    m_dmlDeviceId->setValue(0);
    m_dmlDeviceRow = FormKit::fieldRow(QStringLiteral("DML 设备"), m_dmlDeviceId);
    bc->addWidget(m_dmlDeviceRow);

    // Keep the label handle pointing at the row's label for compatibility.
    m_dmlDeviceLabel = m_dmlDeviceRow->findChild<QLabel*>();
    m_dmlDeviceRow->setVisible(false);

    m_modelFileLabel = new QLabel(QStringLiteral("(未选择)"));
    m_modelFileLabel->setProperty("class", "secondary");
    bc->addWidget(FormKit::fieldRow(QStringLiteral("模型"), m_modelFileLabel));

    layout->addWidget(backendCard);

    // -- Card 2: 推理控制 --
    auto* controlCard = new CardWidget(QStringLiteral("推理控制"), QStringLiteral("player-play"), container);
    auto* cc = controlCard->contentLayout();

    m_toggleBtn = new QPushButton(QStringLiteral("启动推理"));
    m_toggleBtn->setProperty("class", "primary");
    m_toggleBtn->setMinimumHeight(40);
    m_toggleBtn->setFixedWidth(160);
    m_toggleBtn->setCursor(Qt::PointingHandCursor);

    auto* btnRow = new QHBoxLayout;
    btnRow->setContentsMargins(0, 0, 0, 0);
    btnRow->setSpacing(12);
    btnRow->addWidget(m_toggleBtn);
    m_statusLabel = new QLabel(QStringLiteral("已停止"));
    m_statusLabel->setProperty("class", "secondary");
    btnRow->addWidget(m_statusLabel);
    btnRow->addStretch();
    cc->addLayout(btnRow);

    layout->addWidget(controlCard);

    // -- Card 3: CUDA 设置 (collapsible) --
    auto* cudaCard = new CardWidget(QStringLiteral("CUDA 设置"), QStringLiteral("settings"), container);
    cudaCard->setCollapsible(true);
    auto* gc = cudaCard->contentLayout();

    gc->addWidget(FormKit::toggleRow(QStringLiteral("CUDA Graph"), false, m_cudaGraph));
    gc->addWidget(FormKit::toggleRow(QStringLiteral("双缓冲流水线"), false, m_dualBuffer));
    gc->addWidget(FormKit::toggleRow(QStringLiteral("固定内存"), false, m_pinnedMemory));

    QSlider* gpuSlider = nullptr;
    gc->addWidget(FormKit::sliderRow(QStringLiteral("GPU 显存"), 256, 8192, 2048,
                                     gpuSlider, m_gpuReserve, QStringLiteral(" MB")));
    m_gpuReserve->setSingleStep(256);

    QSlider* cpuSlider = nullptr;
    gc->addWidget(FormKit::sliderRow(QStringLiteral("CPU 核心"), 1, 32, 4,
                                     cpuSlider, m_cpuReserve));

    layout->addWidget(cudaCard);

    layout->addStretch();

    scroll->setWidget(container);
    root->addWidget(scroll);

    connect(m_backendCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SessionPage::onBackendChanged);
    connect(m_toggleBtn, &QPushButton::clicked,
            this, &SessionPage::onToggleInference);
}

void SessionPage::onBackendChanged(int index) {
    bool isDml = (m_backendCombo->itemData(index).toString() == "dml");
    m_dmlDeviceRow->setVisible(isDml);
}

void SessionPage::onToggleInference() {
    m_running = !m_running;

    if (m_running) {
        m_toggleBtn->setText(QStringLiteral("停止推理"));
        m_toggleBtn->setProperty("class", "danger");
        m_statusLabel->setText(QStringLiteral("运行中"));
    } else {
        m_toggleBtn->setText(QStringLiteral("启动推理"));
        m_toggleBtn->setProperty("class", "primary");
        m_statusLabel->setText(QStringLiteral("已停止"));
    }

    m_toggleBtn->style()->unpolish(m_toggleBtn);
    m_toggleBtn->style()->polish(m_toggleBtn);
}
