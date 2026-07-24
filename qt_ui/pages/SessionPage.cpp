#include "pages/SessionPage.h"
#include "config/ConfigManager.h"
#include "config/config_bridge.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"
#include "widgets/ToggleSwitch.h"

#include "Apotheosis.h"
#include "config.h"

#include <mutex>

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
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

    auto& cfg = ConfigManager::instance();

    // ── 推理后端 (Backend) ──
    auto* backendCard = new CardWidget(tr("推理后端"), QStringLiteral("cpu"), container);
    auto* bc = backendCard->contentLayout();

    m_backendCombo = new QComboBox();
    m_backendCombo->addItem(QStringLiteral("TensorRT (CUDA)"), QStringLiteral("TRT"));
    m_backendCombo->addItem(QStringLiteral("DirectML (CPU/GPU)"), QStringLiteral("DML"));
    auto* backendRow = FormKit::fieldRow(tr("推理后端"), m_backendCombo);
    bc->addWidget(backendRow);
    m_backendCombo->setToolTip(tr(
        "TRT(CUDA): N 卡专用,延迟最低,需要 CUDA 与 TensorRT 运行时。\n"
        "DML(DirectML): 通用后端,A 卡/Intel 卡也能跑。停止推理后才能切换。"));

    // DML device ID
    m_dmlDeviceId = new QSpinBox();
    m_dmlDeviceId->setRange(0, 15);
    m_dmlDeviceId->setValue(cfg.dmlDeviceId());
    m_dmlDeviceRow = FormKit::fieldRow(tr("DirectML 显卡"), m_dmlDeviceId);
    bc->addWidget(m_dmlDeviceRow);
    m_dmlDeviceId->setToolTip(tr(
        "DirectML 后端跑在哪个显卡上。多显卡机器请选独显;只在 DML 后端时生效。"));

    // Current backend status line
    m_backendStatusLabel = new QLabel();
    m_backendStatusLabel->setProperty("class", "secondary");
    bc->addWidget(m_backendStatusLabel);

    // Set initial backend selection and visibility
    {
        QString be = cfg.backend();
        int idx = m_backendCombo->findData(be);
        if (idx >= 0)
            m_backendCombo->setCurrentIndex(idx);
        bool isDml = (be == QStringLiteral("DML"));
        m_dmlDeviceRow->setVisible(isDml);
        m_backendStatusLabel->setText(
            isDml ? tr("当前选择：DirectML (CPU/GPU)")
                  : tr("当前选择：TensorRT (CUDA)"));
    }

    layout->addWidget(backendCard);

    // ── 检测预览 (Preview) ──
    auto* previewCard = new CardWidget(tr("检测预览"), QStringLiteral("eye"), container);
    auto* pc = previewCard->contentLayout();

    auto* previewToggleRow = FormKit::toggleRow(tr("启用独立检测预览窗口"), cfg.showWindow(), m_showWindow);
    pc->addWidget(previewToggleRow);
    m_showWindow->setToolTip(tr(
        "开启后弹出一个独立窗口,实时叠画检测框/锁定目标/瞄准 FOV,\n"
        "用来验证模型识别和瞄准逻辑;不影响游戏画面与瞄准。"));

    auto* previewHint = new QLabel(tr("勾选后会在控制台外浮出一个可拖动 / 缩放的预览窗口。"));
    previewHint->setProperty("class", "secondary");
    previewHint->setWordWrap(true);
    pc->addWidget(previewHint);

    layout->addWidget(previewCard);

    // ── CUDA 设置 (collapsible) ──
    auto* cudaCard = new CardWidget(tr("CUDA 设置"), QStringLiteral("settings"), container);
    cudaCard->setCollapsible(true);
    auto* gc = cudaCard->contentLayout();

    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        gc->addWidget(FormKit::toggleRow(tr("CUDA Graph"), config.use_cuda_graph, m_cudaGraph));
        gc->addWidget(FormKit::toggleRow(tr("双缓冲流水线"), config.use_double_buffer, m_dualBuffer));
        gc->addWidget(FormKit::toggleRow(tr("GPU 独占模式"), config.enableGpuExclusiveMode, m_gpuExclusive));

        QSlider* gpuSlider = nullptr;
        gc->addWidget(FormKit::sliderRow(tr("GPU 显存"), 256, 8192, config.gpuMemoryReserveMB,
                                         gpuSlider, m_gpuReserve, QStringLiteral(" MB")));
        m_gpuReserve->setSingleStep(256);

        QSlider* cpuSlider = nullptr;
        gc->addWidget(FormKit::sliderRow(tr("CPU 核心"), 1, 32, config.cpuCoreReserveCount,
                                         cpuSlider, m_cpuReserve));

        QSlider* memorySlider = nullptr;
        gc->addWidget(FormKit::sliderRow(tr("系统内存"), 0, 32768, config.systemMemoryReserveMB,
                                         memorySlider, m_systemMemoryReserve, QStringLiteral(" MB")));
        m_systemMemoryReserve->setSingleStep(256);
    }

    auto* restartHint = new QLabel(tr("GPU/CPU/系统资源预留与 GPU 独占模式在下次启动应用时生效；双缓冲在重新启动推理后生效。"));
    restartHint->setProperty("class", "secondary");
    restartHint->setWordWrap(true);
    gc->addWidget(restartHint);

    connect(m_cudaGraph, &ToggleSwitch::toggled, this, [this](bool v) {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        config.use_cuda_graph = v;
        ConfigBridge::instance().markDirty();
    });
    connect(m_dualBuffer, &ToggleSwitch::toggled, this, [this](bool v) {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        config.use_double_buffer = v;
        ConfigBridge::instance().markDirty();
    });
    connect(m_gpuExclusive, &ToggleSwitch::toggled, this, [this](bool v) {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        config.enableGpuExclusiveMode = v;
        ConfigBridge::instance().markDirty();
    });
    connect(m_gpuReserve, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        config.gpuMemoryReserveMB = v;
        ConfigBridge::instance().markDirty();
    });
    connect(m_cpuReserve, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        config.cpuCoreReserveCount = v;
        ConfigBridge::instance().markDirty();
    });
    connect(m_systemMemoryReserve, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        config.systemMemoryReserveMB = v;
        ConfigBridge::instance().markDirty();
    });

    layout->addWidget(cudaCard);

    layout->addStretch();

    scroll->setWidget(container);
    root->addWidget(scroll);

    // ── Connections ──
    connect(m_backendCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SessionPage::onBackendChanged);
    connect(m_dmlDeviceId, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &SessionPage::onDmlDeviceChanged);
    connect(m_showWindow, &ToggleSwitch::toggled,
            this, &SessionPage::onShowWindowChanged);
    connect(&cfg, &ConfigManager::configLoaded,
            this, &SessionPage::loadConfig);
}

