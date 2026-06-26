#include "pages/GlassFilterPage.h"

#include "Apotheosis.h"
#include "config/ConfigManager.h"
#include "config/config.h"
#include "crosshair/color_picker.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"
#include "widgets/ToggleSwitch.h"

#include <algorithm>

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

#include <mutex>

namespace {

void insertGlassRow(QTableWidget* table,
                    const QString& name, bool enabled,
                    int hLo, int hHi, int sLo, int sHi, int vLo, int vHi) {
    int row = table->rowCount();
    table->insertRow(row);

    auto* chk = new QCheckBox;
    chk->setChecked(enabled);
    table->setCellWidget(row, 0, chk);
    QObject::connect(chk, &QCheckBox::toggled, table, [table](bool){
        // 触发外部 save 通过 cellChanged 信号:复选框路径用 itemChanged 收不到,
        // 这里通过修改一个隐藏 item 的方式触发. 简化:让外部用按钮显式 save。
        Q_UNUSED(table);
    });

    table->setItem(row, 1, new QTableWidgetItem(name));
    table->setItem(row, 2, new QTableWidgetItem(QString::number(hLo)));
    table->setItem(row, 3, new QTableWidgetItem(QString::number(hHi)));
    table->setItem(row, 4, new QTableWidgetItem(QString::number(sLo)));
    table->setItem(row, 5, new QTableWidgetItem(QString::number(sHi)));
    table->setItem(row, 6, new QTableWidgetItem(QString::number(vLo)));
    table->setItem(row, 7, new QTableWidgetItem(QString::number(vHi)));
}

} // namespace

GlassFilterPage::GlassFilterPage(QWidget* parent)
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
    // Detection params card
    // ====================================================================
    auto* paramCard = new CardWidget(
        QStringLiteral("检测参数"),
        QStringLiteral("filter"));

    paramCard->contentLayout()->addWidget(
        FormKit::toggleRow(QStringLiteral("显示预览(用于调试)"),
                           cfg.glassFilterShowPreview(), m_showPreview));
    m_showPreview->setToolTip(tr("在检测预览窗口里给每个 box 画环 —"
        "红 = 判玻璃已被剔除,绿 = 通过,灰 = 未参与判断。"));
    connect(m_showPreview, &ToggleSwitch::toggled,
            this, [](bool v) {
                ConfigManager::instance().setGlassFilterShowPreview(v);
                std::lock_guard<std::recursive_mutex> lk(configMutex);
                config.glass_filter_show_preview = v;
            });

    paramCard->contentLayout()->addWidget(
        FormKit::sliderRow(QStringLiteral("过滤强度"),
                           0, 100, cfg.glassFilterStrength(),
                           m_filterStrengthSlider, m_filterStrength));
    m_filterStrengthSlider->setToolTip(tr(
        "玻璃过滤的激进程度(0 ~ 100)。\n"
        "低 = 保守,只剔除边缘几乎全是玻璃膜色的框;高 = 激进,薄淡玻璃也判、剔得更狠。\n"
        "50 ≈ 旧默认表现。误杀真人往下调,玻璃后的人漏过太多往上调。\n"
        "(环厚已固定 0.15、最小框按分辨率自动,无需再单独调。)"));
    m_filterStrength->setToolTip(m_filterStrengthSlider->toolTip());
    connect(m_filterStrength, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) {
                ConfigManager::instance().setGlassFilterStrength(v);
                std::lock_guard<std::recursive_mutex> lk(configMutex);
                config.glass_filter_strength = v;
            });

    layout->addWidget(paramCard);

    // ====================================================================
    // Glass color palette card
    // ====================================================================
    auto* colorCard = new CardWidget(
        QStringLiteral("玻璃膜色带"),
        QStringLiteral("palette"));

    m_colorTable = new QTableWidget(0, 8, this);
    m_colorTable->setHorizontalHeaderLabels({
        QStringLiteral("启用"),
        QStringLiteral("名称"),
        QStringLiteral("H 低"), QStringLiteral("H 高"),
        QStringLiteral("S 低"), QStringLiteral("S 高"),
        QStringLiteral("V 低"), QStringLiteral("V 高"),
    });
    m_colorTable->horizontalHeader()->setStretchLastSection(true);
    m_colorTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_colorTable->setMinimumHeight(180);
    colorCard->contentLayout()->addWidget(m_colorTable);

    auto* btnRow = new QHBoxLayout;
    m_pickColorBtn = new QPushButton(QStringLiteral("取色"));
    m_pickColorBtn->setToolTip(QStringLiteral(
        "点击后切到「检测预览」窗口,在玻璃膜上单击一下:\n"
        "自动取该处 5×5 区域的 HSV(中位数)并按宽容差加一行到下面的色带"
        "(跨 0/179 接缝自动拆两行)。\n"
        "预览里右键、或再点一次本按钮可取消;预览窗口没开时会自动打开。\n"
        "取到的颜色会立即生效(自动应用)。"));
    auto* addBtn   = new QPushButton(QStringLiteral("+ 新增空白"));
    auto* delBtn   = new QPushButton(QStringLiteral("- 删除选中"));
    auto* applyBtn = new QPushButton(QStringLiteral("应用色带"));
    btnRow->addWidget(m_pickColorBtn);
    btnRow->addWidget(addBtn);
    btnRow->addWidget(delBtn);
    btnRow->addStretch();
    btnRow->addWidget(applyBtn);
    colorCard->contentLayout()->addLayout(btnRow);

    m_pickTimer = new QTimer(this);
    m_pickTimer->setInterval(120);
    connect(m_pickTimer, &QTimer::timeout, this, &GlassFilterPage::pollPickedColor);
    connect(m_pickColorBtn, &QPushButton::clicked, this, &GlassFilterPage::toggleColorPick);
    connect(addBtn,   &QPushButton::clicked, this, &GlassFilterPage::addEmptyColor);
    connect(delBtn,   &QPushButton::clicked, this, &GlassFilterPage::removeSelectedRows);
    connect(applyBtn, &QPushButton::clicked, this, &GlassFilterPage::saveGlassColors);

    layout->addWidget(colorCard);
    layout->addStretch();

    loadConfig();
    connect(&cfg, &ConfigManager::configLoaded, this, &GlassFilterPage::loadConfig);
}

