#include <QProcess>
#include <QCoreApplication>
#include "PortalService.hpp"

#include "../atlifm-theme/Theme.hpp"
#include "../atlifm-widgets/FileChooserDialog.hpp"

#include <QApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusError>
#include <QDBusReply>
#include <QDir>
#include <QEvent>
#include <QGuiApplication>
#include <QScreen>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>
#include <algorithm>
#include <filesystem>

namespace Portal {

namespace {

// "ay" из options (current_folder/current_file) это байтовая строка с NUL в конце
QString stringFromVariant(const QVariant& value) {
    if (!value.isValid()) return {};
    const auto id = value.metaType().id();
    if (id == QMetaType::QString) return value.toString();

    QByteArray raw;
    if (id == QMetaType::QByteArray) {
        raw = value.toByteArray();
    } else if (id == qMetaTypeId<QDBusArgument>()) {
        const QDBusArgument arg = value.value<QDBusArgument>();
        // читать как массив можно только ровно "ay",
        // иной тип даст мусор или рассинхронизацию потока аргументов
        if (arg.currentSignature() != QLatin1String("ay")) return {};
        arg.beginArray();
        while (!arg.atEnd()) {
            uchar byte = 0;
            arg >> byte;
            raw.append(char(byte));
        }
        arg.endArray();
    } else {
        return {};
    }
    const int nul = raw.indexOf('\0');
    if (nul >= 0) raw.truncate(nul);
    return QString::fromUtf8(raw);
}

std::filesystem::path resolveStartDirectory(const QVariantMap& options) {
    for (const char* key : {"current_file", "current_folder"}) {
        const QString value = stringFromVariant(options.value(QLatin1String(key)));
        if (value.isEmpty()) continue;
        std::error_code ec;
        const std::filesystem::path path(value.toUtf8().toStdString());
        if (std::filesystem::is_directory(path, ec)) return path;
        if (std::filesystem::is_regular_file(path, ec)) return path.parent_path();
    }
    return {};
}

QString initialSaveName(const QVariantMap& options) {
    const QString name = options.value(QStringLiteral("current_name")).toString();
    if (!name.isEmpty()) return name;
    const QString file = stringFromVariant(options.value(QStringLiteral("current_file")));
    if (!file.isEmpty()) {
        return QString::fromUtf8(
            std::filesystem::path(file.toUtf8().toStdString()).filename().c_str());
    }
    return {};
}

// parent_window от фронтенда: "x11:0xHEX" | "wayland:HANDLE" | ""
// импорт хендла wayland средствами qt невозможен, на wayland пропускаем;
// на x11 foreign QWindow делает диалог модальным к окну вызывающего (S5),
// владельцем остаёмся мы, держим до конца сессии
QWindow* foreignParentFrom(const QString& parentWindow) {
    if (!parentWindow.startsWith(QLatin1String("x11:"))) return nullptr;
    if (QGuiApplication::platformName() != QLatin1String("xcb")) return nullptr;
    bool ok = false;
    const qulonglong id = parentWindow.mid(4).toULongLong(&ok, 16);
    if (!ok || id == 0) return nullptr;
    return QWindow::fromWinId(static_cast<WId>(id));
}

// backend вправе вызывать только фронтенд xdg-desktop-portal: иначе любой
// процесс получил бы диалог и вернул приложению произвольные пути;
// service() вызова даёт уникальное имя отправителя, сверяем с владельцем имени фронтенда
bool senderIsPortalFrontend(const QDBusMessage& call) {
    const QString sender = call.service();
    if (sender.isEmpty()) return false;
    QDBusConnectionInterface* busIface = QDBusConnection::sessionBus().interface();
    if (!busIface) return false;
    const QDBusReply<QString> owner =
        busIface->serviceOwner(QStringLiteral("org.freedesktop.portal.Desktop"));
    return owner.isValid() && owner.value() == sender;
}

}

void PortalRequest::Close() {
    if (!calledFromDBus()) return;
    if (!senderIsPortalFrontend(message())) {
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("Unmatched caller"));
        return;
    }
    emit closeRequested();
}