void SessionPage::onBackendChanged(int index) {
    auto& cfg = ConfigManager::instance();
    QString val = m_backendCombo->itemData(index).toString();
    bool isDml = (val == QStringLiteral("DML"));
    m_dmlDeviceRow->setVisible(isDml);
    m_backendStatusLabel->setText(
        isDml ? tr("当前选择：DirectML (CPU/GPU)")
              : tr("当前选择：TensorRT (CUDA)"));
    cfg.setBackend(val);
}

void SessionPage::onDmlDeviceChanged(int value) {
    ConfigManager::instance().setDmlDeviceId(value);
}


void SessionPage::onShowWindowChanged(bool checked) {
    ConfigManager::instance().setShowWindow(checked);
}

void SessionPage::loadConfig() {
    auto& cfg = ConfigManager::instance();

    // Backend
    m_backendCombo->blockSignals(true);
    int idx = m_backendCombo->findData(cfg.backend());
    if (idx >= 0)
        m_backendCombo->setCurrentIndex(idx);
    bool isDml = (cfg.backend() == QStringLiteral("DML"));
    m_dmlDeviceRow->setVisible(isDml);
    m_backendStatusLabel->setText(
        isDml ? tr("当前选择：DirectML (CPU/GPU)")
              : tr("当前选择：TensorRT (CUDA)"));
    m_backendCombo->blockSignals(false);

    // DML device ID
    m_dmlDeviceId->blockSignals(true);
    m_dmlDeviceId->setValue(cfg.dmlDeviceId());
    m_dmlDeviceId->blockSignals(false);

    // Preview window
    m_showWindow->blockSignals(true);
    m_showWindow->setChecked(cfg.showWindow());
    m_showWindow->blockSignals(false);

    // CUDA settings
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        m_cudaGraph->blockSignals(true);
        m_cudaGraph->setChecked(config.use_cuda_graph);
        m_cudaGraph->blockSignals(false);
        m_dualBuffer->blockSignals(true);
        m_dualBuffer->setChecked(config.use_double_buffer);
        m_dualBuffer->blockSignals(false);
        m_gpuExclusive->blockSignals(true);
        m_gpuExclusive->setChecked(config.enableGpuExclusiveMode);
        m_gpuExclusive->blockSignals(false);
        m_gpuReserve->blockSignals(true);
        m_gpuReserve->setValue(config.gpuMemoryReserveMB);
        m_gpuReserve->blockSignals(false);
        m_cpuReserve->blockSignals(true);
        m_cpuReserve->setValue(config.cpuCoreReserveCount);
        m_cpuReserve->blockSignals(false);
        m_systemMemoryReserve->blockSignals(true);
        m_systemMemoryReserve->setValue(config.systemMemoryReserveMB);
        m_systemMemoryReserve->blockSignals(false);
    }

}
