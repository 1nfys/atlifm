#pragma once

#include <QString>
#include <QStringList>
#include <QFile>
#include <QTextStream>
#include <QProcess>
#include <QProcessEnvironment>
#include <QDir>
#include <QFileInfo>
#include <QCoreApplication>
#include <QSet>
#include <vector>
#include <filesystem>
#include <functional>
#include <utility>
#include <cstdlib>

namespace Core::Plugins {

struct Plugin {
    QString id;
    QString name;
    QString exec;
    QString type;
    QStringList extensions;
    QString appliesTo;
    bool detach{false};
    bool isPreview{false};
    QString description;
    QString sourceFile;
    QString action;
    QString command;
    QString commandTitle;
    QString commandDescription;
    QString commandMode;
    QString commandPlaceholder;
    QStringList keys;
    QStringList overrides;
    QStringList hooks;
    QStringList rejected;
    // нативный плагин выполняется внутри процесса а не через отдельный процесс
    bool native{false};
};

// манифесты нативных плагинов прокидываются из слоя виджетов
using NativeManifestProvider = std::function<std::vector<Plugin>()>;

inline NativeManifestProvider& nativeManifestProvider() {
    static NativeManifestProvider provider;
    return provider;
}

inline void setNativeProvider(NativeManifestProvider provider) {
    nativeManifestProvider() = std::move(provider);
}

inline bool isValidActionId(const QString& id) {
    if (id.isEmpty()) return false;
    const char first = id.at(0).toLatin1();
    if (first < 'a' || first > 'z') return false;
    for (const QChar c : id) {
        const char ch = c.toLatin1();
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                        ch == '_';
        if (!ok) return false;
    }
    return true;
}

inline bool isProtectedActionId(const QString& id) {
    return id.contains(QLatin1String("sudo"), Qt::CaseInsensitive);
}

inline bool isValidCommandToken(const QString& token) {
    if (token.isEmpty()) return false;
    const QChar first = token.at(0);
    if (!first.isLetter()) return false;
    for (const QChar c : token) {
        if (!c.isLetterOrNumber() && c != QLatin1Char('_') && c != QLatin1Char('-'))
            return false;
    }
    return true;
}

inline QString configDir() {
    const char* override = std::getenv("ATLIFM_CONFIG_DIR");
    if (override && *override) return QString::fromUtf8(override);
    const char* home = std::getenv("HOME");
    if (!home) return {};
    return QString::fromUtf8(home) + "/.config/atlifm";
}

inline QString pluginsDir() {
    const QString dir = configDir();
    return dir.isEmpty() ? QString() : dir + "/plugins";
}

inline bool parsePluginFile(const QString& path, Plugin* out) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return false;

    QTextStream in(&file);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        const int eq = line.indexOf('=');
        if (eq <= 0) continue;
        const QString key = line.left(eq).trimmed().toLower();
        QString val = line.mid(eq + 1).trimmed();
        if (val.size() >= 2 && val.startsWith('"') && val.endsWith('"'))
            val = val.mid(1, val.size() - 2);

