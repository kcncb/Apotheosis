#include "pages/AutoCapturePage.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <mutex>
#include <sstream>
#include <string>

#include "Apotheosis.h"
#include "capture/auto_capture.h"
#include "config.h"
#include "config/config_bridge.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"
#include "widgets/ToggleSwitch.h"

AutoCapturePage::AutoCapturePage(QWidget* parent)
    : QWidget(parent)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);

    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(14);
    scroll->setWidget(content);

    auto* topCard = new CardWidget(
        QStringLiteral("\xe8\x87\xaa\xe5\x8a\xa8\xe9\x87\x87\xe9\x9b\x86"),
        QStringLiteral("camera"));
    auto* tcl = topCard->contentLayout();
    tcl->addWidget(FormKit::toggleRow(
        QStringLiteral("\xe5\x90\xaf\xe7\x94\xa8\xe8\x87\xaa\xe5\x8a\xa8\xe9\x87\x87\xe9\x9b\x86"),
        false, m_enabled));
    layout->addWidget(topCard);

    // ── 阈值卡 ──
    auto* thCard = new CardWidget(
        QStringLiteral("\xe7\xbd\xae\xe4\xbf\xa1\xe5\xba\xa6\xe9\x97\xa8\xe6\xa7\x9b"),
        QStringLiteral("gauge"));
    auto* thcl = thCard->contentLayout();

    thcl->addWidget(FormKit::toggleRow(
        QStringLiteral("\xe9\xab\x98\xe7\xbd\xae\xe4\xbf\xa1\xe5\xba\xa6\xe9\x87\x87\xe9\x9b\x86"
                       " (conf \xe2\x89\xa5 \xe9\x98\x88\xe5\x80\xbc)"),
        true, m_useHigh));

    QSlider* hSl = nullptr;
    thcl->addWidget(FormKit::sliderRowD(
        QStringLiteral("\xe9\xab\x98\xe9\x98\x88\xe5\x80\xbc"),
        0.0, 1.0, 0.85, 0.01, 2, hSl, m_highConf));
    m_highConf->setToolTip(QStringLiteral(
        "\xe9\xab\x98\xe4\xba\x8e\xe6\xad\xa4\xe5\x80\xbc\xe7\x9a\x84\xe6\xa3\x80"
        "\xe6\xb5\x8b\xe7\xbb\x93\xe6\x9e\x9c\xe8\xa7\xa6\xe5\x8f\x91\xe9\x87\x87"
        "\xe9\x9b\x86\xe3\x80\x82"));

    thcl->addWidget(FormKit::toggleRow(
        QStringLiteral("\xe4\xbd\x8e\xe7\xbd\xae\xe4\xbf\xa1\xe5\xba\xa6\xe9\x87\x87\xe9\x9b\x86"
                       " (conf \xe2\x89\xa4 \xe9\x98\x88\xe5\x80\xbc)"),
        false, m_useLow));

    QSlider* lSl = nullptr;
    thcl->addWidget(FormKit::sliderRowD(
        QStringLiteral("\xe4\xbd\x8e\xe9\x98\x88\xe5\x80\xbc"),
        0.0, 1.0, 0.30, 0.01, 2, lSl, m_lowConf));
    m_lowConf->setToolTip(QStringLiteral(
        "\xe4\xbd\x8e\xe4\xba\x8e\xe6\xad\xa4\xe5\x80\xbc\xe7\x9a\x84\xe6\xa3\x80"
        "\xe6\xb5\x8b\xe7\xbb\x93\xe6\x9e\x9c\xe8\xa7\xa6\xe5\x8f\x91\xe9\x87\x87"
        "\xe9\x9b\x86 (\xe9\x9a\xbe\xe4\xbe\x8b\xe6\xa0\xb7\xe6\x9c\xac)\xe3\x80\x82"));

    QSlider* cdSl = nullptr;
    thcl->addWidget(FormKit::sliderRow(
        QStringLiteral("\xe9\x87\x87\xe9\x9b\x86\xe9\x97\xb4\xe9\x9a\x94"),
        0, 5000, 200, cdSl, m_cooldownMs, QStringLiteral(" ms")));
    m_cooldownMs->setSingleStep(50);

    layout->addWidget(thCard);

    // ── 强制采集卡 ──
    auto* fkCard = new CardWidget(
        QStringLiteral("\xe5\xbc\xba\xe5\x88\xb6\xe9\x87\x87\xe9\x9b\x86"),
        QStringLiteral("hand"));
    auto* fkcl = fkCard->contentLayout();

    auto* fkHint = new QLabel(QStringLiteral(
        "\xe6\x8c\x89\xe4\xbd\x8f\xe6\xad\xa4\xe6\x8c\x89\xe9\x94\xae\xe6\x9c\x9f"
        "\xe9\x97\xb4\xe6\xaf\x8f\xe5\xb8\xa7\xe9\x83\xbd\xe5\xad\x98\xe7\x9b\x98,"
        "\xe6\x9d\xbe\xe5\xbc\x80\xe5\x90\x8e\xe5\x88\x87\xe6\x8d\xa2\xe5\x9b\x9e"
        "\xe9\x98\x88\xe5\x80\xbc\xe8\x87\xaa\xe5\x8a\xa8\xe9\x87\x87\xe9\x9b\x86\xe3\x80\x82"
        "\xe5\xa4\x9a\xe6\x8c\x89\xe9\x94\xae\xe7\x94\xa8\xe9\x80\x97\xe5\x8f\xb7"
        "\xe5\x88\x86\xe9\x9a\x94\xe3\x80\x82"));
    fkHint->setProperty("class", "secondary");
    fkHint->setWordWrap(true);
    fkcl->addWidget(fkHint);

    m_forceKeys = new QLineEdit;
    m_forceKeys->setPlaceholderText(QStringLiteral("X2MouseButton,..."));
    fkcl->addWidget(FormKit::fieldRow(
        QStringLiteral("\xe5\xbc\xba\xe5\x88\xb6\xe6\x8c\x89\xe9\x94\xae"),
        m_forceKeys));

    layout->addWidget(fkCard);

    // ── 输出卡 ──
    auto* outCard = new CardWidget(
        QStringLiteral("\xe8\xbe\x93\xe5\x87\xba"),
        QStringLiteral("folder"));
    auto* ocl = outCard->contentLayout();

    m_outputDir = new QLineEdit;
    m_outputDir->setPlaceholderText(QStringLiteral("screenshots/auto"));
    ocl->addWidget(FormKit::fieldRow(
        QStringLiteral("\xe8\xbe\x93\xe5\x87\xba\xe7\x9b\xae\xe5\xbd\x95"),
        m_outputDir));

    ocl->addWidget(FormKit::toggleRow(
        QStringLiteral("\xe5\x90\x8c\xe6\x97\xb6\xe5\x86\x99 YOLO \xe6\xa0\x87\xe7\xad\xbe (.txt)"),
        true, m_saveLabel));

    m_openDirBtn = new QPushButton(QStringLiteral("\xe6\x89\x93\xe5\xbc\x80\xe7\x9b\xae\xe5\xbd\x95"));
    ocl->addWidget(m_openDirBtn);

    layout->addWidget(outCard);

    // ── 状态卡 ──
    auto* stCard = new CardWidget(
        QStringLiteral("\xe7\x8a\xb6\xe6\x80\x81"),
        QStringLiteral("activity"));
    auto* scl = stCard->contentLayout();

    m_forceHeldLabel = new QLabel(QStringLiteral("\xe5\xbc\xba\xe5\x88\xb6\xe9\x94\xae: -"));
    scl->addWidget(m_forceHeldLabel);

    m_savedSessionLabel = new QLabel(QStringLiteral("\xe6\x9c\xac\xe6\xac\xa1\xe5\xad\x98\xe7\x9b\x98: 0"));
    scl->addWidget(m_savedSessionLabel);

    m_savedTotalLabel = new QLabel(QStringLiteral("\xe7\xb4\xaf\xe8\xae\xa1\xe5\xad\x98\xe7\x9b\x98: 0"));
    scl->addWidget(m_savedTotalLabel);

    m_resetBtn = new QPushButton(QStringLiteral("\xe9\x87\x8d\xe7\xbd\xae\xe6\x9c\xac\xe6\xac\xa1\xe8\xae\xa1\xe6\x95\xb0"));
    scl->addWidget(m_resetBtn);

    layout->addWidget(stCard);
    layout->addStretch();

    // ── Wiring ──
    auto wire_save = [this](auto* w, auto sig) {
        connect(w, sig, this, &AutoCapturePage::saveToConfig);
    };
    wire_save(m_enabled,   &ToggleSwitch::toggled);
    wire_save(m_useHigh,   &ToggleSwitch::toggled);
    wire_save(m_useLow,    &ToggleSwitch::toggled);
    wire_save(m_saveLabel, &ToggleSwitch::toggled);
    connect(m_highConf,   QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &AutoCapturePage::saveToConfig);
    connect(m_lowConf,    QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &AutoCapturePage::saveToConfig);
    connect(m_cooldownMs, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &AutoCapturePage::saveToConfig);
    connect(m_forceKeys,  &QLineEdit::editingFinished,
            this, &AutoCapturePage::saveToConfig);
    connect(m_outputDir,  &QLineEdit::editingFinished,
            this, &AutoCapturePage::saveToConfig);
    connect(m_openDirBtn, &QPushButton::clicked,
            this, &AutoCapturePage::onOpenDir);
    connect(m_resetBtn,   &QPushButton::clicked,
            this, &AutoCapturePage::onResetCounter);

    onLoadConfig();
}

