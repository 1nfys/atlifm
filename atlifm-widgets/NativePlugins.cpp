#include "NativePlugins.hpp"
#include "MainWindow.hpp"
#include "TuiInputPopup.hpp"
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QProcess>
#include <QWidget>
#include <utility>

namespace Widgets {

namespace {

QByteArray utf8(const std::filesystem::path& p) {
    return QString::fromUtf8(p.c_str()).toUtf8();
}

// хранилище си строк вызова живет на стеке до конца колбека
struct InvocationStorage {
    QByteArray event;
    QByteArray action;
    QByteArray cwd;
    QByteArray query;
    std::vector<QByteArray> selection;
    std::vector<const char*> selectionPtrs;
    AtlifmInvocation inv;

    InvocationStorage(const QString& eventName, const QString& actionId,
                      const std::filesystem::path& cwdPath,
                      const std::vector<std::filesystem::path>& paths,
                      const QString& queryText = QString()) {
        event = eventName.toUtf8();
        action = actionId.toUtf8();
        cwd = utf8(cwdPath);
        query = queryText.toUtf8();
        selection.reserve(paths.size());
        for (const auto& p : paths) selection.push_back(utf8(p));
        selectionPtrs.reserve(selection.size());
        for (const auto& s : selection) selectionPtrs.push_back(s.constData());
        inv.struct_size = static_cast<uint32_t>(sizeof(AtlifmInvocation));
        inv.event = eventName.isEmpty() ? nullptr : event.constData();
        inv.action = actionId.isEmpty() ? nullptr : action.constData();
        inv.cwd = cwd.constData();
        inv.selection = selectionPtrs.empty() ? nullptr : selectionPtrs.data();
        inv.selection_count = selectionPtrs.size();
        inv.query = queryText.isEmpty() ? nullptr : query.constData();
    }
};

QStringList normalizeExtensions(const char* spec) {
    QStringList out;
    if (!spec) return out;
    const QStringList parts = QString::fromUtf8(spec).split(',', Qt::SkipEmptyParts);
    for (const QString& raw : parts) {
        QString e = raw.trimmed().toLower();
        if (e.isEmpty()) continue;
        if (!e.startsWith('.')) e.prepend('.');
        out.append(e);
    }
    return out;
}

}

NativePluginHost& NativePluginHost::instance() {
    static NativePluginHost host;
    return host;
}

const AtlifmHostApi& NativePluginHost::hostApi() {
    static const AtlifmHostApi table = {
        ATLIFM_PLUGIN_ABI_VERSION,
        static_cast<uint32_t>(sizeof(AtlifmHostApi)),
        &NativePluginHost::apiVersion,
        &NativePluginHost::apiOutputWrite,
        &NativePluginHost::apiNotify,
        &NativePluginHost::apiRunCommand,
        &NativePluginHost::apiRefresh,
        &NativePluginHost::apiRegisterAction,
        &NativePluginHost::apiRegisterOverride,
        &NativePluginHost::apiRegisterPreview,
        &NativePluginHost::apiRegisterHook,
        &NativePluginHost::apiLog,
        &NativePluginHost::apiOutputWritePane,
    };
    return table;
}

void NativePluginHost::fireHook(const QString& event, const QStringList& paths,
                                const QString& cwd) {
    Core::Plugins::triggerHook(event, paths, cwd);
    instance().triggerHook(event, paths, cwd);
}

void NativePluginHost::initialize() {
    if (m_initialized) return;
    m_initialized = true;
    const QString dir = Core::Plugins::pluginsDir();
    if (dir.isEmpty()) return;
    const QDir d(dir);
    const QStringList files = d.entryList({QStringLiteral("*.so")}, QDir::Files, QDir::Name);
    for (const QString& f : files) loadPluginFile(d.filePath(f));
    qInfo("[plugins] plugins: %d, actions: %d, overrides: %d, previews: %d, hooks: %d",
          static_cast<int>(m_plugins.size()), static_cast<int>(m_actions.size()),
          static_cast<int>(m_overrides.size()), static_cast<int>(m_previews.size()),
          static_cast<int>(m_hooks.size()));
}

void NativePluginHost::loadPluginFile(const QString& path) {
    const QString id = QFileInfo(path).completeBaseName();
    for (const PluginInstance& inst : m_plugins)
        if (inst.id == id) return;

    const qsizetype actionMark = static_cast<qsizetype>(m_actions.size());
    const qsizetype overrideMark = static_cast<qsizetype>(m_overrides.size());
    const qsizetype previewMark = static_cast<qsizetype>(m_previews.size());
    const qsizetype hookMark = static_cast<qsizetype>(m_hooks.size());

    auto* lib = new QLibrary(path);
    auto fail = [this, lib, path, actionMark, overrideMark, previewMark, hookMark](const QString& why) {
        qWarning("[plugins] %s skipped: %s", qUtf8Printable(path), qUtf8Printable(why));
        m_actions.resize(actionMark);
        m_overrides.resize(overrideMark);
        m_previews.resize(previewMark);
        m_hooks.resize(hookMark);
        delete lib;
    };

    if (!lib->load()) {
        fail(lib->errorString());
        return;
    }

    using InitFn = int (*)(const AtlifmHostApi*);
    auto initFn = reinterpret_cast<InitFn>(lib->resolve("atlifm_plugin_init"));
    if (!initFn) {
        fail(QStringLiteral("no atlifm_plugin_init symbol"));
        return;
    }
    using ShutdownFn = void (*)();
    auto shutdownFn = reinterpret_cast<ShutdownFn>(lib->resolve("atlifm_plugin_shutdown"));

    m_loading = true;
    m_currentPlugin = id;
    const int rc = initFn(&hostApi());
    m_loading = false;
    m_currentPlugin.clear();

    if (rc != 0) {
        fail(QStringLiteral("init returned %1").arg(rc));
        return;
    }

    qInfo("[plugins] loaded plugin: %s", qUtf8Printable(id));
    m_plugins.push_back({id, path, lib, shutdownFn});
}

void NativePluginHost::shutdownAll() {
    QMutexLocker lock(&m_registryMutex);
    for (PluginInstance& inst : m_plugins) {
        if (inst.shutdownFn) inst.shutdownFn();
        delete inst.lib;
        inst.lib = nullptr;
    }
    m_plugins.clear();
    m_actions.clear();
    m_overrides.clear();
    m_previews.clear();
    m_hooks.clear();
    m_initialized = false;
}

std::vector<Core::Plugins::Plugin> NativePluginHost::manifests() const {
    std::vector<Core::Plugins::Plugin> out;
    out.reserve(m_actions.size());
    for (const ActionRecord& a : m_actions) {
        Core::Plugins::Plugin p;
        p.native = true;
        p.id = a.pluginId;
        p.name = a.name;
        p.description = a.description;
        p.action = a.actionId;
        p.keys = a.keys;
        p.appliesTo = a.appliesTo;
        p.extensions = a.extensions;
        p.command = a.command;
        p.commandMode = a.commandMode;
        p.commandTitle = a.commandTitle;
        p.commandDescription = a.commandDescription;
        p.commandPlaceholder = a.commandPlaceholder;
        p.sourceFile = a.pluginId + QStringLiteral(".so");
        out.push_back(std::move(p));
    }
    return out;
}

QStringList NativePluginHost::overrideSummaries() const {
    QStringList out;
    QStringList seen;
    for (const OverrideRecord& o : m_overrides) {
        if (seen.contains(o.pluginId)) continue;
        seen.append(o.pluginId);
        QStringList ids;
        for (const OverrideRecord& q : m_overrides)
            if (q.pluginId == o.pluginId) ids.append(q.actionId);
        out.append(QStringLiteral("%1 : Plugin %2 overrides: %3")
                       .arg(ids.join(QStringLiteral(", ")).leftJustified(16),
                            pluginDisplayName(o.pluginId),
                            o.pluginId));
    }
    return out;
}

QString NativePluginHost::pluginDisplayName(const QString& pluginId) const {
    for (const ActionRecord& a : m_actions)
        if (a.pluginId == pluginId) return a.name;
    return pluginId;
}

bool NativePluginHost::hasAction(const QString& actionId) const {
    for (const ActionRecord& a : m_actions)
        if (a.actionId == actionId) return true;
    return false;
}

QString NativePluginHost::actionDisplayName(const QString& actionId) const {
    for (const ActionRecord& a : m_actions)
        if (a.actionId == actionId) return a.name;
    return {};
}

bool NativePluginHost::recordApplies(const QString& appliesTo,
                                     const QStringList& extensions,
                                     const std::vector<std::filesystem::path>& selection) const {
    if (selection.empty()) {
        // фон каталога: как conf плагин применяется только к каталогу
        if (appliesTo == QLatin1String("files")) return false;
        if (!extensions.isEmpty()) return false;
        return true;
    }
    for (const auto& p : selection) {
        const bool isDir = Core::Plugins::isDirPath(p);
        if (appliesTo == QLatin1String("files") && isDir) return false;
        if (appliesTo == QLatin1String("dirs") && !isDir) return false;
        if (!extensions.isEmpty()) {
            if (isDir) return false;
            const QString name = QString::fromUtf8(p.filename().c_str()).toLower();
            bool ok = false;
            for (const QString& e : extensions)
                if (name.endsWith(e)) { ok = true; break; }
            if (!ok) return false;
        }
    }
    return true;
}

bool NativePluginHost::isActionEnabled(const QString& actionId,
                                       const std::vector<std::filesystem::path>& selection,
                                       const std::filesystem::path& cwd) const {
    for (const ActionRecord& a : m_actions) {
        if (a.actionId != actionId) continue;
        if (!recordApplies(a.appliesTo, a.extensions, selection)) return false;
        if (!a.enabled) return true;
        InvocationStorage st(QString(), actionId, cwd, selection);
        return a.enabled(&st.inv, a.user) > 0;
    }
    return false;
}

bool NativePluginHost::tryOverride(const QString& actionId,
                                   const std::vector<std::filesystem::path>& selection,
                                   const std::filesystem::path& cwd) {
    return runOverrideChain(actionId, selection, cwd, QString());
}

bool NativePluginHost::runOverrideChain(const QString& actionId,
                                        const std::vector<std::filesystem::path>& selection,
                                        const std::filesystem::path& cwd,
                                        const QString& skipPluginId,
                                        const QString& query) {
    const QString id = actionId.trimmed().toLower();
    if (!Core::Plugins::isValidActionId(id) || Core::Plugins::isProtectedActionId(id))
        return false;
    for (const OverrideRecord& rec : m_overrides) {
        if (rec.actionId != id || !rec.run) continue;
        if (!skipPluginId.isEmpty() && rec.pluginId == skipPluginId) continue;
        beginSession(pluginDisplayName(rec.pluginId), id, selection, cwd, query);
        m_session.viaOverride = true;
        InvocationStorage st(QString(), id, cwd, selection, query);
        m_activePlugin = rec.pluginId;
        const int handled = rec.run(&st.inv, rec.user);
        m_activePlugin.clear();
        const bool beganPanes = m_session.panesBegun;
        endSession(0);
        if (handled) return true;
        if (beganPanes && m_window) m_window->closePluginPanes();
    }
    return false;
}

bool NativePluginHost::invokeAction(const QString& actionId,
                                    const std::vector<std::filesystem::path>& selection,
                                    const std::filesystem::path& cwd,
                                    const QString& query,
                                    bool allowQueryPrompt) {
    const ActionRecord* act = nullptr;
    for (const ActionRecord& a : m_actions)
        if (a.actionId == actionId) { act = &a; break; }
    if (!act) return false;

    QString effectiveQuery = query;
    if (allowQueryPrompt && effectiveQuery.isEmpty() &&
        act->commandMode == QLatin1String("query") && !act->command.isEmpty()) {
        bool ok = false;
        const QString title = act->commandTitle.isEmpty() ? act->name : act->commandTitle;
        const QString label = !act->commandDescription.isEmpty()
            ? act->commandDescription
            : (!act->commandPlaceholder.isEmpty() ? act->commandPlaceholder : QStringLiteral("Query:"));
        QWidget* parent = m_window ? static_cast<QWidget*>(m_window) : QApplication::activeWindow();
        effectiveQuery = TuiInputPopup::getText(parent, title, label, QString(), &ok);
        if (!ok) return true;
    }

    // действие плагина тоже перехватываемо другими плагинами
    if (runOverrideChain(actionId, selection, cwd, act->pluginId, effectiveQuery)) return true;

    beginSession(act->name, actionId, selection, cwd, effectiveQuery);
    InvocationStorage st(QString(), actionId, cwd, selection, effectiveQuery);
    m_activePlugin = act->pluginId;
    const int rc = act->run ? act->run(&st.inv, act->user) : 1;
    m_activePlugin.clear();
    endSession(rc);
    return true;
}

bool NativePluginHost::hasPreview(const std::filesystem::path& path) const {
    QMutexLocker lock(&m_registryMutex);
    return previewRecordFor(path) != nullptr;
}

const NativePluginHost::PreviewRecord* NativePluginHost::previewRecordFor(
        const std::filesystem::path& path) const {
    const QString name = QString::fromUtf8(path.filename().c_str()).toLower();
    for (const PreviewRecord& rec : m_previews) {
        for (const QString& e : rec.extensions)
            if (name.endsWith(e)) return &rec;
    }
    return nullptr;
}

void NativePluginHost::renderPreview(const std::filesystem::path& path,
                                     std::function<void(const char*, size_t)> write) const {
    QMutexLocker lock(&m_registryMutex);
    const PreviewRecord* rec = previewRecordFor(path);
    if (!rec || !rec->render) return;
    const QByteArray qPath = utf8(path);
    rec->render(qPath.constData(),
                [](void* user, const char* data, size_t len) {
                    (*static_cast<std::function<void(const char*, size_t)>*>(user))(data, len);
                },
                &write, rec->user);
}

void NativePluginHost::triggerHook(const QString& event, const QStringList& paths,
                                   const QString& cwd) {
    const QString ev = event.trimmed().toLower();
    std::vector<std::filesystem::path> selection;
    selection.reserve(paths.size());
    for (const QString& p : paths) selection.emplace_back(p.toUtf8().toStdString());
    const std::filesystem::path cwdPath(cwd.toUtf8().toStdString());
    for (const HookRecord& h : m_hooks) {
        if (h.event != ev || !h.fn) continue;
        InvocationStorage st(ev, QString(), cwdPath, selection);
        m_activePlugin = h.pluginId;
        h.fn(&st.inv, h.user);
    }
    m_activePlugin.clear();
}

void NativePluginHost::beginSession(const QString& title, const QString& actionId,
                                    const std::vector<std::filesystem::path>& selection,
                                    const std::filesystem::path& cwd,
                                    const QString& query) {
    m_session.context = true;
    m_session.panesBegun = false;
    m_session.viaOverride = false;
    m_session.title = title;
    m_session.actionId = actionId;
    m_session.cwd = QString::fromUtf8(cwd.c_str());
    m_session.query = query;
    m_session.selection.clear();
    for (const auto& p : selection) m_session.selection.append(QString::fromUtf8(p.c_str()));
    m_session.pending = 0;
    m_session.runReturned = false;
    m_session.runResult = 0;
    ++m_session.generation;
}

void NativePluginHost::endSession(int result) {
    if (!m_session.context) return;
    m_session.context = false;
    m_session.runReturned = true;
    m_session.runResult = result;
    tryFinishSession();
}

void NativePluginHost::ensurePanesBegun() {
    if (m_session.panesBegun) return;
    if (!m_session.context && m_session.pending == 0) return;
    if (!m_window) return;
    m_window->beginPluginPanes(m_session.title);
    m_session.panesBegun = true;
}

void NativePluginHost::tryFinishSession() {
    if (m_session.context || !m_session.runReturned || m_session.pending > 0) return;
    if (m_session.panesBegun && m_window) {
        m_window->finishPluginPanes(m_session.runResult);
    } else if (!m_session.viaOverride) {
        emit messageEmitted(m_session.runResult == 0
            ? QStringLiteral("Plugin %1 done").arg(m_session.title)
            : QStringLiteral("Plugin %1 failed: exit %2").arg(m_session.title).arg(m_session.runResult));
    }
    m_session.runReturned = false;
    m_session.panesBegun = false;
    m_session.selection.clear();
}

void NativePluginHost::sessionAppend(const QByteArray& data) {
    if (data.isEmpty()) return;
    if (!m_session.context && m_session.pending == 0) return;
    ensurePanesBegun();
    if (m_window) m_window->appendPluginPanesOutput(data);
}

void NativePluginHost::sessionAppendPane(int pane, const QByteArray& data) {
    if (data.isEmpty() || pane < 0 || pane > 2) return;
    if (!m_session.context && m_session.pending == 0) return;
    ensurePanesBegun();
    if (m_window) m_window->appendPluginPaneOutput(pane, data);
}

void NativePluginHost::onSessionProcessFinished(quint64 generation) {
    if (m_session.generation != generation) return;
    if (m_session.pending > 0) --m_session.pending;
    tryFinishSession();
}

// C ABI

const char* NativePluginHost::apiVersion() {
    return "1.0.1";
}

void NativePluginHost::apiOutputWrite(const char* text) {
    if (!text) return;
    instance().sessionAppend(QByteArray(text));
}

void NativePluginHost::apiOutputWritePane(int pane, const char* text) {
    if (!text) return;
    instance().sessionAppendPane(pane, QByteArray(text));
}

void NativePluginHost::apiNotify(const char* message) {
    if (!message) return;
    emit instance().messageEmitted(QString::fromUtf8(message));
}

void NativePluginHost::apiRefresh() {
    emit instance().refreshRequested();
}

int NativePluginHost::apiRunCommand(const char* commandLine, int detach) {
    if (!commandLine || !*commandLine) return -1;
    auto& self = instance();
    Core::Plugins::Plugin proc;
    proc.exec = QString::fromUtf8(commandLine);
    proc.action = self.m_session.actionId;
    const QStringList paths = self.m_session.selection;
    const QString cwd = self.m_session.cwd.isEmpty()
        ? QString::fromUtf8(std::filesystem::current_path().c_str())
        : self.m_session.cwd;

    if (detach) {
        QString err;
        return Core::Plugins::spawnDetached(proc, paths, cwd, &err) ? 0 : -1;
    }
    if (!self.m_session.context) return -1;

    QString err;
    QProcess* p = Core::Plugins::spawn(proc, paths, cwd, &self, &err, self.m_session.query);
    if (!p) {
        self.sessionAppend("error: " + err.toUtf8() + "\n");
        return -1;
    }
    const quint64 gen = self.m_session.generation;
    ++self.m_session.pending;
    QObject::connect(p, &QProcess::readyRead, &self, [p, &self, gen]() {
        const QByteArray chunk = p->readAll();
        if (self.m_session.generation == gen) self.sessionAppend(chunk);
    });
    QObject::connect(p, &QProcess::finished, &self,
                     [p, &self, gen](int, QProcess::ExitStatus) {
        const QByteArray rest = p->readAll();
        if (self.m_session.generation == gen) self.sessionAppend(rest);
        p->deleteLater();
        self.onSessionProcessFinished(gen);
    });
    return 0;
}

int NativePluginHost::apiRegisterAction(const AtlifmActionDesc* desc) {
    auto& self = instance();
    if (!self.m_loading || !desc || !desc->run) return -1;
    const QString id = QString::fromUtf8(desc->id ? desc->id : "").trimmed().toLower();
    if (!Core::Plugins::isValidActionId(id) || Core::Plugins::isProtectedActionId(id))
        return -1;
    const QString name = desc->name ? QString::fromUtf8(desc->name) : QString();
    if (name.isEmpty()) return -1;
    for (const ActionRecord& a : self.m_actions)
        if (a.actionId == id) return -1;

    ActionRecord rec;
    rec.pluginId = self.m_currentPlugin;
    rec.actionId = id;
    rec.name = name;
    rec.description = desc->description ? QString::fromUtf8(desc->description) : QString();
    if (desc->keys) {
        const QStringList parts = QString::fromUtf8(desc->keys).split(',', Qt::SkipEmptyParts);
        for (const QString& k : parts)
            if (!k.trimmed().isEmpty()) rec.keys.append(k.trimmed());
    }
    const QString applies = desc->applies_to ? QString::fromUtf8(desc->applies_to).toLower()
                                             : QString();
    rec.appliesTo = (applies == QLatin1String("files") || applies == QLatin1String("dirs"))
                        ? applies : QStringLiteral("any");
    rec.extensions = normalizeExtensions(desc->extensions);
    if (desc->struct_size >= offsetof(AtlifmActionDesc, command) + sizeof(const char*)) {
        rec.command = desc->command ? QString::fromUtf8(desc->command).trimmed().toLower()
                                    : QString();
    }
    if (desc->struct_size >= offsetof(AtlifmActionDesc, command_mode) + sizeof(const char*)) {
        rec.commandMode = desc->command_mode ? QString::fromUtf8(desc->command_mode).trimmed().toLower()
                                             : QString();
    }
    if (desc->struct_size >= offsetof(AtlifmActionDesc, command_title) + sizeof(const char*)) {
        rec.commandTitle = desc->command_title ? QString::fromUtf8(desc->command_title) : QString();
    }
    if (desc->struct_size >= offsetof(AtlifmActionDesc, command_description) + sizeof(const char*)) {
        rec.commandDescription = desc->command_description
            ? QString::fromUtf8(desc->command_description)
            : QString();
    }
    if (desc->struct_size >= offsetof(AtlifmActionDesc, command_placeholder) + sizeof(const char*)) {
        rec.commandPlaceholder = desc->command_placeholder
            ? QString::fromUtf8(desc->command_placeholder)
            : QString();
    }
    if (!rec.command.isEmpty()) {
        if (!Core::Plugins::isValidCommandToken(rec.command) ||
            rec.command == QLatin1String("fd") || rec.command == QLatin1String("rg")) {
            rec.command.clear();
        }
    }
    if (rec.commandMode != QLatin1String("query")) rec.commandMode = QStringLiteral("action");
    rec.user = desc->user;
    rec.run = desc->run;
    rec.enabled = desc->enabled;
    self.m_actions.push_back(std::move(rec));
    return 0;
}

int NativePluginHost::apiRegisterOverride(const char* actionId, AtlifmOverrideFn run, void* user) {
    auto& self = instance();
    if (!self.m_loading || !actionId || !run) return -1;
    const QString id = QString::fromUtf8(actionId).trimmed().toLower();
    if (!Core::Plugins::isValidActionId(id) || Core::Plugins::isProtectedActionId(id))
        return -1;
    self.m_overrides.push_back({self.m_currentPlugin, id, user, run});
    return 0;
}

int NativePluginHost::apiRegisterPreview(const char* extensions, AtlifmPreviewFn render, void* user) {
    auto& self = instance();
    if (!self.m_loading || !render) return -1;
    const QStringList exts = normalizeExtensions(extensions);
    if (exts.isEmpty()) return -1;
    self.m_previews.push_back({self.m_currentPlugin, exts, user, render});
    return 0;
}

int NativePluginHost::apiRegisterHook(const char* event, AtlifmHookFn fn, void* user) {
    auto& self = instance();
    if (!self.m_loading || !fn || !event) return -1;
    const QString ev = QString::fromUtf8(event).trimmed().toLower();
    if (ev != QLatin1String("on_start") && ev != QLatin1String("on_navigate") &&
        ev != QLatin1String("on_select"))
        return -1;
    self.m_hooks.push_back({self.m_currentPlugin, ev, user, fn});
    return 0;
}

void NativePluginHost::apiLog(const char* level, const char* message) {
    auto& self = instance();
    const QString who = self.m_activePlugin.isEmpty() ? self.m_currentPlugin : self.m_activePlugin;
    qWarning("[plugin:%s][%s] %s", qUtf8Printable(who),
             level ? level : "info", message ? message : "");
}

}
