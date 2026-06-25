#include "pages/LinksPage.h"

#include <QColor>
#include <QDesktopServices>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QScrollArea>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>

namespace {

// ── 可点击瓦片(QFrame + std::function 回调)──────────────────────────
// 无信号槽,靠重写 mouseReleaseEvent 触发,不需要 MOC,可直接放在 .cpp 里。
class LinkTile : public QFrame {
public:
    std::function<void()> onClick;

protected:
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton && rect().contains(e->pos()) && onClick)
            onClick();
        QFrame::mouseReleaseEvent(e);
    }
};

// 按名字稳定地挑一个角标底色,让瓦片有区分度(不是一片死板的紫)。
QColor badgeColor(const QString& key) {
    static const QColor kPalette[] = {
        QColor(0x5E, 0x6A, 0xD2), QColor(0x2B, 0xB0, 0xA6),
        QColor(0xE0, 0x88, 0x3A), QColor(0xC2, 0x55, 0x7A),
        QColor(0x7A, 0x6F, 0xF0), QColor(0x3D, 0x8B, 0xD4),
    };
    constexpr int kN = 6;
    unsigned h = 0;
    for (const QChar c : key)
        h = h * 31u + c.unicode();
    return kPalette[h % kN];
}

}  // namespace

LinksPage::LinksPage(QWidget* parent)
    : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* container = new QWidget();
    auto* col = new QVBoxLayout(container);
    col->setContentsMargins(24, 24, 24, 24);
    col->setSpacing(16);

    // 上方留白 → 整组瓦片垂直居中。
    col->addStretch(1);

    // 居中小标题(不想要可删这一段)。
    auto* title = new QLabel(QStringLiteral("快速跳转"));
    title->setAlignment(Qt::AlignHCenter);
    title->setStyleSheet(QStringLiteral("color:#52525B;font-size:15px;font-weight:600;"));
    col->addWidget(title);
    col->addSpacing(6);

    // 瓦片行:左右各一个弹簧 → 整行水平居中。
    auto* rowWrap = new QHBoxLayout();
    rowWrap->addStretch(1);
    auto* row = new QHBoxLayout();
    row->setSpacing(18);
    rowWrap->addLayout(row);
    rowWrap->addStretch(1);
    col->addLayout(rowWrap);

    col->addStretch(1);

    // ╔═══════════════════════ 名字字体(自己改)═══════════════════════╗
    //   改字体名 / 字号 / 是否加粗即可,所有瓦片主标题统一用这个字体。
    QFont nameFont(QStringLiteral("Microsoft YaHei"));
    nameFont.setPointSize(12);
    nameFont.setBold(true);
    // ╚════════════════════════════════════════════════════════════════╝

    // 一个瓦片 = 一次 addLink(显示名字, 点击跳转的 URL)。
    auto addLink = [&](const QString& label, const QString& url) {
        auto* tile = new LinkTile();
        tile->setObjectName(QStringLiteral("linkTile"));
        tile->setCursor(Qt::PointingHandCursor);
        tile->setFixedSize(160, 184);
        tile->setStyleSheet(QStringLiteral(
            "#linkTile{background:#FAFBFD;border:1px solid #E6E7EB;border-radius:16px;}"
            "#linkTile:hover{background:#FFFFFF;border:1px solid #5E6AD2;}"));
        tile->onClick = [url] {
            if (!url.isEmpty())
                QDesktopServices::openUrl(QUrl(url));
        };

        auto* tc = new QVBoxLayout(tile);
        tc->setContentsMargins(10, 16, 10, 16);
        tc->setSpacing(9);
        tc->addStretch(1);

        // 大角标:首字母 + 按名字派生的底色。
        auto* badge = new QLabel(label.left(1).toUpper());
        badge->setFixedSize(66, 66);
        badge->setAlignment(Qt::AlignCenter);
        badge->setAttribute(Qt::WA_TransparentForMouseEvents);
        badge->setStyleSheet(QStringLiteral(
            "background:%1;color:#FFFFFF;border-radius:18px;font-size:26px;font-weight:700;")
            .arg(badgeColor(label).name()));
        tc->addWidget(badge, 0, Qt::AlignHCenter);

        tc->addSpacing(2);

        // 名字(用户字体)+ 主机名(灰色小字),均居中。
        auto* name = new QLabel(label);
        name->setAlignment(Qt::AlignHCenter);
        name->setAttribute(Qt::WA_TransparentForMouseEvents);
        name->setFont(nameFont);
        name->setStyleSheet(QStringLiteral("color:#1A1A1F;background:transparent;"));
        tc->addWidget(name);

        auto* host = new QLabel(QUrl(url).host());
        host->setAlignment(Qt::AlignHCenter);
        host->setAttribute(Qt::WA_TransparentForMouseEvents);
        host->setStyleSheet(QStringLiteral("color:#9A9AA3;font-size:11px;background:transparent;"));
        tc->addWidget(host);

        tc->addStretch(1);
        row->addWidget(tile);
    };

    // ╔═══════════════════════ 站点列表(自己改)═══════════════════════╗
    //   每行一个:addLink(显示名字, 跳转网址);
    //   加一行就多一个瓦片,删一行就少一个,顺序即显示顺序。
    addLink(QStringLiteral("rouman5"), QStringLiteral("https://rouman5.com"));
    addLink(QStringLiteral("javdb"), QStringLiteral("https://javdb.com/"));
    addLink(QStringLiteral("18comic"), QStringLiteral("https://18comic.vip"));
    addLink(QStringLiteral("pornhub"), QStringLiteral("https://cn.pornhub.com/"));
    addLink(QStringLiteral("51吃瓜"), QStringLiteral("https://chigua.com/"));
    // ╚════════════════════════════════════════════════════════════════╝

    scroll->setWidget(container);
    root->addWidget(scroll);
}