static QString joinKeys(const std::vector<std::string>& keys) {
    QString out;
    for (size_t i = 0; i < keys.size(); ++i) {
        if (i > 0) out.append(',');
        out.append(QString::fromStdString(keys[i]));
    }
    return out;
}

static std::vector<std::string> splitKeysQ(const QString& s) {
    std::vector<std::string> out;
    for (const auto& part : s.split(',', Qt::SkipEmptyParts)) {
        const QString trimmed = part.trimmed();
        if (!trimmed.isEmpty()) out.push_back(trimmed.toStdString());
    }
    return out;
}

void AutoCapturePage::onLoadConfig()
{
    m_loading = true;
    std::lock_guard<std::recursive_mutex> lk(configMutex);

    m_enabled->setChecked(config.auto_capture_enabled);
    m_useHigh->setChecked(config.auto_capture_use_high);
    m_highConf->setValue(static_cast<double>(config.auto_capture_high_conf));
    m_useLow->setChecked(config.auto_capture_use_low);
    m_lowConf->setValue(static_cast<double>(config.auto_capture_low_conf));
    m_cooldownMs->setValue(config.auto_capture_cooldown_ms);
    m_forceKeys->setText(joinKeys(config.auto_capture_force_keys));
    m_outputDir->setText(QString::fromStdString(config.auto_capture_output_dir));
    m_saveLabel->setChecked(config.auto_capture_save_label);

    m_loading = false;
}

