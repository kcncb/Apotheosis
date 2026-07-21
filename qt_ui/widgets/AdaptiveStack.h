#pragma once

#include <QSize>
#include <QStackedWidget>

// A QStackedWidget that reports only the CURRENT page's size hint instead of the
// maximum over all pages. Lets a container shrink/grow to fit the active page's
// content (e.g. a mover's parameter list) rather than permanently reserving the
// height of the largest page. No Q_OBJECT / new signals -> no MOC needed.
class AdaptiveStack : public QStackedWidget {
public:
    explicit AdaptiveStack(QWidget* parent = nullptr) : QStackedWidget(parent) {
        // When the visible page changes, invalidate our geometry so the parent
        // layout re-queries sizeHint() and resizes us to the new page.
        connect(this, &QStackedWidget::currentChanged,
                this, [this](int) { updateGeometry(); });
    }

    QSize sizeHint() const override {
        if (QWidget* w = currentWidget())
            return w->sizeHint();
        return QStackedWidget::sizeHint();
    }

    QSize minimumSizeHint() const override {
        if (QWidget* w = currentWidget())
            return w->minimumSizeHint();
        return QStackedWidget::minimumSizeHint();
    }
};