PortalService::PortalService(const QString& busName, QObject* parent)
    : QObject(parent), m_busName(busName) {}

void PortalService::OpenFile(const QDBusObjectPath& handle, const QString& appId,
                             const QString& parentWindow, const QString& title,
                             const QVariantMap& options) {
    Q_UNUSED(appId);
    if (!calledFromDBus()) return;
    const QDBusMessage call = message();
    if (!senderIsPortalFrontend(call)) {
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("Unmatched caller"));
        return;
    }
    setDelayedReply(true);
    handleCall(call, handle, parentWindow, title, options, /*save=*/false);
}

void PortalService::SaveFile(const QDBusObjectPath& handle, const QString& appId,
                             const QString& parentWindow, const QString& title,
                             const QVariantMap& options) {
    Q_UNUSED(appId);
    if (!calledFromDBus()) return;
    const QDBusMessage call = message();
    if (!senderIsPortalFrontend(call)) {
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("Unmatched caller"));
        return;
    }
    setDelayedReply(true);
    handleCall(call, handle, parentWindow, title, options, /*save=*/true);
}

bool PortalService::start() {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) return false;
    if (!bus.registerObject(QStringLiteral("/org/freedesktop/portal/desktop"), this,
                            QDBusConnection::ExportAllSlots |
                                QDBusConnection::ExportAllProperties)) {
        return false;
    }
    if (!bus.registerService(m_busName)) return false;

    // автовыход, сервис не должен висеть в памяти вечно
    m_idleTimer = new QTimer(this);
    m_idleTimer->setInterval(5 * 60 * 1000);
    m_idleTimer->setSingleShot(true);
    connect(m_idleTimer, &QTimer::timeout, this, [this]() {
        if (m_sessions.isEmpty()) {
            QCoreApplication::quit();
        } else {
            m_idleTimer->start();
        }
    });
    m_idleTimer->start();
    return true;
}

