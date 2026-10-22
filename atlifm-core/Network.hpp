#pragma once

#include <QString>

namespace Core::Network {

enum class Protocol { Sftp, Ftp, Ftps, Ftpi, Smb };

struct Location {
    Protocol protocol{Protocol::Sftp};
    QString host;
    int port{22};
    QString path;
    QString username;
    QString password;
    QString sshKeyPath;
    QString domain;
    bool anonymous{false};
    bool acceptUnknownHostKey{false};
    bool rememberPassword{false};
    QString original;
};

QString protocolName(Protocol protocol);
Protocol protocolFromName(const QString& name);
int defaultPort(Protocol protocol);
bool parse(const QString& input, Location* out, QString* error);
bool looksLikeNetwork(const QString& input);
QString toUri(const Location& location);

}
