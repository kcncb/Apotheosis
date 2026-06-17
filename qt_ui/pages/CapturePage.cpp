#include "pages/CapturePage.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"
#include "widgets/ToggleSwitch.h"

#include <QComboBox>
#include <QLineEdit>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>

CapturePage::CapturePage(QWidget* parent)
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

    // ── Card 1: 采集方式 ──
    auto* methodCard = new CardWidget(QStringLiteral("采集方式"),
                                      QStringLiteral("device-desktop"));

    m_methodCombo = new QComboBox;
    m_methodCombo->addItems({
        QStringLiteral("桌面复制 (DXGI)"),
        QStringLiteral("窗口采集 (WinRT)"),
        QStringLiteral("UDP"),
        QStringLiteral("采集卡"),
    });
    methodCard->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("方式"), m_methodCombo));

    m_methodStack = new QStackedWidget;

    // Page 0/1: empty (DXGI / WinRT)
    m_methodStack->addWidget(new QWidget);
    m_methodStack->addWidget(new QWidget);

    // Page 2: UDP
    auto* udpPage = new QWidget;
    auto* udpLayout = new QVBoxLayout(udpPage);
    udpLayout->setContentsMargins(0, 0, 0, 0);
    udpLayout->setSpacing(10);
    m_udpIp = new QLineEdit(QStringLiteral("127.0.0.1"));
    m_udpPort = new QSpinBox;
    m_udpPort->setRange(1, 65535);
    m_udpPort->setValue(9870);
    udpLayout->addWidget(FormKit::fieldRow(QStringLiteral("IP 地址"), m_udpIp));
    udpLayout->addWidget(FormKit::fieldRow(QStringLiteral("端口"), m_udpPort));
    m_methodStack->addWidget(udpPage);

    // Page 3: 采集卡
    auto* cardPage = new QWidget;
    auto* cardLayout = new QVBoxLayout(cardPage);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(10);

    m_cardIndex = new QSpinBox;
    m_cardIndex->setRange(0, 16);
    m_cardWidth = new QSpinBox;
    m_cardWidth->setRange(1, 7680);
    m_cardWidth->setValue(1920);
    m_cardHeight = new QSpinBox;
    m_cardHeight->setRange(1, 4320);
    m_cardHeight->setValue(1080);
    m_cardFps = new QSpinBox;
    m_cardFps->setRange(1, 240);
    m_cardFps->setValue(60);
    m_cardFormat = new QComboBox;
    m_cardFormat->addItems({
        QStringLiteral("AUTO"),
        QStringLiteral("NV12"),
        QStringLiteral("MJPG"),
        QStringLiteral("YUY2"),
        QStringLiteral("RGB32"),
    });
    m_cropWidth = new QSpinBox;
    m_cropWidth->setRange(0, 7680);
    m_cropHeight = new QSpinBox;
    m_cropHeight->setRange(0, 4320);

    cardLayout->addWidget(FormKit::fieldRow(QStringLiteral("设备索引"), m_cardIndex));
    cardLayout->addWidget(FormKit::fieldRow(QStringLiteral("宽度"), m_cardWidth));
    cardLayout->addWidget(FormKit::fieldRow(QStringLiteral("高度"), m_cardHeight));
    cardLayout->addWidget(FormKit::fieldRow(QStringLiteral("帧率"), m_cardFps));
    cardLayout->addWidget(FormKit::fieldRow(QStringLiteral("格式"), m_cardFormat));
    cardLayout->addWidget(FormKit::fieldRow(QStringLiteral("裁剪宽度"), m_cropWidth));
    cardLayout->addWidget(FormKit::fieldRow(QStringLiteral("裁剪高度"), m_cropHeight));
    m_methodStack->addWidget(cardPage);

    methodCard->contentLayout()->addWidget(m_methodStack);
    layout->addWidget(methodCard);

    connect(m_methodCombo, &QComboBox::currentIndexChanged,
            this, &CapturePage::onMethodChanged);
    onMethodChanged(0);

    // ── Card 2: 检测参数 ──
    auto* detCard = new CardWidget(QStringLiteral("检测参数"),
                                   QStringLiteral("photo"));

    m_detResolution = new QComboBox;
    m_detResolution->addItems({
        QStringLiteral("256"),
        QStringLiteral("320"),
        QStringLiteral("416"),
        QStringLiteral("512"),
        QStringLiteral("640"),
    });
    m_detResolution->setCurrentIndex(1);
    detCard->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("检测分辨率"), m_detResolution));

    QSlider* captureFpsSlider = nullptr;
    detCard->contentLayout()->addWidget(
        FormKit::sliderRow(QStringLiteral("采集帧率"), 1, 240, 60,
                           captureFpsSlider, m_captureFps, QStringLiteral(" fps")));

    detCard->contentLayout()->addWidget(
        FormKit::toggleRow(QStringLiteral("圆形遮罩"), true, m_circleMask));

    layout->addWidget(detCard);
    layout->addStretch();
}

void CapturePage::onMethodChanged(int index) {
    m_methodStack->setCurrentIndex(index);

    // QStackedWidget sizes to its tallest page, so empty methods (DXGI / WinRT)
    // would leave a big blank block. Pin the height to the current page instead
    // so the area collapses when there is nothing to configure and expands for
    // UDP / capture-card parameters.
    auto* current = m_methodStack->currentWidget();
    const int h = current ? qMax(0, current->sizeHint().height()) : 0;
    m_methodStack->setFixedHeight(h);
}
