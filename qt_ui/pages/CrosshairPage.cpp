#include "pages/CrosshairPage.h"
#include "config/ConfigManager.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"

#include "crosshair/color_picker.h"

#include <algorithm>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace {

class SpinBoxDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem&,
                          const QModelIndex& index) const override {
        auto* editor = new QSpinBox(parent);
        editor->setFrame(false);
        int col = index.column();
        if (col == 2 || col == 3) {
            editor->setRange(0, 179);
        } else {
            editor->setRange(0, 255);
        }
        return editor;
    }

    void setEditorData(QWidget* editor, const QModelIndex& index) const override {
        auto* spin = qobject_cast<QSpinBox*>(editor);
        spin->setValue(index.data(Qt::EditRole).toInt());
    }

    void setModelData(QWidget* editor, QAbstractItemModel* model,
                      const QModelIndex& index) const override {
        auto* spin = qobject_cast<QSpinBox*>(editor);
        spin->interpretText();
        model->setData(index, spin->value(), Qt::EditRole);
    }
};

// Color presets matching the ImGui version
struct ColorPreset {
    const char* label;
    QString nameA;
    int hLoA, hHiA;
    bool hasSecondary;
    QString nameB;
    int hLoB, hHiB;
};

const ColorPreset kPresets[] = {
    { "\xe7\xba\xa2\xe8\x89\xb2\xef\xbc\x88\xe5\x8f\x8c\xe5\x8c\xba\xe9\x97\xb4\xef\xbc\x89",   // 红色（双区间）
      QStringLiteral("Red-Low"), 0, 10, true, QStringLiteral("Red-High"), 160, 179 },
    { "\xe7\xbb\xbf\xe8\x89\xb2",                                                                   // 绿色
      QStringLiteral("Green"), 40, 85, false, QString(), 0, 0 },
    { "\xe9\x9d\x92\xe8\x89\xb2",                                                                   // 青色
      QStringLiteral("Cyan"), 85, 100, false, QString(), 0, 0 },
    { "\xe7\xb4\xab\xe8\x89\xb2",                                                                   // 紫色
      QStringLiteral("Purple"), 125, 155, false, QString(), 0, 0 },
    { "\xe9\xbb\x84\xe8\x89\xb2",                                                                   // 黄色
      QStringLiteral("Yellow"), 20, 35, false, QString(), 0, 0 },
};
constexpr int kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);

QTableWidget* createColorTable(QWidget* parent) {
    auto* table = new QTableWidget(0, 8, parent);
    table->setHorizontalHeaderLabels({
        QStringLiteral("\xe5\x90\xaf\xe7\x94\xa8"),     // 启用
        QStringLiteral("\xe5\x90\x8d\xe7\xa7\xb0"),     // 名称
        QStringLiteral("H \xe4\xbd\x8e"),               // H 低
        QStringLiteral("H \xe9\xab\x98"),               // H 高
        QStringLiteral("S \xe4\xbd\x8e"),               // S 低
        QStringLiteral("S \xe9\xab\x98"),               // S 高
        QStringLiteral("V \xe4\xbd\x8e"),               // V 低
        QStringLiteral("V \xe9\xab\x98"),               // V 高
    });
    table->horizontalHeader()->setStretchLastSection(true);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);

    auto* delegate = new SpinBoxDelegate(table);
    for (int col = 2; col <= 7; ++col) {
        table->setItemDelegateForColumn(col, delegate);
    }

    return table;
}

void insertColorRow(QTableWidget* table, const QString& name, bool enabled,
                    int hLo, int hHi, int sLo, int sHi, int vLo, int vHi) {
    int row = table->rowCount();
    table->insertRow(row);

    auto* chk = new QCheckBox;
    chk->setChecked(enabled);
    table->setCellWidget(row, 0, chk);

    table->setItem(row, 1, new QTableWidgetItem(name));
    table->setItem(row, 2, new QTableWidgetItem(QString::number(hLo)));
    table->setItem(row, 3, new QTableWidgetItem(QString::number(hHi)));
    table->setItem(row, 4, new QTableWidgetItem(QString::number(sLo)));
    table->setItem(row, 5, new QTableWidgetItem(QString::number(sHi)));
    table->setItem(row, 6, new QTableWidgetItem(QString::number(vLo)));
    table->setItem(row, 7, new QTableWidgetItem(QString::number(vHi)));
}

