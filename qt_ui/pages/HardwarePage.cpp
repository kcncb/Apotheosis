#include "pages/HardwarePage.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"
#include "widgets/ToggleSwitch.h"

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>

HardwarePage::HardwarePage(QWidget* parent)
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

    // ── Card 1: 输入方式 ──
    auto* inputCard = new CardWidget(QStringLiteral("输入方式"),
                                     QStringLiteral("plug"));

    m_inputMethodCombo = new QComboBox;
    m_inputMethodCombo->addItems({
        QStringLiteral("Win32"),
        QStringLiteral("Logitech GHUB"),
        QStringLiteral("Arduino"),
        QStringLiteral("Kmbox Net"),
        QStringLiteral("Kmbox A"),
        QStringLiteral("MAKCU"),
    });
    inputCard->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("方式"), m_inputMethodCombo));
    layout->addWidget(inputCard);

    // ── Card 2: 设备参数 ──
    auto* deviceCard = new CardWidget(QStringLiteral("设备参数"),
                                      QStringLiteral("adjustments"));
    m_deviceStack = new QStackedWidget;

    auto makePanel = [](QVBoxLayout*& panelLayout) -> QWidget* {
        auto* page = new QWidget;
        panelLayout = new QVBoxLayout(page);
        panelLayout->setContentsMargins(0, 0, 0, 0);
        panelLayout->setSpacing(10);
        return page;
    };

    auto noConfigPanel = []() -> QWidget* {
        auto* page = new QWidget;
        auto* l = new QVBoxLayout(page);
        l->setContentsMargins(0, 0, 0, 0);
        auto* label = new QLabel(QStringLiteral("无需额外配置"));
        label->setProperty("class", "secondary");
        l->addWidget(label);
        return page;
    };

    // Index 0: Win32
    m_deviceStack->addWidget(noConfigPanel());

    // Index 1: Logitech GHUB
    m_deviceStack->addWidget(noConfigPanel());

    // Index 2: Arduino
    QVBoxLayout* arduinoLayout = nullptr;
    auto* arduinoPage = makePanel(arduinoLayout);

    auto* arduinoPort = new QLineEdit(QStringLiteral("COM3"));
    arduinoLayout->addWidget(FormKit::fieldRow(QStringLiteral("串口"), arduinoPort));

    auto* arduinoBaud = new QSpinBox;
    arduinoBaud->setRange(1200, 921600);
    arduinoBaud->setValue(115200);
    arduinoLayout->addWidget(FormKit::fieldRow(QStringLiteral("波特率"), arduinoBaud));

    ToggleSwitch* arduino16bit = nullptr;
    arduinoLayout->addWidget(
        FormKit::toggleRow(QStringLiteral("16位鼠标"), false, arduino16bit));

    ToggleSwitch* arduinoKeys = nullptr;
    arduinoLayout->addWidget(
        FormKit::toggleRow(QStringLiteral("启用按键"), false, arduinoKeys));
    m_deviceStack->addWidget(arduinoPage);

    // Index 3: Kmbox Net
    QVBoxLayout* kmnetLayout = nullptr;
    auto* kmnetPage = makePanel(kmnetLayout);
    kmnetLayout->addWidget(
        FormKit::fieldRow(QStringLiteral("IP 地址"),
                          new QLineEdit(QStringLiteral("192.168.2.188"))));
    kmnetLayout->addWidget(
        FormKit::fieldRow(QStringLiteral("端口"),
                          new QLineEdit(QStringLiteral("16896"))));
    kmnetLayout->addWidget(
        FormKit::fieldRow(QStringLiteral("UUID"), new QLineEdit));
    m_deviceStack->addWidget(kmnetPage);

    // Index 4: Kmbox A
    QVBoxLayout* kmaLayout = nullptr;
    auto* kmaPage = makePanel(kmaLayout);
    kmaLayout->addWidget(
        FormKit::fieldRow(QStringLiteral("PID/VID"), new QLineEdit));
    m_deviceStack->addWidget(kmaPage);

    // Index 5: MAKCU
    QVBoxLayout* makcuLayout = nullptr;
    auto* makcuPage = makePanel(makcuLayout);
    makcuLayout->addWidget(
        FormKit::fieldRow(QStringLiteral("串口"),
                          new QLineEdit(QStringLiteral("COM3"))));

    auto* makcuBaud = new QSpinBox;
    makcuBaud->setRange(1200, 921600);
    makcuBaud->setValue(115200);
    makcuLayout->addWidget(FormKit::fieldRow(QStringLiteral("波特率"), makcuBaud));
    m_deviceStack->addWidget(makcuPage);

    deviceCard->contentLayout()->addWidget(m_deviceStack);
    layout->addWidget(deviceCard);

    connect(m_inputMethodCombo, &QComboBox::currentIndexChanged,
            this, &HardwarePage::onInputMethodChanged);
    m_deviceStack->setCurrentIndex(0);

    layout->addStretch();
}

void HardwarePage::onInputMethodChanged(int index) {
    m_deviceStack->setCurrentIndex(index);
}
