#include "pages/TargetPage.h"
#include "config/config_bridge.h"
#include "widgets/CardWidget.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "Apotheosis.h"
#include "config.h"
#include "runtime/active_hotkey.h"

struct BucketStyle {
    const char* label;
    const char* activeColor;
    const char* activeBorder;
    const char* hoverBg;
};

static const BucketStyle kBucketStyles[] = {
    {"\xe5\x88\xa0\xe9\x99\xa4", "#D25A5A", "#C04848", "rgba(210,90,90,0.08)"},
    {"\xe8\xbf\x87\xe6\xbb\xa4", "#C9A832", "#B89828", "rgba(201,168,50,0.08)"},
    {"\xe7\x9e\x84\xe5\x87\x86", "#3DAB5C", "#2E964D", "rgba(61,171,92,0.08)"},
};

TargetPage::TargetPage(QWidget* parent)
    : QWidget(parent)
{
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

    auto* infoCard = new CardWidget(
        QStringLiteral("\xe6\xb5\x81\xe7\xa8\x8b\xe8\xaf\xb4\xe6\x98\x8e"),
        QStringLiteral("info-circle"));
    auto* infoLabel = new QLabel(
        QStringLiteral(
            "YOLO \xe8\xbe\x93\xe5\x87\xba\xe4\xbc\x9a\xe6\x8c\x89\xe9\xa1\xba\xe5\xba\x8f\xe6\xb5\x81\xe7\xbb\x8f\xef\xbc\x9a"
            "\xe5\x88\xa0\xe9\x99\xa4 \xe2\x86\x92 \xe8\xbf\x87\xe6\xbb\xa4 \xe2\x86\x92 \xe7\x9e\x84\xe5\x87\x86\xe3\x80\x82\n"
            "\xe5\x88\xa0\xe9\x99\xa4\xef\xbc\x9a\xe6\xa3\x80\xe6\xb5\x8b\xe6\xa1\x86\xe7\x9b\xb4\xe6\x8e\xa5\xe4\xb8\xa2\xe5\xbc\x83\xe3\x80\x82"
            "\xe8\xbf\x87\xe6\xbb\xa4\xef\xbc\x9a\xe4\xbf\x9d\xe7\x95\x99\xe6\xa3\x80\xe6\xb5\x8b\xe6\xa1\x86\xe4\xbd\x86\xe4\xb8\x8d\xe7\x9e\x84\xe5\x87\x86\xe3\x80\x82"
            "\xe7\x9e\x84\xe5\x87\x86\xef\xbc\x9a\xe4\xbd\x9c\xe4\xb8\xba\xe7\x9e\x84\xe5\x87\x86\xe5\x80\x99\xe9\x80\x89\xef\xbc\x8c"
            "\xe5\x85\xb7\xe4\xbd\x93\xe4\xbc\x98\xe5\x85\x88\xe7\xba\xa7\xe7\x94\xb1\xe7\x83\xad\xe9\x94\xae\xe7\xbb\x84\xe7\x9a\x84\xe7\xb1\xbb\xe5\x88\xab\xe9\xa1\xba\xe5\xba\x8f\xe5\x86\xb3\xe5\xae\x9a\xe3\x80\x82"));
    infoLabel->setWordWrap(true);
    infoLabel->setStyleSheet("color:#71717A; font-size:12px;");
    infoCard->contentLayout()->addWidget(infoLabel);
    layout->addWidget(infoCard);

    auto* card = new CardWidget(
        QStringLiteral("\xe7\x9b\xae\xe6\xa0\x87\xe7\xb1\xbb\xe5\x88\xab"),
        QStringLiteral("target"));

    m_statusLabel = new QLabel;
    m_statusLabel->setStyleSheet("color:#A1A1AA; font-size:12px;");
    card->contentLayout()->addWidget(m_statusLabel);

    m_tableWidget = new QWidget;
    m_tableLayout = new QVBoxLayout(m_tableWidget);
    m_tableLayout->setContentsMargins(0, 0, 0, 0);
    m_tableLayout->setSpacing(6);
    card->contentLayout()->addWidget(m_tableWidget);

    layout->addWidget(card);
    layout->addStretch();

    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(500);
    connect(m_pollTimer, &QTimer::timeout, this, &TargetPage::refreshFromRuntime);
    m_pollTimer->start();

    refreshFromRuntime();
}

size_t TargetPage::computeFilterFingerprint()
{
    size_t h = 0;
    for (const auto& cf : config.class_filters) {
        h ^= std::hash<int>()(cf.class_id) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<std::string>()(cf.class_name) + 0x9e3779b9 + (h << 6) + (h >> 2);
    }
    return h;
}

void TargetPage::refreshFromRuntime()
{
    int count;
    size_t fp;
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        count = static_cast<int>(config.class_filters.size());
        fp = computeFilterFingerprint();
    }
    if (count != m_lastFilterCount || fp != m_lastFingerprint) {
        m_lastFingerprint = fp;
        rebuildTable();
        emit classFiltersChanged();
    }
}