void GlassFilterPage::loadConfig() {
    auto& cfg = ConfigManager::instance();
    m_showPreview->setChecked(cfg.glassFilterShowPreview());
    m_filterStrength->setValue(cfg.glassFilterStrength());

    m_colorTable->blockSignals(true);
    m_colorTable->setRowCount(0);
    auto colors = cfg.glassColors();
    for (const auto& c : colors) {
        insertGlassRow(m_colorTable, c.name, c.enabled,
                       c.hLow, c.hHigh, c.sMin, c.sMax, c.vMin, c.vMax);
    }
    m_colorTable->blockSignals(false);
}

void GlassFilterPage::saveGlassColors() {
    QList<ConfigManager::ColorProfile> colors;
    for (int row = 0; row < m_colorTable->rowCount(); ++row) {
        ConfigManager::ColorProfile c;
        auto* chk = qobject_cast<QCheckBox*>(m_colorTable->cellWidget(row, 0));
        c.enabled = chk ? chk->isChecked() : true;
        auto* nameItem = m_colorTable->item(row, 1);
        c.name = nameItem ? nameItem->text() : QStringLiteral("Glass");
        auto val = [&](int col, int def) {
            auto* item = m_colorTable->item(row, col);
            return item ? item->text().toInt() : def;
        };
        c.hLow  = val(2, 90);
        c.hHigh = val(3, 115);
        c.sMin  = val(4, 5);
        c.sMax  = val(5, 90);
        c.vMin  = val(6, 170);
        c.vMax  = val(7, 255);
        colors.append(c);
    }
    ConfigManager::instance().setGlassColors(colors);

    // Sync directly into the runtime config so the new bands take effect
    // immediately without waiting for a save/reload round-trip.
    std::lock_guard<std::recursive_mutex> lk(configMutex);
    config.glass_colors.clear();
    for (const auto& c : colors) {
        CrosshairColorProfileConfig pc;
        pc.name    = c.name.toStdString();
        pc.enabled = c.enabled;
        pc.h_low   = c.hLow;
        pc.h_high  = c.hHigh;
        pc.s_min   = c.sMin;
        pc.s_max   = c.sMax;
        pc.v_min   = c.vMin;
        pc.v_max   = c.vMax;
        config.glass_colors.push_back(pc);
    }
}