void AutoCapturePage::saveToConfig()
{
    if (m_loading) return;

    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        config.auto_capture_enabled     = m_enabled->isChecked();
        config.auto_capture_use_high    = m_useHigh->isChecked();
        config.auto_capture_high_conf   = static_cast<float>(m_highConf->value());
        config.auto_capture_use_low     = m_useLow->isChecked();
        config.auto_capture_low_conf    = static_cast<float>(m_lowConf->value());
        config.auto_capture_cooldown_ms = m_cooldownMs->value();
        config.auto_capture_force_keys  = splitKeysQ(m_forceKeys->text());
        config.auto_capture_output_dir  = m_outputDir->text().trimmed().toStdString();
        if (config.auto_capture_output_dir.empty())
            config.auto_capture_output_dir = "screenshots/auto";
        config.auto_capture_save_label  = m_saveLabel->isChecked();
    }
    ConfigBridge::instance().markDirty();
}

void AutoCapturePage::setForceHeld(bool held)
{
    m_forceHeldLabel->setText(held
        ? QStringLiteral("\xe5\xbc\xba\xe5\x88\xb6\xe9\x94\xae: \xe6\x8c\x89\xe4\xbd\x8f\xe4\xb8\xad")
        : QStringLiteral("\xe5\xbc\xba\xe5\x88\xb6\xe9\x94\xae: -"));
}

void AutoCapturePage::setSavedCounts(int session, int total)
{
    m_savedSessionLabel->setText(QStringLiteral("\xe6\x9c\xac\xe6\xac\xa1\xe5\xad\x98\xe7\x9b\x98: %1").arg(session));
    m_savedTotalLabel->setText(QStringLiteral("\xe7\xb4\xaf\xe8\xae\xa1\xe5\xad\x98\xe7\x9b\x98: %1").arg(total));
}

void AutoCapturePage::onResetCounter()
{
    AutoCapture::reset_session_counter();
}

void AutoCapturePage::onOpenDir()
{
    QString dir = m_outputDir->text().trimmed();
    if (dir.isEmpty()) dir = QStringLiteral("screenshots/auto");
    QDir().mkpath(dir);
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(dir).absoluteFilePath()));
}