QComboBox* createPresetCombo(QWidget* parent) {
    auto* combo = new QComboBox(parent);
    for (int i = 0; i < kPresetCount; ++i)
        combo->addItem(QString::fromUtf8(kPresets[i].label));
    return combo;
}

} // namespace

CrosshairPage::CrosshairPage(QWidget* parent)
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
    // Card 1: 取样区域
    // ====================================================================
    auto* regionCard = new CardWidget(
        QStringLiteral("取样区域"),
        QStringLiteral("color-swatch"));

    // 宽度（像素）
    QSlider* wSlider = nullptr;
    regionCard->contentLayout()->addWidget(
        FormKit::sliderRow(
            QStringLiteral("宽度（px）"),
            4, 256, cfg.crosshairRectW(), wSlider, m_rectW));
    connect(m_rectW, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { ConfigManager::instance().setCrosshairRectW(v); });

    // 高度（像素）
    QSlider* hSlider = nullptr;
    regionCard->contentLayout()->addWidget(
        FormKit::sliderRow(
            QStringLiteral("高度（px）"),
            4, 256, cfg.crosshairRectH(), hSlider, m_rectH));
    connect(m_rectH, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { ConfigManager::instance().setCrosshairRectH(v); });

    // 取色
    auto* pickRow = new QHBoxLayout;
    m_pickColorBtn = new QPushButton(QStringLiteral("屏幕取色"));
    pickRow->addWidget(m_pickColorBtn);
    pickRow->addStretch();
    regionCard->contentLayout()->addLayout(pickRow);

    m_pickTimer = new QTimer(this);
    m_pickTimer->setInterval(120);
    connect(m_pickTimer, &QTimer::timeout, this, &CrosshairPage::pollPickedColor);
    connect(m_pickColorBtn, &QPushButton::clicked, this, &CrosshairPage::toggleColorPick);

    layout->addWidget(regionCard);

    // ====================================================================
    // Card 2: 准星颜色
    // ====================================================================
    auto* colorCard = new CardWidget(
        QStringLiteral("准星颜色"),
        QStringLiteral("palette"));

    m_colorTable = createColorTable(this);
    colorCard->contentLayout()->addWidget(m_colorTable);

    auto* btnRow = new QHBoxLayout;
    m_addColorBtn = new QPushButton(QStringLiteral("添加颜色"));
    m_removeColorBtn = new QPushButton(QStringLiteral("删除选中"));
    m_presetCombo = createPresetCombo(this);
    auto* addPresetBtn = new QPushButton(QStringLiteral("应用预设"));
    btnRow->addWidget(m_addColorBtn);
    btnRow->addWidget(m_removeColorBtn);
    btnRow->addWidget(m_presetCombo);
    btnRow->addWidget(addPresetBtn);
    btnRow->addStretch();
    colorCard->contentLayout()->addLayout(btnRow);

    layout->addWidget(colorCard);

    connect(m_addColorBtn, &QPushButton::clicked,
            this, &CrosshairPage::addEmptyCrosshairColor);
    connect(m_removeColorBtn, &QPushButton::clicked,
            this, &CrosshairPage::removeCrosshairSelectedRows);
    connect(addPresetBtn, &QPushButton::clicked,
            this, &CrosshairPage::addCrosshairPreset);
    connect(m_colorTable, &QTableWidget::cellChanged,
            this, &CrosshairPage::saveCrosshairColors);

    // ====================================================================
    // Card 3: 找色参数
    // ====================================================================
    auto* shapeCard = new CardWidget(
        QStringLiteral("找色参数"),
        QStringLiteral("target"));

    // 最小像素数
    QSlider* mpSlider = nullptr;
    shapeCard->contentLayout()->addWidget(
        FormKit::sliderRow(
            QStringLiteral("最小像素阈值"),
            1, 200, cfg.crosshairMinPixelCount(), mpSlider, m_minPixels));
    connect(m_minPixels, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { ConfigManager::instance().setCrosshairMinPixelCount(v); });

    // 闭合半径
    QSlider* crSlider = nullptr;
    shapeCard->contentLayout()->addWidget(
        FormKit::sliderRow(
            QStringLiteral("闭合滤波半径"),
            0, 7, cfg.crosshairCloseRadius(), crSlider, m_closeRadius));
    connect(m_closeRadius, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { ConfigManager::instance().setCrosshairCloseRadius(v); });

    // 平滑防抖强度
    shapeCard->contentLayout()->addWidget(
        FormKit::sliderRowD(
            QStringLiteral("平滑防抖强度"),
            0.0, 1.0, static_cast<double>(cfg.crosshairSmooth()), 0.01, 2,
            m_smoothSlider, m_smoothSpin));
    connect(m_smoothSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [](double v) { ConfigManager::instance().setCrosshairSmooth(static_cast<float>(v)); });

    layout->addWidget(shapeCard);

    layout->addStretch();

    // Load saved config into widgets
    loadConfig();
    connect(&cfg, &ConfigManager::configLoaded, this, &CrosshairPage::loadConfig);
}

