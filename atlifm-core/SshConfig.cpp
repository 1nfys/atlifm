#include "SshConfig.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QSaveFile>
#include <QStandardPaths>
#include <cstdlib>

namespace Core::Network {

namespace {

QString sshRoot() {
    const char* override = std::getenv("ATLIFM_SSH_DIR");
    if (override && *override) return QString::fromUtf8(override);
    return QDir::homePath() + QStringLiteral("/.ssh");
}

QString configPath() {
    return sshRoot() + QStringLiteral("/config");
}

QString managedDir() {
    return sshRoot() + QStringLiteral("/atlifm");
}

QString expandPath(QString path) {
    path = path.trimmed();
    const QString home = QDir::homePath();
    if (path == QLatin1String("~")) return home;
    if (path.startsWith(QLatin1String("~/"))) return home + path.mid(1);
    return path;
}

QString quoted(const QString& value) {
    QString escaped = value;
    escaped.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
    escaped.replace(QLatin1Char('"'), QLatin1String("\\\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

QString identityFor(const Location& location) {
    QString host = location.host.trimmed().toLower();
    QString user = location.username.trimmed();
    const QByteArray digest = QCryptographicHash::hash((host + QLatin1Char('\n') + user).toUtf8(),
                                                       QCryptographicHash::Sha256)
                                  .toHex()
                                  .left(24);
    return QString::fromLatin1(digest);
}

QString managedFile(const Location& location) {
    return managedDir() + QLatin1Char('/') + identityFor(location) + QStringLiteral(".conf");
}

QString appConfigDir() {
    const char* override = std::getenv("ATLIFM_CONFIG_DIR");
    if (override && *override) return QString::fromUtf8(override);
    const char* home = std::getenv("HOME");
    if (home && *home) return QString::fromUtf8(home) + QStringLiteral("/.config/atlifm");
    const QString fallback = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    return fallback.isEmpty() ? QString() : fallback + QStringLiteral("/atlifm");
}

QString keyStorePath() {
    const QString dir = appConfigDir();
    return dir.isEmpty() ? QString() : dir + QStringLiteral("/ssh_keys.ini");
}

QString rememberedKeyId(const Location& location) {
    const int port = location.port > 0 ? location.port : defaultPort(location.protocol);
    const QString seed = location.host.trimmed().toLower() + QLatin1Char('\n') +
                         QString::number(port) + QLatin1Char('\n') +
                         location.username.trimmed();
    return QString::fromLatin1(QCryptographicHash::hash(seed.toUtf8(), QCryptographicHash::Sha256)
                                   .toHex()
                                   .left(32));
}

bool ensureSshRoot(QString* error) {
    const QString root = sshRoot();
    QFileInfo info(root);
    if (info.exists()) {
        if (!info.isDir()) {
            if (error) *error = QStringLiteral("SSH config path is not a directory: %1").arg(root);
            return false;
        }
        return true;
    }
    if (!QDir().mkpath(root)) {
        if (error) *error = QStringLiteral("Cannot create SSH config directory: %1").arg(root);
        return false;
    }
    QFile::setPermissions(root, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                    QFileDevice::ExeOwner);
    return true;
}

bool ensureManagedDir(QString* error) {
    const QString dir = managedDir();
    QFileInfo info(dir);
    if (info.exists()) {
        if (!info.isDir()) {
            if (error) *error = QStringLiteral("SSH key config path is not a directory: %1").arg(dir);
            return false;
        }
        return true;
    }
    if (!QDir().mkpath(dir)) {
        if (error) *error = QStringLiteral("Cannot create SSH key config directory: %1").arg(dir);
        return false;
    }
    QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                   QFileDevice::ExeOwner);
    return true;
}

bool ensureInclude(QString* error) {
    if (!ensureSshRoot(error)) return false;

    QString path = configPath();
    const QFileInfo info(path);
    if (info.isSymLink()) {
        const QString target = info.canonicalFilePath();
        if (target.isEmpty()) {
            if (error) *error = QStringLiteral("Cannot resolve SSH config symlink: %1").arg(path);
            return false;
        }
        path = target;
    }

    const QString includePath = managedDir() + QStringLiteral("/*.conf");
    const QByteArray includeLine = QStringLiteral("Include %1\n").arg(quoted(includePath)).toUtf8();

    QByteArray content;
    QFile input(path);
    if (input.exists()) {
        if (!input.open(QIODevice::ReadOnly)) {
            if (error) *error = QStringLiteral("Cannot read SSH config: %1").arg(path);
            return false;
        }
        content = input.readAll();
        if (content.contains(includeLine)) return true;
    }

    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("Cannot write SSH config: %1").arg(path);
        return false;
    }
    const QByteArray header =
        QByteArrayLiteral("# atlifm: managed SSH key settings\n");
    if (output.write(header) < 0 ||
        output.write(includeLine) < 0 ||
        output.write("\n") < 0 ||
        output.write(content) < 0 ||
        !output.commit()) {
        if (error) *error = QStringLiteral("Cannot update SSH config: %1").arg(path);
        return false;
    }
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

}

QString configureSsh(const Location& location) {
    if (location.protocol != Protocol::Sftp) return {};

    const QString key = expandPath(location.sshKeyPath);
    if (key.isEmpty()) {
        removeSshConfiguration(location);
        return {};
    }
    if (location.host.trimmed().isEmpty()) {
        return QStringLiteral("SSH host is empty");
    }
    if (location.host.contains(QLatin1Char('\n')) || location.host.contains(QLatin1Char('\r')) ||
        location.username.contains(QLatin1Char('\n')) ||
        location.username.contains(QLatin1Char('\r'))) {
        return QStringLiteral("SSH connection contains an invalid character");
    }
    if (key.contains(QLatin1Char('\n')) || key.contains(QLatin1Char('\r'))) {
        return QStringLiteral("SSH key path contains an invalid character");
    }

    const QFileInfo keyInfo(key);
    if (!keyInfo.exists() || !keyInfo.isFile() || !keyInfo.isReadable()) {
        return QStringLiteral("SSH key file is missing or unreadable:\n%1").arg(key);
    }
    const QString canonicalKey = keyInfo.canonicalFilePath();
    if (canonicalKey.isEmpty()) {
        return QStringLiteral("Cannot resolve SSH key file:\n%1").arg(key);
    }

    QString error;
    if (!ensureInclude(&error) || !ensureManagedDir(&error)) return error;

    QString block = QStringLiteral("# Managed by atlifm for %1@%2:%3\n")
                        .arg(location.username, location.host)
                        .arg(location.port);
    block += QStringLiteral("Match originalhost %1").arg(quoted(location.host.trimmed()));
    if (!location.username.trimmed().isEmpty()) {
        block += QStringLiteral(" user %1").arg(quoted(location.username.trimmed()));
    }
    block += QStringLiteral(" command sftp\n");
    block += QStringLiteral("    IdentityFile %1\n").arg(quoted(canonicalKey));
    block += QStringLiteral("    IdentitiesOnly yes\n");

    QSaveFile output(managedFile(location));
    if (!output.open(QIODevice::WriteOnly) ||
        output.write(block.toUtf8()) < 0 ||
        !output.commit()) {
        return QStringLiteral("Cannot write SSH key config for this connection");
    }
    QFile::setPermissions(managedFile(location),
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return {};
}

void removeSshConfiguration(const Location& location) {
    if (location.protocol != Protocol::Sftp) return;
    const QString file = managedFile(location);
    if (QFileInfo::exists(file)) QFile::remove(file);
}

QString rememberedSshKey(const Location& location) {
    if (location.protocol != Protocol::Sftp || location.host.trimmed().isEmpty()) return {};
    const QString path = keyStorePath();
    if (path.isEmpty() || !QFileInfo::exists(path)) return {};
    QSettings settings(path, QSettings::IniFormat);
    return settings.value(rememberedKeyId(location) + QStringLiteral("/key")).toString();
}

void rememberSshKey(const Location& location) {
    if (location.protocol != Protocol::Sftp || location.host.trimmed().isEmpty()) return;
    const QString path = keyStorePath();
    if (path.isEmpty()) return;
    const QString key = location.sshKeyPath.trimmed();
    if (key.isEmpty() && !QFileInfo::exists(path)) return;
    const QString dir = QFileInfo(path).absolutePath();
    if (!QFileInfo(dir).isDir() && !QDir().mkpath(dir)) return;

    QSettings settings(path, QSettings::IniFormat);
    const QString id = rememberedKeyId(location);
    if (key.isEmpty()) {
        settings.remove(id);
    } else {
        settings.setValue(id + QStringLiteral("/key"), key);
    }
    settings.sync();
    if (QFileInfo::exists(path)) {
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
}

}
