// main.cpp — application bootstrap (port of app.py): creates QApplication
// and shows the main window.

#include <QApplication>
#include <QFileInfo>
#include <QIcon>

#ifdef Q_OS_WIN
#include <shobjidl.h>
#endif

#include "MainWindow.hpp"
#include "Theme.hpp"
#include "Tooltips.hpp"

// Application icon — prefer the multi-size .ico (crisp Windows taskbar /
// title bar), fall back to the 256px .png elsewhere. Both live in the
// assets/ folder next to the executable.
static QIcon app_icon() {
    const QString assets =
        QCoreApplication::applicationDirPath() + QStringLiteral("/assets");
    const QString ico = assets + QStringLiteral("/app_icon.ico");
    const QString png = assets + QStringLiteral("/app_icon.png");
#ifdef Q_OS_WIN
    if (QFileInfo::exists(ico)) return QIcon(ico);
#endif
    return QIcon(png);
}

int main(int argc, char** argv) {
#ifdef Q_OS_WIN
    // Detach from any generic taskbar identity so Windows shows our own
    // icon (and groups our windows separately).
    SetCurrentProcessExplicitAppUserModelID(L"JadeModding.JadeExplorer");
#endif

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Jade Explorer"));
    app.setOrganizationName(QStringLiteral("JadeModding"));
    app.setWindowIcon(app_icon());
    theme::apply(app);
    // Keep every tooltip up while the cursor is on its field (image
    // tooltips otherwise expire mid-read). Owned by the app.
    tooltips::install_persistent_tooltips(&app);

    MainWindow window;
    window.show();

    // If a .bf path was passed as argument, open it
    const QStringList args = app.arguments();
    if (args.size() > 1 && args.at(1).endsWith(QStringLiteral(".bf")))
        window.load_bf(args.at(1));

    return app.exec();
}
