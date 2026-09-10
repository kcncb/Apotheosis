#include "pages/CapturePage.h"
#include "config/ConfigManager.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"
#include "widgets/ToggleSwitch.h"

#include "capture/capture_card_probe.h"

#include <QComboBox>
#include <QLabel>
#include <QPoint>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

// ────────────────────────────────────────────────────────────────────────────
// 采集设置页 —— 只有一种采集方式: 采集卡
// ────────────────────────────────────────────────────────────────────────────
//
// 【三级联动的全部意义】: 下拉里只出现设备真实支持的组合。
//
// 例如某张卡的 NV12 只到 1080p60, 而 MJPG 能到 1080p240:
//   选 NV12 -> 分辨率有 1080p, 但帧率下拉里【不会有 240】
//   选 MJPG -> 帧率下拉里才出现 240
//
// 这和"让用户手填宽/高/fps, 对不上就静默降级"是本质区别: 后者会让用户以为
// 自己跑在 1080p240, 实际可能跑在 720p60, 而手上只有"手感不对"这一条线索。
//
// 【全程无回退】:
//   - 上次选的卡没插 -> 不选任何卡 + 提示, 绝不自动换一张
//   - 配置里的组合失效 -> 提示 + 让用户重选, 绝不"吸附"到别的模式
//   - 采集时协商失败 -> 采集线程直接报错退出, 绝不换模式继续跑

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
    buildCardCard(layout);

    layout->addStretch();

    auto& cfg = ConfigManager::instance();
    connect(&cfg, &ConfigManager::configLoaded, this, &CapturePage::onLoadConfig);
    onLoadConfig();
}

// ────────────────────────────────────────────────────────────────────────────
// 通用采集
// ────────────────────────────────────────────────────────────────────────────

void CapturePage::buildGeneralCard(QVBoxLayout* layout) {
    auto* card = new CardWidget(
        QStringLiteral("通用采集"),
        QStringLiteral("device-desktop"));

    m_detResolution = new QSpinBox;
    m_detResolution->setRange(32, 2048);
    m_detResolution->setToolTip(tr(
        "模型输入边长。\n"
        "中心裁切区域【恒等于】该值 —— 送进检测器的永远正好是模型要的尺寸,\n"
        "不多裁也不少裁再缩, 省掉一次缩放, 同时避免裁切尺寸与模型尺寸不一致\n"
        "导致检测框和鼠标坐标空间错位。"));
    card->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("检测分辨率"), m_detResolution));
    connect(m_detResolution, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { ConfigManager::instance().setDetectionResolution(v); });

    m_circleMask = new ToggleSwitch;
    m_circleMask->setToolTip(tr("把检测区域限制在画面中心的圆形内, 屏蔽四角干扰。"));
    card->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("圆形遮罩"), m_circleMask));
    connect(m_circleMask, &ToggleSwitch::toggled,
            this, [](bool on) { ConfigManager::instance().setCircleMask(on); });

    layout->addWidget(card);
}

// ────────────────────────────────────────────────────────────────────────────
// 采集卡
// ────────────────────────────────────────────────────────────────────────────

