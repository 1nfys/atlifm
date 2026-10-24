#pragma once

#include <QObject>
#include <QStringList>
#include <QTimer>

namespace Portal {

class FileManagerService : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.FileManager1")

public:
    explicit FileManagerService(QObject* parent = nullptr);
    bool start();
    static bool setup();

public slots:
    void ShowItems(const QStringList& uris, const QString& startupId);
    void ShowItemProperties(const QStringList& uris, const QString& startupId);
    void ShowFolders(const QStringList& uris, const QString& startupId);

private:
    void launch(const QStringList& uris, bool properties);

    QTimer m_idleTimer;
};

}
