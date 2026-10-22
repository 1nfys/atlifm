#include "Credentials.hpp"

#ifdef ATLIFM_HAVE_LIBSECRET
#ifdef signals
#pragma push_macro("signals")
#undef signals
#define ATLIFM_RESTORE_QT_SIGNALS
#endif
#include <libsecret/secret.h>
#ifdef ATLIFM_RESTORE_QT_SIGNALS
#pragma pop_macro("signals")
#undef ATLIFM_RESTORE_QT_SIGNALS
#endif
#endif

namespace Core::Network {

namespace {

#ifdef ATLIFM_HAVE_LIBSECRET
const SecretSchema kNetworkSchema{
    .name = "org.atlifm.Network",
    .flags = SECRET_SCHEMA_NONE,
    .attributes = {
        {"protocol", SECRET_SCHEMA_ATTRIBUTE_STRING},
        {"host", SECRET_SCHEMA_ATTRIBUTE_STRING},
        {"port", SECRET_SCHEMA_ATTRIBUTE_STRING},
        {"username", SECRET_SCHEMA_ATTRIBUTE_STRING},
        {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING},
    },
    .reserved = 0,
    .reserved1 = nullptr,
    .reserved2 = nullptr,
    .reserved3 = nullptr,
    .reserved4 = nullptr,
    .reserved5 = nullptr,
    .reserved6 = nullptr,
    .reserved7 = nullptr,
};

QString normalizedHost(const Location& location) {
    return location.host.trimmed().toLower();
}

QString portText(const Location& location) {
    return QString::number(location.port > 0 ? location.port
                                             : defaultPort(location.protocol));
}

bool canStore(const Location& location) {
    return !location.anonymous && !location.host.trimmed().isEmpty() &&
           !location.username.isEmpty() && !location.password.isEmpty();
}
#endif

}

bool credentialsAvailable() {
#ifdef ATLIFM_HAVE_LIBSECRET
    return true;
#else
    return false;
#endif
}

QString lookupPassword(const Location& location) {
#ifdef ATLIFM_HAVE_LIBSECRET
    if (location.anonymous || location.host.trimmed().isEmpty() ||
        location.username.isEmpty()) {
        return {};
    }
    GError* error = nullptr;
    gchar* value = secret_password_lookup_sync(
        &kNetworkSchema, nullptr, &error,
        "protocol", protocolName(location.protocol).toUtf8().constData(),
        "host", normalizedHost(location).toUtf8().constData(),
        "port", portText(location).toUtf8().constData(),
        "username", location.username.toUtf8().constData(),
        static_cast<const char*>(nullptr));
    if (error) {
        g_error_free(error);
        return {};
    }
    const QString password = value ? QString::fromUtf8(value) : QString();
    if (value) secret_password_free(value);
    return password;
#else
    Q_UNUSED(location);
    return {};
#endif
}

bool savePassword(const Location& location) {
#ifdef ATLIFM_HAVE_LIBSECRET
    if (!canStore(location)) return false;
    const QString label = QStringLiteral("atlifm: %1://%2@%3:%4")
                              .arg(protocolName(location.protocol),
                                   normalizedHost(location),
                                   location.username,
                                   portText(location));
    GError* error = nullptr;
    const gboolean stored = secret_password_store_sync(
        &kNetworkSchema, SECRET_COLLECTION_DEFAULT, label.toUtf8().constData(),
        location.password.toUtf8().constData(), nullptr, &error,
        "protocol", protocolName(location.protocol).toUtf8().constData(),
        "host", normalizedHost(location).toUtf8().constData(),
        "port", portText(location).toUtf8().constData(),
        "username", location.username.toUtf8().constData(),
        static_cast<const char*>(nullptr));
    if (error) {
        g_error_free(error);
        return false;
    }
    return stored;
#else
    Q_UNUSED(location);
    return false;
#endif
}

void removePassword(const Location& location) {
#ifdef ATLIFM_HAVE_LIBSECRET
    if (location.anonymous || location.host.trimmed().isEmpty() ||
        location.username.isEmpty()) {
        return;
    }
    GError* error = nullptr;
    secret_password_clear_sync(
        &kNetworkSchema, nullptr, &error,
        "protocol", protocolName(location.protocol).toUtf8().constData(),
        "host", normalizedHost(location).toUtf8().constData(),
        "port", portText(location).toUtf8().constData(),
        "username", location.username.toUtf8().constData(),
        static_cast<const char*>(nullptr));
    if (error) g_error_free(error);
#else
    Q_UNUSED(location);
#endif
}

}
