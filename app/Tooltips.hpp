// Tooltips.hpp — persistent tooltips (port of gui/tooltips.py).
//
// Keep every tooltip up while the cursor is on its field. Qt's native
// QTipLabel hides on an expire timer and on a tangle of internal triggers
// (Leave events, window activation, its own window appearing near the
// cursor — empirically it dies ~300 ms after showing even with a 12 h
// duration and a binding rect). So the toolkit owns the tooltip window:
//
// * TipWindow — a frameless, always-on-top, input-transparent rich-text
//   label. Input transparency means the OS never hit-tests it, so it can
//   sit at the cursor without stealing Enter/Leave events from the widget
//   underneath.
// * PersistentTipManager — shows the tip and polls the cursor: the tip
//   stays up exactly as long as the cursor is inside the field's rect, and
//   hides the moment it leaves (or the host widget vanishes, or the app
//   deactivates).
// * PersistentToolTipFilter — an application-wide event filter (installed
//   once in main()) that re-routes every static tooltip (setToolTip on
//   widgets, Qt::ToolTipRole on item-view items / combo dropdown rows)
//   through the manager. Manual hover tooltips call
//   show_persistent_tooltip directly.
#pragma once

#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QRect>

class QApplication;
class QWidget;

namespace tooltips {

// Show `text` near global `pos`, kept visible while the cursor stays
// inside `rect` (in `widget`'s coordinates).
void show_persistent_tooltip(const QPoint& pos, const QString& text,
                             QWidget* widget, const QRect& rect);

// Hide both the persistent tip and any stock QToolTip.
void hide_persistent_tooltip();

// App-wide filter that routes static tooltips through the manager.
class PersistentToolTipFilter : public QObject {
    Q_OBJECT
public:
    explicit PersistentToolTipFilter(QObject* parent = nullptr)
        : QObject(parent) {}
    bool eventFilter(QObject* obj, QEvent* ev) override;
};

// Install the filter on the QApplication; returns it (owned by the app).
PersistentToolTipFilter* install_persistent_tooltips(QApplication* app);

}  // namespace tooltips
