#include "atlifm-core/XdgUtils.hpp"
#include <QApplication>
#include <QFontDatabase>
#include <QIcon>
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QPalette>
#include <QStyleFactory>
#include <QUrl>
#include <filesystem>
#include <cstdlib>
#include "atlifm-theme/Theme.hpp"
#include "atlifm-core/UserConfig.hpp"
#include "atlifm-portal/PortalService.hpp"
#include "atlifm-widgets/MainWindow.hpp"

int main(int argc, char* argv[]) {
    QCoreApplication::setApplicationName("atlifm");
    QCoreApplication::setApplicationVersion("1.0.1");
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
        QStringLiteral("Install and configure atlifm as the default XDG FileChooser portal backend"));
    parser.addOption(setupPortalOption);


    parser.process(app);

    // портал xdg и отдельное имя чтобы hyprland открывал окно во флоате
        if (parser.isSet(setupPortalOption)) {
        if (Portal::PortalService::setupPortalEnvironment()) {
            QTextStream(stdout) << QStringLiteral("atlifm portal configured as default file chooser successfully.\n");
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

    const QStringList args = parser.positionalArguments();
    std::filesystem::path initialPath;

    // имя окна для правил wm
    QGuiApplication::setDesktopFileName(QStringLiteral("atlifm"));

    if (!args.isEmpty()) {
        QString arg = args.first();
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

    Widgets::MainWindow window(initialPath);
    window.show();

    return app.exec();
}
