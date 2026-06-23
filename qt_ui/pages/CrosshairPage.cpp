#include "pages/CrosshairPage.h"
#include "config/ConfigManager.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"

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
    // Card 0: 说明
    // ====================================================================
    auto* introCard = new CardWidget(
        QStringLiteral("\xe8\xaf\xb4\xe6\x98\x8e"),       // 说明
        QStringLiteral("info"));
    auto* introLabel = new QLabel(tr(
        "\xe5\x87\x86\xe6\x98\x9f\xe6\x89\xbe\xe8\x89\xb2\xe4\xbc\x9a\xe6\x8a\x8a\xe7\x94\xbb\xe9\x9d\xa2\xe4\xb8\xad\xe5\xbf\x83\xe5\x8f\x96\xe6\xa0\xb7\xe5\x8c\xba\xe5\x9f\x9f\xe4\xbb\x8e BGR \xe8\xbd\xac\xe4\xb8\xba HSV\xef\xbc\x8c\xe5\xb9\xb6\xe5\x8c\xb9\xe9\x85\x8d\xe4\xb8\x8b\xe9\x9d\xa2\xe9\x85\x8d\xe7\xbd\xae\xe7\x9a\x84\xe9\xa2\x9c\xe8\x89\xb2\xe5\x8c\xba\xe9\x97\xb4\xe3\x80\x82\n"
        "\xe6\xaf\x8f\xe4\xb8\xaa\xe7\x9e\x84\xe5\x87\x86\xe7\x83\xad\xe9\x94\xae\xe5\x8f\xaf\xe5\x8d\x95\xe7\x8b\xac\xe6\x8e\xa7\xe5\x88\xb6\xe6\x98\xaf\xe5\x90\xa6\xe5\x90\xaf\xe7\x94\xa8\xef\xbc\x9b\xe6\x9c\xac\xe9\xa1\xb5\xe7\x94\xa8\xe4\xba\x8e\xe9\x85\x8d\xe7\xbd\xae\xe5\x8f\x96\xe6\xa0\xb7\xe5\x8c\xba\xe5\x9f\x9f\xe3\x80\x81\xe9\xa2\x9c\xe8\x89\xb2\xe5\x92\x8c\xe9\x9d\xa2\xe7\xa7\xaf\xe8\xbf\x87\xe6\xbb\xa4\xe3\x80\x82"));
    // 准星找色会把画面中心取样区域从 BGR 转为 HSV，并匹配下面配置的颜色区间。
    // 每个瞄准热键可单独控制是否启用；本页用于配置取样区域、颜色和面积过滤。
    introLabel->setWordWrap(true);
    introCard->contentLayout()->addWidget(introLabel);
    layout->addWidget(introCard);

    // ====================================================================
    // Card 1: 取样区域
    // ====================================================================
    auto* regionCard = new CardWidget(
        QStringLiteral("\xe5\x8f\x96\xe6\xa0\xb7\xe5\x8c\xba\xe5\x9f\x9f"),  // 取样区域
        QStringLiteral("color-swatch"));

    // 宽度（像素）
    QSlider* wSlider = nullptr;
    regionCard->contentLayout()->addWidget(
        FormKit::sliderRow(
            QStringLiteral("\xe5\xae\xbd\xe5\xba\xa6\xef\xbc\x88\xe5\x83\x8f\xe7\xb4\xa0\xef\xbc\x89"),  // 宽度（像素）
            4, 256, cfg.crosshairRectW(), wSlider, m_rectW));
    wSlider->setToolTip(tr(
        "\xe7\x94\xbb\xe9\x9d\xa2\xe4\xb8\xad\xe5\xbf\x83\xe5\x8f\x96\xe6\xa0\xb7\xe7\x9f\xa9\xe5\xbd\xa2\xe7\x9a\x84\xe5\xae\xbd\xe5\xba\xa6(\xe6\xa3\x80\xe6\xb5\x8b\xe5\x83\x8f\xe7\xb4\xa0)\xe3\x80\x82\xe5\x80\xbc\xe5\xb0\x8f=\xe5\x8f\xaa\xe7\x9c\x8b\xe5\x87\x86\xe6\x98\x9f\xe6\xad\xa3\xe4\xb8\xad,\xe7\xb2\xbe\xe5\x87\x86\xe4\xbd\x86\xe9\x95\x9c\xe6\x8a\xac\xe9\xab\x98\xe6\x97\xb6\xe5\x8f\xaf\xe8\x83\xbd\xe6\xbc\x8f;\n"
        "\xe5\x80\xbc\xe5\xa4\xa7=\xe8\xa6\x86\xe7\x9b\x96\xe6\x9b\xb4\xe5\xb9\xbf\xe5\x8c\xba\xe5\x9f\x9f,\xe5\xae\xb9\xe5\xbf\x8d\xe5\x87\x86\xe6\x98\x9f\xe5\x81\x8f\xe7\xa7\xbb\xe4\xbd\x86\xe5\x8f\xaf\xe8\x83\xbd\xe8\xaf\xaf\xe8\xaf\x86\xe5\x88\xab\xe5\x85\xb6\xe5\xae\x83\xe7\xba\xa2\xe8\x89\xb2\xe5\x83\x8f\xe7\xb4\xa0\xe3\x80\x82"));
    // 画面中心取样矩形的宽度(检测像素)。值小=只看准星正中,精准但镜抬高时可能漏;
    // 值大=覆盖更广区域,容忍准星偏移但可能误识别其它红色像素。
    m_rectW->setToolTip(wSlider->toolTip());
    connect(m_rectW, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { ConfigManager::instance().setCrosshairRectW(v); });

    // 高度（像素）
    QSlider* hSlider = nullptr;
    regionCard->contentLayout()->addWidget(
        FormKit::sliderRow(
            QStringLiteral("\xe9\xab\x98\xe5\xba\xa6\xef\xbc\x88\xe5\x83\x8f\xe7\xb4\xa0\xef\xbc\x89"),  // 高度（像素）
            4, 256, cfg.crosshairRectH(), hSlider, m_rectH));
    hSlider->setToolTip(tr(
        "\xe5\x8f\x96\xe6\xa0\xb7\xe7\x9f\xa9\xe5\xbd\xa2\xe7\x9a\x84\xe9\xab\x98\xe5\xba\xa6(\xe6\xa3\x80\xe6\xb5\x8b\xe5\x83\x8f\xe7\xb4\xa0)\xe3\x80\x82\xe5\x92\x8c\xe5\xae\xbd\xe5\xba\xa6\xe7\x8b\xac\xe7\xab\x8b\xe8\xb0\x83,\xe9\x80\x82\xe5\x90\x88\xe6\xa4\xad\xe5\x9c\x86\xe5\xbd\xa2/\xe9\x95\xbf\xe6\x96\xb9\xe5\xbd\xa2\xe5\x8f\x96\xe6\xa0\xb7\xe3\x80\x82"));
    // 取样矩形的高度(检测像素)。和宽度独立调,适合椭圆形/长方形取样。
    m_rectH->setToolTip(hSlider->toolTip());
    connect(m_rectH, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { ConfigManager::instance().setCrosshairRectH(v); });

    layout->addWidget(regionCard);

    // ====================================================================
    // Card 2: 颜色区间
    // ====================================================================
    auto* colorCard = new CardWidget(
        QStringLiteral("\xe9\xa2\x9c\xe8\x89\xb2\xe5\x8c\xba\xe9\x97\xb4"),  // 颜色区间
        QStringLiteral("palette"));

    auto* colorHint = new QLabel(tr(
        "OpenCV HSV \xe8\x8c\x83\xe5\x9b\xb4\xef\xbc\x9aH [0,179]\xef\xbc\x8cS/V [0,255]\xe3\x80\x82\xe7\xba\xa2\xe8\x89\xb2\xe9\x80\x9a\xe5\xb8\xb8\xe9\x9c\x80\xe8\xa6\x81\xe4\xb8\xa4\xe4\xb8\xaa\xe5\x8c\xba\xe9\x97\xb4\xef\xbc\x9aH 0-10 \xe5\x92\x8c H 160-179\xe3\x80\x82"));
    // OpenCV HSV 范围：H [0,179]，S/V [0,255]。红色通常需要两个区间：H 0-10 和 H 160-179。
    colorHint->setWordWrap(true);
    colorHint->setStyleSheet(QStringLiteral("color: #888;"));
    colorCard->contentLayout()->addWidget(colorHint);

    m_colorTable = createColorTable(this);
    colorCard->contentLayout()->addWidget(m_colorTable);

    auto* btnRow = new QHBoxLayout;
    m_addColorBtn = new QPushButton(
        QStringLiteral("\xe6\x96\xb0\xe5\xa2\x9e\xe7\xa9\xba\xe9\xa2\x9c\xe8\x89\xb2"));       // 新增空颜色
    m_removeColorBtn = new QPushButton(
        QStringLiteral("\xe5\x88\xa0\xe9\x99\xa4\xe9\x80\x89\xe4\xb8\xad"));                     // 删除选中
    m_presetCombo = createPresetCombo(this);
    auto* addPresetBtn = new QPushButton(
        QStringLiteral("\xe6\xb7\xbb\xe5\x8a\xa0\xe9\xa2\x84\xe8\xae\xbe"));                     // 添加预设
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
    // Card 3: 形状容差
    // ====================================================================
    auto* shapeCard = new CardWidget(
        QStringLiteral("\xe5\xbd\xa2\xe7\x8a\xb6\xe5\xae\xb9\xe5\xb7\xae"),  // 形状容差
        QStringLiteral("target"));

    // 最少红像素数
    QSlider* mpSlider = nullptr;
    shapeCard->contentLayout()->addWidget(
        FormKit::sliderRow(
            QStringLiteral("\xe6\x9c\x80\xe5\xb0\x91\xe7\xba\xa2\xe5\x83\x8f\xe7\xb4\xa0\xe6\x95\xb0"),  // 最少红像素数
            1, 200, cfg.crosshairMinPixelCount(), mpSlider, m_minPixels));
    mpSlider->setToolTip(tr(
        "ROI \xe5\x86\x85\xe7\xb4\xaf\xe8\xae\xa1\xe7\x9a\x84\xe7\x9b\xae\xe6\xa0\x87\xe9\xa2\x9c\xe8\x89\xb2\xe5\x83\x8f\xe7\xb4\xa0\xe4\xbd\x8e\xe4\xba\x8e\xe6\xad\xa4\xe9\x98\x88\xe5\x80\xbc\xe6\x97\xb6\xe8\xae\xa4\xe4\xb8\xba'\xe6\xb2\xa1\xe7\x9c\x8b\xe5\x88\xb0\xe5\x87\x86\xe6\x98\x9f'\xe3\x80\x82\n"
        "\xe5\x80\xbc\xe8\xb6\x8a\xe5\xa4\xa7\xe8\xb6\x8a\xe4\xb8\xa5\xe6\xa0\xbc,\xe8\xaf\xaf\xe8\xaf\x86\xe5\xb0\x91\xe4\xbd\x86\xe5\xb0\x8f\xe5\x87\x86\xe6\x98\x9f\xe5\x8f\xaf\xe8\x83\xbd\xe6\xbc\x8f\xe6\xa3\x80;\xe5\x80\xbc\xe8\xb6\x8a\xe5\xb0\x8f\xe8\xb6\x8a\xe5\xae\xbd\xe6\x9d\xbe\xe3\x80\x82"));
    // ROI 内累计的目标颜色像素低于此阈值时认为'没看到准星'。
    // 值越大越严格,误识少但小准星可能漏检;值越小越宽松。
    m_minPixels->setToolTip(mpSlider->toolTip());
    connect(m_minPixels, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { ConfigManager::instance().setCrosshairMinPixelCount(v); });

    // 形状闭合半径（px）
    QSlider* crSlider = nullptr;
    shapeCard->contentLayout()->addWidget(
        FormKit::sliderRow(
            QStringLiteral("\xe5\xbd\xa2\xe7\x8a\xb6\xe9\x97\xad\xe5\x90\x88\xe5\x8d\x8a\xe5\xbe\x84\xef\xbc\x88px\xef\xbc\x89"),  // 形状闭合半径（px）
            0, 7, cfg.crosshairCloseRadius(), crSlider, m_closeRadius));
    crSlider->setToolTip(tr(
        "\xe5\xbd\xa2\xe6\x80\x81\xe5\xad\xa6\xe9\x97\xad\xe8\xbf\x90\xe7\xae\x97\xe5\x8d\x8a\xe5\xbe\x84(\xe5\x83\x8f\xe7\xb4\xa0),\xe7\x94\xa8\xe4\xba\x8e\xe6\x8a\x8a\xe9\x95\x82\xe7\xa9\xba\xe5\x87\x86\xe6\x98\x9f(\xe7\x99\xbd\xe5\xbf\x83\xe7\xba\xa2\xe7\x82\xb9/\xe5\x8d\x81\xe5\xad\x97/\xe6\xb8\x90\xe5\x8f\x98)\xe7\x9a\x84\xe5\xb0\x8f\xe7\xbc\x9d\xe5\x90\x88\xe5\xb9\xb6\xe6\x88\x90\xe4\xb8\x80\xe4\xb8\xaa blob\xe3\x80\x82\n"
        "0 = \xe4\xb8\x8d\xe9\x97\xad\xe5\x90\x88(\xe9\x80\x82\xe5\x90\x88\xe5\xae\x9e\xe5\xbf\x83\xe5\x87\x86\xe6\x98\x9f)\xe3\x80\x82" "1-3\xe9\x80\x9a\xe7\x94\xa8\xe3\x80\x82\xe8\xbf\x87\xe5\xa4\xa7\xe4\xbc\x9a\xe6\x8a\x8a\xe8\xa1\x80\xe9\x87\x8f\xe6\x9d\xa1/\xe5\x87\xbb\xe4\xb8\xad\xe5\x8f\x8d\xe9\xa6\x88\xe7\xad\x89\xe9\x99\x84\xe8\xbf\x91\xe7\xba\xa2\xe8\x89\xb2\xe7\xb2\x98\xe8\xbf\x9b\xe5\x87\x86\xe6\x98\x9f\xe9\x87\x8c\xe3\x80\x82"));
    // 形态学闭运算半径(像素),用于把镂空准星(白心红点/十字/渐变)的小缝合并成一个 blob。
    // 0 = 不闭合(适合实心准星)。1-3 通用。过大会把血量条/击中反馈等附近红色粘进准星里。
    m_closeRadius->setToolTip(crSlider->toolTip());
    connect(m_closeRadius, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { ConfigManager::instance().setCrosshairCloseRadius(v); });

    // 抗抖动平滑
    shapeCard->contentLayout()->addWidget(
        FormKit::sliderRowD(
            QStringLiteral("\xe6\x8a\x97\xe6\x8a\x96\xe5\x8a\xa8\xe5\xb9\xb3\xe6\xbb\x91"),  // 抗抖动平滑
            0.0, 1.0, static_cast<double>(cfg.crosshairSmooth()), 0.01, 2,
            m_smoothSlider, m_smoothSpin));
    m_smoothSlider->setToolTip(tr(
        "\xe5\xaf\xb9\xe5\x87\x86\xe6\x98\x9f\xe7\x82\xb9\xe5\x81\x9a\xe8\x87\xaa\xe9\x80\x82\xe5\xba\x94\xe4\xbd\x8e\xe9\x80\x9a(One-Euro):\xe9\x9d\x99\xe6\xad\xa2/\xe5\xbe\xae\xe6\x8a\x96\xe6\x97\xb6\xe5\xbc\xba\xe5\xb9\xb3\xe6\xbb\x91\xe6\xb6\x88\xe6\x8a\x96,\xe5\xbf\xab\xe9\x80\x9f\xe7\xa7\xbb\xe5\x8a\xa8\xe6\x97\xb6\xe5\x87\xa0\xe4\xb9\x8e\xe4\xb8\x8d\xe5\xb9\xb3\xe6\xbb\x91\xe3\x80\x81\n"
        "\xe4\xb8\x8d\xe5\xa2\x9e\xe5\x8a\xa0\xe5\xbb\xb6\xe8\xbf\x9f,\xe6\x89\x80\xe4\xbb\xa5\xe4\xb8\x8d\xe4\xbc\x9a\xe6\x8b\x96\xe6\x85\xa2\xe8\xb7\x9f\xe6\x9e\xaa/\xe5\x8e\x8b\xe6\x9e\xaa\xe3\x80\x82" "0=\xe5\x85\xb3(\xe5\x8e\x9f\xe5\xa7\x8b);\xe6\x8a\x96\xe5\xb0\xb1\xe5\xbe\x80\xe4\xb8\x8a\xe8\xb0\x83,\xe8\xa7\x89\xe5\xbe\x97\xe5\x8f\x91\xe9\xbb\x8f/\xe8\xbf\x9f\xe9\x92\x9d\xe5\xb0\xb1\xe8\xb0\x83\xe5\xb0\x8f\xe3\x80\x82\xe5\xb8\xb8\xe7\x94\xa8 0.3~0.7\xe3\x80\x82"));
    // 对准星点做自适应低通(One-Euro):静止/微抖时强平滑消抖,快速移动时几乎不平滑、
    // 不增加延迟,所以不会拖慢跟枪/压枪。0=关(原始);抖就往上调,觉得发黏/迟钝就调小。常用 0.3~0.7。
    m_smoothSpin->setToolTip(m_smoothSlider->toolTip());
    connect(m_smoothSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [](double v) { ConfigManager::instance().setCrosshairSmooth(static_cast<float>(v)); });

    layout->addWidget(shapeCard);

    // ====================================================================
    // Card 4: 镭射找色 - 说明
    // ====================================================================
    auto* laserIntroCard = new CardWidget(
        QStringLiteral("\xe9\x95\xad\xe5\xb0\x84\xe6\x89\xbe\xe8\x89\xb2 - \xe8\xaf\xb4\xe6\x98\x8e"),  // 镭射找色 - 说明
        QStringLiteral("zap"));
    auto* laserDesc = new QLabel(tr(
        "\xe9\x95\xad\xe5\xb0\x84\xe6\x89\xbe\xe8\x89\xb2\xe4\xb8\x8e\xe5\x87\x86\xe6\x98\x9f\xe6\x89\xbe\xe8\x89\xb2\xe7\x9b\xb8\xe4\xba\x92\xe7\x8b\xac\xe7\xab\x8b\xe3\x80\x81\xe5\x8f\xaf\xe5\x90\x8c\xe6\x97\xb6\xe5\xbc\x80\xe5\x90\xaf;\xe4\xb8\xa4\xe8\x80\x85\xe9\x83\xbd\xe5\x91\xbd\xe4\xb8\xad\xe6\x97\xb6'\xe5\x87\x86\xe6\x98\x9f\xe6\x89\xbe\xe8\x89\xb2'\xe4\xbc\x98\xe5\x85\x88,\n"
        "\xe9\x95\xad\xe5\xb0\x84\xe6\x9c\xab\xe7\xab\xaf\xe4\xbb\x85\xe4\xbd\x9c\xe4\xb8\xba\xe5\x87\x86\xe6\x98\x9f\xe6\xb2\xa1\xe6\x89\xbe\xe5\x88\xb0\xe6\x97\xb6\xe7\x9a\x84\xe8\xa1\xa5\xe5\x85\x85\xe3\x80\x82\xe6\xaf\x8f\xe4\xb8\xaa\xe7\x83\xad\xe9\x94\xae\xe7\x9a\x84\xe5\xbc\x80\xe5\x85\xb3\xe5\x9c\xa8'\xe7\x9e\x84\xe5\x87\x86\xe7\x83\xad\xe9\x94\xae'\xe9\xa1\xb5\xe3\x80\x82\n\n"
        "\xe5\x8e\x9f\xe7\x90\x86:\xe6\x8a\x8a\xe9\x95\xad\xe5\xb0\x84\xe5\xbd\x93\xe4\xbd\x9c\xe4\xb8\x80\xe6\x9d\xa1\xe7\xba\xbf,\xe6\x8b\x9f\xe5\x90\x88\xe4\xb8\xbb\xe8\xbd\xb4\xe5\x8f\x96'\xe7\x9e\x84\xe5\x87\x86\xe7\xab\xaf'(\xe9\x9d\xa0\xe4\xb8\xad\xe5\xbf\x83\xe9\x82\xa3\xe7\xab\xaf)\xe4\xbd\x9c\xe5\x91\xbd\xe4\xb8\xad\xe7\x82\xb9,\xe5\x8f\xaf\xe8\x99\x9a\xe6\x8b\x9f\xe5\xa4\x96\xe6\x8e\xa8\xe8\xa1\xa5\xe9\xbd\x90\xe5\x8f\x91\xe6\xb7\xa1\xe6\x9c\xab\xe7\xab\xaf\xe3\x80\x82\n"
        "\xe9\x9d\xa0'\xe7\xbb\x86\xe9\x95\xbf\xe7\xba\xbf + \xe6\x9e\xaa\xe5\x8f\xa3\xe7\xab\xaf\xe5\x9c\xa8\xe4\xb8\x8b/\xe6\x9c\xab\xe7\xab\xaf\xe9\x9d\xa0\xe4\xb8\xad\xe5\xbf\x83'\xe7\x9a\x84\xe5\x87\xa0\xe4\xbd\x95\xe7\x89\xb9\xe5\xbe\x81\xe7\xad\x9b\xe9\x80\x89,\xe8\x83\x8c\xe6\x99\xaf\xe9\x9a\x8f\xe6\x9c\xba(\xe5\xa4\xa9\xe7\xa9\xba/\xe7\xba\xa2\xe5\xa2\x99)\xe4\xb9\x9f\xe4\xb8\x8d\xe6\x98\x93\xe8\xaf\xaf\xe8\xaf\x86\xe5\x88\xab\xe3\x80\x82"));
    // 镭射找色与准星找色相互独立、可同时开启;两者都命中时'准星找色'优先,
    // 镭射末端仅作为准星没找到时的补充。每个热键的开关在'瞄准热键'页。
    // 原理:把镭射当作一条线,拟合主轴取'瞄准端'(靠中心那端)作命中点,可虚拟外推补齐发淡末端。
    // 靠'细长线 + 枪口端在下/末端靠中心'的几何特征筛选,背景随机(天空/红墙)也不易误识别。
    laserDesc->setWordWrap(true);
    laserIntroCard->contentLayout()->addWidget(laserDesc);
    layout->addWidget(laserIntroCard);

    // ====================================================================
    // Card 5: 取样区域与参数 (laser)
    // ====================================================================
    auto* laserParamCard = new CardWidget(
        QStringLiteral("\xe5\x8f\x96\xe6\xa0\xb7\xe5\x8c\xba\xe5\x9f\x9f\xe4\xb8\x8e\xe5\x8f\x82\xe6\x95\xb0"),  // 取样区域与参数
        QStringLiteral("crosshair"));

    // ① 识别框(黄):框住镭射可识别的线身。
    auto* detectHint = new QLabel(tr(
        "\xe2\x91\xa0 \xe8\xaf\x86\xe5\x88\xab\xe6\xa1\x86(\xe9\xbb\x84):\xe6\xa1\x86\xe4\xbd\x8f\xe9\x95\xad\xe5\xb0\x84\xe5\x8f\xaf\xe8\xaf\x86\xe5\x88\xab\xe7\x9a\x84\xe7\xba\xbf\xe8\xba\xab\xe3\x80\x82"));
    detectHint->setStyleSheet(QStringLiteral("color: #888;"));
    laserParamCard->contentLayout()->addWidget(detectHint);

    // 识别框宽度（px）
    QSlider* lrwSlider = nullptr;
    laserParamCard->contentLayout()->addWidget(
        FormKit::sliderRow(
            QStringLiteral("\xe8\xaf\x86\xe5\x88\xab\xe6\xa1\x86\xe5\xae\xbd\xe5\xba\xa6\xef\xbc\x88px\xef\xbc\x89"),  // 识别框宽度（px）
            4, 640, cfg.laserRectW(), lrwSlider, m_laserRectW));
    lrwSlider->setToolTip(tr(
        "\xe9\x95\xad\xe5\xb0\x84\xe8\xaf\x86\xe5\x88\xab\xe7\x9f\xa9\xe5\xbd\xa2\xe5\xae\xbd\xe5\xba\xa6(\xe6\xa3\x80\xe6\xb5\x8b\xe5\x83\x8f\xe7\xb4\xa0)\xe3\x80\x82\xe6\xa8\xaa\xe5\x90\x91\xe9\x9c\x80\xe7\xbd\xa9\xe4\xbd\x8f\xe6\x95\xb4\xe6\x9d\xa1\xe6\x96\x9c\xe7\xba\xbf\xe3\x80\x82"));
    // 镭射识别矩形宽度(检测像素)。横向需罩住整条斜线。
    m_laserRectW->setToolTip(lrwSlider->toolTip());
    connect(m_laserRectW, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { ConfigManager::instance().setLaserRectW(v); });

    // 识别框高度（px）
    QSlider* lrhSlider = nullptr;
    laserParamCard->contentLayout()->addWidget(
        FormKit::sliderRow(
            QStringLiteral("\xe8\xaf\x86\xe5\x88\xab\xe6\xa1\x86\xe9\xab\x98\xe5\xba\xa6\xef\xbc\x88px\xef\xbc\x89"),  // 识别框高度（px）
            4, 640, cfg.laserRectH(), lrhSlider, m_laserRectH));
    lrhSlider->setToolTip(tr(
        "\xe9\x95\xad\xe5\xb0\x84\xe8\xaf\x86\xe5\x88\xab\xe7\x9f\xa9\xe5\xbd\xa2\xe9\xab\x98\xe5\xba\xa6(\xe6\xa3\x80\xe6\xb5\x8b\xe5\x83\x8f\xe7\xb4\xa0)\xe3\x80\x82\xe7\xba\xb5\xe5\x90\x91\xe9\x9c\x80\xe7\xbd\xa9\xe4\xbd\x8f\xe5\x89\x8d\xe6\xae\xb5\xe6\xb8\x85\xe6\x99\xb0\xe7\x9a\x84\xe7\xba\xbf\xe8\xba\xab,\xe4\xb8\xbb\xe8\xbd\xb4\xe6\x8b\x9f\xe5\x90\x88\xe6\x89\x8d\xe7\xa8\xb3\xe3\x80\x82"));
    // 镭射识别矩形高度(检测像素)。纵向需罩住前段清晰的线身,主轴拟合才稳。
    m_laserRectH->setToolTip(lrhSlider->toolTip());
    connect(m_laserRectH, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { ConfigManager::instance().setLaserRectH(v); });

    // 识别框中心 X（px）
    QSlider* lcxSlider = nullptr;
    laserParamCard->contentLayout()->addWidget(
        FormKit::sliderRow(
            QStringLiteral("\xe8\xaf\x86\xe5\x88\xab\xe6\xa1\x86\xe4\xb8\xad\xe5\xbf\x83 X\xef\xbc\x88px\xef\xbc\x89"),  // 识别框中心 X（px）
            0, 640, cfg.laserCenterX(), lcxSlider, m_laserCenterX));
    lcxSlider->setToolTip(tr(
        "\xe8\xaf\x86\xe5\x88\xab\xe6\xa1\x86\xe4\xb8\xad\xe5\xbf\x83\xe6\xa8\xaa\xe5\x9d\x90\xe6\xa0\x87(\xe6\xa3\x80\xe6\xb5\x8b\xe5\x83\x8f\xe7\xb4\xa0,0=\xe6\x9c\x80\xe5\xb7\xa6)\xe3\x80\x82\xe7\x94\xbb\xe9\x9d\xa2\xe4\xb8\xad\xe5\xbf\x83\xe7\xba\xa6\xe4\xb8\xba \xe6\xa3\x80\xe6\xb5\x8b\xe5\x88\x86\xe8\xbe\xa8\xe7\x8e\x87/2\xe3\x80\x82"));
    // 识别框中心横坐标(检测像素,0=最左)。画面中心约为 检测分辨率/2。
    m_laserCenterX->setToolTip(lcxSlider->toolTip());
    connect(m_laserCenterX, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { ConfigManager::instance().setLaserCenterX(v); });

    // 识别框中心 Y（px）
    QSlider* lcySlider = nullptr;
    laserParamCard->contentLayout()->addWidget(
        FormKit::sliderRow(
            QStringLiteral("\xe8\xaf\x86\xe5\x88\xab\xe6\xa1\x86\xe4\xb8\xad\xe5\xbf\x83 Y\xef\xbc\x88px\xef\xbc\x89"),  // 识别框中心 Y（px）
            0, 640, cfg.laserCenterY(), lcySlider, m_laserCenterY));
    lcySlider->setToolTip(tr(
        "\xe8\xaf\x86\xe5\x88\xab\xe6\xa1\x86\xe4\xb8\xad\xe5\xbf\x83\xe7\xba\xb5\xe5\x9d\x90\xe6\xa0\x87(\xe6\xa3\x80\xe6\xb5\x8b\xe5\x83\x8f\xe7\xb4\xa0,0=\xe6\x9c\x80\xe4\xb8\x8a)\xe3\x80\x82\xe9\x95\xad\xe5\xb0\x84\xe7\xba\xbf\xe8\xba\xab\xe9\x80\x9a\xe5\xb8\xb8\xe5\x9c\xa8\xe4\xb8\xad\xe5\xbf\x83\xe4\xb8\x8b\xe6\x96\xb9,\xe5\x8f\xaf\xe6\x8a\x8a Y \xe8\xb0\x83\xe5\xa4\xa7\xe4\xb8\x80\xe4\xba\x9b\xe3\x80\x82"));
    // 识别框中心纵坐标(检测像素,0=最上)。镭射线身通常在中心下方,可把 Y 调大一些。
    m_laserCenterY->setToolTip(lcySlider->toolTip());
    connect(m_laserCenterY, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { ConfigManager::instance().setLaserCenterY(v); });

    // ② 终点框(棕)
    auto* targetHint = new QLabel(tr(
        "\xe2\x91\xa1 \xe7\xbb\x88\xe7\x82\xb9\xe6\xa1\x86(\xe6\xa3\x95):\xe9\x9d\x99\xe6\x80\x81\xe4\xb8\xad\xe5\xbf\x83\xe9\x99\x84\xe8\xbf\x91\xe3\x80\x81\xe7\x9c\x9f\xe6\xad\xa3\xe5\x91\xbd\xe4\xb8\xad\xe7\x82\xb9\xe6\x89\x80\xe5\x9c\xa8\xe7\x9a\x84\xe5\xb0\x8f\xe5\x8c\xba\xe5\x9f\x9f\xe3\x80\x82\xe6\x8a\x8a\xe8\xaf\x86\xe5\x88\xab\xe5\x88\xb0\xe7\x9a\x84\xe7\xba\xbf\n"
        "\xe6\xb2\xbf\xe5\x89\x8d\xe6\xae\xb5\xe6\x96\xb9\xe5\x90\x91\xe6\x8a\x95\xe5\xbd\xb1\xe8\xbf\x9b\xe8\xbf\x99\xe4\xb8\xaa\xe6\xa1\x86\xe6\x9d\xa5\xe4\xbc\xb0\xe8\xae\xa1\xe7\xbb\x88\xe7\x82\xb9 \xe2\x80\x94\xe2\x80\x94 \xe5\x8f\x96\xe4\xbb\xa3'\xe5\xbb\xb6\xe4\xbc\xb8\xe5\x83\x8f\xe7\xb4\xa0',\xe6\x9c\xab\xe7\xab\xaf\xe5\x86\x8d\xe7\xb3\x8a\xe4\xb9\x9f\xe4\xb8\x8d\xe4\xb9\xb1\xe8\xb7\xb3\xe3\x80\x82"));
    // ② 终点框(棕):静态中心附近、真正命中点所在的小区域。把识别到的线
    // 沿前段方向投影进这个框来估计终点 -- 取代'延伸像素',末端再糊也不乱跳。
    targetHint->setWordWrap(true);
    targetHint->setStyleSheet(QStringLiteral("color: #888;"));
    laserParamCard->contentLayout()->addWidget(targetHint);

    // 终点框中心 X（px）
    QSlider* ltcxSlider = nullptr;
    laserParamCard->contentLayout()->addWidget(
        FormKit::sliderRow(
            QStringLiteral("\xe7\xbb\x88\xe7\x82\xb9\xe6\xa1\x86\xe4\xb8\xad\xe5\xbf\x83 X\xef\xbc\x88px\xef\xbc\x89"),  // 终点框中心 X（px）
            0, 640, cfg.laserTargetCenterX(), ltcxSlider, m_laserTargetCenterX));
    ltcxSlider->setToolTip(tr(
        "\xe7\xbb\x88\xe7\x82\xb9\xe6\xa1\x86\xe4\xb8\xad\xe5\xbf\x83\xe6\xa8\xaa\xe5\x9d\x90\xe6\xa0\x87\xe3\x80\x82\xe4\xb8\x80\xe8\x88\xac\xe8\xb4\xb4\xe7\x9d\x80\xe7\x94\xbb\xe9\x9d\xa2\xe9\x9d\x99\xe6\x80\x81\xe4\xb8\xad\xe5\xbf\x83(\xe5\x87\x86\xe6\x98\x9f\xe5\xa4\x84)\xe3\x80\x82"));
    // 终点框中心横坐标。一般贴着画面静态中心(准星处)。
    m_laserTargetCenterX->setToolTip(ltcxSlider->toolTip());
    connect(m_laserTargetCenterX, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { ConfigManager::instance().setLaserTargetCenterX(v); });

    // 终点框中心 Y（px）
    QSlider* ltcySlider = nullptr;
    laserParamCard->contentLayout()->addWidget(
        FormKit::sliderRow(
            QStringLiteral("\xe7\xbb\x88\xe7\x82\xb9\xe6\xa1\x86\xe4\xb8\xad\xe5\xbf\x83 Y\xef\xbc\x88px\xef\xbc\x89"),  // 终点框中心 Y（px）
            0, 640, cfg.laserTargetCenterY(), ltcySlider, m_laserTargetCenterY));
    ltcySlider->setToolTip(tr(
        "\xe7\xbb\x88\xe7\x82\xb9\xe6\xa1\x86\xe4\xb8\xad\xe5\xbf\x83\xe7\xba\xb5\xe5\x9d\x90\xe6\xa0\x87\xe3\x80\x82\xe4\xb8\x80\xe8\x88\xac\xe8\xb4\xb4\xe7\x9d\x80\xe7\x94\xbb\xe9\x9d\xa2\xe9\x9d\x99\xe6\x80\x81\xe4\xb8\xad\xe5\xbf\x83(\xe5\x87\x86\xe6\x98\x9f\xe5\xa4\x84)\xe3\x80\x82"));
    // 终点框中心纵坐标。一般贴着画面静态中心(准星处)。
    m_laserTargetCenterY->setToolTip(ltcySlider->toolTip());
    connect(m_laserTargetCenterY, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { ConfigManager::instance().setLaserTargetCenterY(v); });

    // 终点框宽度（px）
    QSlider* ltwSlider = nullptr;
    laserParamCard->contentLayout()->addWidget(
        FormKit::sliderRow(
            QStringLiteral("\xe7\xbb\x88\xe7\x82\xb9\xe6\xa1\x86\xe5\xae\xbd\xe5\xba\xa6\xef\xbc\x88px\xef\xbc\x89"),  // 终点框宽度（px）
            4, 640, cfg.laserTargetRectW(), ltwSlider, m_laserTargetRectW));
    ltwSlider->setToolTip(tr(
        "\xe7\xbb\x88\xe7\x82\xb9\xe6\xa1\x86\xe5\xae\xbd\xe5\xba\xa6\xe3\x80\x82\xe7\xbb\x88\xe7\x82\xb9\xe4\xbc\x9a\xe8\xa2\xab\xe5\xa4\xb9\xe5\x9c\xa8\xe8\xbf\x99\xe4\xb8\xaa\xe6\xa1\x86\xe5\x86\x85 \xe2\x86\x92 \xe6\xa1\x86\xe8\xb6\x8a\xe5\xb0\x8f\xe7\xba\xa6\xe6\x9d\x9f\xe8\xb6\x8a\xe5\xbc\xba(\xe9\x98\xb2\xe5\xbb\xb6\xe4\xbc\xb8\xe8\xbf\x87\xe5\xa4\xb4),\xe5\xa4\xaa\xe5\xb0\x8f\xe5\x88\x99\xe7\xba\xbf\xe6\xb2\xa1\xe7\xa9\xbf\xe8\xbf\x87\xe6\xa1\x86\xe6\x97\xb6\xe5\x9b\x9e\xe9\x80\x80\xe5\x88\xb0\xe5\x8f\xaf\xe8\xa7\x81\xe6\x9c\xab\xe7\xab\xaf\xe3\x80\x82"));
    // 终点框宽度。终点会被夹在这个框内 -> 框越小约束越强(防延伸过头),太小则线没穿过框时回退到可见末端。
    m_laserTargetRectW->setToolTip(ltwSlider->toolTip());
    connect(m_laserTargetRectW, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { ConfigManager::instance().setLaserTargetRectW(v); });

    // 终点框高度（px）
    QSlider* lthSlider = nullptr;
    laserParamCard->contentLayout()->addWidget(
        FormKit::sliderRow(
            QStringLiteral("\xe7\xbb\x88\xe7\x82\xb9\xe6\xa1\x86\xe9\xab\x98\xe5\xba\xa6\xef\xbc\x88px\xef\xbc\x89"),  // 终点框高度（px）
            4, 640, cfg.laserTargetRectH(), lthSlider, m_laserTargetRectH));
    lthSlider->setToolTip(tr(
        "\xe7\xbb\x88\xe7\x82\xb9\xe6\xa1\x86\xe9\xab\x98\xe5\xba\xa6\xe3\x80\x82\xe5\x90\x8c\xe4\xb8\x8a\xe3\x80\x82\xe7\xbb\x88\xe7\x82\xb9\xe5\x8f\x96'\xe7\xba\xbf\xe4\xb8\x8a\xe6\x9c\x80\xe9\x9d\xa0\xe8\xbf\x91\xe6\x9c\xac\xe6\xa1\x86\xe4\xb8\xad\xe5\xbf\x83\xe3\x80\x81\xe4\xb8\x94\xe8\x90\xbd\xe5\x9c\xa8\xe6\xa1\x86\xe5\x86\x85'\xe7\x9a\x84\xe7\x82\xb9\xe3\x80\x82"));
    // 终点框高度。同上。终点取'线上最靠近本框中心、且落在框内'的点。
    m_laserTargetRectH->setToolTip(lthSlider->toolTip());
    connect(m_laserTargetRectH, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { ConfigManager::instance().setLaserTargetRectH(v); });

    // 细长度门限
    laserParamCard->contentLayout()->addWidget(
        FormKit::sliderRowD(
            QStringLiteral("\xe7\xbb\x86\xe9\x95\xbf\xe5\xba\xa6\xe9\x97\xa8\xe9\x99\x90"),  // 细长度门限
            1.0, 15.0, static_cast<double>(cfg.laserMinElongation()), 0.1, 1,
            m_laserElongSlider, m_laserElongSpin));
    m_laserElongSlider->setToolTip(tr(
        "\xe5\x88\xa4\xe5\xae\x9a'\xe6\x98\xaf\xe4\xb8\x8d\xe6\x98\xaf\xe4\xb8\x80\xe6\x9d\xa1\xe7\xba\xbf'\xe7\x9a\x84\xe6\x9c\x80\xe5\xb0\x8f\xe9\x95\xbf\xe7\x9f\xad\xe8\xbd\xb4\xe6\xaf\x94\xe3\x80\x82\xe8\xb6\x8a\xe5\xa4\xa7\xe8\xb6\x8a\xe4\xb8\xa5\xe6\xa0\xbc,\xe8\xb6\x8a\xe8\x83\xbd\xe6\x8c\xa1\xe6\x8e\x89\xe8\x83\x8c\xe6\x99\xaf\xe9\x87\x8c\xe5\x9d\x97\xe7\x8a\xb6\xe7\xba\xa2\xe8\x89\xb2\xe3\x80\x82\n"
        "\xe6\xbc\x8f\xe6\xa3\x80(\xe9\x95\xad\xe5\xb0\x84\xe8\xbe\x83\xe7\x9f\xad)\xe5\xb0\xb1\xe8\xb0\x83\xe5\xb0\x8f,\xe8\xaf\xaf\xe8\xaf\x86\xe5\x88\xab\xe5\xb0\xb1\xe8\xb0\x83\xe5\xa4\xa7\xe3\x80\x82\xe5\xb8\xb8\xe7\x94\xa8 2.5~5\xe3\x80\x82"));
    // 判定'是不是一条线'的最小长短轴比。越大越严格,越能挡掉背景里块状红色。
    // 漏检(镭射较短)就调小,误识别就调大。常用 2.5~5。
    m_laserElongSpin->setToolTip(m_laserElongSlider->toolTip());
    connect(m_laserElongSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [](double v) { ConfigManager::instance().setLaserMinElongation(static_cast<float>(v)); });

    // 最少像素数
    QSlider* lmpSlider = nullptr;
    laserParamCard->contentLayout()->addWidget(
        FormKit::sliderRow(
            QStringLiteral("\xe6\x9c\x80\xe5\xb0\x91\xe5\x83\x8f\xe7\xb4\xa0\xe6\x95\xb0"),  // 最少像素数
            1, 500, cfg.laserMinPixelCount(), lmpSlider, m_laserMinPixels));
    lmpSlider->setToolTip(tr(
        "\xe4\xb8\x80\xe6\x9d\xa1\xe9\x95\xad\xe5\xb0\x84\xe8\xbf\x9e\xe9\x80\x9a\xe5\x9f\x9f\xe8\x87\xb3\xe5\xb0\x91\xe8\xa6\x81\xe8\xbf\x99\xe4\xb9\x88\xe5\xa4\x9a\xe5\x83\x8f\xe7\xb4\xa0\xe6\x89\x8d\xe8\x80\x83\xe8\x99\x91\xe3\x80\x82\xe5\xa4\xaa\xe5\xa4\xa7\xe6\xbc\x8f\xe6\x8e\x89\xe7\xbb\x86/\xe8\xbf\x9c\xe9\x95\xad\xe5\xb0\x84,\xe5\xa4\xaa\xe5\xb0\x8f\xe6\x98\x93\xe5\x8f\x97\xe5\x99\xaa\xe5\xa3\xb0\xe3\x80\x82"));
    // 一条镭射连通域至少要这么多像素才考虑。太大漏掉细/远镭射,太小易受噪声。
    m_laserMinPixels->setToolTip(lmpSlider->toolTip());
    connect(m_laserMinPixels, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { ConfigManager::instance().setLaserMinPixelCount(v); });

    // 形状闭合半径（px）
    QSlider* lcrSlider = nullptr;
    laserParamCard->contentLayout()->addWidget(
        FormKit::sliderRow(
            QStringLiteral("\xe5\xbd\xa2\xe7\x8a\xb6\xe9\x97\xad\xe5\x90\x88\xe5\x8d\x8a\xe5\xbe\x84\xef\xbc\x88px\xef\xbc\x89"),  // 形状闭合半径（px）
            0, 9, cfg.laserCloseRadius(), lcrSlider, m_laserCloseRadius));
    lcrSlider->setToolTip(tr(
        "\xe5\xbd\xa2\xe6\x80\x81\xe5\xad\xa6\xe9\x97\xad\xe8\xbf\x90\xe7\xae\x97\xe5\x8d\x8a\xe5\xbe\x84,\xe6\x8a\x8a\xe6\x96\xad\xe7\xbb\xad/\xe6\xb8\x90\xe5\x8f\x98\xe7\x9a\x84\xe9\x95\xad\xe5\xb0\x84\xe6\xa1\xa5\xe6\x8e\xa5\xe6\x88\x90\xe4\xb8\x80\xe6\x95\xb4\xe6\x9d\xa1\xe8\xbf\x9e\xe9\x80\x9a\xe7\xba\xbf\xe3\x80\x82" "0=\xe5\x85\xb3\xe3\x80\x82\xe9\x95\xad\xe5\xb0\x84\xe5\x8f\x91\xe8\x99\x9a\xe6\x97\xb6\xe8\xb0\x83\xe5\xa4\xa7(2~4)\xe3\x80\x82"));
    // 形态学闭运算半径,把断续/渐变的镭射桥接成一整条连通线。0=关。镭射发虚时调大(2~4)。
    m_laserCloseRadius->setToolTip(lcrSlider->toolTip());
    connect(m_laserCloseRadius, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { ConfigManager::instance().setLaserCloseRadius(v); });

    // 抗抖动平滑 (laser)
    laserParamCard->contentLayout()->addWidget(
        FormKit::sliderRowD(
            QStringLiteral("\xe6\x8a\x97\xe6\x8a\x96\xe5\x8a\xa8\xe5\xb9\xb3\xe6\xbb\x91"),  // 抗抖动平滑
            0.0, 1.0, static_cast<double>(cfg.laserSmooth()), 0.01, 2,
            m_laserSmoothSlider, m_laserSmoothSpin));
    m_laserSmoothSlider->setToolTip(tr(
        "\xe5\xaf\xb9\xe9\x95\xad\xe5\xb0\x84\xe6\x9c\xab\xe7\xab\xaf\xe7\x82\xb9\xe5\x81\x9a\xe8\x87\xaa\xe9\x80\x82\xe5\xba\x94\xe4\xbd\x8e\xe9\x80\x9a(One-Euro):\xe9\x9d\x99\xe6\xad\xa2/\xe5\xbe\xae\xe6\x8a\x96\xe6\x97\xb6\xe5\xbc\xba\xe5\xb9\xb3\xe6\xbb\x91\xe6\xb6\x88\xe6\x8a\x96,\xe5\xbf\xab\xe9\x80\x9f\xe7\xa7\xbb\xe5\x8a\xa8\xe6\x97\xb6\xe5\x87\xa0\xe4\xb9\x8e\xe4\xb8\x8d\xe5\xb9\xb3\xe6\xbb\x91\xe3\x80\x81\n"
        "\xe4\xb8\x8d\xe5\xa2\x9e\xe5\x8a\xa0\xe5\xbb\xb6\xe8\xbf\x9f\xe3\x80\x82" "0=\xe5\x85\xb3;\xe6\x9c\xab\xe7\xab\xaf\xe6\x8a\x96\xe5\xb0\xb1\xe5\xbe\x80\xe4\xb8\x8a\xe8\xb0\x83,\xe8\xa7\x89\xe5\xbe\x97\xe5\x8f\x91\xe9\xbb\x8f\xe5\xb0\xb1\xe8\xb0\x83\xe5\xb0\x8f\xe3\x80\x82\xe5\xb8\xb8\xe7\x94\xa8 0.3~0.7\xe3\x80\x82"));
    // 对镭射末端点做自适应低通(One-Euro):静止/微抖时强平滑消抖,快速移动时几乎不平滑、
    // 不增加延迟。0=关;末端抖就往上调,觉得发黏就调小。常用 0.3~0.7。
    m_laserSmoothSpin->setToolTip(m_laserSmoothSlider->toolTip());
    connect(m_laserSmoothSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [](double v) { ConfigManager::instance().setLaserSmooth(static_cast<float>(v)); });

    layout->addWidget(laserParamCard);

    // ====================================================================
    // Card 6: 镭射颜色区间
    // ====================================================================
    auto* laserColorCard = new CardWidget(
        QStringLiteral("\xe9\x95\xad\xe5\xb0\x84\xe9\xa2\x9c\xe8\x89\xb2\xe5\x8c\xba\xe9\x97\xb4"),  // 镭射颜色区间
        QStringLiteral("palette"));

    auto* laserColorHint = new QLabel(tr(
        "\xe7\x8b\xac\xe7\xab\x8b\xe4\xba\x8e\xe5\x87\x86\xe6\x98\x9f\xe9\xa2\x9c\xe8\x89\xb2\xe3\x80\x82\xe9\x95\xad\xe5\xb0\x84\xe5\xa4\x9a\xe4\xb8\xba\xe7\xba\xa2\xe8\x89\xb2(\xe9\x9c\x80 H 0-10 \xe4\xb8\x8e 160-179 \xe4\xb8\xa4\xe6\xae\xb5)\xe3\x80\x82\n"
        "\xe6\x9c\xab\xe7\xab\xaf\xe5\x8f\x91\xe6\xb7\xa1 \xe2\x86\x92 \xe9\x80\x82\xe5\xbd\x93\xe8\xb0\x83\xe4\xbd\x8e S/V \xe4\xb8\x8b\xe9\x99\x90\xe4\xbb\xa5\xe5\x90\x83\xe4\xbd\x8f\xe6\x9b\xb4\xe9\x95\xbf\xe7\x9a\x84\xe7\xba\xbf\xe8\xba\xab\xe3\x80\x82"));
    // 独立于准星颜色。镭射多为红色(需 H 0-10 与 160-179 两段)。
    // 末端发淡 -> 适当调低 S/V 下限以吃住更长的线身。
    laserColorHint->setWordWrap(true);
    laserColorHint->setStyleSheet(QStringLiteral("color: #888;"));
    laserColorCard->contentLayout()->addWidget(laserColorHint);

    m_laserColorTable = createColorTable(this);
    laserColorCard->contentLayout()->addWidget(m_laserColorTable);

    auto* laserBtnRow = new QHBoxLayout;
    m_addLaserColorBtn = new QPushButton(
        QStringLiteral("\xe6\x96\xb0\xe5\xa2\x9e\xe7\xa9\xba\xe9\xa2\x9c\xe8\x89\xb2"));       // 新增空颜色
    m_removeLaserColorBtn = new QPushButton(
        QStringLiteral("\xe5\x88\xa0\xe9\x99\xa4\xe9\x80\x89\xe4\xb8\xad"));                     // 删除选中
    m_laserPresetCombo = createPresetCombo(this);
    auto* addLaserPresetBtn = new QPushButton(
        QStringLiteral("\xe6\xb7\xbb\xe5\x8a\xa0\xe9\xa2\x84\xe8\xae\xbe"));                     // 添加预设
    laserBtnRow->addWidget(m_addLaserColorBtn);
    laserBtnRow->addWidget(m_removeLaserColorBtn);
    laserBtnRow->addWidget(m_laserPresetCombo);
    laserBtnRow->addWidget(addLaserPresetBtn);
    laserBtnRow->addStretch();
    laserColorCard->contentLayout()->addLayout(laserBtnRow);

    layout->addWidget(laserColorCard);

    connect(m_addLaserColorBtn, &QPushButton::clicked,
            this, &CrosshairPage::addEmptyLaserColor);
    connect(m_removeLaserColorBtn, &QPushButton::clicked,
            this, &CrosshairPage::removeLaserSelectedRows);
    connect(addLaserPresetBtn, &QPushButton::clicked,
            this, &CrosshairPage::addLaserPreset);
    connect(m_laserColorTable, &QTableWidget::cellChanged,
            this, &CrosshairPage::saveLaserColors);

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

    // Laser params
    m_laserRectW->setValue(cfg.laserRectW());
    m_laserRectH->setValue(cfg.laserRectH());
    m_laserCenterX->setValue(cfg.laserCenterX());
    m_laserCenterY->setValue(cfg.laserCenterY());
    m_laserTargetCenterX->setValue(cfg.laserTargetCenterX());
    m_laserTargetCenterY->setValue(cfg.laserTargetCenterY());
    m_laserTargetRectW->setValue(cfg.laserTargetRectW());
    m_laserTargetRectH->setValue(cfg.laserTargetRectH());
    m_laserElongSpin->setValue(static_cast<double>(cfg.laserMinElongation()));
    m_laserMinPixels->setValue(cfg.laserMinPixelCount());
    m_laserCloseRadius->setValue(cfg.laserCloseRadius());
    m_laserSmoothSpin->setValue(static_cast<double>(cfg.laserSmooth()));

    // Load laser color table
    m_laserColorTable->blockSignals(true);
    m_laserColorTable->setRowCount(0);
    auto laserColors = cfg.laserColors();
    for (const auto& c : laserColors) {
        insertColorRow(m_laserColorTable, c.name, c.enabled,
                       c.hLow, c.hHigh, c.sMin, c.sMax, c.vMin, c.vMax);
    }
    m_laserColorTable->blockSignals(false);
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

