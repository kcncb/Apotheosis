#include "pages/FlashlightPage.h"

#include "Apotheosis.h"
#include "config/ConfigManager.h"
#include "config/config.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"
#include "widgets/ToggleSwitch.h"

#include <QDoubleSpinBox>
#include <QLabel>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

#include <mutex>

FlashlightPage::FlashlightPage(QWidget* parent)
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
    // Intro card
    // ====================================================================
    auto* introCard = new CardWidget(
        QStringLiteral("寻光 - 说明"),
        QStringLiteral("info"));
    auto* intro = new QLabel(tr(
        "在整个画面里搜亮度满足阈值、且呈径向衰减的圆形光斑(手电筒/闪光筒眩光)。"
        "命中后固定作为「shoudiantong」类别注入瞄准管线 —— 在「瞄准热键」页把 "
        "shoudiantong 加进该热键的瞄准类别并排好优先级,即可像普通目标一样被瞄。\n\n"
        "原理:亮度门限基于 max(B,G,R),不挑颜色;靠圆度 + 径向衰减(核心比外圈亮、"
        "向外递减)判定,远近通吃,不再依赖固定像素半径。结果去重后最多取「最多识别"
        "数量」个,远处不会误识别成一大片。\n"
        "每个热键单独开关在「瞄准热键」页的「启用寻光检测」。"));
    intro->setWordWrap(true);
    introCard->contentLayout()->addWidget(intro);
    layout->addWidget(introCard);

    // ====================================================================
    // Routing hint card — the halo is filed under a fixed class now
    // ====================================================================
    auto* classCard = new CardWidget(
        QStringLiteral("瞄准类别"),
        QStringLiteral("target"));
    auto* classHint = new QLabel(tr(
        "光斑固定作为「shoudiantong」类别注入,这里无需选类。\n"
        "到「瞄准热键」页 → 选中热键 → 在瞄准类别里添加 shoudiantong,"
        "拖到合适位置定优先级(和模型类完全一样)。不添加 = 检测照跑、但不会瞄它。"));
    classHint->setWordWrap(true);
    classHint->setStyleSheet(QStringLiteral("color: #888;"));
    classCard->contentLayout()->addWidget(classHint);
    layout->addWidget(classCard);

    // ====================================================================
    // Detection params card
    // ====================================================================
    auto* paramCard = new CardWidget(
        QStringLiteral("检测参数"),
        QStringLiteral("zap"));

    paramCard->contentLayout()->addWidget(
        FormKit::toggleRow(QStringLiteral("显示预览(用于调试)"),
                           cfg.flashlightShowPreview(), m_showPreview));
    m_showPreview->setToolTip(tr("在检测预览窗口画黄圈 + 中心十字 + 置信度标签。"));
    connect(m_showPreview, &ToggleSwitch::toggled,
            this, [](bool v) {
                ConfigManager::instance().setFlashlightShowPreview(v);
                std::lock_guard<std::recursive_mutex> lk(configMutex);
                config.flashlight_show_preview = v;
            });

    paramCard->contentLayout()->addWidget(
        FormKit::sliderRow(QStringLiteral("亮度阈值"),
                           0, 255, cfg.flashlightBrightnessThreshold(),
                           m_thresholdSlider, m_threshold));
    m_thresholdSlider->setToolTip(tr(
        "越高越严格,仅检测超亮区域。手电筒眩光通常 200-240。基于 max(B,G,R)。"));
    m_threshold->setToolTip(m_thresholdSlider->toolTip());
    connect(m_threshold, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) {
                ConfigManager::instance().setFlashlightBrightnessThreshold(v);
                std::lock_guard<std::recursive_mutex> lk(configMutex);
                config.flashlight_brightness_threshold = v;
            });

    paramCard->contentLayout()->addWidget(
        FormKit::sliderRow(QStringLiteral("最小光斑半径"),
                           1, 512, cfg.flashlightMinRadius(),
                           m_minRadiusSlider, m_minRadius));
    m_minRadiusSlider->setToolTip(tr(
        "等价面积半径(检测像素)。小于此值的亮斑(枪口反光/远处灯点)忽略。"));
    m_minRadius->setToolTip(m_minRadiusSlider->toolTip());
    connect(m_minRadius, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) {
                ConfigManager::instance().setFlashlightMinRadius(v);
                std::lock_guard<std::recursive_mutex> lk(configMutex);
                config.flashlight_min_radius = v;
            });

    paramCard->contentLayout()->addWidget(
        FormKit::sliderRow(QStringLiteral("最大光斑半径"),
                           1, 512, cfg.flashlightMaxRadius(),
                           m_maxRadiusSlider, m_maxRadius));
    m_maxRadiusSlider->setToolTip(tr(
        "大于此值的亮区(白墙/天空)忽略。贴脸眩光很大,可调到 100-200。"));
    m_maxRadius->setToolTip(m_maxRadiusSlider->toolTip());
    connect(m_maxRadius, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) {
                ConfigManager::instance().setFlashlightMaxRadius(v);
                std::lock_guard<std::recursive_mutex> lk(configMutex);
                config.flashlight_max_radius = v;
            });

    paramCard->contentLayout()->addWidget(
        FormKit::sliderRowD(QStringLiteral("圆度要求"),
                            0.0, 1.0, static_cast<double>(cfg.flashlightMinCircularity()),
                            0.01, 2,
                            m_circularitySlider, m_circularity));
    m_circularitySlider->setToolTip(tr(
        "1.0 为完美圆形,建议 0.5-0.8。越大越严格,挡掉 UI 长条/斜向反光。"));
    m_circularity->setToolTip(m_circularitySlider->toolTip());
    connect(m_circularity, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [](double v) {
                ConfigManager::instance().setFlashlightMinCircularity(static_cast<float>(v));
                std::lock_guard<std::recursive_mutex> lk(configMutex);
                config.flashlight_min_circularity = static_cast<float>(v);
            });

    paramCard->contentLayout()->addWidget(
        FormKit::sliderRow(QStringLiteral("开运算半径(px)"),
                           0, 9, cfg.flashlightOpenRadius(),
                           m_openRadiusSlider, m_openRadius));
    m_openRadiusSlider->setToolTip(tr(
        "去掉单像素热点/镜面高光毛刺。0 = 关。1-2 通用。"));
    m_openRadius->setToolTip(m_openRadiusSlider->toolTip());
    connect(m_openRadius, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) {
                ConfigManager::instance().setFlashlightOpenRadius(v);
                std::lock_guard<std::recursive_mutex> lk(configMutex);
                config.flashlight_open_radius = v;
            });

    // 局部对比度 — 抑制天空/大片亮区误识别的主开关
    paramCard->contentLayout()->addWidget(
        FormKit::sliderRow(QStringLiteral("局部对比度"),
                           0, 150, cfg.flashlightMinLocalContrast(),
                           m_localContrastSlider, m_localContrast));
    m_localContrastSlider->setToolTip(tr(
        "光斑'内部平均亮度 − 紧邻外圈平均亮度' 至少要达到此值。\n"
        "0 = 关(只看绝对亮度,远距离对着天空会有大量误识别)。\n"
        "30 = 推荐(挡掉天空/云层,留下光晕)。80+ = 严格,只要明显贴在暗背景上的光斑。"));
    m_localContrast->setToolTip(m_localContrastSlider->toolTip());
    connect(m_localContrast, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) {
                ConfigManager::instance().setFlashlightMinLocalContrast(v);
                std::lock_guard<std::recursive_mutex> lk(configMutex);
                config.flashlight_min_local_contrast = v;
            });

    // 最多识别数量 — 去重后最多保留几个光斑,封顶避免远处误识别成一片
    paramCard->contentLayout()->addWidget(
        FormKit::sliderRow(QStringLiteral("最多识别数量"),
                           1, 8, cfg.flashlightMaxSpots(),
                           m_maxSpotsSlider, m_maxSpots));
    m_maxSpots->setToolTip(tr(
        "一帧最多上报几个光斑(按置信度排序、去重后截断)。\n"
        "瞄准只会用其中最佳的一个 —— 这里主要是封顶:远距离误识别一大片时,"
        "不会把一堆幻影目标塞给瞄准管线。默认 3。"));
    m_maxSpotsSlider->setToolTip(m_maxSpots->toolTip());
    connect(m_maxSpots, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) {
                ConfigManager::instance().setFlashlightMaxSpots(v);
                std::lock_guard<std::recursive_mutex> lk(configMutex);
                config.flashlight_max_spots = v;
            });

    layout->addWidget(paramCard);
    layout->addStretch();

    loadConfig();
    connect(&cfg, &ConfigManager::configLoaded, this, &FlashlightPage::loadConfig);
}

void FlashlightPage::loadConfig() {
    auto& cfg = ConfigManager::instance();
    m_showPreview->setChecked(cfg.flashlightShowPreview());
    m_threshold->setValue(cfg.flashlightBrightnessThreshold());
    m_minRadius->setValue(cfg.flashlightMinRadius());
    m_maxRadius->setValue(cfg.flashlightMaxRadius());
    m_circularity->setValue(static_cast<double>(cfg.flashlightMinCircularity()));
    m_openRadius->setValue(cfg.flashlightOpenRadius());
    m_localContrast->setValue(cfg.flashlightMinLocalContrast());
    m_maxSpots->setValue(cfg.flashlightMaxSpots());
}
