#pragma once

// бэкенд xdg desktop portal filechooser: системные диалоги выбора файла
// открывают наш выбор файлов а сервис активируется через dbus
// конфиги лежат в xdg desktop portal portals и dbus сервисах

#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVariantMap>

class QWidget;
class QWindow;

namespace Widgets {
class FileChooserDialog;
}

namespace Portal {

// org.freedesktop.impl.portal.request это объект на пути handle:
// close вызывает фронтенд, проверяем отправителя; объект регистрируется напрямую, без адаптера
class PortalRequest : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.Request")

public:
    explicit PortalRequest(QObject* parent = nullptr) : QObject(parent) {}

signals:
    void closeRequested();

public slots:
    void Close();
};

// org.freedesktop.impl.portal.filechooser: openfile/savefile, ответ отдаётся
// отложенно (setdelayedreply), response 0 это выбрано (results.uris), 1 отмена, 2 ошибка
// адаптер не используется: qt перекладывает qdbuscontext на parent адаптера,
// а контекст нужен у самого регистрируемого объекта
// (qdbuscontext.cpp: qdbuscontextprivate::set)
class PortalService : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.FileChooser")
    Q_PROPERTY(uint version READ version)

public:
    explicit PortalService(
        const QString& busName = QStringLiteral("org.freedesktop.impl.portal.desktop.atlifm"),
        QObject* parent = nullptr);

    // регистрирует объект и имя сервиса на сессионной шине,
    // false если шина недоступна или имя уже занято
    bool start();
    static bool setupPortalEnvironment();
    uint version() const { return 3; }

public slots:
    void OpenFile(const QDBusObjectPath& handle, const QString& appId, const QString& parentWindow,
                  const QString& title, const QVariantMap& options);
    void SaveFile(const QDBusObjectPath& handle, const QString& appId, const QString& parentWindow,
                  const QString& title, const QVariantMap& options);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Session {
        QDBusMessage call;
        QPointer<QWidget> window;
        QPointer<Widgets::FileChooserDialog> page;
        QPointer<PortalRequest> request;
        QPointer<QWindow> foreignParent; // родитель x11 из parent_window (s5)
    };

    void handleCall(const QDBusMessage& call, const QDBusObjectPath& handle,
                    const QString& parentWindow, const QString& title, const QVariantMap& options,
                    bool save);
    void finishSession(const QString& key, uint response, const QVariantMap& results);

    QString m_busName;
    QHash<QString, Session*> m_sessions;
    QTimer* m_idleTimer{nullptr};
    QString m_lastDirectory;       // стартовая точка для следующих запросов
    QString m_lastDirectoryCaller; // чей был запрос: чужому клиенту путь не отдаём
};

}