void CapturePage::buildCardCard(QVBoxLayout* layout) {
    m_cardCard = new CardWidget(
        QStringLiteral("采集卡"),
        QStringLiteral("device-camera-phone"));

    m_error = new QLabel;
    m_error->setWordWrap(true);
    m_error->setStyleSheet(QStringLiteral("color:#ff6b6b;"));
    m_cardCard->contentLayout()->addWidget(m_error);
    m_error->hide();

    // ── 设备 ──
    m_devCombo = new QComboBox;
    m_devCombo->setToolTip(tr(
        "系统实际枚举到的视频采集卡。\n"
        "只有一张卡也请显式选择 —— 你选的那张不在时程序不会自动换一张。"));
    m_cardCard->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("采集卡"), m_devCombo));
    connect(m_devCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CapturePage::onDeviceChanged);

    m_refreshBtn = new QPushButton(QStringLiteral("重新探测采集卡能力"));
    m_refreshBtn->setToolTip(tr(
        "重新枚举设备, 并读取每张卡真实支持的 格式 / 分辨率 / 帧率。\n"
        "每张卡需要 20-300ms, 所以只在打开本页或手动点击时执行。"));
    m_cardCard->contentLayout()->addWidget(m_refreshBtn);
    connect(m_refreshBtn, &QPushButton::clicked, this, &CapturePage::refreshDevices);

    // ── 像素格式 ──
    m_fmtCombo = new QComboBox;
    m_fmtCombo->setToolTip(tr(
        "当前采集卡真实支持的像素格式, 按延迟从低到高排列。\n"
        "NV12 / YUY2 / RGB32 是未压缩格式, 没有编解码往返, 延迟最低;\n"
        "MJPG 需要卡内编码 + 进程内解码, 但在高分辨率高帧率下更省带宽。"));
    m_cardCard->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("像素格式"), m_fmtCombo));
    connect(m_fmtCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CapturePage::onFormatChanged);

    // ── 分辨率 ──
    m_resCombo = new QComboBox;
    m_resCombo->setToolTip(tr(
        "当前格式下设备真实支持的分辨率。换成别的格式, 这里的可选项会跟着变。"));
    m_cardCard->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("分辨率"), m_resCombo));
    connect(m_resCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CapturePage::onResolutionChanged);

    // ── 帧率 ──
    m_fpsCombo = new QComboBox;
    m_fpsCombo->setToolTip(tr(
        "当前 格式 + 分辨率 下设备真实支持的帧率。\n"
        "换个格式或分辨率, 这里的可选项会跟着变 —— 不支持的帧率不会出现。"));
    m_cardCard->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("帧率"), m_fpsCombo));
    connect(m_fpsCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CapturePage::onFpsChanged);

    // ── GPU 解码 ──
    m_gpuDecode = new ToggleSwitch;
    m_gpuDecode->setToolTip(tr(
        "MJPG 用 nvJPEG 在 GPU 上解码, 原始格式用 NPP / CUDA kernel 转换,\n"
        "产出 GPU 帧直送 TensorRT, 延迟最低。"));
    m_cardCard->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("GPU 解码"), m_gpuDecode));
    connect(m_gpuDecode, &ToggleSwitch::toggled,
            this, [](bool on) { ConfigManager::instance().setCaptureGpuDecode(on); });

    // ── 设备真实能力 (只读) ──
    m_capSummary = new QLabel;
    m_capSummary->setWordWrap(true);
    m_capSummary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_cardCard->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("设备能力"), m_capSummary));

    // ── 推荐配置 (只读) ──
    m_recommend = new QLabel;
    m_recommend->setWordWrap(true);
    m_recommend->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_cardCard->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("推荐配置"), m_recommend));

    layout->addWidget(m_cardCard);
}

const MFDeviceInfo* CapturePage::currentDevice() const {
    const int i = m_devCombo->currentIndex();
    if (i < 0 || i >= static_cast<int>(m_devices.size()))
        return nullptr;
    return &m_devices[static_cast<size_t>(i)];
}

void CapturePage::showError(const QString& text) {
    m_error->setText(text);
    m_error->setVisible(!text.isEmpty());
}

void CapturePage::clearError() { showError(QString()); }

void CapturePage::refreshDevices() {
    // 按【名字】记住用户的选择 —— index 会随插拔顺序变化, 名字不会。
    QString want = m_devCombo->currentIndex() >= 0
        ? m_devCombo->currentData().toString()
        : ConfigManager::instance().captureDevice();
    if (want.isEmpty())
        want = ConfigManager::instance().captureDevice();

    m_devices = capture_card::ProbeAll();

    {
        QSignalBlocker block(m_devCombo);
        m_devCombo->clear();
        for (const auto& d : m_devices)
            m_devCombo->addItem(QString::fromStdString(d.name),
                                QString::fromStdString(d.friendly_name));
    }

    if (m_devices.empty()) {
        showError(QStringLiteral(
            "没有检测到任何视频采集设备。请确认采集卡已经插好、驱动正常。"));
        m_devCombo->setCurrentIndex(-1);
        rebuildFormatCombo();
        return;
    }

    // 还原用户的选择。找不到就【不选任何卡】并把话说清楚 —— 绝不自动
    // 切到另一张卡上, 否则用户会对着另一张卡的画面调半天参数。
    const int idx = m_devCombo->findData(want);
    if (want.isEmpty()) {
        m_devCombo->setCurrentIndex(-1);
        clearError();
    } else if (idx < 0) {
        m_devCombo->setCurrentIndex(-1);
        showError(QStringLiteral(
            "上次选择的采集卡「%1」当前不在线。请重新选择一张卡。")
            .arg(want));
    } else {
        m_devCombo->setCurrentIndex(idx);
        clearError();
    }

    rebuildFormatCombo();
}

