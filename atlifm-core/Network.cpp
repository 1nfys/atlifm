#include "Network.hpp"

#include <QRegularExpression>
#include <QStringList>
#include <QUrl>

namespace Core::Network {

QString protocolName(Protocol protocol) {
    switch (protocol) {
    case Protocol::Ftp: return QStringLiteral("ftp");
    case Protocol::Ftps: return QStringLiteral("ftps");
    case Protocol::Ftpi: return QStringLiteral("ftpis");
    case Protocol::Sftp: return QStringLiteral("sftp");
    case Protocol::Smb: return QStringLiteral("smb");
    }
    return QStringLiteral("sftp");
}

Protocol protocolFromName(const QString& name) {
    const QString lower = name.trimmed().toLower();
    if (lower == QLatin1String("ftp")) return Protocol::Ftp;
    if (lower == QLatin1String("ftps")) return Protocol::Ftps;
    if (lower == QLatin1String("ftpis")) return Protocol::Ftpi;
    if (lower == QLatin1String("smb") || lower == QLatin1String("cifs"))
        return Protocol::Smb;
    return Protocol::Sftp;
}

int defaultPort(Protocol protocol) {
    switch (protocol) {
    case Protocol::Ftp: return 21;
    case Protocol::Ftps: return 21;
    case Protocol::Ftpi: return 990;
    case Protocol::Sftp: return 22;
    case Protocol::Smb: return 445;
    }
    return 22;
}

static QString normalizedPath(const QString& path) {
    if (path.isEmpty()) return {};
    return path.startsWith(QLatin1Char('/')) ? path : QLatin1Char('/') + path;
}

static bool parsePort(const QString& text, int* port) {
    bool ok = false;
    const int value = text.toInt(&ok);
    if (!ok || value <= 0 || value > 65535) return false;
    *port = value;
    return true;
}

static bool parseExplicit(const QString& input, Location* out, QString* error) {
    QUrl url(input, QUrl::TolerantMode);
    const QString scheme = url.scheme().toLower();
    if (scheme != QLatin1String("ftp") && scheme != QLatin1String("ftps") &&
        scheme != QLatin1String("ftpis") && scheme != QLatin1String("sftp") &&
        scheme != QLatin1String("smb")) {
        if (error) *error = QStringLiteral("Unsupported network protocol");
        return false;
    }
    if (!url.isValid() || url.host().isEmpty()) {
        if (error) *error = QStringLiteral("Network address has no host");
        return false;
    }

    out->protocol = protocolFromName(scheme);
    out->host = url.host();
    out->port = url.port() > 0 ? url.port() : defaultPort(out->protocol);
    out->path = normalizedPath(url.path(QUrl::FullyDecoded));
    out->username = url.userName(QUrl::FullyDecoded);
    out->password = url.password(QUrl::FullyDecoded);
    out->anonymous = out->username.compare(QLatin1String("anonymous"), Qt::CaseInsensitive) == 0;
    out->original = input;
    return true;
}

static bool parseBare(const QString& input, Location* out, QString* error) {
    QString rest = input.trimmed();
    if (rest.isEmpty()) {
        if (error) *error = QStringLiteral("Network address is empty");
        return false;
    }

    const int at = rest.lastIndexOf(QLatin1Char('@'));
    if (at > 0) {
        const QString userInfo = rest.left(at);
        rest = rest.mid(at + 1);
        const int colon = userInfo.indexOf(QLatin1Char(':'));
        if (colon >= 0) {
            out->username = userInfo.left(colon);
            out->password = userInfo.mid(colon + 1);
        } else {
            out->username = userInfo;
        }
    }

    QString hostPort = rest;
    QString pathPart;
    const int slash = rest.indexOf(QLatin1Char('/'));
    if (slash >= 0) {
        hostPort = rest.left(slash);
        pathPart = rest.mid(slash);
    }

    QString host;
    int port = 0;
    if (hostPort.startsWith(QLatin1Char('['))) {
        const int close = hostPort.indexOf(QLatin1Char(']'));
        if (close <= 1) {
            if (error) *error = QStringLiteral("Invalid IPv6 address");
            return false;
        }
        host = hostPort.mid(1, close - 1);
        const QString tail = hostPort.mid(close + 1);
        if (!tail.isEmpty()) {
            if (!tail.startsWith(QLatin1Char(':')) || !parsePort(tail.mid(1), &port)) {
                if (error) *error = QStringLiteral("Invalid port");
                return false;
            }
        }
    } else {
        const int colon = hostPort.lastIndexOf(QLatin1Char(':'));
        if (colon > 0 && hostPort.count(QLatin1Char(':')) == 1) {
            const QString tail = hostPort.mid(colon + 1);
            static const QRegularExpression digits(QStringLiteral("^\\d+$"));
            if (!tail.isEmpty() && digits.match(tail).hasMatch()) {
                if (!parsePort(tail, &port)) {
                    if (error) *error = QStringLiteral("Invalid port");
                    return false;
                }
                hostPort = hostPort.left(colon);
            }
        }
        host = hostPort;
    }

    if (host.isEmpty()) {
        if (error) *error = QStringLiteral("Network address has no host");
        return false;
    }

    out->host = host;
    out->path = normalizedPath(pathPart);
    if (port > 0) {
        out->port = port;
        if (port == 21) out->protocol = Protocol::Ftp;
        else if (port == 990) out->protocol = Protocol::Ftpi;
        else if (port == 445) out->protocol = Protocol::Smb;
        else out->protocol = Protocol::Sftp;
    } else {
        out->protocol = Protocol::Sftp;
        out->port = defaultPort(out->protocol);
    }
    out->anonymous = out->username.compare(QLatin1String("anonymous"), Qt::CaseInsensitive) == 0;
    out->original = input;
    return true;
}

bool parse(const QString& input, Location* out, QString* error) {
    if (!out) return false;
    *out = Location{};
    if (error) error->clear();

    const QString trimmed = input.trimmed();
    if (trimmed.isEmpty()) {
        if (error) *error = QStringLiteral("Network address is empty");
        return false;
    }

    const QRegularExpression explicitScheme(
        QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]*://"));
    if (explicitScheme.match(trimmed).hasMatch()) {
        return parseExplicit(trimmed, out, error);
    }

    if (trimmed.startsWith(QLatin1String("\\\\")) ||
        trimmed.startsWith(QLatin1String("//"))) {
        QString body = trimmed.mid(2);
        body.replace(QLatin1Char('\\'), QLatin1Char('/'));
        const QStringList parts = body.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (parts.isEmpty()) {
            if (error) *error = QStringLiteral("Network address has no host");
            return false;
        }
        out->protocol = Protocol::Smb;
        out->host = parts.first();
        out->port = defaultPort(out->protocol);
        QStringList pathParts = parts.mid(1);
        out->path = pathParts.isEmpty() ? QString() : QLatin1Char('/') + pathParts.join(QLatin1Char('/'));
        out->original = input;
        return true;
    }

    return parseBare(trimmed, out, error);
}

bool looksLikeNetwork(const QString& input) {
    const QString trimmed = input.trimmed();
    if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('~'))) {
        return false;
    }
    if (trimmed.startsWith(QLatin1String("\\\\")) ||
        trimmed.startsWith(QLatin1String("//"))) {
        return true;
    }
    if (trimmed.startsWith(QLatin1Char('/'))) return false;
    static const QRegularExpression scheme(
        QStringLiteral("^(ftps|ftpis|ftp|sftp|smb)://"), QRegularExpression::CaseInsensitiveOption);
    if (scheme.match(trimmed).hasMatch()) return true;
    if (trimmed.contains(QLatin1Char('@'))) return true;
    if (trimmed.startsWith(QLatin1Char('['))) return true;

    static const QRegularExpression ipv4(
        QStringLiteral("^\\d{1,3}(?:\\.\\d{1,3}){3}(?::\\d{1,5})?(?:/.*)?$"));
    if (ipv4.match(trimmed).hasMatch()) return true;

    static const QRegularExpression hostPort(
        QStringLiteral("^[A-Za-z0-9._-]+:\\d{1,5}(?:/.*)?$"));
    if (hostPort.match(trimmed).hasMatch()) return true;

    static const QRegularExpression localHost(
        QStringLiteral("^localhost(?::\\d{1,5})?(?:/.*)?$"),
        QRegularExpression::CaseInsensitiveOption);
    return localHost.match(trimmed).hasMatch();
}

QString toUri(const Location& location) {
    QUrl url;
    url.setScheme(protocolName(location.protocol));
    url.setHost(location.host);
    if (location.port > 0) url.setPort(location.port);
    if (!location.path.isEmpty()) {
        url.setPath(location.path.startsWith(QLatin1Char('/'))
                        ? location.path
                        : QLatin1Char('/') + location.path);
    }
    return url.toString(QUrl::FullyEncoded);
}

}