void PortalService::handleCall(const QDBusMessage& call, const QDBusObjectPath& handle,
                               const QString& parentWindow, const QString& title,
                               const QVariantMap& options, bool save) {
    const QString key = handle.path();
    // фронтенд всегда даёт handle вида /org/freedesktop/portal/desktop/request/<sender>/<token>,
    // всё остальное это нештатный вызов
    if (!key.startsWith(QStringLiteral("/org/freedesktop/portal/desktop/request/"))) {
        QDBusConnection::sessionBus().send(call.createErrorReply(
            QDBusError::InvalidArgs, QStringLiteral("Invalid request handle")));
        return;
    }
    if (m_sessions.contains(key)) {
        // иначе вызывающий ждал бы ответ вечно, а объект request был бы перезаписан
        QDBusConnection::sessionBus().send(call.createErrorReply(
            QDBusError::InvalidArgs, QStringLiteral("Duplicate request handle")));
        return;
    }
    m_idleTimer->start();

    auto* request = new PortalRequest(this);
    if (!QDBusConnection::sessionBus().registerObject(key, request,
                                                      QDBusConnection::ExportAllSlots)) {
        request->deleteLater();
        QDBusConnection::sessionBus().send(call.createErrorReply(
            QDBusError::InvalidArgs, QStringLiteral("Cannot export request object")));
        return;
    }

    // tui диалог в отдельном frameless окне
    // qt::Dialog вместо qt::Window: на wayland композитор держит такое
    // окно плавающим по центру, а не тайлом
    using namespace Theme::Colors;
    auto* window = new QWidget(nullptr, Qt::Dialog | Qt::FramelessWindowHint);
    window->setObjectName(QStringLiteral("portalChooserWindow"));
    window->setAttribute(Qt::WA_StyledBackground, true);
    window->setWindowTitle(title.isEmpty() ? QStringLiteral("Open Files") : title);
    window->setStyleSheet(QStringLiteral(
        "QWidget#portalChooserWindow { background-color: %1; }")
        .arg(Base().name()));
    auto* layout = new QVBoxLayout(window);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* page = new Widgets::FileChooserDialog(window);
    layout->addWidget(page);

    page->setMultipleSelectionEnabled(options.value(QStringLiteral("multiple")).toBool());
    page->setDirectoryMode(options.value(QStringLiteral("directory")).toBool());
    if (save) page->setSaveMode(true, initialSaveName(options));

    std::filesystem::path start = resolveStartDirectory(options);
    // последний каталог переиспользуем только для того же вызывающего:
    // иначе клиенты видели бы пути чатов друг друга (S7)
    const QString caller = call.service();
    if (start.empty() && !m_lastDirectory.isEmpty() && m_lastDirectoryCaller == caller)
        start = std::filesystem::path(m_lastDirectory.toStdString());
    if (start.empty()) start = std::filesystem::path(QDir::homePath().toUtf8().toStdString());
    page->setDirectory(start);

    QWindow* foreignParent = foreignParentFrom(parentWindow);
    auto* session = new Session{call, window, page, request, foreignParent};
    m_sessions.insert(key, session);
    window->installEventFilter(this);

    if (foreignParent) {
        // нативный handle создаём до show(): платформа применит
        // WM_TRANSIENT_FOR при маппинге окна
        window->createWinId();
        if (auto* handle = window->windowHandle())
            handle->setTransientParent(foreignParent);
    }

    connect(request, &PortalRequest::closeRequested, this,
            [this, key]() { finishSession(key, 1, {}); });
    connect(page, &Widgets::FileChooserDialog::closeRequested, this,
            [this, key]() { finishSession(key, 1, {}); });
    connect(page, &Widgets::FileChooserDialog::fileChosen, this,
            [this, key](const QString& path) {
                QVariantMap results;
                results.insert(QStringLiteral("uris"),
                               QStringList{QUrl::fromLocalFile(path).toString()});
                finishSession(key, 0, results);
            });
    connect(page, &Widgets::FileChooserDialog::filesChosen, this,
            [this, key](const QStringList& paths) {
                QStringList uris;
                uris.reserve(paths.size());
                for (const QString& path : paths)
                    uris << QUrl::fromLocalFile(path).toString();
                QVariantMap results;
                results.insert(QStringLiteral("uris"), uris);
                finishSession(key, 0, results);
            });

    QScreen* screen = QGuiApplication::primaryScreen();
    const QRect avail = screen ? screen->availableGeometry() : QRect(0, 0, 1280, 800);
    const int w = std::clamp(int(avail.width() * 78 / 100), 640, 1000);
    const int h = std::clamp(int(avail.height() * 82 / 100), 420, 700);
    window->resize(w, h);
    window->move(avail.x() + (avail.width() - w) / 2, avail.y() + (avail.height() - h) / 2);
    window->show();
    window->raise();
    window->activateWindow();
    page->focusPrimaryInput();
}

void PortalService::finishSession(const QString& key, uint response, const QVariantMap& results) {
    Session* session = m_sessions.take(key);
    if (!session) return;

    if (session->page && !session->page->currentDirectory().empty()) {
        m_lastDirectory = QString::fromUtf8(session->page->currentDirectory().c_str());
        m_lastDirectoryCaller = session->call.service();
    }

    const QDBusMessage reply = session->call.createReply(
        QVariantList{QVariant(response), QVariant(results)});
    QDBusConnection::sessionBus().send(reply);

    QDBusConnection::sessionBus().unregisterObject(key);

    QPointer<QWidget> window = session->window;
    QPointer<PortalRequest> request = session->request;
    QPointer<QWindow> foreignParent = session->foreignParent;
    delete session;

    if (request) request->deleteLater();
    if (window) {
        window->removeEventFilter(this);
        window->hide();
        window->deleteLater();
    }
    if (foreignParent) foreignParent->deleteLater();
}

bool PortalService::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Close) {
        for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
            if (it.value()->window == watched) {
                finishSession(it.key(), 1, {});
                break;
            }
        }
        event->accept();
        return true;
    }
    return QObject::eventFilter(watched, event);
}


