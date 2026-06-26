#include "pages/FlashlightPage.h"

#include "Apotheosis.h"
#include "config/ConfigManager.h"
#include "config/config.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"
#include "widgets/ToggleSwitch.h"

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
        "在整个画面里找「亮 + 圆 + 径向衰减」的光斑(手电筒/闪光筒眩光),命中后固定作为"
        "「shoudiantong」类别注入瞄准管线 —— 在「瞄准热键」页把 shoudiantong 加进该热键的"
        "瞄准类别并排好优先级,即可像普通目标一样被瞄。\n\n"
        "户外抗误锁:单帧只看外观分不清手电和太阳/天空/反光(它们也是亮圆渐变),所以加了"
        "三道判别 —— 深度门(杀太阳/天空/远处泛光,需在「深度」页开启深度推理)、时间门"
        "(杀水面/玻璃的闪烁反光)、与模型框联动(白天模型已看见人时直接采信)。三道的强度"
        "由「抗误锁」一个旋钮统一控制。\n\n"
        "调参只有三个旋钮:灵敏度 / 抗误锁 / 光斑大小,其余全部内部自动。\n"
        "每个热键单独开关在「瞄准热键」页的「启用寻光检测」。"));
    intro->setWordWrap(true);
    introCard->contentLayout()->addWidget(intro);
    layout->addWidget(introCard);

    // ====================================================================
    // Routing hint card — the halo is filed under a fixed class
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
    // Detection params card — three macro knobs only
    // ====================================================================
    auto* paramCard = new CardWidget(
        QStringLiteral("检测参数"),
        QStringLiteral("zap"));

    paramCard->contentLayout()->addWidget(
        FormKit::toggleRow(QStringLiteral("显示预览(用于调试)"),
                           cfg.flashlightShowPreview(), m_showPreview));
    m_showPreview->setToolTip(tr("在检测预览窗口画圈 + 中心十字 + 置信度标签(绿=接受,红=拒绝)。"));
    connect(m_showPreview, &ToggleSwitch::toggled,
            this, [](bool v) {
                ConfigManager::instance().setFlashlightShowPreview(v);
                std::lock_guard<std::recursive_mutex> lk(configMutex);
                config.flashlight_show_preview = v;
            });

    // 灵敏度 — 外观轴
    paramCard->contentLayout()->addWidget(
        FormKit::sliderRow(QStringLiteral("灵敏度"),
                           0, 100, cfg.flashlightSensitivity(),
                           m_sensitivitySlider, m_sensitivity));
    m_sensitivitySlider->setToolTip(tr(
        "锁得勤 ↔ 锁得稳(外观轴)。\n"
        "越高:更亮/更不圆/对比更低的光也收,锁得快但更易误锁;\n"
        "越低:只收又亮又圆、明显贴在暗背景上的光,稳但可能漏。\n"
        "内部自动驱动 亮度阈值 / 圆度 / 局部对比度 / 接受门槛。"));
    m_sensitivity->setToolTip(m_sensitivitySlider->toolTip());
    connect(m_sensitivity, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) {
                ConfigManager::instance().setFlashlightSensitivity(v);
                std::lock_guard<std::recursive_mutex> lk(configMutex);
                config.flashlight_sensitivity = v;
            });

    // 抗误锁 — 行为/环境轴
    paramCard->contentLayout()->addWidget(
        FormKit::sliderRow(QStringLiteral("抗误锁强度"),
                           0, 100, cfg.flashlightRejectStrength(),
                           m_rejectSlider, m_reject));
    m_rejectSlider->setToolTip(tr(
        "信外观 ↔ 信判别。户外乱锁就往上调。\n"
        "0 ≈ 旧行为(纯看外观);往上依次叠加:\n"
        "  深度门(杀太阳/天空/远处泛光,需「深度」页开启深度推理)\n"
        "  时间门(必须连续几帧稳定成轨迹,杀水面/玻璃闪烁反光)\n"
        "  与模型框联动(有人形框=快速采信;孤儿光斑须过深度+时间门)。"));
    m_reject->setToolTip(m_rejectSlider->toolTip());
    connect(m_reject, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) {
                ConfigManager::instance().setFlashlightRejectStrength(v);
                std::lock_guard<std::recursive_mutex> lk(configMutex);
                config.flashlight_reject_strength = v;
            });

    // 光斑大小 — 半径档(按分辨率自动)
    paramCard->contentLayout()->addWidget(
        FormKit::sliderRow(QStringLiteral("光斑大小"),
                           0, 100, cfg.flashlightSpotSize(),
                           m_spotSizeSlider, m_spotSize));
    m_spotSizeSlider->setToolTip(tr(
        "可接受的光斑半径档,按检测分辨率自动换算(不用再填像素)。\n"
        "越小:只收较小的光斑(远处/小光点);越大:贴脸的大片眩光也算。"));
    m_spotSize->setToolTip(m_spotSizeSlider->toolTip());
    connect(m_spotSize, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) {
                ConfigManager::instance().setFlashlightSpotSize(v);
                std::lock_guard<std::recursive_mutex> lk(configMutex);
                config.flashlight_spot_size = v;
            });

    layout->addWidget(paramCard);
    layout->addStretch();

    loadConfig();
    connect(&cfg, &ConfigManager::configLoaded, this, &FlashlightPage::loadConfig);
}

void FlashlightPage::loadConfig() {
    auto& cfg = ConfigManager::instance();
    m_showPreview->setChecked(cfg.flashlightShowPreview());
    m_sensitivity->setValue(cfg.flashlightSensitivity());
    m_reject->setValue(cfg.flashlightRejectStrength());
    m_spotSize->setValue(cfg.flashlightSpotSize());
}
