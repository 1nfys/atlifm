#pragma once

#include "Network.hpp"
#include <QObject>
#include <QString>

#ifdef signals
#pragma push_macro("signals")
#undef signals
#define ATLIFM_RESTORE_QT_SIGNALS
#endif
#include <gio/gio.h>
#ifdef ATLIFM_RESTORE_QT_SIGNALS
#pragma pop_macro("signals")
#undef ATLIFM_RESTORE_QT_SIGNALS
#endif

namespace Core::Network {

class NetworkMount : public QObject {
    Q_OBJECT

public:
    explicit NetworkMount(QObject* parent = nullptr);
    ~NetworkMount() override;

    void mount(const Location& location);
    bool busy() const { return m_busy; }

signals:
    void mounted(const QString& localPath, const QString& uri);
    void failed(const QString& message);

private:
    static void onAskPassword(GMountOperation* operation, const char* message,
                              const char* defaultUser, const char* defaultDomain,
                              GAskPasswordFlags flags, gpointer userData);
    static void onAskQuestion(GMountOperation* operation, const char* message,
                              char** choices, gpointer userData);
    static void onMountFinished(GObject* source, GAsyncResult* result, gpointer userData);
    void finishMount(GAsyncResult* result);
    void release();

    GFile* m_file{nullptr};
    GMount* m_mount{nullptr};
    GCancellable* m_cancellable{nullptr};
    Location m_location;
    bool m_busy{false};
    bool m_authFailed{false};
    bool m_hostKeyRejected{false};
};

}