// ---- Laser color helpers ----

void CrosshairPage::addLaserColorRow(const QString& name, bool enabled,
                                      int hLo, int hHi, int sLo, int sHi,
                                      int vLo, int vHi) {
    insertColorRow(m_laserColorTable, name, enabled, hLo, hHi, sLo, sHi, vLo, vHi);
    saveLaserColors();
}

void CrosshairPage::addEmptyLaserColor() {
    auto idx = m_laserColorTable->rowCount() + 1;
    addLaserColorRow(
        QStringLiteral("\xe9\x95\xad\xe5\xb0\x84\xe9\xa2\x9c\xe8\x89\xb2 %1").arg(idx),  // 镭射颜色 N
        true, 0, 10, 45, 255, 50, 255);
}

void CrosshairPage::removeLaserSelectedRows() {
    auto ranges = m_laserColorTable->selectedRanges();
    for (int i = ranges.size() - 1; i >= 0; --i) {
        for (int row = ranges[i].bottomRow(); row >= ranges[i].topRow(); --row) {
            m_laserColorTable->removeRow(row);
        }
    }
    saveLaserColors();
}

void CrosshairPage::addLaserPreset() {
    int idx = m_laserPresetCombo->currentIndex();
    if (idx < 0 || idx >= kPresetCount) return;
    const auto& p = kPresets[idx];

    m_laserColorTable->blockSignals(true);
    insertColorRow(m_laserColorTable, p.nameA, true,
                   p.hLoA, p.hHiA, 45, 255, 50, 255);
    if (p.hasSecondary) {
        insertColorRow(m_laserColorTable, p.nameB, true,
                       p.hLoB, p.hHiB, 45, 255, 50, 255);
    }
    m_laserColorTable->blockSignals(false);
    saveLaserColors();
}

void CrosshairPage::saveLaserColors() {
    QList<ConfigManager::ColorProfile> colors;
    for (int row = 0; row < m_laserColorTable->rowCount(); ++row) {
        ConfigManager::ColorProfile c;
        auto* chk = qobject_cast<QCheckBox*>(m_laserColorTable->cellWidget(row, 0));
        c.enabled = chk ? chk->isChecked() : true;
        auto* nameItem = m_laserColorTable->item(row, 1);
        c.name = nameItem ? nameItem->text() : QStringLiteral("Color");
        auto val = [&](int col, int def) {
            auto* item = m_laserColorTable->item(row, col);
            return item ? item->text().toInt() : def;
        };
        c.hLow  = val(2, 0);
        c.hHigh = val(3, 10);
        c.sMin  = val(4, 45);
        c.sMax  = val(5, 255);
        c.vMin  = val(6, 50);
        c.vMax  = val(7, 255);
        colors.append(c);
    }
    ConfigManager::instance().setLaserColors(colors);
}
