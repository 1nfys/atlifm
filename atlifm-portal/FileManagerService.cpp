#include "FileManagerService.hpp"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTextStream>
#include <QUrl>

namespace Portal {

FileManagerService::FileManagerService(QObject* parent) : QObject(parent) {
    m_idleTimer.setSingleShot(true);
    m_idleTimer.setInterval(60 * 1000);
    connect(&m_idleTimer, &QTimer::timeout, qApp, &QCoreApplication::quit);
}

bool FileManagerService::start() {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) return false;
    if (!bus.registerObject(QStringLiteral("/org/freedesktop/FileManager1"), this,
                            QDBusConnection::ExportAllSlots)) {
        return false;
    }
    if (!bus.registerService(QStringLiteral("org.freedesktop.FileManager1"))) return false;
    m_idleTimer.start();
    return true;
}

bool FileManagerService::setup() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                        + QStringLiteral("/dbus-1/services");
    if (!QDir().mkpath(dir)) return false;

    QString exec = QCoreApplication::applicationFilePath();
    exec.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    exec.replace(QLatin1Char('"'), QStringLiteral("\\\""));

    QFile file(dir + QStringLiteral("/org.freedesktop.FileManager1.service"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return false;
    QTextStream out(&file);
    out << "[D-BUS Service]\n"
        << "Name=org.freedesktop.FileManager1\n"
        << "Exec=\"" << exec << "\" --file-manager\n";
    file.close();

    const QString gio = QStandardPaths::findExecutable(QStringLiteral("gio"));
    if (!gio.isEmpty()) {
        QProcess::execute(gio, {QStringLiteral("mime"), QStringLiteral("inode/directory"),
                                QStringLiteral("atlifm.desktop")});
    } else {
        const QString xdgMime = QStandardPaths::findExecutable(QStringLiteral("xdg-mime"));
        if (!xdgMime.isEmpty()) {
            QProcess::execute(xdgMime, {QStringLiteral("default"), QStringLiteral("atlifm.desktop"),
                                        QStringLiteral("inode/directory")});
        }
    }

    return true;
}

void FileManagerService::ShowItems(const QStringList& uris, const QString& startupId) {
    Q_UNUSED(startupId);
    launch(uris, false);
}

void FileManagerService::ShowItemProperties(const QStringList& uris, const QString& startupId) {
    Q_UNUSED(startupId);
    launch(uris, true);
}

void FileManagerService::ShowFolders(const QStringList& uris, const QString& startupId) {
    Q_UNUSED(startupId);
    launch(uris, false);
}

void FileManagerService::launch(const QStringList& uris, bool properties) {
    m_idleTimer.start();
    const QString exec = QCoreApplication::applicationFilePath();
    for (const QString& uri : uris) {
        const QUrl url(uri);
        const QString path = url.isLocalFile() ? url.toLocalFile() : uri;
        if (path.isEmpty()) continue;

        QStringList args;
        if (properties) args << QStringLiteral("--properties");
        args << path;
        QProcess::startDetached(exec, args);
    }
}

}
