#include "pages/TargetPage.h"
#include "widgets/CardWidget.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPushButton>
#include <QScrollArea>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

class ActionDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem&,
                          const QModelIndex&) const override {
        auto* combo = new QComboBox(parent);
        combo->addItems({
            QStringLiteral("删除"),
            QStringLiteral("过滤"),
            QStringLiteral("瞄准"),
        });
        return combo;
    }

    void setEditorData(QWidget* editor, const QModelIndex& index) const override {
        auto* combo = qobject_cast<QComboBox*>(editor);
        if (!combo) return;
        combo->setCurrentText(index.data(Qt::EditRole).toString());
    }

    void setModelData(QWidget* editor, QAbstractItemModel* model,
                      const QModelIndex& index) const override {
        auto* combo = qobject_cast<QComboBox*>(editor);
        if (!combo) return;
        model->setData(index, combo->currentText(), Qt::EditRole);
    }

    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option,
                              const QModelIndex&) const override {
        editor->setGeometry(option.rect);
    }
};

} // namespace

TargetPage::TargetPage(QWidget* parent)
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

    // ── Card 1: 目标分类 ──
    auto* card = new CardWidget(QStringLiteral("目标分类"),
                                QStringLiteral("target"));

    m_table = new QTableWidget(0, 3);
    m_table->setHorizontalHeaderLabels({
        QStringLiteral("类别 ID"),
        QStringLiteral("名称"),
        QStringLiteral("处理方式"),
    });
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setAlternatingRowColors(true);
    m_table->setItemDelegateForColumn(2, new ActionDelegate(m_table));

    card->contentLayout()->addWidget(m_table);

    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch();
    m_addBtn = new QPushButton(QStringLiteral("添加类别"));
    m_removeBtn = new QPushButton(QStringLiteral("删除选中"));
    m_removeBtn->setProperty("class", "danger");
    btnRow->addWidget(m_addBtn);
    btnRow->addWidget(m_removeBtn);
    card->contentLayout()->addLayout(btnRow);

    layout->addWidget(card);
    layout->addStretch();

    connect(m_addBtn, &QPushButton::clicked, this, &TargetPage::addRow);
    connect(m_removeBtn, &QPushButton::clicked, this, &TargetPage::removeSelectedRows);
}

void TargetPage::addRow() {
    int row = m_table->rowCount();
    m_table->insertRow(row);

    auto* idItem = new QTableWidgetItem(QString::number(row));
    idItem->setFlags(idItem->flags() & ~Qt::ItemIsEditable);
    m_table->setItem(row, 0, idItem);

    m_table->setItem(row, 1, new QTableWidgetItem(QStringLiteral("新类别")));
    m_table->setItem(row, 2, new QTableWidgetItem(QStringLiteral("瞄准")));
}

void TargetPage::removeSelectedRows() {
    auto selected = m_table->selectionModel()->selectedRows();
    std::sort(selected.begin(), selected.end(),
              [](const QModelIndex& a, const QModelIndex& b) {
                  return a.row() > b.row();
              });
    for (const auto& idx : selected) {
        m_table->removeRow(idx.row());
    }
}
