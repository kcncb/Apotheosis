#include "pages/CapturePage.h"
#include "config/ConfigManager.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"
#include "widgets/ToggleSwitch.h"

#include "capture/eth_capture.h"

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

// ── capture method key <-> combo index mapping ──
static const QStringList kMethodKeys = {
    QStringLiteral("udp_capture"),
    QStringLiteral("tcp_capture"),
    QStringLiteral("eth_capture"),
    QStringLiteral("opencv_capture"),
    QStringLiteral("mf_capture"),
    QStringLiteral("avermedia_capture"),
};
static const QStringList kMethodLabels = {
    QStringLiteral("UDP"),
    QStringLiteral("TCP"),
    QStringLiteral("以太网原始帧 (ProSexy)"),   // 以太网原始帧
    QStringLiteral("OpenCV 采集卡"),                         // OpenCV 采集卡
    QStringLiteral("MF 采集卡（自写）"),     // MF 采集卡（自写）
    QStringLiteral("圆刚 SDK 采集卡"),
};

static const QStringList kApiKeys = {
    QStringLiteral("DSHOW"),
    QStringLiteral("MSMF"),
    QStringLiteral("FFMPEG"),
    QStringLiteral("ANY"),
};

static const QStringList kFormatKeys = {
    QStringLiteral("NV12"),
    QStringLiteral("MJPG"),
    QStringLiteral("YUY2"),
    QStringLiteral("RGB32"),
};

static const QStringList kDecodeLabels = {
    QStringLiteral("GPU (nvJPEG/NPP)"),
    QStringLiteral("CPU (OpenCV)"),
};

// ────────────────────────────────────────────────────────────────────────────
// Construction
// ────────────────────────────────────────────────────────────────────────────

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

    buildGeneralCard(layout);
    buildUdpCard(layout);
    buildTcpCard(layout);
    buildEthCard(layout);
    buildCardCard(layout);

    layout->addStretch();

    // Load initial values from config and connect reload signal.
    auto& cfg = ConfigManager::instance();
    connect(&cfg, &ConfigManager::configLoaded, this, &CapturePage::onLoadConfig);
    onLoadConfig();
}

// ────────────────────────────────────────────────────────────────────────────
// Card 1 : General capture settings
// ────────────────────────────────────────────────────────────────────────────

void CapturePage::buildGeneralCard(QVBoxLayout* layout) {
    auto* card = new CardWidget(
        QStringLiteral("通用采集"),   // 通用采集
        QStringLiteral("device-desktop"));

    // ── Capture method ──
    m_methodCombo = new QComboBox;
    m_methodCombo->addItems(kMethodLabels);
    m_methodCombo->setToolTip(tr(
        "画面来源:\n"
        "UDP / TCP: 远端推流送过来（适合双机方案，主机拉显卡录屏推到本机）。\n"
        "采集卡（直采）: 本机插了 HDMI 采集卡，双机直采主机画面延迟最低。"));
    auto* methodRow = FormKit::fieldRow(
        QStringLiteral("采集方式"), m_methodCombo);   // 采集方式
    card->contentLayout()->addWidget(methodRow);

    connect(m_methodCombo, &QComboBox::currentIndexChanged,
            this, &CapturePage::onMethodChanged);

    // ── Detection resolution ──
    m_detResolution = new QSpinBox;
    m_detResolution->setRange(32, 2048);
    m_detResolution->setSingleStep(16);
    m_detResolution->setToolTip(tr(
        "手动输入检测图像边长，建议使用 32 的倍数。"
        "TensorRT 动态模型会按该值重建/更新输入尺寸。"));
    card->contentLayout()->addWidget(
        FormKit::fieldRow(
            QStringLiteral("检测分辨率"), m_detResolution));   // 检测分辨率

    connect(m_detResolution, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) {
                ConfigManager::instance().setDetectionResolution(v);
            });

    // ── Capture FPS ──
    QSlider* fpsSlider = nullptr;
    card->contentLayout()->addWidget(
        FormKit::sliderRow(
            QStringLiteral("采集 FPS"), 0, 240, 60,   // 采集 FPS
            fpsSlider, m_captureFps,
            QStringLiteral(" fps")));
    m_captureFps->setToolTip(tr(
        "采集源每秒抓取的帧数上限。0 = 不主动节流，跟着上游推送速度走。\n"
        "高 FPS 让瞄准更跟手但 CPU/GPU 负担大；一般和显示器刷新率持平即可。"));

    // FPS warning label (shown when 0 or >= 61)
    m_fpsWarning = new QLabel;
    m_fpsWarning->setStyleSheet(QStringLiteral("color:#CCCC00; font-size:12px;"));
    m_fpsWarning->setWordWrap(true);
    m_fpsWarning->hide();
    card->contentLayout()->addWidget(m_fpsWarning);

    auto updateFpsWarning = [this](int fps) {
        ConfigManager::instance().setCaptureFps(fps);
        if (fps == 0) {
            m_fpsWarning->setText(
                QStringLiteral("→ 已禁用。警告：FPS 过高会影响性能。"));
            m_fpsWarning->show();
        } else if (fps >= 61) {
            m_fpsWarning->setText(
                QStringLiteral("警告：FPS 过高会影响性能。"));
            m_fpsWarning->show();
        } else {
            m_fpsWarning->hide();
        }
    };
    connect(m_captureFps, QOverload<int>::of(&QSpinBox::valueChanged), this, updateFpsWarning);

    // ── Circle mask ──
    card->contentLayout()->addWidget(
        FormKit::toggleRow(
            QStringLiteral("圆形遮罩"), true, m_circleMask));   // 圆形遮罩
    m_circleMask->setToolTip(tr(
        "开启后画面边缘按圆形蒙板裁剪，只看正中心的圆形区域。\n"
        "用于排除画面四角的 UI / 准星元素干扰检测。"));
    connect(m_circleMask, &ToggleSwitch::toggled,
            this, [](bool v) { ConfigManager::instance().setCircleMask(v); });

    layout->addWidget(card);
}