void GlassFilterPage::addEmptyColor() {
    int idx = m_colorTable->rowCount() + 1;
    insertGlassRow(m_colorTable,
                   QStringLiteral("玻璃 %1").arg(idx),
                   true,
                   90, 115, 5, 90, 170, 255);
}

void GlassFilterPage::removeSelectedRows() {
    auto ranges = m_colorTable->selectedRanges();
    for (int i = ranges.size() - 1; i >= 0; --i) {
        for (int row = ranges[i].bottomRow(); row >= ranges[i].topRow(); --row) {
            m_colorTable->removeRow(row);
        }
    }
}

// ---- Glass colour eyedropper (mirrors CrosshairPage; shares color_picker) ----

void GlassFilterPage::toggleColorPick() {
    if (m_pickToken != 0) {
        crosshair::CancelColorPick();
        finishPicking();
        return;
    }
    auto& cm = ConfigManager::instance();
    if (!cm.showWindow())
        cm.setShowWindow(true);

    m_pickToken = crosshair::ArmColorPick();
    m_pickColorBtn->setText(QStringLiteral("取消取色"));
    m_pickColorBtn->setStyleSheet(QStringLiteral("color:#D23B3B; font-weight:600;"));
    m_pickTimer->start();
}

void GlassFilterPage::pollPickedColor() {
    int h = 0, s = 0, v = 0;
    if (crosshair::TakePickedColor(m_pickToken, h, s, v)) {
        applyPickedColor(h, s, v);
        finishPicking();
    } else if (crosshair::ArmedToken() != m_pickToken) {
        // Superseded by another page's 取色, or cancelled in the preview.
        finishPicking();
    }
}

void GlassFilterPage::applyPickedColor(int h, int s, int v) {
    // "宽" tolerance: H ±15, S/V lower = sample − 70 (clamped to 0), upper 255.
    constexpr int kHueHalf = 15;
    constexpr int kSvMargin = 70;
    const int sLo = std::max(0, s - kSvMargin);
    const int vLo = std::max(0, v - kSvMargin);
    const QString base = QStringLiteral("取色 H%1 S%2 V%3").arg(h).arg(s).arg(v);

    const int lo = h - kHueHalf;
    const int hi = h + kHueHalf;

    m_colorTable->blockSignals(true);
    if (lo < 0) {
        // Hue band wraps below 0: [0, hi] + [180+lo, 179].
        insertGlassRow(m_colorTable, base + QStringLiteral(" 低"), true, 0, hi, sLo, 255, vLo, 255);
        insertGlassRow(m_colorTable, base + QStringLiteral(" 高"), true, 180 + lo, 179, sLo, 255, vLo, 255);
    } else if (hi > 179) {
        // Hue band wraps above 179: [0, hi-180] + [lo, 179].
        insertGlassRow(m_colorTable, base + QStringLiteral(" 低"), true, 0, hi - 180, sLo, 255, vLo, 255);
        insertGlassRow(m_colorTable, base + QStringLiteral(" 高"), true, lo, 179, sLo, 255, vLo, 255);
    } else {
        insertGlassRow(m_colorTable, base, true, lo, hi, sLo, 255, vLo, 255);
    }
    m_colorTable->blockSignals(false);

    // Apply immediately so the picked band takes effect without also having to
    // click 应用色带 (easy to forget in the pick workflow).
    saveGlassColors();
}

void GlassFilterPage::finishPicking() {
    m_pickToken = 0;
    if (m_pickTimer)
        m_pickTimer->stop();
    m_pickColorBtn->setText(QStringLiteral("取色"));
    m_pickColorBtn->setStyleSheet(QString());
}
