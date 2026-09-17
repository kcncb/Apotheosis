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
    // ★ 2026-09-17: DirectML 后端整条移除 → 后端下拉框与 "DirectML 显卡" 一起删除。
    //   TensorRT 是唯一后端, 所以这里只做只读展示 + 状态行。
    auto* backendCard = new CardWidget(tr("推理后端"), QStringLiteral("cpu"), container);
    auto* bc = backendCard->contentLayout();

    auto* backendLabel = new QLabel(QStringLiteral("TensorRT (CUDA)"));
    backendLabel->setToolTip(tr(
        "TRT(CUDA): N 卡专用,延迟最低,需要 CUDA 与 TensorRT 运行时。\n"
        "DirectML 后端已于 2026-09-17 整条移除, 本程序现在只有这一个后端。"));
    bc->addWidget(FormKit::fieldRow(tr("推理后端"), backendLabel));

    // Current backend status line
    m_backendStatusLabel = new QLabel();
    m_backendStatusLabel->setProperty("class", "secondary");
    m_backendStatusLabel->setText(tr("当前选择：TensorRT (CUDA)"));
    bc->addWidget(m_backendStatusLabel);

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
        // ★ 2026-09-17: "双缓冲流水线" 开关已删除 —— 双缓冲整条移除
        //   (它白加一整帧延迟, 与"降推理延迟"的目标相反)。
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

    auto* restartHint = new QLabel(tr("GPU/CPU/系统资源预留与 GPU 独占模式在下次启动应用时生效。"));
    restartHint->setProperty("class", "secondary");
    restartHint->setWordWrap(true);
    gc->addWidget(restartHint);

    connect(m_cudaGraph, &ToggleSwitch::toggled, this, [this](bool v) {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        config.use_cuda_graph = v;
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
    // ★ 2026-09-17: 后端下拉框/DML 设备的连接已删除 (DirectML 后端整条移除)。
    connect(m_showWindow, &ToggleSwitch::toggled,
            this, &SessionPage::onShowWindowChanged);
    connect(&cfg, &ConfigManager::configLoaded,
            this, &SessionPage::loadConfig);
}

void SessionPage::onShowWindowChanged(bool checked) {
    ConfigManager::instance().setShowWindow(checked);
}

void SessionPage::loadConfig() {
    auto& cfg = ConfigManager::instance();

    // ★ 2026-09-17: 后端恒为 TensorRT, 状态行是固定文案, 不需要还原控件状态。
    m_backendStatusLabel->setText(tr("当前选择：TensorRT (CUDA)"));

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