// ────────────────────────────────────────────────────────────────────────────
// Card 2 : UDP
// ────────────────────────────────────────────────────────────────────────────

void CapturePage::buildUdpCard(QVBoxLayout* layout) {
    m_udpCard = new CardWidget(
        QStringLiteral("UDP 采集"),   // UDP 采集
        QStringLiteral("cloud-down"));

    m_udpIp = new QLineEdit;
    m_udpIp->setToolTip(tr(
        "监听绑定的本机 IP。0.0.0.0 = 监听所有网卡，"
        "如果对方推流找不到本机，改成本机内网 IP。"));
    m_udpCard->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("IP 地址"), m_udpIp));   // IP 地址

    m_udpPort = new QSpinBox;
    m_udpPort->setRange(1, 65535);
    m_udpPort->setToolTip(tr(
        "监听的 UDP 端口，需要和对方推流端口对应。"
        "被防火墙拦截会收不到包。"));
    m_udpCard->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("端口"), m_udpPort));   // 端口

    auto* applyBtn = new QPushButton(QStringLiteral("应用 UDP 设置"));   // 应用 UDP 设置
    m_udpCard->contentLayout()->addWidget(applyBtn);
    connect(applyBtn, &QPushButton::clicked, this, &CapturePage::applyUdp);

    layout->addWidget(m_udpCard);
}

// ────────────────────────────────────────────────────────────────────────────
// Card 3 : TCP
// ────────────────────────────────────────────────────────────────────────────

void CapturePage::buildTcpCard(QVBoxLayout* layout) {
    m_tcpCard = new CardWidget(
        QStringLiteral("TCP 采集"),   // TCP 采集
        QStringLiteral("cloud-down"));

    m_tcpIp = new QLineEdit;
    m_tcpIp->setToolTip(tr(
        "监听绑定的本机 IP（0.0.0.0 = 全部网卡）。"));
    m_tcpCard->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("IP 地址"), m_tcpIp));   // IP 地址

    m_tcpPort = new QSpinBox;
    m_tcpPort->setRange(1, 65535);
    m_tcpPort->setToolTip(tr(
        "监听的 TCP 端口，需要和对方推流端口对应。"));
    m_tcpCard->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("端口"), m_tcpPort));   // 端口

    auto* applyBtn = new QPushButton(QStringLiteral("应用 TCP 设置"));   // 应用 TCP 设置
    m_tcpCard->contentLayout()->addWidget(applyBtn);
    connect(applyBtn, &QPushButton::clicked, this, &CapturePage::applyTcp);

    layout->addWidget(m_tcpCard);
}

// ────────────────────────────────────────────────────────────────────────────
// Card 4 : Ethernet raw-frame (ProSexy)
// ────────────────────────────────────────────────────────────────────────────