        if (key == "name") out->name = val;
        else if (key == "exec") out->exec = val;
        else if (key == "type") {
            out->type = val.toLower();
            if (out->type == "preview") out->isPreview = true;
        }
        else if (key == "appliesto") out->appliesTo = val.toLower();
        else if (key == "detach")
            out->detach = (val.compare("true", Qt::CaseInsensitive) == 0 ||
                           val == "1" || val.compare("yes", Qt::CaseInsensitive) == 0);
        else if (key == "description") out->description = val;
        else if (key == "action") out->action = val.trimmed().toLower();
        else if (key == "command" || key == "prefix") out->command = val.trimmed().toLower();
        else if (key == "command_title") out->commandTitle = val;
        else if (key == "command_description") out->commandDescription = val;
        else if (key == "command_mode") out->commandMode = val.trimmed().toLower();
        else if (key == "command_placeholder") out->commandPlaceholder = val;
        else if (key == "keys") out->keys.append(val);
        else if (key == "hook" || key == "hooks") {
            for (const QString& raw : val.split(',', Qt::SkipEmptyParts))
                out->hooks.append(raw.trimmed().toLower());
        }
        else if (key == "override") {
            for (const QString& raw : val.split(',', Qt::SkipEmptyParts))
                out->overrides.append(raw.trimmed().toLower());
        }
        else if (key == "extensions") {
            out->extensions.clear();
            for (const QString& raw : val.split(',', Qt::SkipEmptyParts)) {
                QString e = raw.trimmed().toLower();
                if (e.isEmpty()) continue;
                if (!e.startsWith('.')) e.prepend('.');
                out->extensions.append(e);
            }
        }
    }

    out->sourceFile = path;

    if (!out->action.isEmpty()) {
        if (isProtectedActionId(out->action)) {
            out->rejected.append(QStringLiteral("action:%1 (sudo protected)").arg(out->action));
            out->action.clear();
        } else if (!isValidActionId(out->action)) {
            out->rejected.append(QStringLiteral("action:%1 (invalid id)").arg(out->action));
            out->action.clear();
        }
    }
    if (!out->command.isEmpty()) {
        if (!isValidCommandToken(out->command)) {
            out->rejected.append(QStringLiteral("command:%1 (invalid token)").arg(out->command));
            out->command.clear();
        } else if (out->command == QLatin1String("fd") ||
                   out->command == QLatin1String("rg")) {
            out->rejected.append(QStringLiteral("command:%1 (reserved)").arg(out->command));
            out->command.clear();
        }
    }
    if (out->commandMode.isEmpty()) out->commandMode = QStringLiteral("action");
    if (out->commandMode != QLatin1String("query") &&
        out->commandMode != QLatin1String("action")) {
        out->commandMode = QStringLiteral("action");
    }
    {
        QStringList kept;
        for (const QString& id : std::as_const(out->overrides)) {
            if (kept.contains(id)) continue;
            if (isProtectedActionId(id)) {
                out->rejected.append(QStringLiteral("override:%1 (sudo protected)").arg(id));
                continue;
            }
            if (!isValidActionId(id)) {
                out->rejected.append(QStringLiteral("override:%1 (invalid id)").arg(id));
                continue;
            }
            kept.append(id);
        }
        out->overrides = kept;
    }

    if (out->appliesTo.isEmpty()) out->appliesTo = "any";
    return !out->name.isEmpty() && !out->exec.isEmpty();
}

inline std::vector<Plugin> load() {
    std::vector<Plugin> plugins;
    const QString dir = pluginsDir();
    if (dir.isEmpty()) return plugins;

    QDir d(dir);
    const QStringList files = d.entryList({QStringLiteral("*.conf")}, QDir::Files, QDir::Name);
    QSet<QString> loadedIds;
    for (const QString& f : files) {
        Plugin p;
        p.id = QFileInfo(f).completeBaseName();
        if (parsePluginFile(d.filePath(f), &p)) {
            loadedIds.insert(p.id);
            plugins.push_back(p);
        }
    }

    const QStringList scripts = d.entryList({QStringLiteral("*.sh"), QStringLiteral("*.py"), QStringLiteral("*.bash")}, QDir::Files | QDir::Executable, QDir::Name);
    for (const QString& s : scripts) {
        const QString baseId = QFileInfo(s).completeBaseName();
        if (loadedIds.contains(baseId)) continue;
        Plugin p;
        p.id = baseId;
        p.name = baseId;
        p.exec = d.filePath(s);
        p.appliesTo = QStringLiteral("any");
        p.detach = false;
        p.sourceFile = d.filePath(s);
        plugins.push_back(p);
    }

    const NativeManifestProvider& nativeProvider = nativeManifestProvider();
    if (nativeProvider) {
        std::vector<Plugin> nativePlugins = nativeProvider();
        for (Plugin& p : nativePlugins) {
            // конфиги и скрипты с тем же именем имеют приоритет перед нативными библиотеками
            bool shadowed = false;
            for (const Plugin& q : plugins)
                if (!q.native && q.id == p.id) { shadowed = true; break; }
            if (!shadowed) plugins.push_back(std::move(p));
        }
    }

    return plugins;
}

inline Plugin findById(const QString& id) {
    for (const Plugin& p : load())
        if (p.id == id) return p;
    return Plugin{};
}

inline bool matches(const Plugin& p, const std::filesystem::path& path, bool isDir) {
    if (p.appliesTo == "files" && isDir) return false;
    if (p.appliesTo == "dirs" && !isDir) return false;
    if (!p.extensions.isEmpty()) {
        if (isDir) return false;
        const QString name = QString::fromUtf8(path.filename().c_str()).toLower();
        bool ok = false;
        for (const QString& e : p.extensions) {
            if (name.endsWith(e)) { ok = true; break; }
        }
        if (!ok) return false;
    }
    return true;
}

inline bool isDirPath(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::is_directory(p, ec);
}

