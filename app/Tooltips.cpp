#include "Tooltips.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QCursor>
#include <QEvent>
#include <QGuiApplication>
#include <QHelpEvent>
#include <QLabel>
#include <QScreen>
#include <QTimer>
#include <QToolTip>

namespace tooltips {

// ── TipWindow — tooltip-look window the manager fully controls ──

class TipWindow : public QLabel {
public:
    TipWindow()
        : QLabel(nullptr,
                 Qt::ToolTip | Qt::FramelessWindowHint
                     | Qt::WindowStaysOnTopHint
                     | Qt::WindowTransparentForInput
                     | Qt::WindowDoesNotAcceptFocus) {
        setAttribute(Qt::WA_ShowWithoutActivating, true);
        setTextFormat(Qt::RichText);
        setStyleSheet(
            "QLabel { background-color: palette(toolTipBase); "
            "color: palette(toolTipText); "
            "border: 1px solid palette(mid); padding: 4px; }");
    }
};

// ── PersistentTipManager — owns the tip window; hides it when the cursor
// leaves the anchor rect (polled — no reliance on Enter/Leave delivery) ──

class PersistentTipManager : public QObject {
public:
    static constexpr int POLL_MS = 80;

    PersistentTipManager() {
        timer_.setInterval(POLL_MS);
        connect(&timer_, &QTimer::timeout, this,
                &PersistentTipManager::poll);
    }

    void show(const QPoint& pos, const QString& text, QWidget* widget,
              const QRect& rect) {
        QToolTip::hideText();  // never show both kinds
        if (!tip_) tip_ = new TipWindow();
        TipWindow* tip = tip_;
        if (tip->text() != text) {
            tip->setText(text);
            tip->adjustSize();
            // Rich text with images needs a second pass — the first
            // adjustSize can measure before the document loads data URIs.
            tip->adjustSize();
        }
        anchor_widget_ = widget;
        anchor_rect_ = rect;
        has_anchor_ = true;
        tip->move(place(pos, tip->size()));
        if (!tip->isVisible()) tip->show();
        timer_.start();
    }

    void hide() {
        timer_.stop();
        has_anchor_ = false;
        anchor_widget_.clear();
        if (tip_ && tip_->isVisible()) tip_->hide();
    }

private:
    static QPoint place(const QPoint& pos, const QSize& size) {
        QScreen* screen = QGuiApplication::screenAt(pos);
        if (!screen) screen = QGuiApplication::primaryScreen();
        const QRect geo = screen->availableGeometry();
        QPoint p(pos.x() + 14, pos.y() + 20);
        if (p.x() + size.width() > geo.right())
            p.setX(qMax(geo.left(), pos.x() - 14 - size.width()));
        if (p.y() + size.height() > geo.bottom())
            p.setY(qMax(geo.top(), pos.y() - 12 - size.height()));
        return p;
    }

    void poll() {
        if (!has_anchor_) {
            hide();
            return;
        }
        QWidget* widget = anchor_widget_.data();
        const bool host_ok = widget && widget->isVisible();
        if (!host_ok
            || QGuiApplication::applicationState() != Qt::ApplicationActive
            || !anchor_rect_.contains(
                   widget->mapFromGlobal(QCursor::pos()))) {
            hide();
        }
    }

    TipWindow* tip_ = nullptr;
    QPointer<QWidget> anchor_widget_;
    QRect anchor_rect_;
    bool has_anchor_ = false;
    QTimer timer_;
};

static PersistentTipManager* mgr() {
    static PersistentTipManager* manager = new PersistentTipManager();
    return manager;
}

void show_persistent_tooltip(const QPoint& pos, const QString& text,
                             QWidget* widget, const QRect& rect) {
    mgr()->show(pos, text, widget, rect);
}

void hide_persistent_tooltip() {
    mgr()->hide();
    QToolTip::hideText();
}

// ── PersistentToolTipFilter ──
//
// Handles the two ways tooltips are declared in the toolkit:
//
// * item views (trees, tables, lists, combo dropdowns): the tooltip is
//   per-item Qt::ToolTipRole data, anchored to the item's visual rect — so
//   the tip follows row-to-row hovering and never times out within a row;
// * plain widgets (buttons, labels, line edits…): widget->toolTip()
//   anchored to the widget's own rect.
//
// Anything without a tooltip falls through to Qt's default handling
// (which cannot hide the manager's tip — different window).

bool PersistentToolTipFilter::eventFilter(QObject* obj, QEvent* ev) {
    if (ev->type() != QEvent::ToolTip || !obj->isWidgetType()) return false;
    QWidget* w = static_cast<QWidget*>(obj);
    auto* he = static_cast<QHelpEvent*>(ev);

    // Item views deliver ToolTip events to their viewport; the text lives
    // on the hovered item (ToolTipRole), not the widget.
    auto* view = qobject_cast<QAbstractItemView*>(w->parent());
    if (view && w == view->viewport()) {
        const QModelIndex index = view->indexAt(he->pos());
        const QString text =
            index.isValid() ? index.data(Qt::ToolTipRole).toString()
                            : QString();
        if (!text.isEmpty()) {
            show_persistent_tooltip(he->globalPos(), text, w,
                                    view->visualRect(index));
            return true;
        }
        return false;  // no per-item tip — let Qt do its thing
    }

    const QString text = w->toolTip();
    if (!text.isEmpty()) {
        show_persistent_tooltip(he->globalPos(), text, w, w->rect());
        return true;
    }
    return false;
}

PersistentToolTipFilter* install_persistent_tooltips(QApplication* app) {
    auto* filt = new PersistentToolTipFilter(app);
    app->installEventFilter(filt);
    return filt;
}

}  // namespace tooltips
