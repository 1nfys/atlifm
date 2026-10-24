#pragma once

#include "../atlifm-core/atlifm-plugin-api.h"
#include "../atlifm-core/Plugins.hpp"
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <QMutex>
#include <filesystem>
#include <functional>
#include <vector>

class QLibrary;

namespace Widgets {

class MainWindow;

// хост нативных плагинов dlopen plugins so
// сбор регистраций и диспетчеризация все колбеки кроме render превью в gui потоке
// после initialize реестр только читается
class NativePluginHost : public QObject {
    Q_OBJECT
public:
    static NativePluginHost& instance();

    // общая точка входа хуков: conf плагины процессы и нативные
    static void fireHook(const QString& event, const QStringList& paths, const QString& cwd);

    void initialize();
    void shutdownAll();
    void setWindow(MainWindow* window) { m_window = window; }

    // манифесты действий для Core::Plugins::load (меню, биндинги, читшит)
    std::vector<Core::Plugins::Plugin> manifests() const;
    QStringList overrideSummaries() const;

    bool hasAction(const QString& actionId) const;
    QString actionDisplayName(const QString& actionId) const;
    bool isActionEnabled(const QString& actionId,
                         const std::vector<std::filesystem::path>& selection,
                         const std::filesystem::path& cwd) const;

    // перехват действия, true если плагин обработал (вернул 1)
    bool tryOverride(const QString& actionId,
                     const std::vector<std::filesystem::path>& selection,
                     const std::filesystem::path& cwd);
    // запуск действия: сначала цепочка override других плагинов, затем собственный run
    bool invokeAction(const QString& actionId,
                      const std::vector<std::filesystem::path>& selection,
                      const std::filesystem::path& cwd,
                      const QString& query = QString(),
                      bool allowQueryPrompt = false);

    bool hasPreview(const std::filesystem::path& path) const;
    // вызывается из рабочего потока: доступен только write
    void renderPreview(const std::filesystem::path& path,
                       std::function<void(const char*, size_t)> write) const;

    void triggerHook(const QString& event, const QStringList& paths, const QString& cwd);

signals:
    void messageEmitted(const QString& message);
    void refreshRequested();

private:
    struct ActionRecord {
        QString pluginId;
        QString actionId;
        QString name;
        QString description;
        QStringList keys;
        QString appliesTo;
        QStringList extensions;
        QString command;
        QString commandMode;
        QString commandTitle;
        QString commandDescription;
        QString commandPlaceholder;
        void* user{nullptr};
        int (*run)(const AtlifmInvocation*, void*) {nullptr};
        int (*enabled)(const AtlifmInvocation*, void*) {nullptr};
    };
    struct OverrideRecord {
        QString pluginId;
        QString actionId;
        void* user{nullptr};
        AtlifmOverrideFn run{nullptr};
    };
    struct PreviewRecord {
        QString pluginId;
        QStringList extensions;
        void* user{nullptr};
        AtlifmPreviewFn render{nullptr};
    };
    struct HookRecord {
        QString pluginId;
        QString event;
        void* user{nullptr};
        AtlifmHookFn fn{nullptr};
    };
    struct PluginInstance {
        QString id;
        QString path;
        QLibrary* lib{nullptr};
        void (*shutdownFn)() {nullptr};
    };

    // C ABI точки входа таблицы AtlifmHostApi
    static const AtlifmHostApi& hostApi();
    static const char* apiVersion();
    static void apiOutputWrite(const char* text);
    static void apiOutputWritePane(int pane, const char* text);
    static void apiNotify(const char* message);
    static int apiRunCommand(const char* commandLine, int detach);
    static void apiRefresh();
    static int apiRegisterAction(const AtlifmActionDesc* desc);
    static int apiRegisterOverride(const char* actionId, AtlifmOverrideFn run, void* user);
    static int apiRegisterPreview(const char* extensions, AtlifmPreviewFn render, void* user);
    static int apiRegisterHook(const char* event, AtlifmHookFn fn, void* user);
    static void apiLog(const char* level, const char* message);

    void loadPluginFile(const QString& path);
    QString pluginDisplayName(const QString& pluginId) const;
    const PreviewRecord* previewRecordFor(const std::filesystem::path& path) const;
    bool recordApplies(const QString& appliesTo, const QStringList& extensions,
                       const std::vector<std::filesystem::path>& selection) const;
    bool runOverrideChain(const QString& actionId,
                          const std::vector<std::filesystem::path>& selection,
                          const std::filesystem::path& cwd,
                          const QString& skipPluginId,
                          const QString& query = QString());
    void beginSession(const QString& title, const QString& actionId,
                      const std::vector<std::filesystem::path>& selection,
                      const std::filesystem::path& cwd,
                      const QString& query = QString());
    void endSession(int result);
    void tryFinishSession();
    void ensurePanesBegun();
    void sessionAppend(const QByteArray& data);
    void sessionAppendPane(int pane, const QByteArray& data);
    void onSessionProcessFinished(quint64 generation);

    MainWindow* m_window{nullptr};
    bool m_initialized{false};
    bool m_loading{false};
    QString m_currentPlugin;
    QString m_activePlugin;
    std::vector<PluginInstance> m_plugins;
    std::vector<ActionRecord> m_actions;
    std::vector<OverrideRecord> m_overrides;
    std::vector<PreviewRecord> m_previews;
    std::vector<HookRecord> m_hooks;
    // renderPreview идёт из рабочего потока: сериализует registry против
    // shutdownAll, выгрузка .so не должна застать render в полёте
    mutable QMutex m_registryMutex;

    struct Session {
        bool context{false};
        bool panesBegun{false};
        bool viaOverride{false};
        QString title;
        QString actionId;
        QString cwd;
        QString query;
        QStringList selection;
        quint64 generation{0};
        int pending{0};
        bool runReturned{false};
        int runResult{0};
    } m_session;
};

}