void CapturePage::onDeviceChanged(int) {
    rebuildFormatCombo();
}

// 第一级: 该设备真实支持的像素格式, 未压缩在前。
void CapturePage::rebuildFormatCombo() {
    const MFDeviceInfo* dev = currentDevice();

    {
        QSignalBlocker b1(m_fmtCombo), b2(m_resCombo), b3(m_fpsCombo);
        m_fmtCombo->clear();
        m_resCombo->clear();
        m_fpsCombo->clear();
    }

    if (!dev) {
        updateCapabilitySummary();
        return;
    }

    for (const auto& f : mfcap::Formats(*dev))
        m_fmtCombo->addItem(QString::fromStdString(f));

    // 还原配置里的格式。该格式在这张卡上不存在时保持未选中 —— 让用户在
    // 可见的选项里自己挑, 而不是替他决定。
    const QString want = ConfigManager::instance().captureFormat();
    const int fi = m_fmtCombo->findText(want);
    m_fmtCombo->setCurrentIndex(fi);

    if (fi < 0 && !want.isEmpty() && m_fmtCombo->count() > 0) {
        showError(QStringLiteral(
            "采集卡不支持配置里的像素格式「%1」, 请从下面的选项里重新选择。")
            .arg(want));
    } else if (fi >= 0) {
        clearError();
    }

    rebuildResolutionCombo();
}

void CapturePage::onFormatChanged(int) { rebuildResolutionCombo(); }

// 第二级: 该格式下设备真实支持的分辨率。
// 换成别的格式后, 这里会重新构建 —— 这就是联动的核心。
void CapturePage::rebuildResolutionCombo() {
    const MFDeviceInfo* dev = currentDevice();
    const QString fmt = m_fmtCombo->currentIndex() >= 0 ? m_fmtCombo->currentText()
                                                       : QString();

    {
        QSignalBlocker b2(m_resCombo), b3(m_fpsCombo);
        m_resCombo->clear();
        m_fpsCombo->clear();
    }

    if (!dev || fmt.isEmpty()) {
        updateCapabilitySummary();
        return;
    }

    const auto res = mfcap::Resolutions(*dev, fmt.toStdString());
    for (const auto& wh : res)
        m_resCombo->addItem(QStringLiteral("%1 × %2").arg(wh.first).arg(wh.second),
                            QPoint(wh.first, wh.second));

    const auto& cfg = ConfigManager::instance();
    int ri = -1;
    for (int i = 0; i < m_resCombo->count(); ++i) {
        const QPoint p = m_resCombo->itemData(i).toPoint();
        if (p.x() == cfg.captureWidth() && p.y() == cfg.captureHeight()) { ri = i; break; }
    }
    m_resCombo->setCurrentIndex(ri);
    if (ri < 0 && m_resCombo->count() > 0)
        m_resCombo->setCurrentIndex(0);   // 换格式后旧分辨率必然失效, 落到最大档

    rebuildFpsCombo();
}

void CapturePage::onResolutionChanged(int) { rebuildFpsCombo(); }