void TargetPage::rebuildTable()
{
    while (m_tableLayout->count() > 0) {
        auto* item = m_tableLayout->takeAt(0);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    std::lock_guard<std::recursive_mutex> lk(configMutex);
    const int count = static_cast<int>(config.class_filters.size());
    m_lastFilterCount = count;

    if (count <= 0) {
        m_statusLabel->setText(
            QStringLiteral("\xe5\xb0\x9a\xe6\x9c\xaa\xe5\x8a\xa0\xe8\xbd\xbd\xe6\xa8\xa1\xe5\x9e\x8b\xe3\x80\x82"
                           "\xe5\x90\xaf\xe5\x8a\xa8\xe6\x8e\xa8\xe7\x90\x86\xe4\xbc\x9a\xe8\xaf\x9d\xe5\x90\x8e\xef\xbc\x8c"
                           "\xe7\xb1\xbb\xe5\x88\xab\xe5\x88\x97\xe8\xa1\xa8\xe4\xbc\x9a\xe8\x87\xaa\xe5\x8a\xa8\xe5\xa1\xab\xe5\x85\x85\xe3\x80\x82"));
        return;
    }

    m_statusLabel->setText(
        QStringLiteral("\xe6\xa8\xa1\xe5\x9e\x8b\xe7\xb1\xbb\xe5\x88\xab\xe6\x95\xb0\xe9\x87\x8f\xef\xbc\x9a%1").arg(count));

    for (int i = 0; i < count; ++i) {
        auto& cf = config.class_filters[i];

        auto* row = new QWidget;
        row->setStyleSheet(
            "QWidget#classRow { background:#FAFAFA; border:1px solid #E8E8EC; border-radius:8px; }");
        row->setObjectName("classRow");
        auto* rowLay = new QHBoxLayout(row);
        rowLay->setContentsMargins(12, 8, 8, 8);
        rowLay->setSpacing(10);

        auto* idLabel = new QLabel(QStringLiteral("[%1]").arg(cf.class_id));
        idLabel->setFixedWidth(36);
        idLabel->setStyleSheet("color:#71717A; font-size:12px; font-weight:600;");
        rowLay->addWidget(idLabel);

        QString displayName = cf.class_name.empty()
            ? QStringLiteral("class_%1").arg(cf.class_id)
            : QString::fromUtf8(cf.class_name.c_str());
        auto* nameLabel = new QLabel(displayName);
        nameLabel->setStyleSheet("color:#3C3C44; font-size:13px; font-weight:500;");
        rowLay->addWidget(nameLabel, 1);

        auto* segWidget = new QWidget;
        segWidget->setStyleSheet(
            "QWidget#seg { background:#F0F0F3; border-radius:6px; }");
        segWidget->setObjectName("seg");
        auto* segLay = new QHBoxLayout(segWidget);
        segLay->setContentsMargins(3, 3, 3, 3);
        segLay->setSpacing(3);

        auto btnGroup = std::make_shared<std::array<QPushButton*, 3>>();
        const int classId = cf.class_id;
        const int currentBucket = static_cast<int>(cf.bucket);

        auto styleBtn = [](QPushButton* btn, int bucketIdx, bool active) {
            if (active) {
                btn->setStyleSheet(
                    QStringLiteral(
                        "QPushButton{background:%1; color:white; border:none;"
                        " border-radius:5px; font-size:12px; font-weight:600; padding:2px 10px;}")
                        .arg(kBucketStyles[bucketIdx].activeColor));
            } else {
                btn->setStyleSheet(
                    QStringLiteral(
                        "QPushButton{background:transparent; color:#71717A; border:none;"
                        " border-radius:5px; font-size:12px; padding:2px 10px;}"
                        "QPushButton:hover{background:%1; color:%2;}")
                        .arg(kBucketStyles[bucketIdx].hoverBg)
                        .arg(kBucketStyles[bucketIdx].activeColor));
            }
        };

        for (int b = 0; b < 3; ++b) {
            auto* btn = new QPushButton(QString::fromUtf8(kBucketStyles[b].label));
            btn->setFixedHeight(28);
            btn->setMinimumWidth(52);
            btn->setCursor(Qt::PointingHandCursor);
            btn->setCheckable(true);
            btn->setChecked(b == currentBucket);
            (*btnGroup)[b] = btn;

            styleBtn(btn, b, b == currentBucket);
            segLay->addWidget(btn);

            connect(btn, &QPushButton::clicked, this, [this, classId, b, btnGroup, styleBtn]() {
                for (int k = 0; k < 3; ++k) {
                    (*btnGroup)[k]->blockSignals(true);
                    (*btnGroup)[k]->setChecked(k == b);
                    (*btnGroup)[k]->blockSignals(false);
                    styleBtn((*btnGroup)[k], k, k == b);
                }

                {
                    std::lock_guard<std::recursive_mutex> lk2(configMutex);
                    for (auto& cf2 : config.class_filters) {
                        if (cf2.class_id == classId) {
                            cf2.bucket = static_cast<ClassBucket>(b);
                            break;
                        }
                    }
                }
                ConfigBridge::instance().markDirty();
                emit classFiltersChanged();
            });
        }

        rowLay->addWidget(segWidget);
        m_tableLayout->addWidget(row);
    }
}
