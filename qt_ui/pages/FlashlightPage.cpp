#include "pages/FlashlightPage.h"

#include "Apotheosis.h"
#include "config/ConfigManager.h"
#include "config/config.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"
#include "widgets/ToggleSwitch.h"

#include <QLabel>
#include <QHBoxLayout>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QToolButton>
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
        QString::fromUtf8(u8"寻光 - 说明"),
        QStringLiteral("info"));
    auto* intro = new QLabel(QString::fromUtf8(
        u8"寻找手电筒过曝白核及其径向光晕。检测与 YOLO 推理同频：没有人物框时必须连续三次"
        "确认才会作为「shoudiantong」独立目标；任一推理帧丢失就立即撤销。\n\n"
        "白核与人物框关联时不会生成额外光斑目标，瞄准仍完全遵守人物类别、优先级和锁点设置。"
        "只有模型看不见人物时才瞄光核。默认策略偏向少误锁，边界案例可能被放弃。\n\n"
        "每个热键的开关位于「瞄准热键」页。"));
    intro->setWordWrap(true);
    introCard->contentLayout()->addWidget(intro);
    layout->addWidget(introCard);

    // ====================================================================
    // Routing hint card — the halo is filed under a fixed class
    // ====================================================================
    auto* classCard = new CardWidget(
        QString::fromUtf8(u8"瞄准类别"),
        QStringLiteral("target"));
    auto* classHint = new QLabel(QString::fromUtf8(
        u8"独立光斑仍使用「shoudiantong」类别。到「瞄准热键」页把它加入瞄准类别并排序；"
        "不添加时只检测和预览，不会独立瞄光。有人物框时始终使用原人物类别。"));
    classHint->setWordWrap(true);
    classHint->setStyleSheet(QStringLiteral("color: #888;"));
    classCard->contentLayout()->addWidget(classHint);
    layout->addWidget(classCard);

    // ====================================================================
    // Detection params card — three macro knobs only
    // ====================================================================
    auto* paramCard = new CardWidget(
        QString::fromUtf8(u8"检测参数"),
        QStringLiteral("zap"));

    paramCard->contentLayout()->addWidget(
        FormKit::toggleRow(QString::fromUtf8(u8"显示识别圆圈"),
                           cfg.flashlightShowPreview(), m_showPreview));
    m_showPreview->setToolTip(QString::fromUtf8(u8"只在最终识别成功的白核外画一个圆圈。"));
    connect(m_showPreview, &ToggleSwitch::toggled,
            this, [](bool v) {
                ConfigManager::instance().setFlashlightShowPreview(v);
                std::lock_guard<std::recursive_mutex> lk(configMutex);
                config.flashlight_show_preview = v;
            });

    // 主参数：普通用户只需调整这一项。
    paramCard->contentLayout()->addWidget(
        FormKit::sliderRow(QString::fromUtf8(u8"检测倾向"),
                           0, 100, cfg.flashlightSensitivity(),
                           m_sensitivitySlider, m_sensitivity));
    m_sensitivitySlider->setToolTip(QString::fromUtf8(
        u8"左侧更稳健：宁可漏掉边界光斑也减少误锁；右侧更灵敏：更容易发现弱小白核。"));
    m_sensitivity->setToolTip(m_sensitivitySlider->toolTip());
    connect(m_sensitivity, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) {
                ConfigManager::instance().setFlashlightSensitivity(v);
                std::lock_guard<std::recursive_mutex> lk(configMutex);
                config.flashlight_sensitivity = v;
            });

    auto* presetRow = new QWidget;
    auto* presetLayout = new QHBoxLayout(presetRow);
    presetLayout->setContentsMargins(0, 0, 0, 0);
    presetLayout->setSpacing(8);
    auto* stableButton = new QPushButton(QString::fromUtf8(u8"稳健"));
    auto* balancedButton = new QPushButton(QString::fromUtf8(u8"均衡"));
    auto* sensitiveButton = new QPushButton(QString::fromUtf8(u8"灵敏"));
    presetLayout->addWidget(stableButton);
    presetLayout->addWidget(balancedButton);
    presetLayout->addWidget(sensitiveButton);
    presetLayout->addStretch();
    paramCard->contentLayout()->addWidget(presetRow);

    auto applyPreset = [this](int tendency, int reject, int size) {
        m_sensitivity->setValue(tendency);
        m_reject->setValue(reject);
        m_spotSize->setValue(size);
    };
    connect(stableButton, &QPushButton::clicked, this,
            [applyPreset]() { applyPreset(30, 80, 45); });
    connect(balancedButton, &QPushButton::clicked, this,
            [applyPreset]() { applyPreset(50, 70, 55); });
    connect(sensitiveButton, &QPushButton::clicked, this,
            [applyPreset]() { applyPreset(75, 55, 65); });

    auto* advancedButton = new QToolButton;
    advancedButton->setText(QString::fromUtf8(u8"高级设置"));
    advancedButton->setCheckable(true);
    advancedButton->setChecked(false);
    advancedButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    advancedButton->setArrowType(Qt::RightArrow);
    paramCard->contentLayout()->addWidget(advancedButton);

    auto* advancedBody = new QWidget;
    auto* advancedLayout = new QVBoxLayout(advancedBody);
    advancedLayout->setContentsMargins(0, 0, 0, 0);
    advancedLayout->setSpacing(8);
    advancedBody->setVisible(false);
    connect(advancedButton, &QToolButton::toggled, advancedBody,
            [advancedButton, advancedBody](bool open) {
                advancedButton->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
                advancedBody->setVisible(open);
            });

    // 抗误锁 — 行为/环境轴
    advancedLayout->addWidget(
        FormKit::sliderRow(QString::fromUtf8(u8"抗误锁强度"),
                           0, 100, cfg.flashlightRejectStrength(),
                           m_rejectSlider, m_reject));
    m_rejectSlider->setToolTip(QString::fromUtf8(
        u8"越高越依赖深度和严格外观判别。建议保持预设值；独立光斑始终需要连续三次确认。"));
    m_reject->setToolTip(m_rejectSlider->toolTip());
    connect(m_reject, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) {
                ConfigManager::instance().setFlashlightRejectStrength(v);
                std::lock_guard<std::recursive_mutex> lk(configMutex);
                config.flashlight_reject_strength = v;
            });

    // 光斑大小 — 半径档(按分辨率自动)
    advancedLayout->addWidget(
        FormKit::sliderRow(QString::fromUtf8(u8"最大核心大小"),
                           0, 100, cfg.flashlightSpotSize(),
                           m_spotSizeSlider, m_spotSize));
    m_spotSizeSlider->setToolTip(QString::fromUtf8(
        u8"限制可接受白核的最大尺寸，按检测分辨率自动换算。过大容易把白色区域当成手电。"));
    m_spotSize->setToolTip(m_spotSizeSlider->toolTip());
    connect(m_spotSize, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) {
                ConfigManager::instance().setFlashlightSpotSize(v);
                std::lock_guard<std::recursive_mutex> lk(configMutex);
                config.flashlight_spot_size = v;
            });

    advancedLayout->addWidget(new QLabel(QString::fromUtf8(
        u8"确认次数固定为连续 3 个 YOLO 推理帧；丢失一帧立即撤销。")));
    paramCard->contentLayout()->addWidget(advancedBody);

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