void CrosshairPage::loadConfig() {
    auto& cfg = ConfigManager::instance();

    m_rectW->setValue(cfg.crosshairRectW());
    m_rectH->setValue(cfg.crosshairRectH());
    m_minPixels->setValue(cfg.crosshairMinPixelCount());
    m_closeRadius->setValue(cfg.crosshairCloseRadius());
    m_smoothSpin->setValue(static_cast<double>(cfg.crosshairSmooth()));

    // Load crosshair color table
    m_colorTable->blockSignals(true);
    m_colorTable->setRowCount(0);
    auto colors = cfg.crosshairColors();
    for (const auto& c : colors) {
        insertColorRow(m_colorTable, c.name, c.enabled,
                       c.hLow, c.hHigh, c.sMin, c.sMax, c.vMin, c.vMax);
    }
    m_colorTable->blockSignals(false);

}

// ---- Crosshair color helpers ----

void CrosshairPage::addCrosshairColorRow(const QString& name, bool enabled,
                                          int hLo, int hHi, int sLo, int sHi,
                                          int vLo, int vHi) {
    insertColorRow(m_colorTable, name, enabled, hLo, hHi, sLo, sHi, vLo, vHi);
    saveCrosshairColors();
}

void CrosshairPage::addEmptyCrosshairColor() {
    auto idx = m_colorTable->rowCount() + 1;
    addCrosshairColorRow(
        QStringLiteral("\xe9\xa2\x9c\xe8\x89\xb2 %1").arg(idx),  // 颜色 N
        true, 0, 10, 120, 255, 120, 255);
}

void CrosshairPage::removeCrosshairSelectedRows() {
    auto ranges = m_colorTable->selectedRanges();
    for (int i = ranges.size() - 1; i >= 0; --i) {
        for (int row = ranges[i].bottomRow(); row >= ranges[i].topRow(); --row) {
            m_colorTable->removeRow(row);
        }
    }
    saveCrosshairColors();
}

void CrosshairPage::addCrosshairPreset() {
    int idx = m_presetCombo->currentIndex();
    if (idx < 0 || idx >= kPresetCount) return;
    const auto& p = kPresets[idx];

    m_colorTable->blockSignals(true);
    insertColorRow(m_colorTable, p.nameA, true,
                   p.hLoA, p.hHiA, 120, 255, 120, 255);
    if (p.hasSecondary) {
        insertColorRow(m_colorTable, p.nameB, true,
                       p.hLoB, p.hHiB, 120, 255, 120, 255);
    }
    m_colorTable->blockSignals(false);
    saveCrosshairColors();
}

