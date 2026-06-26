#include "pages/DepthPage.h"
#include "config/ConfigManager.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"
#include "widgets/ToggleSwitch.h"

#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
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

    auto& cfg = ConfigManager::instance();

    // ── Page intro ──
    auto* intro = new QLabel(QStringLiteral(
        "深度推理现在只用于「寻光」的深度门(判断光斑远近、剔除天空/太阳)。\n"
        "这里只保留让深度推理跑起来所需的几个开关。"));
    intro->setWordWrap(true);
    intro->setObjectName(QStringLiteral("pageIntro"));
    layout->addWidget(intro);

    // ====================================================================
    // Card 1: 深度推理
    // ====================================================================
    auto* inferCard = new CardWidget(QStringLiteral("深度推理"),
                                     QStringLiteral("stack-2"));

    // ── 启用深度推理 ──
    inferCard->contentLayout()->addWidget(
        FormKit::toggleRow(QStringLiteral("启用深度推理"), true, m_depthEnabled));
    m_depthEnabled->setToolTip(tr(
        "开启后会运行 Depth-Anything 模型估计画面深度图,供「寻光」的深度门使用,\n"
        "用来判断光斑远近、剔除天空/太阳等远景误检。\n"
        "代价:占额外 GPU 资源,关闭后深度功能(含寻光深度门)都不生效。"));
    connect(m_depthEnabled, &ToggleSwitch::toggled,
            this, [](bool v) { ConfigManager::instance().setDepthInferenceEnabled(v); });

    // ── 模型文件 ──
    auto* browseWidget = new QWidget;
    auto* browseRow = new QHBoxLayout(browseWidget);
    browseRow->setContentsMargins(0, 0, 0, 0);
    browseRow->setSpacing(8);
    m_modelPath = new QLineEdit;
    m_modelPath->setPlaceholderText(QStringLiteral("选择模型文件..."));
    m_modelPath->setToolTip(tr(
        "models/depth/ 下的深度模型文件。.engine 已编译(直接用),.onnx 需要先点'导出深度引擎'。"));
    browseRow->addWidget(m_modelPath, 1);
    auto* browseBtn = new QPushButton(QStringLiteral("浏览..."));
    browseRow->addWidget(browseBtn);
    inferCard->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("模型文件"), browseWidget));

    connect(browseBtn, &QPushButton::clicked, this, &DepthPage::browseModel);
    connect(m_modelPath, &QLineEdit::editingFinished,
            this, [this]() {
                ConfigManager::instance().setDepthModelPath(m_modelPath->text().trimmed());
            });

    // ── 深度引擎 OPT 尺寸 ──
    QSlider* optSizeSlider = nullptr;
    inferCard->contentLayout()->addWidget(
        FormKit::sliderRow(QStringLiteral("引擎 OPT 尺寸"), 160, 640, 518,
                           optSizeSlider, m_optInputSize));
    optSizeSlider->setToolTip(tr(
        "TensorRT 构建深度 .engine 时使用的最优输入边长(方形),用作 kernel autotune 的中心尺寸。\n"
        "例如采集是 640x640,这里设 640 可让深度推理在 640 档跑得最快;\n"
        "小尺寸(224)更省 GPU,大尺寸(640)精度更高。\n"
        "改这个值后必须删掉 models/depth/ 下的旧 .engine 重新点'导出深度引擎'才生效。"));
    m_optInputSize->setToolTip(optSizeSlider->toolTip());
    connect(m_optInputSize, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { ConfigManager::instance().setDepthOptInputSize(v); });

    // ── 深度推理频率 (fps) ──
    QSlider* maskFpsSlider = nullptr;
    inferCard->contentLayout()->addWidget(
        FormKit::sliderRow(QStringLiteral("深度推理频率 (fps)"), 1, 240, 5,
                           maskFpsSlider, m_maskFpsRuntime));
    maskFpsSlider->setToolTip(tr(
        "深度推理每秒运行次数(限流)。深度图变化慢,一般 5~10 即可;\n"
        "供「寻光」深度门使用,频率高只是更跟手,过高会和检测争 GPU。"));
    m_maskFpsRuntime->setToolTip(maskFpsSlider->toolTip());
    connect(m_maskFpsRuntime, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { ConfigManager::instance().setDepthMaskFps(v); });

    layout->addWidget(inferCard);

    layout->addStretch();

    // ── Load initial config and connect reload signal ──
    connect(&cfg, &ConfigManager::configLoaded, this, &DepthPage::onLoadConfig);
    onLoadConfig();
}

void DepthPage::onLoadConfig() {
    auto& cfg = ConfigManager::instance();

    // Block signals during bulk load to avoid writing back to config
    const auto block = [](QObject* w, bool b) { if (w) w->blockSignals(b); };

    block(m_depthEnabled, true);
    block(m_modelPath, true);
    block(m_optInputSize, true);
    block(m_maskFpsRuntime, true);

    m_depthEnabled->setChecked(cfg.depthInferenceEnabled());
    m_modelPath->setText(cfg.depthModelPath());
    m_optInputSize->setValue(cfg.depthOptInputSize());
    m_maskFpsRuntime->setValue(cfg.depthMaskFps());

    block(m_depthEnabled, false);
    block(m_modelPath, false);
    block(m_optInputSize, false);
    block(m_maskFpsRuntime, false);
}

void DepthPage::browseModel() {
    auto path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择深度模型文件"), {},
        QStringLiteral("模型文件 (*.onnx *.engine *.trt);;所有文件 (*)"));
    if (!path.isEmpty()) {
        m_modelPath->setText(path);
        ConfigManager::instance().setDepthModelPath(path);
    }
}