inline bool appliesToSelection(const Plugin& p,
                               const std::vector<std::filesystem::path>& selection) {
    if (selection.empty()) return true;
    for (const auto& s : selection)
        if (!matches(p, s, isDirPath(s))) return false;
    return true;
}

inline Plugin overrideFor(const QString& actionId,
                          const std::vector<std::filesystem::path>& selection = {}) {
    const QString id = actionId.trimmed().toLower();
    if (!isValidActionId(id) || isProtectedActionId(id)) return Plugin{};
    for (const Plugin& p : load()) {
        if (p.native) continue; // override в .so обслуживает nativepluginhost
        if (!p.overrides.contains(id)) continue;
        if (!selection.empty() && !appliesToSelection(p, selection)) continue;
        return p;
    }
    return Plugin{};
}

inline Plugin previewPluginFor(const std::filesystem::path& path) {
    for (const Plugin& p : load()) {
        if (p.native) continue; // превью в .so обслуживает nativepluginhost
        if (!p.isPreview && p.type != QLatin1String("preview")) continue;
        if (matches(p, path, false)) return p;
    }
    return Plugin{};
}

inline QProcessEnvironment pluginEnvironment(const QString& cwd, const QStringList& paths,
                                             const QString& action = QString(),
                                             const QString& query = QString()) {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("ATLIFM_DIR"), cwd);
    env.insert(QStringLiteral("ATLIFM_SELECTION"), paths.join(QLatin1Char('\n')));
    env.insert(QStringLiteral("ATLIFM_FILE"), paths.isEmpty() ? QString() : paths.first());
    env.insert(QStringLiteral("ATLIFM_TARGET"), paths.isEmpty() ? cwd : paths.first());
    env.insert(QStringLiteral("ATLIFM_PID"), QString::number(QCoreApplication::applicationPid()));
    if (!action.isEmpty()) {
        env.insert(QStringLiteral("ATLIFM_ACTION"), action);
    }
    env.insert(QStringLiteral("ATLIFM_QUERY"), query);
    return env;
}

inline QProcess* spawn(const Plugin& plugin, const QStringList& paths,
                       const QString& cwd, QObject* parent, QString* errorOut = nullptr,
                       const QString& query = QString()) {
    const QStringList parts = QProcess::splitCommand(plugin.exec);
    if (parts.isEmpty()) {
        if (errorOut) *errorOut = QStringLiteral("empty Exec");
        return nullptr;
    }
    auto* proc = new QProcess(parent);
    proc->setProgram(parts.first());
    QStringList arguments = parts.mid(1);
    if (!query.isEmpty()) arguments << query;
    arguments << paths;
    proc->setArguments(arguments);
    proc->setWorkingDirectory(cwd);
    proc->setProcessEnvironment(pluginEnvironment(cwd, paths, plugin.action, query));
    proc->setProcessChannelMode(QProcess::MergedChannels);
    proc->start();
    if (!proc->waitForStarted(3000)) {
        if (errorOut) *errorOut = proc->errorString();
        delete proc;
        return nullptr;
    }
    if (errorOut) errorOut->clear();
    return proc;
}

inline bool spawnDetached(const Plugin& plugin, const QStringList& paths,
                          const QString& cwd, QString* errorOut = nullptr,
                          const QString& query = QString()) {
    const QStringList parts = QProcess::splitCommand(plugin.exec);
    if (parts.isEmpty()) {
        if (errorOut) *errorOut = QStringLiteral("empty Exec");
        return false;
    }
    QProcess proc;
    proc.setProgram(parts.first());
    QStringList arguments = parts.mid(1);
    if (!query.isEmpty()) arguments << query;
    arguments << paths;
    proc.setArguments(arguments);
    proc.setWorkingDirectory(cwd);
    proc.setProcessEnvironment(pluginEnvironment(cwd, paths, plugin.action, query));
    qint64 pid = 0;
    const bool ok = proc.startDetached(&pid);
    if (!ok && errorOut)
        *errorOut = QStringLiteral("failed to start %1").arg(parts.first());
    return ok;
}

inline void triggerHook(const QString& eventName, const QStringList& paths, const QString& cwd) {
    const QString ev = eventName.trimmed().toLower();
    for (const Plugin& p : load()) {
        if (p.native) continue; // hooks в .so обслуживает nativepluginhost
        for (const QString& h : p.hooks) {
            if (h.trimmed().toLower() == ev) {
                QString err;
                spawnDetached(p, paths, cwd, &err);
                break;
            }
        }
    }
}

}