// 第三级: 该 格式+分辨率 下设备真实支持的帧率。
// 用户在 NV12 下选 1080p 时, 这里【不会】出现 240 —— 如果这块卡只支持到 60。
void CapturePage::rebuildFpsCombo() {
    const MFDeviceInfo* dev = currentDevice();
    const QString fmt = m_fmtCombo->currentIndex() >= 0 ? m_fmtCombo->currentText()
                                                       : QString();
    const QPoint res = m_resCombo->currentIndex() >= 0
        ? m_resCombo->currentData().toPoint() : QPoint(0, 0);

    {
        QSignalBlocker b3(m_fpsCombo);
        m_fpsCombo->clear();
    }

    if (!dev || fmt.isEmpty() || res.x() <= 0) {
        updateCapabilitySummary();
        return;
    }

    for (int f : mfcap::FpsList(*dev, fmt.toStdString(), res.x(), res.y()))
        m_fpsCombo->addItem(QStringLiteral("%1 fps").arg(f), f);

    const int want = ConfigManager::instance().captureFps();
    int fi = m_fpsCombo->findData(want);
    if (fi < 0 && m_fpsCombo->count() > 0)
        fi = m_fpsCombo->count() - 1;   // 取该组合下最高帧率
    m_fpsCombo->setCurrentIndex(fi);

    // 只有三级都选定之后, 这个组合才是"可提交"的。
    if (fi >= 0)
        applySelectionToConfig();

    updateCapabilitySummary();
}

void CapturePage::onFpsChanged(int) {
    applySelectionToConfig();
}

// 把当前三级下拉的选择写回配置。任何一级没选中就不写, 由 UI 的提示承担。
void CapturePage::applySelectionToConfig() {
    const MFDeviceInfo* dev = currentDevice();
    if (!dev) return;
    if (m_fmtCombo->currentIndex() < 0) return;
    if (m_resCombo->currentIndex() < 0) return;
    if (m_fpsCombo->currentIndex() < 0) return;

    const QString fmt = m_fmtCombo->currentText();
    const QPoint res = m_resCombo->currentData().toPoint();
    const int fps = m_fpsCombo->currentData().toInt();

    // 提交前再校验一次: 只有设备真的支持这个组合才写进配置。
    // 采集侧同样会严格校验一次, 两层都不放行任何"差不多"的组合。
    if (!mfcap::Validate(*dev, fmt.toStdString(), res.x(), res.y(), fps)) {
        showError(QStringLiteral(
            "内部错误: 选中的组合 %1 %2×%3@%4fps 未通过设备能力校验。")
            .arg(fmt).arg(res.x()).arg(res.y()).arg(fps));
        return;
    }

    auto& cfg = ConfigManager::instance();
    cfg.setCaptureDevice(m_devCombo->currentData().toString());
    cfg.setCaptureFormat(fmt);
    cfg.setCaptureWidth(res.x());
    cfg.setCaptureHeight(res.y());
    cfg.setCaptureFps(fps);
    clearError();
}

void CapturePage::updateCapabilitySummary() {
    const MFDeviceInfo* dev = currentDevice();

    if (!dev) {
        m_capSummary->setText(QStringLiteral("—"));
        m_recommend->setText(QStringLiteral("—"));
        return;
    }

    // 完整能力表, 原样展示。H264 之类本程序不吃的格式也会列出来并标注,
    // 这样用户看到"我的卡明明支持 4K"时不会以为程序探测错了。
    m_capSummary->setText(QString::fromStdString(mfcap::Describe(*dev)));

    // 推荐配置 + 理由。推荐用模型输入尺寸作为目标分辨率 —— 裁切跟着模型走,
    // 所以采集尺寸越接近模型输入越好。
    const auto& cfg = ConfigManager::instance();
    const int side = cfg.detectionResolution();

    std::string rf, why;
    int rw = 0, rh = 0, rffps = 0;
    if (mfcap::PickBest(*dev, side, side, 240, rf, rw, rh, rffps, &why))
        m_recommend->setText(QString::fromStdString(why));
    else
        m_recommend->setText(QStringLiteral("该设备没有可用于采集的组合。"));
}

// ────────────────────────────────────────────────────────────────────────────
// 载入配置
// ────────────────────────────────────────────────────────────────────────────

void CapturePage::onLoadConfig() {
    auto& cfg = ConfigManager::instance();

    m_detResolution->setValue(cfg.detectionResolution());
    m_circleMask->setChecked(cfg.circleMask());
    m_gpuDecode->setChecked(cfg.captureGpuDecode());

    // 探测 + 还原三级选择。refreshDevices 内部会按名字找回配置里的设备,
    // 找不到就明确报错 —— 不会退到第 0 张卡。
    refreshDevices();
    updateCapabilitySummary();
}