bool PortalService::setupPortalEnvironment() {
    const QString home = QDir::homePath();
    const QString xdgData = home + QStringLiteral("/.local/share");
    const QString xdgConfig = home + QStringLiteral("/.config");

    QDir().mkpath(xdgData + QStringLiteral("/xdg-desktop-portal/portals"));
    QDir().mkpath(xdgData + QStringLiteral("/dbus-1/services"));
    QDir().mkpath(xdgData + QStringLiteral("/applications"));
    QDir().mkpath(xdgConfig + QStringLiteral("/xdg-desktop-portal"));

    QFile portalFile(xdgData + QStringLiteral("/xdg-desktop-portal/portals/atlifm.portal"));
    if (portalFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        portalFile.write("[portal]\nDBusName=org.freedesktop.impl.portal.desktop.atlifm\nInterfaces=org.freedesktop.impl.portal.FileChooser;\n");
        portalFile.close();
    }

    QFile serviceFile(xdgData + QStringLiteral("/dbus-1/services/org.freedesktop.impl.portal.desktop.atlifm.service"));
    if (serviceFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        const QString appPath = QCoreApplication::applicationFilePath();
        serviceFile.write(QStringLiteral("[D-BUS Service]\nName=org.freedesktop.impl.portal.desktop.atlifm\nExec=%1 --portal\n").arg(appPath).toUtf8());
        serviceFile.close();
    }

    QFile desktopFile(xdgData + QStringLiteral("/applications/atlifm-portal.desktop"));
    if (desktopFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        desktopFile.write("[Desktop Entry]\nName=atlifm Portal\nComment=XDG Desktop Portal FileChooser backend for atlifm\nExec=atlifm --portal\nIcon=atlifm\nTerminal=false\nType=Application\nNoDisplay=true\nStartupNotify=true\nStartupWMClass=atlifm-portal\n");
        desktopFile.close();
    }

    auto updatePortalsConf = [](const QString& path) {
        QString content;
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) {
            content = QString::fromUtf8(file.readAll());
            file.close();
        }
        if (content.contains(QStringLiteral("FileChooser=atlifm"))) return;

        if (content.contains(QStringLiteral("[preferred]"))) {
            content.replace(QStringLiteral("[preferred]"),
                            QStringLiteral("[preferred]\norg.freedesktop.impl.portal.FileChooser=atlifm"));
        } else {
            content += QStringLiteral("\n[preferred]\ndefault=hyprland;gtk\norg.freedesktop.impl.portal.FileChooser=atlifm\n");
        }
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(content.trimmed().toUtf8() + '\n');
            file.close();
        }
    };

    updatePortalsConf(xdgConfig + QStringLiteral("/xdg-desktop-portal/portals.conf"));
    updatePortalsConf(xdgConfig + QStringLiteral("/xdg-desktop-portal/hyprland-portals.conf"));

    const QString hyprLua = xdgConfig + QStringLiteral("/hypr/hyprland.lua");
    if (QFile::exists(hyprLua)) {
        QFile hf(hyprLua);
        if (hf.open(QIODevice::ReadOnly)) {
            QString hContent = QString::fromUtf8(hf.readAll());
            hf.close();
            if (!hContent.contains(QStringLiteral("atlifm-portal"))) {
                if (hContent.contains(QStringLiteral("consplorer-portal"))) {
                    hContent.replace(QStringLiteral("class = \"^consplorer-portal$\""),
                                     QStringLiteral("class = \"^(consplorer|atlifm)-portal$\""));
                } else {
                    hContent += QStringLiteral("\no.window({ class = \"^atlifm-portal$\" }, { tag = \"+floating-window\" })\n");
                }
                if (hf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    hf.write(hContent.toUtf8());
                    hf.close();
                }
            }
        }
    }

    QProcess::execute(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("restart"), QStringLiteral("xdg-desktop-portal")});
    return true;
}

}
