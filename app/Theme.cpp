#include "Theme.hpp"

#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QStyleFactory>

namespace theme {

static QColor c(const char* hex) { return QColor(hex); }

void apply(QApplication& app) {
    app.setStyle(QStyleFactory::create("Fusion"));

    QPalette p;
    p.setColor(QPalette::Window, c(WINDOW_BG));
    p.setColor(QPalette::WindowText, c(TEXT));
    p.setColor(QPalette::Base, c(BASE_BG));
    p.setColor(QPalette::AlternateBase, c(ALT_BASE_BG));
    p.setColor(QPalette::Text, c(TEXT));
    p.setColor(QPalette::Button, c(PANEL_BG));
    p.setColor(QPalette::ButtonText, c(TEXT));
    p.setColor(QPalette::BrightText, QColor("#FFFFFF"));
    p.setColor(QPalette::Highlight, c(ACCENT));
    p.setColor(QPalette::HighlightedText, QColor("#FFFFFF"));
    p.setColor(QPalette::ToolTipBase, c(TOOLTIP_BG));
    p.setColor(QPalette::ToolTipText, c(TEXT));
    p.setColor(QPalette::Link, c(ACCENT_HOVER));
    p.setColor(QPalette::Mid, c(BORDER));
    p.setColor(QPalette::Dark, QColor("#0A100D"));
    p.setColor(QPalette::Light, QColor("#24382E"));
    p.setColor(QPalette::PlaceholderText, c(DIM_TEXT));
    p.setColor(QPalette::Disabled, QPalette::Text, c(DIM_TEXT));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, c(DIM_TEXT));
    p.setColor(QPalette::Disabled, QPalette::WindowText, c(DIM_TEXT));
    app.setPalette(p);

    // Square corners everywhere — no border-radius on any control.
    const QString qss = QString(R"(
QWidget { font-size: 9pt; }

QPushButton {
    background: %PANEL%;
    color: %TEXT%;
    border: 1px solid %BORDER%;
    border-radius: 0;
    padding: 4px 12px;
}
QPushButton:hover { background: #223428; border-color: %ACCENT%; }
QPushButton:pressed { background: %ACCENT_DOWN%; }
QPushButton:checked { background: %ACCENT%; color: #FFFFFF; border-color: %ACCENT%; }
QPushButton:disabled { color: %DIM%; background: #16211B; border-color: #22322A; }
QPushButton:default { border: 1px solid %ACCENT%; }

QLineEdit, QPlainTextEdit, QTextEdit, QSpinBox, QDoubleSpinBox, QComboBox {
    background: %BASE%;
    color: %TEXT%;
    border: 1px solid %BORDER%;
    border-radius: 0;
    padding: 2px 4px;
    selection-background-color: %ACCENT%;
}
QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus,
QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus { border-color: %ACCENT%; }
QComboBox::drop-down { border: none; width: 18px; }
QComboBox QAbstractItemView {
    background: %BASE%; color: %TEXT%;
    border: 1px solid %BORDER%;
    selection-background-color: %ACCENT%;
}

QTabWidget::pane { border: 1px solid %BORDER%; top: -1px; }
QTabBar::tab {
    background: %PANEL%;
    color: %DIM%;
    border: 1px solid %BORDER%;
    border-bottom: none;
    border-radius: 0;
    padding: 5px 14px;
    margin-right: 1px;
}
QTabBar::tab:selected {
    background: %WINDOW%;
    color: %TEXT%;
    border-top: 2px solid %ACCENT%;
}
QTabBar::tab:hover:!selected { background: #223428; color: %TEXT%; }

QTreeView, QTableView, QTableWidget, QListView, QListWidget {
    background: %BASE%;
    alternate-background-color: %ALT%;
    color: %TEXT%;
    border: 1px solid %BORDER%;
    selection-background-color: %ACCENT%;
    selection-color: #FFFFFF;
}
QHeaderView::section {
    background: %PANEL%;
    color: %TEXT%;
    border: none;
    border-right: 1px solid %BORDER%;
    border-bottom: 1px solid %BORDER%;
    padding: 3px 6px;
}
QTableCornerButton::section { background: %PANEL%; border: 1px solid %BORDER%; }

QMenuBar { background: %WINDOW%; color: %TEXT%; }
QMenuBar::item:selected { background: %ACCENT%; color: #FFFFFF; }
QMenu { background: %BASE%; color: %TEXT%; border: 1px solid %BORDER%; }
QMenu::item:selected { background: %ACCENT%; color: #FFFFFF; }
QMenu::separator { height: 1px; background: %BORDER%; margin: 3px 6px; }

QStatusBar { background: %PANEL%; color: %TEXT%; border-top: 1px solid %BORDER%; }
QStatusBar::item { border: none; }

QGroupBox {
    border: 1px solid %BORDER%;
    border-radius: 0;
    margin-top: 8px;
    padding-top: 4px;
}
QGroupBox::title {
    subcontrol-origin: margin;
    left: 6px;
    padding: 0 3px;
    color: %ACCENT_HOVER%;
}

QCheckBox, QRadioButton { color: %TEXT%; spacing: 6px; }
QCheckBox::indicator, QRadioButton::indicator {
    width: 13px; height: 13px;
    border: 1px solid %BORDER%;
    background: %BASE%;
}
QCheckBox::indicator:checked, QRadioButton::indicator:checked {
    background: %ACCENT%;
    border-color: %ACCENT%;
}
QCheckBox::indicator:hover, QRadioButton::indicator:hover { border-color: %ACCENT%; }

QProgressBar {
    background: %BASE%;
    border: 1px solid %BORDER%;
    border-radius: 0;
    text-align: center;
    color: %TEXT%;
}
QProgressBar::chunk { background: %ACCENT%; }

QScrollBar:vertical { background: %WINDOW%; width: 12px; margin: 0; }
QScrollBar:horizontal { background: %WINDOW%; height: 12px; margin: 0; }
QScrollBar::handle {
    background: #2E463A;
    border: 1px solid %BORDER%;
    min-height: 20px; min-width: 20px;
}
QScrollBar::handle:hover { background: %ACCENT%; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }

QSplitter::handle { background: %BORDER%; }
QSplitter::handle:hover { background: %ACCENT%; }

QSlider::groove:horizontal {
    height: 4px; background: %BASE%; border: 1px solid %BORDER%;
}
QSlider::handle:horizontal {
    width: 10px; margin: -5px 0;
    background: %ACCENT%;
    border: 1px solid %ACCENT_DOWN%;
    border-radius: 0;
}
QSlider::handle:horizontal:hover { background: %ACCENT_HOVER%; }

QToolTip {
    background: %TIP%;
    color: %TEXT%;
    border: 1px solid %ACCENT%;
    padding: 4px;
}
)");

    QString sheet = qss;
    sheet.replace("%WINDOW%", WINDOW_BG);
    sheet.replace("%BASE%", BASE_BG);
    sheet.replace("%ALT%", ALT_BASE_BG);
    sheet.replace("%PANEL%", PANEL_BG);
    sheet.replace("%BORDER%", BORDER);
    sheet.replace("%ACCENT_HOVER%", ACCENT_HOVER);
    sheet.replace("%ACCENT_DOWN%", ACCENT_DOWN);
    sheet.replace("%ACCENT%", ACCENT);
    sheet.replace("%TEXT%", TEXT);
    sheet.replace("%DIM%", DIM_TEXT);
    sheet.replace("%TIP%", TOOLTIP_BG);
    app.setStyleSheet(sheet);
}

}  // namespace theme