void CapturePage::buildEthCard(QVBoxLayout* layout) {
    m_ethCard = new CardWidget(
        QStringLiteral("以太网原始帧采集 (ProSexy 接收端)"),
        QStringLiteral("network"));

    // ── Adapter combo + refresh button ──
    auto* adapterRow = new QWidget;
    auto* adapterRowLayout = new QHBoxLayout(adapterRow);
    adapterRowLayout->setContentsMargins(0, 0, 0, 0);
    adapterRowLayout->setSpacing(8);

    m_ethAdapterCombo = new QComboBox;
    m_ethAdapterCombo->setToolTip(tr(
        "接收 ProSexy 原始以太网帧的本机网卡（npcap 设备）。\n"
        "需先安装 Npcap 运行时。若列表为空，点下方刷新或确认 Npcap 已安装。"));
    adapterRowLayout->addWidget(m_ethAdapterCombo, 1);

    auto* refreshBtn = new QPushButton(QStringLiteral("刷新网卡"));
    adapterRowLayout->addWidget(refreshBtn);
    connect(refreshBtn, &QPushButton::clicked, this, &CapturePage::refreshEthAdapters);

    m_ethCard->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("网卡"), adapterRow));   // 网卡

    // ── EtherType ──
    m_ethEthertype = new QLineEdit;
    m_ethEthertype->setToolTip(tr(
        "须与发送端一致。ProSexy 默认 0x88B5。"));
    m_ethCard->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("EtherType (hex)"), m_ethEthertype));

    auto* applyBtn = new QPushButton(QStringLiteral("应用以太网设置"));   // 应用以太网设置
    m_ethCard->contentLayout()->addWidget(applyBtn);
    connect(applyBtn, &QPushButton::clicked, this, &CapturePage::applyEth);

    layout->addWidget(m_ethCard);
}

// ────────────────────────────────────────────────────────────────────────────
// Card 5 : Capture card (shared by opencv_capture and mf_capture)
// ────────────────────────────────────────────────────────────────────────────

void CapturePage::buildCardCard(QVBoxLayout* layout) {
    m_cardCard = new CardWidget(
        QStringLiteral("采集卡 (直采)"),   // 采集卡 (直采)
        QStringLiteral("device-camera-phone"));

    // ── Device index ──
    m_devIndex = new QSpinBox;
    m_devIndex->setRange(0, 63);
    m_devIndex->setToolTip(tr(
        "采集卡在系统中的编号（0 = 第一个视频输入）。"));
    m_cardCard->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("设备索引"), m_devIndex));   // 设备索引

    // ── Width / Height ──
    m_devWidth = new QSpinBox;
    m_devWidth->setRange(0, 7680);
    m_devWidth->setToolTip(tr(
        "采集卡输出的原始画面宽度，0 = 让设备自己决定。"));
    m_cardCard->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("原始宽度 (0=自动)"), m_devWidth));

    m_devHeight = new QSpinBox;
    m_devHeight->setRange(0, 4320);
    m_devHeight->setToolTip(tr(
        "采集卡输出的原始画面高度，0 = 让设备自己决定。"));
    m_cardCard->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("原始高度 (0=自动)"), m_devHeight));

    // ── Crop ──
    m_devCrop = new QSpinBox;
    m_devCrop->setRange(0, 2048);
    m_devCrop->setToolTip(tr(
        "从原始画面正中取出的正方形区域边长，直接送去推理（不缩放、不拉伸）。\n"
        ">0 时会驱动“检测分辨率”= 该边长；0 = 把整帧缩放到检测分辨率。"));
    m_cardCard->contentLayout()->addWidget(
        FormKit::fieldRow(
            QStringLiteral("裁切边长 (正方形, 0=不裁切)"),
            m_devCrop));

    // ── FPS ──
    m_devFps = new QSpinBox;
    m_devFps->setRange(0, 240);
    m_devFps->setToolTip(tr(
        "采集卡的目标 FPS，0 = 取设备默认。设过高会被设备截断。"));
    m_cardCard->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("采集 FPS (0=自动)"), m_devFps));

    // ── Pixel format ──
    m_devFormat = new QComboBox;
    m_devFormat->addItems(kFormatKeys);
    m_devFormat->setToolTip(tr(
        "采集卡的输出像素格式。NV12/YUY2/RGB32 为原始格式，MJPG 为压缩格式。\n"
        "需选设备实际支持的格式，否则打开失败。"));
    m_cardCard->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("像素格式"), m_devFormat));   // 像素格式

    // ── API (OpenCV only) ──
    m_devApi = new QComboBox;
    m_devApi->addItems(kApiKeys);
    m_devApi->setToolTip(tr(
        "USB 采集卡通常选 DSHOW；UVC 4K / HDMI 采集卡可试 MSMF。"));
    m_apiRow = FormKit::fieldRow(QStringLiteral("驱动接口"), m_devApi);   // 驱动接口
    m_cardCard->contentLayout()->addWidget(m_apiRow);

    // ── URL (OpenCV only) ──
    m_devUrl = new QLineEdit;
    m_devUrl->setToolTip(tr(
        "留空 = 按索引打开本地采集卡；填 RTSP/文件路径可用网络/文件源。"));
    m_urlRow = FormKit::fieldRow(QStringLiteral("URL (可选)"), m_devUrl);   // URL (可选)
    m_cardCard->contentLayout()->addWidget(m_urlRow);

    // ── Decode location (MF / AVerMedia SDK) ──
    m_devDecode = new QComboBox;
    m_devDecode->addItems(kDecodeLabels);
    m_devDecode->setToolTip(tr(
        "GPU：nvJPEG 解 MJPG、NPP 转原始格式，产出 GPU 帧直送 TensorRT，延迟最低。\n"
        "CPU：OpenCV 解码产出 cv::Mat，再由管线上传 GPU，兼容性好但更慢。"));
    m_decodeRow = FormKit::fieldRow(QStringLiteral("解码位置"), m_devDecode);   // 解码位置
    m_cardCard->contentLayout()->addWidget(m_decodeRow);

    // ── Apply button ──
    auto* applyBtn = new QPushButton(QStringLiteral("应用采集卡设置"));   // 应用采集卡设置
    m_cardCard->contentLayout()->addWidget(applyBtn);
    connect(applyBtn, &QPushButton::clicked, this, &CapturePage::applyCard);

    layout->addWidget(m_cardCard);
}

