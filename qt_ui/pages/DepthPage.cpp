#include "pages/DepthPage.h"
#include "config/ConfigManager.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"
#include "widgets/ToggleSwitch.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

#include <cmath>

// Same 22 colormaps as ImGui draw_depth.cpp kDepthColormapNames[]
static const QStringList kColormapNames = {
    QStringLiteral("Autumn"),
    QStringLiteral("Bone"),
    QStringLiteral("Jet"),
    QStringLiteral("Winter"),
    QStringLiteral("Rainbow"),
    QStringLiteral("Ocean"),
    QStringLiteral("Summer"),
    QStringLiteral("Spring"),
    QStringLiteral("Cool"),
    QStringLiteral("HSV"),
    QStringLiteral("Pink"),
    QStringLiteral("Hot"),
    QStringLiteral("Parula"),
    QStringLiteral("Magma"),
    QStringLiteral("Inferno"),
    QStringLiteral("Plasma"),
    QStringLiteral("Viridis"),
    QStringLiteral("Cividis"),
    QStringLiteral("Twilight"),
    QStringLiteral("Twilight Shifted"),
    QStringLiteral("Turbo"),
    QStringLiteral("Deepgreen")
};

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

    // ====================================================================
    // Card 1: 深度推理
    // ====================================================================
    auto* inferCard = new CardWidget(QStringLiteral("深度推理"),
                                     QStringLiteral("stack-2"));

    // ── 启用深度推理 ──
    inferCard->contentLayout()->addWidget(
        FormKit::toggleRow(QStringLiteral("启用深度推理"), true, m_depthEnabled));
    m_depthEnabled->setToolTip(tr(
        "开启后会运行 Depth-Anything 模型估计画面深度图,主要给'深度遮罩'用,\n"
        "目的:把贴在墙后/远景里的检测框丢弃,减少穿墙误瞄。\n"
        "代价:占额外 GPU 资源,关闭后所有深度功能都不生效。"));
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

    // ── 深度帧率 ──
    QSlider* depthFpsSlider = nullptr;
    inferCard->contentLayout()->addWidget(
        FormKit::sliderRow(QStringLiteral("深度 FPS"), 0, 240, 100,
                           depthFpsSlider, m_depthFps));
    depthFpsSlider->setToolTip(tr(
        "深度模型每秒推理次数。深度图变化慢,5~30 通常够用;\n"
        "过高会和检测争 GPU。0 = 跟着抓帧速率走。"));
    m_depthFps->setToolTip(depthFpsSlider->toolTip());
    connect(m_depthFps, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { ConfigManager::instance().setDepthFps(v); });

    layout->addWidget(inferCard);

    // ====================================================================
    // Card 2: 深度运行时
    // ====================================================================
    auto* runtimeCard = new CardWidget(QStringLiteral("深度运行时"),
                                       QStringLiteral("activity"));

    // ── 深度遮罩 FPS ──
    QSlider* maskFpsSlider = nullptr;
    runtimeCard->contentLayout()->addWidget(
        FormKit::sliderRow(QStringLiteral("遮罩 FPS"), 1, 240, 5,
                           maskFpsSlider, m_maskFpsRuntime));
    maskFpsSlider->setToolTip(tr(
        "深度遮罩(剪裁掉远景目标)的更新频率。一般 5~10 即可,\n"
        "画面静止时基本看不出区别,频率高只是更跟手。"));
    m_maskFpsRuntime->setToolTip(maskFpsSlider->toolTip());
    connect(m_maskFpsRuntime, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { ConfigManager::instance().setDepthMaskFps(v); });

    // ── 深度引擎 OPT 尺寸 ──
    QSlider* optSizeSlider = nullptr;
    runtimeCard->contentLayout()->addWidget(
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

    // ── 归一化下裁剪 % ──
    runtimeCard->contentLayout()->addWidget(
        FormKit::sliderRowD(QStringLiteral("归一化下裁剪 %"), 0.0, 50.0, 0.0, 0.1, 1,
                            m_normClipLowSlider, m_normClipLowSpin));
    m_normClipLowSlider->setToolTip(tr(
        "深度归一化时,把最暗(最远)的 N% 像素裁剪到 0。\n"
        "通常保持 0;场景里有极远的离群值(天空/无穷远)时调到 1~5 可让中段更清晰。"));
    m_normClipLowSpin->setToolTip(m_normClipLowSlider->toolTip());
    connect(m_normClipLowSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [](double v) { ConfigManager::instance().setDepthNormClipLowPct(static_cast<float>(v)); });

    // ── 归一化上裁剪 % ──
    runtimeCard->contentLayout()->addWidget(
        FormKit::sliderRowD(QStringLiteral("归一化上裁剪 %"), 50.0, 100.0, 100.0, 0.1, 1,
                            m_normClipHighSlider, m_normClipHighSpin));
    m_normClipHighSlider->setToolTip(tr(
        "深度归一化时,把最亮(最近)的 (100-N)% 像素裁剪到 255。\n"
        "100 = 不裁剪(传统 MIN-MAX);95 = 把贴脸的枪/手等极近离群值裁掉,\n"
        "避免它把整个亮端拉满、远处敌人被压到 depth_norm=0 然后被遮罩误伤。\n"
        "如果你发现远处的敌人被深度遮罩误抑制,把这个调到 90~95 试试。"));
    m_normClipHighSpin->setToolTip(m_normClipHighSlider->toolTip());
    connect(m_normClipHighSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [](double v) { ConfigManager::instance().setDepthNormClipHighPct(static_cast<float>(v)); });

    layout->addWidget(runtimeCard);

    // ====================================================================
    // Card 3: 深度遮罩
    // ====================================================================
    auto* maskCard = new CardWidget(QStringLiteral("深度遮罩"),
                                    QStringLiteral("layers-intersect"));

    // ── 启用遮罩 ──
    maskCard->contentLayout()->addWidget(
        FormKit::toggleRow(QStringLiteral("启用深度遮罩"), false, m_maskEnabled));
    m_maskEnabled->setToolTip(tr(
        "开启后用深度图生成遮罩,把'远景里的目标'(疑似穿墙看到的)从瞄准候选里剔除。\n"
        "前提:深度推理已开启。"));
    connect(m_maskEnabled, &ToggleSwitch::toggled,
            this, [](bool v) { ConfigManager::instance().setDepthMaskEnabled(v); });

    // ── 近景百分比 ──
    maskCard->contentLayout()->addWidget(
        FormKit::sliderRow(QStringLiteral("近端 %"), 1, 100, 20,
                           m_nearSlider, m_nearSpin, QStringLiteral(" %")));
    m_nearSlider->setToolTip(tr(
        "深度图按从近到远取前 N% 像素作为'前景区',只在前景里的目标会被认可。\n"
        "值小 = 只信很近的物体(墙更可能挡住);值大 = 大部分场景都算前景(误剔少)。"));
    m_nearSpin->setToolTip(m_nearSlider->toolTip());
    connect(m_nearSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { ConfigManager::instance().setDepthMaskNearPercent(v); });

    // ── 扩展像素 ──
    maskCard->contentLayout()->addWidget(
        FormKit::sliderRow(QStringLiteral("扩展 (px)"), 0, 128, 0,
                           m_expandSlider, m_expandSpin));
    m_expandSlider->setToolTip(tr(
        "对前景遮罩做形态学膨胀,以像素为单位。值越大边界外目标也认。\n"
        "用于补偿深度图边缘略微偏内的情况。"));
    m_expandSpin->setToolTip(m_expandSlider->toolTip());
    connect(m_expandSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { ConfigManager::instance().setDepthMaskExpand(v); });

    // ── 保持帧数 ──
    QSlider* holdSlider = nullptr;
    maskCard->contentLayout()->addWidget(
        FormKit::sliderRow(QStringLiteral("保持帧数"), 0, 120, 5,
                           holdSlider, m_holdFrames));
    holdSlider->setToolTip(tr(
        "深度模型暂时没出新结果时,沿用旧遮罩多少帧。0 = 不沿用(无遮罩时不剔除任何目标)。\n"
        "开高一点可以容忍偶尔的深度推理掉帧。"));
    m_holdFrames->setToolTip(holdSlider->toolTip());
    connect(m_holdFrames, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { ConfigManager::instance().setDepthMaskHoldFrames(v); });

    // ── 抑制比例 ──
    maskCard->contentLayout()->addWidget(
        FormKit::sliderRowD(QStringLiteral("抑制占比阈值"), 0.0, 1.0, 0.30, 0.01, 2,
                            m_suppressSlider, m_suppressSpin));
    m_suppressSlider->setToolTip(tr(
        "bbox 落在遮罩上(被认为远景)的像素比例超过这个值才丢弃该检测。\n"
        "0 = 任意 1 个像素命中即丢;0.30 较稳妥默认;1.0 = 永不丢弃。"));
    m_suppressSpin->setToolTip(m_suppressSlider->toolTip());
    connect(m_suppressSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [](double v) { ConfigManager::instance().setDepthMaskSuppressionRatio(static_cast<float>(v)); });

    // ── 遮罩透明度 ──
    maskCard->contentLayout()->addWidget(
        FormKit::sliderRow(QStringLiteral("遮罩透明度"), 0, 255, 90,
                           m_alphaSlider, m_alphaSpin));
    m_alphaSlider->setToolTip(tr(
        "调试视图里把遮罩叠加在画面上的不透明度(255 = 完全不透明)。\n"
        "只影响显示,不影响实际抑制逻辑。"));
    m_alphaSpin->setToolTip(m_alphaSlider->toolTip());
    connect(m_alphaSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { ConfigManager::instance().setDepthMaskAlpha(v); });

    // ── 反转遮罩 ──
    maskCard->contentLayout()->addWidget(
        FormKit::toggleRow(QStringLiteral("反转深度遮罩"), false, m_invertMask));
    m_invertMask->setToolTip(tr(
        "反转近/远的判定:勾上 = 把'远端' N% 当作前景。\n"
        "少数游戏的深度方向反向时用。"));
    connect(m_invertMask, &ToggleSwitch::toggled,
            this, [](bool v) { ConfigManager::instance().setDepthMaskInvert(v); });

    // ── 深度色图 ──
    m_colormapCombo = new QComboBox;
    m_colormapCombo->addItems(kColormapNames);
    m_colormapCombo->setToolTip(tr(
        "深度图可视化时使用的伪彩色映射。只影响调试显示。"));
    maskCard->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("深度色图"), m_colormapCombo));
    connect(m_colormapCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [](int idx) { ConfigManager::instance().setDepthColormap(idx); });

    layout->addWidget(maskCard);

    // ====================================================================
    // Card 4: 深度可视化
    // ====================================================================
    auto* vizCard = new CardWidget(QStringLiteral("深度可视化"),
                                   QStringLiteral("eye"));

    // ── 显示深度热力图 ──
    vizCard->contentLayout()->addWidget(
        FormKit::toggleRow(QStringLiteral("显示深度热力图"), false, m_showHeatmap));
    m_showHeatmap->setToolTip(tr(
        "开启后,独立检测预览窗口的画面会被替换为深度热力图(按当前色图渲染)。\n"
        "前提:已启用深度推理。仅影响显示,不影响检测/瞄准。"));
    connect(m_showHeatmap, &ToggleSwitch::toggled,
            this, [](bool v) { ConfigManager::instance().setDepthShowHeatmap(v); });

    // ── 热力图 Gamma ──
    vizCard->contentLayout()->addWidget(
        FormKit::sliderRowD(QStringLiteral("热力图 Gamma"), 0.1, 5.0, 1.0, 0.01, 2,
                            m_heatmapGammaSlider, m_heatmapGammaSpin));
    m_heatmapGammaSlider->setToolTip(tr(
        "热力图色阶曲线。<1 把暗端(远景)拉亮,适合'近处一个物体把整体亮度拉满,远景全黑'的场景;\n"
        ">1 把亮端压暗、暗端更暗。1.0 = 不变。\n"
        "举例:你想让 70m 目标显示为半亮、100m 显示为全黑,把 gamma 调到 0.3~0.5 试试。\n"
        "只影响显示,不影响深度遮罩。"));
    m_heatmapGammaSpin->setToolTip(m_heatmapGammaSlider->toolTip());
    connect(m_heatmapGammaSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [](double v) { ConfigManager::instance().setDepthHeatmapGamma(static_cast<float>(v)); });

    // ── 在检测框上标注深度 ──
    vizCard->contentLayout()->addWidget(
        FormKit::toggleRow(QStringLiteral("检测框标注深度"), false, m_showBboxDistance));
    m_showBboxDistance->setToolTip(tr(
        "在独立检测预览窗口里,为每个检测框旁边写一个深度值(0.00 = 最远,1.00 = 最近)。\n"
        "是相对深度(每帧 MIN-MAX 归一化),不是绝对距离。\n"
        "前提:已启用深度推理。"));
    connect(m_showBboxDistance, &ToggleSwitch::toggled,
            this, [](bool v) { ConfigManager::instance().setDepthShowBboxDistance(v); });

    layout->addWidget(vizCard);

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
    block(m_depthFps, true);
    block(m_maskFpsRuntime, true);
    block(m_optInputSize, true);
    block(m_normClipLowSpin, true);
    block(m_normClipHighSpin, true);
    block(m_maskEnabled, true);
    block(m_nearSpin, true);
    block(m_expandSpin, true);
    block(m_holdFrames, true);
    block(m_suppressSpin, true);
    block(m_alphaSpin, true);
    block(m_invertMask, true);
    block(m_colormapCombo, true);
    block(m_showHeatmap, true);
    block(m_heatmapGammaSpin, true);
    block(m_showBboxDistance, true);

    m_depthEnabled->setChecked(cfg.depthInferenceEnabled());
    m_modelPath->setText(cfg.depthModelPath());
    m_depthFps->setValue(cfg.depthFps());
    m_maskFpsRuntime->setValue(cfg.depthMaskFps());
    m_optInputSize->setValue(cfg.depthOptInputSize());
    m_normClipLowSpin->setValue(static_cast<double>(cfg.depthNormClipLowPct()));
    m_normClipHighSpin->setValue(static_cast<double>(cfg.depthNormClipHighPct()));
    m_maskEnabled->setChecked(cfg.depthMaskEnabled());
    m_nearSpin->setValue(cfg.depthMaskNearPercent());
    m_expandSpin->setValue(cfg.depthMaskExpand());
    m_holdFrames->setValue(cfg.depthMaskHoldFrames());
    m_suppressSpin->setValue(static_cast<double>(cfg.depthMaskSuppressionRatio()));
    m_alphaSpin->setValue(cfg.depthMaskAlpha());
    m_invertMask->setChecked(cfg.depthMaskInvert());
    m_colormapCombo->setCurrentIndex(cfg.depthColormap());
    m_showHeatmap->setChecked(cfg.depthShowHeatmap());
    m_heatmapGammaSpin->setValue(static_cast<double>(cfg.depthHeatmapGamma()));
    m_showBboxDistance->setChecked(cfg.depthShowBboxDistance());

    block(m_depthEnabled, false);
    block(m_modelPath, false);
    block(m_depthFps, false);
    block(m_maskFpsRuntime, false);
    block(m_optInputSize, false);
    block(m_normClipLowSpin, false);
    block(m_normClipHighSpin, false);
    block(m_maskEnabled, false);
    block(m_nearSpin, false);
    block(m_expandSpin, false);
    block(m_holdFrames, false);
    block(m_suppressSpin, false);
    block(m_alphaSpin, false);
    block(m_invertMask, false);
    block(m_colormapCombo, false);
    block(m_showHeatmap, false);
    block(m_heatmapGammaSpin, false);
    block(m_showBboxDistance, false);

    // Sync sliders with spin boxes (setting spin with signals blocked
    // doesn't trigger the spin->slider connection, so update sliders manually).
    // Int sliders: direct value mapping.
    if (m_nearSlider) m_nearSlider->setValue(m_nearSpin->value());
    if (m_expandSlider) m_expandSlider->setValue(m_expandSpin->value());
    if (m_alphaSlider) m_alphaSlider->setValue(m_alphaSpin->value());
    // Double sliders: slider = (value - min) / step
    auto dSliderSync = [](QSlider* s, QDoubleSpinBox* sp) {
        if (s && sp)
            s->setValue(static_cast<int>(std::lround(
                (sp->value() - sp->minimum()) / sp->singleStep())));
    };
    dSliderSync(m_normClipLowSlider, m_normClipLowSpin);
    dSliderSync(m_normClipHighSlider, m_normClipHighSpin);
    dSliderSync(m_suppressSlider, m_suppressSpin);
    dSliderSync(m_heatmapGammaSlider, m_heatmapGammaSpin);
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
