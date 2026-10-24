#include "atlifm-core/XdgUtils.hpp"
#include <QApplication>
#include <QCoreApplication>
#include <QFontDatabase>
#include <QIcon>
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QFile>
#include <QPalette>
#include <QStyleFactory>
#include <QUrl>
#include <filesystem>
#include <cstdlib>
#include "atlifm-theme/Theme.hpp"
#include "atlifm-core/UserConfig.hpp"
#include "atlifm-portal/FileManagerService.hpp"
#include "atlifm-portal/PortalService.hpp"
#include "atlifm-widgets/FirstRunDialog.hpp"
#include "atlifm-widgets/MainWindow.hpp"
#include <cstring>

namespace {

bool applyDefaultSetup() {
    const bool portal = Portal::PortalService::setupPortalEnvironment();
    const bool fileManager = Portal::FileManagerService::setup();
    return portal && fileManager;
}

}

int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--file-manager") == 0) {
            QCoreApplication app(argc, argv);
            QCoreApplication::setApplicationName("atlifm");
            QCoreApplication::setApplicationVersion("1.0.2");
            QCoreApplication::setOrganizationName("atlifm");
            Portal::FileManagerService service;
            return service.start() ? app.exec() : 1;
        }
    }

    QCoreApplication::setApplicationName("atlifm");
    QCoreApplication::setApplicationVersion("1.0.2");
    QCoreApplication::setOrganizationName("atlifm");

    // омархи ставит gtk тему глобально а мы рисуем все через fusion и свою палитру
    // так что лишняя память не тратится если только явно не попросили через переменную
    if (!qEnvironmentVariableIsSet("ATLIFM_GTK_THEME")) {
        qputenv("QT_QPA_PLATFORMTHEME", QByteArray());
    }

    QApplication app(argc, argv);
    // значок окна для wayland и иксов
    app.setWindowIcon(QIcon(QStringLiteral(":/atlifm.png")));
    qAddPostRoutine(Core::XdgUtils::cleanupSudoSession);

    app.setStyle(QStyleFactory::create("Fusion"));

    // дефолтный конфиг если первый запуск
    Core::UserConfig::ensureDefaultConfigs();

    Theme::reloadPalette();
    const auto& theme = Theme::palette();

    QPalette pal = app.palette();
    pal.setColor(QPalette::Window, theme.base);
    pal.setColor(QPalette::WindowText, theme.text);
    pal.setColor(QPalette::Base, theme.base);
    pal.setColor(QPalette::AlternateBase, theme.mantle);
    pal.setColor(QPalette::Text, theme.text);
    pal.setColor(QPalette::Button, theme.mantle);
    pal.setColor(QPalette::ButtonText, theme.text);
    pal.setColor(QPalette::Highlight, theme.accent);
    pal.setColor(QPalette::HighlightedText, theme.accentFg);
    app.setPalette(pal);

    QFont appFont = Theme::getMonospaceFont(10);
    app.setFont(appFont);

    // вырубаем анимации потому что в qt они лагают
    app.setEffectEnabled(Qt::UI_AnimateMenu, false);
    app.setEffectEnabled(Qt::UI_FadeMenu, false);
    app.setEffectEnabled(Qt::UI_AnimateCombo, false);
    app.setEffectEnabled(Qt::UI_AnimateTooltip, false);
    app.setEffectEnabled(Qt::UI_FadeTooltip, false);
    app.setEffectEnabled(Qt::UI_AnimateToolBox, false);

    app.setStyleSheet(Theme::getApplicationStyleSheet());

    QCommandLineParser parser;
    parser.setApplicationDescription("atlifm: Awesome Terminal-Like Interface File Manager");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("directory", "Initial directory to open", "[directory]");
    QCommandLineOption portalOption(
        QStringLiteral("portal"),
        QStringLiteral("Run as XDG desktop portal FileChooser backend (D-Bus activatable)"));
    parser.addOption(portalOption);
    QCommandLineOption setupPortalOption(
        QStringLiteral("setup-portal"),
        QStringLiteral("install and configure atlifm as the default filechooser portal backend"));
    parser.addOption(setupPortalOption);
    QCommandLineOption fileManagerOption(
        QStringLiteral("file-manager"),
        QStringLiteral("run as the org.freedesktop.filemanager1 dbus service"));
    parser.addOption(fileManagerOption);
    QCommandLineOption setupFileManagerOption(
        QStringLiteral("setup-file-manager"),
        QStringLiteral("install atlifm as the default file manager for the current user"));
    parser.addOption(setupFileManagerOption);
    QCommandLineOption setDefaultOption(
        QStringLiteral("setdefault"),
        QStringLiteral("install atlifm as the default file manager and filechooser portal backend"));
    parser.addOption(setDefaultOption);
    QCommandLineOption propertiesOption(
        QStringLiteral("properties"),
        QStringLiteral("open the properties panel for the given path"),
        QStringLiteral("path"));
    parser.addOption(propertiesOption);

    parser.process(app);

    if (parser.isSet(setDefaultOption)) {
        if (applyDefaultSetup()) {
            QTextStream(stdout) << QStringLiteral("atlifm is now the default file manager and file picker\n");
            return 0;
        }
        QTextStream(stderr) << QStringLiteral("failed to configure atlifm\n");
        return 1;
    }

    // портал xdg и отдельное имя чтобы hyprland открывал окно во флоате
    if (parser.isSet(setupPortalOption)) {
        if (Portal::PortalService::setupPortalEnvironment()) {
            QTextStream(stdout) << QStringLiteral("atlifm portal configured as default file chooser successfully\n");
            return 0;
        }
        return 1;
    }

    if (parser.isSet(portalOption)) {
        QGuiApplication::setDesktopFileName(QStringLiteral("atlifm-portal"));
        app.setQuitOnLastWindowClosed(false);
        Portal::PortalService service;
        if (!service.start()) return 1;
        return app.exec();
    }

    if (parser.isSet(setupFileManagerOption)) {
        if (Portal::FileManagerService::setup()) {
            QTextStream(stdout) << QStringLiteral("atlifm configured as the default file manager successfully\n");
            return 0;
        }
        return 1;
    }

    const QStringList args = parser.positionalArguments();
    const QString propertiesArg = parser.value(propertiesOption);
    std::filesystem::path initialPath;
    std::filesystem::path propertiesPath;

    // имя окна для правил wm
    QGuiApplication::setDesktopFileName(QStringLiteral("atlifm"));

    QString startArg = !args.isEmpty() ? args.first() : propertiesArg;
    if (!startArg.isEmpty()) {
        QString arg = startArg;
        if (arg.startsWith("file://")) {
            arg = QUrl(arg).toLocalFile();
        }
        std::string p = arg.toUtf8().toStdString();
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) {
            // если передан файл то прыгаем в его каталог и выбираем его
            initialPath = std::filesystem::canonical(p, ec);
        }
    }
    if (!propertiesArg.isEmpty()) {
        QString arg = propertiesArg;
        if (arg.startsWith("file://")) arg = QUrl(arg).toLocalFile();
        propertiesPath = std::filesystem::path(arg.toUtf8().toStdString());
    }

    bool firstRun = Theme::uiConfig().firstRun;
    const QString legacyMarker = Theme::configDir() + QStringLiteral("/first-run.done");
    if (QFile::exists(legacyMarker)) {
        Theme::setConfigValue(QStringLiteral("first_run"), QStringLiteral("false"));
        QFile::remove(legacyMarker);
        firstRun = false;
    }
    if (firstRun) {
        Widgets::FirstRunDialog dialog;
        if (dialog.exec() == QDialog::Accepted) applyDefaultSetup();
        Theme::setConfigValue(QStringLiteral("first_run"), QStringLiteral("false"));
    }

    Widgets::MainWindow window(initialPath);
    window.show();
    if (!propertiesPath.empty()) window.showPropertiesOverlay(propertiesPath);

    return app.exec();
}