void CrosshairPage::saveCrosshairColors() {
    QList<ConfigManager::ColorProfile> colors;
    for (int row = 0; row < m_colorTable->rowCount(); ++row) {
        ConfigManager::ColorProfile c;
        auto* chk = qobject_cast<QCheckBox*>(m_colorTable->cellWidget(row, 0));
        c.enabled = chk ? chk->isChecked() : true;
        auto* nameItem = m_colorTable->item(row, 1);
        c.name = nameItem ? nameItem->text() : QStringLiteral("Color");
        auto val = [&](int col, int def) {
            auto* item = m_colorTable->item(row, col);
            return item ? item->text().toInt() : def;
        };
        c.hLow  = val(2, 0);
        c.hHigh = val(3, 10);
        c.sMin  = val(4, 120);
        c.sMax  = val(5, 255);
        c.vMin  = val(6, 120);
        c.vMax  = val(7, 255);
        colors.append(c);
    }
    ConfigManager::instance().setCrosshairColors(colors);
}

// ---- Crosshair colour eyedropper ----

void CrosshairPage::toggleColorPick() {
    if (m_pickToken != 0) {
        crosshair::CancelColorPick();
        finishPicking();
        return;
    }
    // Need the OpenCV "检测预览" window open to click on; open it if the user
    // hasn't. (Left on afterward — they can toggle it off on the 会话/Debug page.)
    auto& cm = ConfigManager::instance();
    if (!cm.showWindow())
        cm.setShowWindow(true);

    m_pickToken = crosshair::ArmColorPick(0);
    m_pickColorBtn->setText(QStringLiteral("取消取色"));
    m_pickColorBtn->setStyleSheet(QStringLiteral("color:#D23B3B; font-weight:600;"));
    m_pickTimer->start();
}

void CrosshairPage::pollPickedColor() {
    int h = 0, s = 0, v = 0;
    if (crosshair::TakePickedColor(m_pickToken, h, s, v)) {
        applyPickedColor(h, s, v);
        finishPicking();
    } else if (crosshair::ArmedToken() != m_pickToken) {
        // Superseded by another page's 取色, or cancelled in the preview.
        finishPicking();
    }
}

void CrosshairPage::applyPickedColor(int h, int s, int v) {
    // "宽" tolerance, per the agreed design: H ±15, S/V lower = sample − 70
    // (clamped to 0), upper = 255.
    constexpr int kHueHalf = 15;
    constexpr int kSvMargin = 70;
    const int sLo = std::max(0, s - kSvMargin);
    const int vLo = std::max(0, v - kSvMargin);
    const QString base = QStringLiteral("取色 H%1 S%2 V%3").arg(h).arg(s).arg(v);

    const int lo = h - kHueHalf;
    const int hi = h + kHueHalf;

    m_colorTable->blockSignals(true);
    if (lo < 0) {
        // Hue band wraps below 0 (reddish): [0, hi] + [180+lo, 179].
        insertColorRow(m_colorTable, base + QStringLiteral(" 低"), true,
                       0, hi, sLo, 255, vLo, 255);
        insertColorRow(m_colorTable, base + QStringLiteral(" 高"), true,
                       180 + lo, 179, sLo, 255, vLo, 255);
    } else if (hi > 179) {
        // Hue band wraps above 179 (reddish): [0, hi-180] + [lo, 179].
        insertColorRow(m_colorTable, base + QStringLiteral(" 低"), true,
                       0, hi - 180, sLo, 255, vLo, 255);
        insertColorRow(m_colorTable, base + QStringLiteral(" 高"), true,
                       lo, 179, sLo, 255, vLo, 255);
    } else {
        insertColorRow(m_colorTable, base, true, lo, hi, sLo, 255, vLo, 255);
    }
    m_colorTable->blockSignals(false);
    saveCrosshairColors();
}

void CrosshairPage::finishPicking() {
    m_pickToken = 0;
    if (m_pickTimer)
        m_pickTimer->stop();
    m_pickColorBtn->setText(QStringLiteral("取色"));
    m_pickColorBtn->setStyleSheet(QString());
}
