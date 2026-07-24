#pragma once

#include <QEvent>
#include <QLayout>
#include <QSize>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QTimer>

// A QStackedWidget that reports only the CURRENT page's size hint instead of the
// maximum over all pages. Lets a container shrink/grow to fit the active page's
// content (e.g. a mover's parameter list) rather than permanently reserving the
// height of the largest page. No Q_OBJECT / new signals -> no MOC needed.
class AdaptiveStack : public QStackedWidget {
public:
    explicit AdaptiveStack(QWidget* parent = nullptr) : QStackedWidget(parent) {
        setFrameShape(QFrame::NoFrame);
        setAutoFillBackground(false);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        connect(this, &QStackedWidget::currentChanged,
                this, [this](int) { refreshGeometry(); });
    }

    // 当前页内部控件显隐时也可主动调用，避免外层继续保留旧页面高度。
    void refreshGeometry() {
        if (QWidget* page = currentWidget()) {
            if (QLayout* pageLayout = page->layout()) {
                pageLayout->invalidate();
                pageLayout->activate();
            }
            page->updateGeometry();
        }
        syncCurrentPageHeight();
        updateGeometry();
        if (QWidget* parent = parentWidget())
            parent->updateGeometry();

        // currentChanged 触发时新页面可能尚未完成布局，下一事件循环再校准一次。
        QTimer::singleShot(0, this, [this] {
            if (QWidget* page = currentWidget()) {
                if (QLayout* pageLayout = page->layout()) {
                    pageLayout->invalidate();
                    pageLayout->activate();
                }
                page->updateGeometry();
            }
            syncCurrentPageHeight();
            updateGeometry();
            if (QWidget* parent = parentWidget()) {
                parent->updateGeometry();
                if (QLayout* parentLayout = parent->layout()) {
                    parentLayout->invalidate();
                    parentLayout->activate();
                }
            }
        });
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

    bool hasHeightForWidth() const override {
        if (QWidget* w = currentWidget())
            return w->hasHeightForWidth();
        return QStackedWidget::hasHeightForWidth();
    }

    int heightForWidth(int width) const override {
        if (QWidget* w = currentWidget())
            return w->hasHeightForWidth() ? w->heightForWidth(width) : w->sizeHint().height();
        return QStackedWidget::heightForWidth(width);
    }

protected:
    bool event(QEvent* event) override {
        const bool handled = QStackedWidget::event(event);
        if (event->type() == QEvent::LayoutRequest) {
            syncCurrentPageHeight();
            updateGeometry();
        }
        return handled;
    }

private:
    void syncCurrentPageHeight() {
        QWidget* page = currentWidget();
        if (!page)
            return;

        // QStackedWidget 的内部布局会默认以“最大页面”作为最小高度。仅重写
        // sizeHint 仍可能让父布局保留旧高度，因此把容器高度明确同步到当前页。
        int height = page->sizeHint().height();
        if (QLayout* pageLayout = page->layout())
            height = qMax(height, pageLayout->sizeHint().height());
        height = qMax(0, height);

        if (minimumHeight() != height)
            setMinimumHeight(height);
        if (maximumHeight() != height)
            setMaximumHeight(height);
    }
};
