#include "pages/CrosshairPage.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QHeaderView>
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
            editor->setRange(0, 180);
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

    // ── Card 1: 采样区域 ──
    auto* regionCard = new CardWidget(QStringLiteral("采样区域"), QStringLiteral("color-swatch"));

    m_width = new QSpinBox;
    m_width->setRange(10, 200);
    m_width->setValue(40);
    regionCard->contentLayout()->addWidget(FormKit::fieldRow(QStringLiteral("宽度"), m_width));

    m_height = new QSpinBox;
    m_height->setRange(10, 200);
    m_height->setValue(40);
    regionCard->contentLayout()->addWidget(FormKit::fieldRow(QStringLiteral("高度"), m_height));

    m_minPixels = new QSpinBox;
    m_minPixels->setRange(1, 100);
    m_minPixels->setValue(4);
    regionCard->contentLayout()->addWidget(FormKit::fieldRow(QStringLiteral("最小像素数"), m_minPixels));

    m_closeRadius = new QSpinBox;
    m_closeRadius->setRange(0, 10);
    m_closeRadius->setValue(1);
    regionCard->contentLayout()->addWidget(FormKit::fieldRow(QStringLiteral("闭运算半径"), m_closeRadius));

    layout->addWidget(regionCard);

    // ── Card 2: 颜色配置 ──
    auto* colorCard = new CardWidget(QStringLiteral("颜色配置"), QStringLiteral("palette"));

    m_colorTable = new QTableWidget(0, 8);
    m_colorTable->setHorizontalHeaderLabels({
        QStringLiteral("启用"),
        QStringLiteral("名称"),
        QStringLiteral("H 低"), QStringLiteral("H 高"),
        QStringLiteral("S 低"), QStringLiteral("S 高"),
        QStringLiteral("V 低"), QStringLiteral("V 高"),
    });
    m_colorTable->horizontalHeader()->setStretchLastSection(true);
    m_colorTable->setSelectionBehavior(QAbstractItemView::SelectRows);

    auto* delegate = new SpinBoxDelegate(m_colorTable);
    for (int col = 2; col <= 7; ++col) {
        m_colorTable->setItemDelegateForColumn(col, delegate);
    }

    colorCard->contentLayout()->addWidget(m_colorTable);

    auto* btnRow = new QHBoxLayout;
    m_addBtn = new QPushButton(QStringLiteral("添加颜色"));
    m_removeBtn = new QPushButton(QStringLiteral("删除选中"));
    btnRow->addWidget(m_addBtn);
    btnRow->addWidget(m_removeBtn);
    btnRow->addStretch();
    colorCard->contentLayout()->addLayout(btnRow);

    layout->addWidget(colorCard);
    layout->addStretch();

    connect(m_addBtn, &QPushButton::clicked, this, &CrosshairPage::addColorRow);
    connect(m_removeBtn, &QPushButton::clicked, this, &CrosshairPage::removeSelectedRows);

    // Default rows
    auto insertRow = [this](const QString& name, bool enabled,
                            int hLo, int hHi, int sLo, int sHi, int vLo, int vHi) {
        int row = m_colorTable->rowCount();
        m_colorTable->insertRow(row);

        auto* chk = new QCheckBox;
        chk->setChecked(enabled);
        m_colorTable->setCellWidget(row, 0, chk);

        m_colorTable->setItem(row, 1, new QTableWidgetItem(name));
        m_colorTable->setItem(row, 2, new QTableWidgetItem(QString::number(hLo)));
        m_colorTable->setItem(row, 3, new QTableWidgetItem(QString::number(hHi)));
        m_colorTable->setItem(row, 4, new QTableWidgetItem(QString::number(sLo)));
        m_colorTable->setItem(row, 5, new QTableWidgetItem(QString::number(sHi)));
        m_colorTable->setItem(row, 6, new QTableWidgetItem(QString::number(vLo)));
        m_colorTable->setItem(row, 7, new QTableWidgetItem(QString::number(vHi)));
    };

    insertRow(QStringLiteral("Red-Low"),  true, 0,   10,  120, 255, 120, 255);
    insertRow(QStringLiteral("Red-High"), true, 170, 180, 120, 255, 120, 255);
}

void CrosshairPage::addColorRow() {
    int row = m_colorTable->rowCount();
    m_colorTable->insertRow(row);

    auto* chk = new QCheckBox;
    chk->setChecked(true);
    m_colorTable->setCellWidget(row, 0, chk);

    m_colorTable->setItem(row, 1, new QTableWidgetItem(QStringLiteral("New")));
    for (int col = 2; col <= 7; ++col) {
        m_colorTable->setItem(row, col, new QTableWidgetItem(QStringLiteral("0")));
    }
}

void CrosshairPage::removeSelectedRows() {
    auto ranges = m_colorTable->selectedRanges();
    for (int i = ranges.size() - 1; i >= 0; --i) {
        for (int row = ranges[i].bottomRow(); row >= ranges[i].topRow(); --row) {
            m_colorTable->removeRow(row);
        }
    }
}
