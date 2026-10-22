#include "NetworkMount.hpp"
#include "Credentials.hpp"
#include "SshConfig.hpp"

#include <QFileInfo>
#include <QUrl>
#include <algorithm>
#include <unistd.h>

namespace Core::Network {

NetworkMount::NetworkMount(QObject* parent)
    : QObject(parent) {}

NetworkMount::~NetworkMount() {
    release();
}

void NetworkMount::release() {
    if (m_cancellable) {
        g_cancellable_cancel(m_cancellable);
        g_object_unref(m_cancellable);
        m_cancellable = nullptr;
    }
    if (m_mount) {
        g_object_unref(m_mount);
        m_mount = nullptr;
    }
    if (m_file) {
        g_object_unref(m_file);
        m_file = nullptr;
    }
}

void NetworkMount::mount(const Location& location) {
    release();
    m_location = location;
    m_busy = true;
    m_authFailed = false;
    m_hostKeyRejected = false;

    if (location.protocol == Protocol::Sftp) {
        const QString sshError = configureSsh(location);
        if (!sshError.isEmpty()) {
            m_busy = false;
            emit failed(sshError);
            return;
        }
    }

    const QString uri = toUri(location);
    m_file = g_file_new_for_uri(uri.toUtf8().constData());
    m_cancellable = g_cancellable_new();

    auto* operation = g_mount_operation_new();
    if (!location.username.isEmpty()) {
        g_mount_operation_set_username(operation, location.username.toUtf8().constData());
    }
    if (!location.password.isEmpty()) {
        g_mount_operation_set_password(operation, location.password.toUtf8().constData());
    }
    if (!location.domain.isEmpty()) {
        g_mount_operation_set_domain(operation, location.domain.toUtf8().constData());
    }
    g_mount_operation_set_anonymous(operation, location.anonymous);
    g_mount_operation_set_password_save(operation, G_PASSWORD_SAVE_NEVER);

    g_signal_connect(operation, "ask-password", G_CALLBACK(onAskPassword), this);
    g_signal_connect(operation, "ask-question", G_CALLBACK(onAskQuestion), this);
    g_file_mount_enclosing_volume(m_file, G_MOUNT_MOUNT_NONE, operation,
                                  m_cancellable, onMountFinished, this);
    g_object_unref(operation);
}

void NetworkMount::onAskPassword(GMountOperation* operation, const char* message,
                                 const char* defaultUser, const char* defaultDomain,
                                 GAskPasswordFlags flags, gpointer userData) {
    Q_UNUSED(message);
    auto* self = static_cast<NetworkMount*>(userData);
    if (!self) return;

    if (flags & G_ASK_PASSWORD_NEED_USERNAME) {
        QString user = self->m_location.username;
        if (user.isEmpty() && defaultUser) user = QString::fromUtf8(defaultUser);
        if (user.isEmpty() && self->m_location.anonymous) user = QStringLiteral("anonymous");
        if (user.isEmpty()) {
            self->m_authFailed = true;
            g_mount_operation_reply(operation, G_MOUNT_OPERATION_ABORTED);
            return;
        }
        g_mount_operation_set_username(operation, user.toUtf8().constData());
    }
    if (flags & G_ASK_PASSWORD_NEED_DOMAIN) {
        QString domain = self->m_location.domain;
        if (domain.isEmpty() && defaultDomain) domain = QString::fromUtf8(defaultDomain);
        if (!domain.isEmpty()) {
            g_mount_operation_set_domain(operation, domain.toUtf8().constData());
        }
    }
    if (flags & G_ASK_PASSWORD_NEED_PASSWORD) {
        const bool keyAuth = self->m_location.protocol == Protocol::Sftp &&
                             !self->m_location.sshKeyPath.isEmpty();
        if (self->m_location.password.isEmpty() && !self->m_location.anonymous && !keyAuth) {
            self->m_authFailed = true;
            g_mount_operation_reply(operation, G_MOUNT_OPERATION_ABORTED);
            return;
        }
        g_mount_operation_set_password(operation,
                                       self->m_location.password.toUtf8().constData());
    }
    g_mount_operation_reply(operation, G_MOUNT_OPERATION_HANDLED);
}

void NetworkMount::onAskQuestion(GMountOperation* operation, const char* message,
                                 char** choices, gpointer userData) {
    Q_UNUSED(choices);
    auto* self = static_cast<NetworkMount*>(userData);
    if (!self) return;

    const QString text = QString::fromUtf8(message ? message : "");
    const bool hostKeyPrompt = text.contains(QLatin1String("Identity Verification"), Qt::CaseInsensitive) ||
                               text.contains(QLatin1String("authenticity of host"), Qt::CaseInsensitive) ||
                               text.contains(QLatin1String("host key"), Qt::CaseInsensitive);
    if (hostKeyPrompt) {
        if (self->m_location.acceptUnknownHostKey) {
            g_mount_operation_set_choice(operation, 0);
            g_mount_operation_reply(operation, G_MOUNT_OPERATION_HANDLED);
        } else {
            self->m_hostKeyRejected = true;
            g_mount_operation_reply(operation, G_MOUNT_OPERATION_ABORTED);
        }
        return;
    }
    g_mount_operation_reply(operation, G_MOUNT_OPERATION_ABORTED);
}

void NetworkMount::onMountFinished(GObject* source, GAsyncResult* result, gpointer userData) {
    Q_UNUSED(source);
    auto* self = static_cast<NetworkMount*>(userData);
    if (self) self->finishMount(result);
}

void NetworkMount::finishMount(GAsyncResult* result) {
    GError* error = nullptr;
    const bool ok = g_file_mount_enclosing_volume_finish(m_file, result, &error);
    if (!ok) {
        const bool alreadyMounted =
            g_error_matches(error, G_IO_ERROR, G_IO_ERROR_ALREADY_MOUNTED);
        if (!alreadyMounted) {
            QString message;
            if (m_hostKeyRejected) {
                message = QStringLiteral("SSH host key is not trusted. Enable \"Accept unknown SSH host key\" "
                                         "in the connection form to continue.");
            } else if (m_authFailed) {
                message = QStringLiteral("Authentication failed: username or password is missing");
            } else if (error && error->message) {
                message = QString::fromUtf8(error->message);
            } else {
                message = QStringLiteral("Could not connect");
            }
            if (message.contains(QLatin1String("Operation not supported"), Qt::CaseInsensitive) ||
                message.contains(QLatin1String("not supported"), Qt::CaseInsensitive)) {
                message += QStringLiteral("\nThe GVfs backend for this protocol may not be installed.");
            }
            if (error) g_error_free(error);
            m_busy = false;
            emit failed(message);
            return;
        }
        g_error_free(error);
    }

    m_mount = g_file_find_enclosing_mount(m_file, nullptr, nullptr);
    char* directPath = g_file_get_path(m_file);
    QString path = directPath ? QString::fromUtf8(directPath) : QString();
    if (directPath) g_free(directPath);

    QString uri;
    char* targetUri = g_file_get_uri(m_file);
    if (targetUri) {
        uri = QString::fromUtf8(targetUri);
        g_free(targetUri);
    }

    if (path.isEmpty() && m_mount) {
        GFile* root = g_mount_get_root(m_mount);
        if (root) {
            char* rootPath = g_file_get_path(root);
            if (rootPath) {
                path = QString::fromUtf8(rootPath);
                if (!m_location.path.isEmpty()) {
                    if (!path.endsWith(QLatin1Char('/'))) path += QLatin1Char('/');
                    QString suffix = m_location.path;
                    while (suffix.startsWith(QLatin1Char('/'))) suffix.remove(0, 1);
                    path += suffix;
                }
                g_free(rootPath);
            }
            g_object_unref(root);
        }
    }

    if (path.isEmpty()) {
        m_busy = false;
        emit failed(QStringLiteral("The network location is mounted, but no local GVfs path is available.\n"
                                   "Install gvfs-fuse and make sure /run/user/%1/gvfs exists.")
                        .arg(getuid()));
        return;
    }

    if (!m_location.anonymous) {
        if (m_location.rememberPassword) {
            savePassword(m_location);
        } else {
            removePassword(m_location);
        }
    }

    m_busy = false;
    emit mounted(path, uri);
}

}
