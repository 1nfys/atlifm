#pragma once

// менеджер фоновых задач для прогресса и отмены

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>

class QThreadPool;

namespace Core::Tasks {

using TaskId = quint64;

enum class State {
    Queued,
    Running,
    Done,
    Failed,
    Cancelled,
};

struct TaskInfo {
    TaskId id{0};
    QString title;
    QString kind;                 // вид пула например io или listing или preview или search или plugin
    State state{State::Queued};
    qint64 done{0};
    qint64 total{-1};
    QString unit{QStringLiteral("items")};
    QString error;
    bool cancelRequested{false};
    QDateTime startedAt;
    QDateTime finishedAt;
};

// контекст для задачи и все методы потокобезопасны
class TaskContext {
public:
    TaskContext();
    ~TaskContext();

    TaskContext(const TaskContext&) = default;
    TaskContext& operator=(const TaskContext&) = default;

    TaskId id() const;
    bool cancelled() const;
    void progress(qint64 done, qint64 total = -1);
    void log(const QString& line);
    void fail(const QString& error);

    // токен отмены для связи с операциями
    std::shared_ptr<std::atomic_bool> cancelToken() const;

private:
    struct Shared;
    explicit TaskContext(std::shared_ptr<Shared> shared);

    std::shared_ptr<Shared> m_shared;
    friend class TaskManager;
};

class TaskManager final : public QObject {
    Q_OBJECT

public:
    static TaskManager& instance();
    ~TaskManager() override;

    // запуск задачи в пуле с приоритетом и токеном отмены
    TaskId submit(QString title,
                  QString kind,
                  std::function<void(TaskContext&)> work,
                  int priority = 0,
                  std::shared_ptr<std::atomic_bool> cancelToken = {},
                  QString unit = QStringLiteral("items"));

    void cancel(TaskId id);
    void cancelAll();
    // отмена всех задач и ожидание завершения до закрытия приложения
    void waitForIdle(int timeoutMs = 5000);

    QVector<TaskInfo> tasks() const;      // список в порядке создания
    TaskInfo task(TaskId id) const;       // пустая задача если не найдена
    QStringList log(TaskId id) const;
    int activeCount() const;

    // именованные пулы для фоновых операций
    QThreadPool* poolForKind(const QString& kind) const;

signals:
    void added(const Core::Tasks::TaskInfo& info);
    void updated(const Core::Tasks::TaskInfo& info);
    void finished(const Core::Tasks::TaskInfo& info);
    void logAppended(Core::Tasks::TaskId id, const QString& line);

private:
    explicit TaskManager(QObject* parent = nullptr);

    friend class TaskContext;

    void execute(TaskId id, std::shared_ptr<TaskContext::Shared> shared);
    void markRunning(TaskId id);
    void updateProgress(TaskId id, qint64 done, qint64 total);
    void appendLog(TaskId id, const QString& line);
    void finishTask(TaskId id, State state, const QString& error);
    void trimHistory();

    struct TaskRecord {
        TaskInfo info;
        std::deque<QString> log;
        std::shared_ptr<std::atomic_bool> token;
    };

    QThreadPool* m_ioPool{nullptr};
    QThreadPool* m_listingPool{nullptr};
    QThreadPool* m_previewPool{nullptr};
    QThreadPool* m_searchPool{nullptr};
    QThreadPool* m_pluginPool{nullptr};

    QVector<TaskId> m_order;
    QHash<TaskId, TaskRecord> m_records;
    TaskId m_nextId{0};
    std::atomic_bool m_shuttingDown{false};
};

} // namespace Core::Tasks

Q_DECLARE_METATYPE(Core::Tasks::TaskInfo)
