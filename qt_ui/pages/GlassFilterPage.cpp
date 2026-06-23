#include "pages/GlassFilterPage.h"

#include "Apotheosis.h"
#include "config/ConfigManager.h"
#include "config/config.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"
#include "widgets/ToggleSwitch.h"

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
        FormKit::sliderRowD(QStringLiteral("边缘环厚度"),
                            0.05, 0.45, static_cast<double>(cfg.glassEdgeRingFrac()),
                            0.01, 2,
                            m_edgeRingFracSlider, m_edgeRingFrac));
    m_edgeRingFracSlider->setToolTip(tr(
        "环厚 = 框短边 × 此值。\n"
        "0.10 ~ 0.20 推荐。太薄信号点少噪声主导;太厚吃进框中心人物,稀释命中率。"));
    m_edgeRingFrac->setToolTip(m_edgeRingFracSlider->toolTip());
    connect(m_edgeRingFrac, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [](double v) {
                ConfigManager::instance().setGlassEdgeRingFrac(static_cast<float>(v));
                std::lock_guard<std::recursive_mutex> lk(configMutex);
                config.glass_edge_ring_frac = static_cast<float>(v);
            });

    paramCard->contentLayout()->addWidget(
        FormKit::sliderRowD(QStringLiteral("命中率阈值"),
                            0.05, 0.95, static_cast<double>(cfg.glassCoverageThreshold()),
                            0.01, 2,
                            m_coverageThresholdSlider, m_coverageThreshold));
    m_coverageThresholdSlider->setToolTip(tr(
        "环内命中色带的像素 ÷ 环总像素 ≥ 此值 → 判玻璃。\n"
        "0.45 推荐。误伤多了往上调,玻璃漏过多往下调。"));
    m_coverageThreshold->setToolTip(m_coverageThresholdSlider->toolTip());
    connect(m_coverageThreshold, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [](double v) {
                ConfigManager::instance().setGlassCoverageThreshold(static_cast<float>(v));
                std::lock_guard<std::recursive_mutex> lk(configMutex);
                config.glass_coverage_threshold = static_cast<float>(v);
            });

    paramCard->contentLayout()->addWidget(
        FormKit::sliderRow(QStringLiteral("最小框短边"),
                           4, 200, cfg.glassMinBoxShortSide(),
                           m_minBoxShortSideSlider, m_minBoxShortSide));
    m_minBoxShortSideSlider->setToolTip(tr(
        "框短边小于此值不参与过滤(信号不可靠,容易把远处真目标误杀)。\n"
        "检测图像素;detection_resolution=320 时 20 ~ 30 较合理。"));
    m_minBoxShortSide->setToolTip(m_minBoxShortSideSlider->toolTip());
    connect(m_minBoxShortSide, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) {
                ConfigManager::instance().setGlassMinBoxShortSide(v);
                std::lock_guard<std::recursive_mutex> lk(configMutex);
                config.glass_min_box_short_side = v;
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
    auto* addBtn   = new QPushButton(QStringLiteral("+ 新增空白"));
    auto* delBtn   = new QPushButton(QStringLiteral("- 删除选中"));
    auto* applyBtn = new QPushButton(QStringLiteral("应用色带"));
    btnRow->addWidget(addBtn);
    btnRow->addWidget(delBtn);
    btnRow->addStretch();
    btnRow->addWidget(applyBtn);
    colorCard->contentLayout()->addLayout(btnRow);

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
    m_edgeRingFrac->setValue(static_cast<double>(cfg.glassEdgeRingFrac()));
    m_coverageThreshold->setValue(static_cast<double>(cfg.glassCoverageThreshold()));
    m_minBoxShortSide->setValue(cfg.glassMinBoxShortSide());

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