// ────────────────────────────────────────────────────────────────────────────
// Load config values into widgets
// ────────────────────────────────────────────────────────────────────────────

void CapturePage::onLoadConfig() {
    auto& cfg = ConfigManager::instance();

    // ── General ──
    const QString method = cfg.captureMethod();
    int methodIdx = kMethodKeys.indexOf(method);
    if (methodIdx < 0) methodIdx = 0;
    m_methodCombo->setCurrentIndex(methodIdx);

    m_detResolution->setValue(cfg.detectionResolution());
    m_captureFps->setValue(cfg.captureFps());
    m_circleMask->setChecked(cfg.circleMask());

    // ── UDP ──
    m_udpIp->setText(cfg.udpIp());
    m_udpPort->setValue(cfg.udpPort());

    // ── TCP ──
    m_tcpIp->setText(cfg.tcpIp());
    m_tcpPort->setValue(cfg.tcpPort());

    // ── Eth ──
    refreshEthAdapters();
    {
        QString saved = cfg.ethAdapter();
        int idx = m_ethAdapterCombo->findData(saved);
        if (idx >= 0) m_ethAdapterCombo->setCurrentIndex(idx);
    }
    m_ethEthertype->setText(
        QStringLiteral("0x%1").arg(cfg.ethEthertype(), 4, 16, QChar('0')).toUpper());

    // ── OpenCV / MF card ──
    m_devIndex->setValue(cfg.opencvCaptureIndex());
    m_devWidth->setValue(cfg.opencvCaptureWidth());
    m_devHeight->setValue(cfg.opencvCaptureHeight());
    m_devCrop->setValue(cfg.captureCrop());
    m_devFps->setValue(cfg.opencvCaptureFps());

    int fmtIdx = kFormatKeys.indexOf(cfg.captureFormat());
    if (fmtIdx < 0) fmtIdx = 0;
    m_devFormat->setCurrentIndex(fmtIdx);

    int apiIdx = kApiKeys.indexOf(cfg.opencvCaptureApi());
    if (apiIdx < 0) apiIdx = 0;
    m_devApi->setCurrentIndex(apiIdx);

    m_devUrl->setText(cfg.opencvCaptureUrl());
    m_devDecode->setCurrentIndex(cfg.captureMfGpu() ? 0 : 1);

    updateSectionVisibility();
}

// ────────────────────────────────────────────────────────────────────────────
// Visibility
// ────────────────────────────────────────────────────────────────────────────

void CapturePage::onMethodChanged(int index) {
    if (index >= 0 && index < kMethodKeys.size()) {
        ConfigManager::instance().setCaptureMethod(kMethodKeys[index]);
    }
    updateSectionVisibility();
}

