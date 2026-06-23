#include "pages/FlashlightPage.h"

#include "Apotheosis.h"
#include "config/ConfigManager.h"
#include "config/config.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"
#include "widgets/ToggleSwitch.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QScrollArea>
#include <QSpinBox>
#include <QTimer>
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
        "在整个画面里搜亮度满足阈值的圆形光斑(手电筒/闪光筒眩光)。命中后会作为"
        "一条虚拟检测注入瞄准管线,以下面选定的「目标类别」上报 —— 该类别在「目标」"
        "页设为 Aim 并且在某个瞄准热键里加进 aim_classes,即可像普通目标一样被瞄。\n\n"
        "原理:亮度门限基于 max(B,G,R),不挑颜色;圆度过滤掉细长 UI 反光/天空块。"
        "整套和准星找色 / 镭射找色相互独立,可并行运行。\n"
        "每个热键单独开关在「瞄准热键」页的「启用寻光检测」。"));
    intro->setWordWrap(true);
    introCard->contentLayout()->addWidget(intro);
    layout->addWidget(introCard);

    // ====================================================================
    // Target class card
    // ====================================================================
    auto* classCard = new CardWidget(
        QStringLiteral("目标类别"),
        QStringLiteral("target"));
    auto* classHint = new QLabel(tr(
        "把检测到的光斑归类为哪个类别。该类别需要在「目标」页存在 (Aim 或 Filter "
        "都可以,Delete 会被直接丢弃)。-1 = 不分类,瞄准管线收不到这个虚拟目标。"));
    classHint->setWordWrap(true);
    classHint->setStyleSheet(QStringLiteral("color: #888;"));
    classCard->contentLayout()->addWidget(classHint);

    m_classCombo = new QComboBox(this);
    classCard->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("光斑类别"), m_classCombo));
    rebuildClassCombo();
    connect(m_classCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int){
                const int cls = m_classCombo->currentData().toInt();
                ConfigManager::instance().setFlashlightTargetClassId(cls);
                std::lock_guard<std::recursive_mutex> lk(configMutex);
                config.flashlight_target_class_id = cls;
            });
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

    layout->addWidget(paramCard);
    layout->addStretch();

    // Re-poll the class list every second — TargetPage may add/remove
    // entries while we're up. The combo's currentData (= class_id) survives
    // a rebuild because we re-select by id, not by row.
    auto* poll = new QTimer(this);
    poll->setInterval(1000);
    connect(poll, &QTimer::timeout, this, &FlashlightPage::rebuildClassCombo);
    poll->start();

    loadConfig();
    connect(&cfg, &ConfigManager::configLoaded, this, &FlashlightPage::loadConfig);
}

void FlashlightPage::rebuildClassCombo() {
    const int current = m_classCombo->currentData().isValid()
        ? m_classCombo->currentData().toInt()
        : ConfigManager::instance().flashlightTargetClassId();

    m_classCombo->blockSignals(true);
    m_classCombo->clear();
    m_classCombo->addItem(QStringLiteral("(-1) 不分类 — 不注入虚拟目标"), -1);
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        for (const auto& cf : config.class_filters) {
            QString name = cf.class_name.empty()
                ? QStringLiteral("class_%1").arg(cf.class_id)
                : QString::fromStdString(cf.class_name);
            m_classCombo->addItem(QStringLiteral("(%1) %2").arg(cf.class_id).arg(name),
                                  cf.class_id);
        }
    }
    // Restore by id.
    int idx = m_classCombo->findData(current);
    if (idx < 0) idx = 0;
    m_classCombo->setCurrentIndex(idx);
    m_classCombo->blockSignals(false);
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
    rebuildClassCombo();
}