void CapturePage::updateSectionVisibility() {
    const int idx = m_methodCombo->currentIndex();
    const QString key = (idx >= 0 && idx < kMethodKeys.size())
                            ? kMethodKeys[idx] : QString();

    m_udpCard->setVisible(key == QStringLiteral("udp_capture"));
    m_tcpCard->setVisible(key == QStringLiteral("tcp_capture"));
    m_ethCard->setVisible(key == QStringLiteral("eth_capture"));

    const bool isOpenCv = (key == QStringLiteral("opencv_capture"));
    const bool isMf     = (key == QStringLiteral("mf_capture"));
    const bool isAver   = (key == QStringLiteral("avermedia_capture"));
    m_cardCard->setVisible(isOpenCv || isMf || isAver);

    // Show/hide sub-rows that are backend-specific.
    m_apiRow->setVisible(isOpenCv);
    m_urlRow->setVisible(isOpenCv);
    m_decodeRow->setVisible(isMf || isAver);
}

// ────────────────────────────────────────────────────────────────────────────
// Apply buttons
// ────────────────────────────────────────────────────────────────────────────

void CapturePage::applyUdp() {
    auto& cfg = ConfigManager::instance();
    cfg.setUdpIp(m_udpIp->text().trimmed());
    int port = m_udpPort->value();
    if (port < 1) port = 1;
    if (port > 65535) port = 65535;
    cfg.setUdpPort(port);
}

void CapturePage::applyTcp() {
    auto& cfg = ConfigManager::instance();
    cfg.setTcpIp(m_tcpIp->text().trimmed());
    int port = m_tcpPort->value();
    if (port < 1) port = 1;
    if (port > 65535) port = 65535;
    cfg.setTcpPort(port);
}

void CapturePage::refreshEthAdapters() {
    QString prev;
    if (m_ethAdapterCombo->currentIndex() >= 0)
        prev = m_ethAdapterCombo->currentData().toString();

    m_ethAdapterCombo->clear();
    auto adapters = EthCapture::ListAdapters();
    for (const auto& [dev, desc] : adapters) {
        QString label = QString::fromStdString(desc);
        if (label.isEmpty()) label = QString::fromStdString(dev);
        m_ethAdapterCombo->addItem(label, QString::fromStdString(dev));
    }

    if (!prev.isEmpty()) {
        int idx = m_ethAdapterCombo->findData(prev);
        if (idx >= 0) m_ethAdapterCombo->setCurrentIndex(idx);
    }
}

void CapturePage::applyEth() {
    auto& cfg = ConfigManager::instance();

    // Parse the adapter selection.
    if (m_ethAdapterCombo->currentIndex() >= 0) {
        cfg.setEthAdapter(m_ethAdapterCombo->currentData().toString());
    }

    // Parse hex ethertype.
    bool ok = false;
    int et = m_ethEthertype->text().toInt(&ok, 16);
    if (!ok || et < 0x0600 || et > 0xFFFF)
        et = 0x88B5;
    cfg.setEthEthertype(et);
}

void CapturePage::applyCard() {
    auto& cfg = ConfigManager::instance();

    int idx = m_devIndex->value();
    if (idx < 0) idx = 0;
    cfg.setOpencvCaptureIndex(idx);

    int w = m_devWidth->value();
    if (w < 0) w = 0;
    cfg.setOpencvCaptureWidth(w);

    int h = m_devHeight->value();
    if (h < 0) h = 0;
    cfg.setOpencvCaptureHeight(h);

    int fps = m_devFps->value();
    if (fps < 0) fps = 0;
    cfg.setOpencvCaptureFps(fps);

    int crop = m_devCrop->value();
    if (crop < 0) crop = 0;
    if (crop > 0 && crop < 32) crop = 32;
    if (crop > 2048) crop = 2048;
    cfg.setCaptureCrop(crop);

    cfg.setCaptureFormat(kFormatKeys[m_devFormat->currentIndex()]);

    const QString method = kMethodKeys[m_methodCombo->currentIndex()];
    if (method == QStringLiteral("opencv_capture")) {
        cfg.setOpencvCaptureApi(kApiKeys[m_devApi->currentIndex()]);
        cfg.setOpencvCaptureUrl(m_devUrl->text().trimmed());
    } else {
        // MF / 圆刚 SDK 共用 GPU 转换开关。
        cfg.setCaptureMfGpu(m_devDecode->currentIndex() == 0);
    }

    // Square crop drives detection_resolution, matching ImGui logic.
    if (crop > 0 && cfg.detectionResolution() != crop) {
        cfg.setDetectionResolution(crop);
        m_detResolution->setValue(crop);
    }
}
