#include "TaskManager.hpp"

#include <QDeadlineTimer>
#include <QMetaObject>
#include <QRunnable>
#include <QThread>
#include <QThreadPool>
#include <algorithm>
#include <exception>
#include <utility>

namespace Core::Tasks {

namespace {

constexpr int kMaxFinishedTasks = 50;
constexpr int kMaxLogLines = 5000;
constexpr auto kProgressInterval = std::chrono::milliseconds(100);

bool isFinal(State state) {
    return state == State::Done || state == State::Failed || state == State::Cancelled;
}

} // namespace

struct TaskContext::Shared {
    TaskId id{0};
    TaskManager* manager{nullptr};
    std::shared_ptr<std::atomic_bool> token{std::make_shared<std::atomic_bool>(false)};
    std::atomic<qint64> done{0};
    std::atomic<qint64> total{-1};
    std::chrono::steady_clock::time_point lastProgress{};
    QString error;
    std::function<void(TaskContext&)> work;
};

TaskContext::TaskContext()
    : m_shared(std::make_shared<Shared>()) {}

TaskContext::TaskContext(std::shared_ptr<Shared> shared)
    : m_shared(std::move(shared)) {}

TaskContext::~TaskContext() = default;

TaskId TaskContext::id() const {
    return m_shared ? m_shared->id : 0;
}

bool TaskContext::cancelled() const {
    return !m_shared || !m_shared->token || m_shared->token->load();
}

void TaskContext::progress(qint64 done, qint64 total) {
    if (!m_shared || !m_shared->manager || m_shared->manager->m_shuttingDown.load()) return;

    if (done < 0) done = 0;
    m_shared->done.store(done);
    if (total >= 0) m_shared->total.store(total);

    const auto now = std::chrono::steady_clock::now();
    const bool final = total >= 0 && done >= total;
    if (!final && now - m_shared->lastProgress < kProgressInterval) return;
    m_shared->lastProgress = now;

    TaskManager* manager = m_shared->manager;
    const TaskId id = m_shared->id;
    QMetaObject::invokeMethod(manager, [manager, id, done, total]() {
        manager->updateProgress(id, done, total);
    }, Qt::QueuedConnection);
}

void TaskContext::log(const QString& line) {
    if (!m_shared || !m_shared->manager || m_shared->manager->m_shuttingDown.load() || line.isEmpty()) return;
    TaskManager* manager = m_shared->manager;
    const TaskId id = m_shared->id;
    QMetaObject::invokeMethod(manager, [manager, id, line]() {
        manager->appendLog(id, line);
    }, Qt::QueuedConnection);
}

void TaskContext::fail(const QString& error) {
    if (!m_shared) return;
    m_shared->error = error;
}

std::shared_ptr<std::atomic_bool> TaskContext::cancelToken() const {
    return m_shared ? m_shared->token : nullptr;
}

TaskManager& TaskManager::instance() {
    static TaskManager manager;
    return manager;
}

TaskManager::TaskManager(QObject* parent)
    : QObject(parent) {
    qRegisterMetaType<Core::Tasks::TaskInfo>("Core::Tasks::TaskInfo");
    setObjectName(QStringLiteral("TaskManager"));

    auto makePool = [this](int threads, const QString& name) {
        auto* pool = new QThreadPool(this);
        pool->setMaxThreadCount(threads);
        pool->setStackSize(1024 * 1024);
        pool->setExpiryTimeout(5000);
        pool->setObjectName(name);
        return pool;
    };
    // отдельные пулы чтобы плагины и приложение не делили одни потоки
    m_ioPool = makePool(2, QStringLiteral("atlifm-io"));
    // листинг файлов отдельно от копирования иначе навигация зависнет
    m_listingPool = makePool(2, QStringLiteral("atlifm-listing"));
    m_previewPool = makePool(1, QStringLiteral("atlifm-preview"));
    m_searchPool = makePool(1, QStringLiteral("atlifm-search"));
    m_pluginPool = makePool(1, QStringLiteral("atlifm-plugin"));
}

TaskManager::~TaskManager() {
    m_shuttingDown = true;
    cancelAll();
    for (QThreadPool* pool : {m_ioPool, m_listingPool, m_previewPool, m_searchPool, m_pluginPool}) {
        if (pool) pool->waitForDone();
    }
}

TaskId TaskManager::submit(QString title,
                           QString kind,
                           std::function<void(TaskContext&)> work,
                           int priority,
                           std::shared_ptr<std::atomic_bool> cancelToken,
                           QString unit) {
    TaskRecord record;
    record.info.id = ++m_nextId;
    if (record.info.id == 0) record.info.id = ++m_nextId;
    record.info.title = std::move(title);
    record.info.kind = std::move(kind);
    record.info.state = State::Queued;
    record.info.unit = std::move(unit);
    record.token = cancelToken ? std::move(cancelToken)
                               : std::make_shared<std::atomic_bool>(false);

    const TaskId id = record.info.id;
    const TaskInfo info = record.info;
    m_order.push_back(id);
    m_records.insert(id, std::move(record));

    emit added(info);

    auto shared = std::make_shared<TaskContext::Shared>();
    shared->id = id;
    shared->manager = this;
    shared->token = m_records.value(id).token;
    shared->work = std::move(work);

    QThreadPool* pool = poolForKind(info.kind);
    // задача удалит себя сама после выполнения
    pool->start(QRunnable::create([this, id, shared]() mutable {
        execute(id, shared);
    }), priority);

    return id;
}

void TaskManager::execute(TaskId id, std::shared_ptr<TaskContext::Shared> shared) {
    if (!shared) return;
    if (m_shuttingDown.load() || shared->token->load()) {
        QMetaObject::invokeMethod(this, [this, id]() {
            finishTask(id, State::Cancelled, QStringLiteral("Cancelled"));
        }, Qt::QueuedConnection);
        return;
    }

    // вызовы сохраняют порядок и запуск придет раньше завершения
    QMetaObject::invokeMethod(this, [this, id]() { markRunning(id); }, Qt::QueuedConnection);

    TaskContext context(shared);
    try {
        if (shared->work) shared->work(context);
    } catch (const std::exception& e) {
        context.fail(QString::fromUtf8(e.what()));
    } catch (...) {
        context.fail(QStringLiteral("Unhandled exception in worker"));
    }
    shared->work = {};

    const bool cancelled = shared->token->load();
    const State state = cancelled ? State::Cancelled
                                  : (shared->error.isEmpty() ? State::Done : State::Failed);
    const QString error = cancelled ? QStringLiteral("Cancelled") : shared->error;

    QMetaObject::invokeMethod(this, [this, id, state, error]() {
        finishTask(id, state, error);
    }, Qt::QueuedConnection);
}

void TaskManager::markRunning(TaskId id) {
    auto it = m_records.find(id);
    if (it == m_records.end()) return;
    if (isFinal(it->info.state)) return;
    if (it->info.state == State::Queued) {
        it->info.state = State::Running;
        it->info.startedAt = QDateTime::currentDateTimeUtc();
        const TaskInfo info = it->info;
        emit updated(info);
    }
}

void TaskManager::updateProgress(TaskId id, qint64 done, qint64 total) {
    auto it = m_records.find(id);
    if (it == m_records.end() || isFinal(it->info.state)) return;
    it->info.done = std::max<qint64>(0, done);
    if (total >= 0) it->info.total = total;
    const TaskInfo info = it->info;
    emit updated(info);
}

void TaskManager::appendLog(TaskId id, const QString& line) {
    auto it = m_records.find(id);
    if (it == m_records.end()) return;
    it->log.push_back(line);
    // удаление из начала очереди быстрое и память не копируется
    if (static_cast<int>(it->log.size()) > kMaxLogLines) {
        it->log.pop_front();
    }
    emit logAppended(id, line);
}

void TaskManager::finishTask(TaskId id, State state, const QString& error) {
    auto it = m_records.find(id);
    if (it == m_records.end() || isFinal(it->info.state)) return;

    it->info.state = state;
    it->info.error = error;
    it->info.cancelRequested = (state == State::Cancelled);
    it->info.finishedAt = QDateTime::currentDateTimeUtc();
    if (state == State::Done && it->info.total >= 0) {
        it->info.done = it->info.total;
    }
    const TaskInfo info = it->info;

    emit updated(info);
    emit finished(info);
    trimHistory();
}

void TaskManager::cancel(TaskId id) {
    auto it = m_records.find(id);
    if (it == m_records.end() || isFinal(it->info.state)) return;
    if (it->token) it->token->store(true);
    it->info.cancelRequested = true;
    const TaskInfo info = it->info;
    emit updated(info);
}

void TaskManager::cancelAll() {
    for (const TaskId id : std::as_const(m_order)) {
        auto it = m_records.find(id);
        if (it == m_records.end() || isFinal(it->info.state)) continue;
        if (it->token) it->token->store(true);
        it->info.cancelRequested = true;
        const TaskInfo info = it->info;
        emit updated(info);
    }
}

void TaskManager::waitForIdle(int timeoutMs) {
    cancelAll();
    QDeadlineTimer deadline(timeoutMs);
    for (QThreadPool* pool : {m_ioPool, m_listingPool, m_previewPool, m_searchPool, m_pluginPool}) {
        if (!pool) continue;
        const qint64 remaining = deadline.remainingTime();
        if (remaining <= 0) break;
        pool->waitForDone(static_cast<int>(remaining));
    }
}

QVector<TaskInfo> TaskManager::tasks() const {
    QVector<TaskInfo> out;
    out.reserve(m_order.size());
    for (const TaskId id : m_order) {
        auto it = m_records.constFind(id);
        if (it != m_records.constEnd()) out.push_back(it->info);
    }
    return out;
}

TaskInfo TaskManager::task(TaskId id) const {
    return m_records.value(id).info;
}

QStringList TaskManager::log(TaskId id) const {
    QStringList out;
    const auto it = m_records.constFind(id);
    if (it == m_records.constEnd()) return out;
    out.reserve(static_cast<int>(it->log.size()));
    for (const QString& line : it->log) out.append(line);
    return out;
}

int TaskManager::activeCount() const {
    int count = 0;
    for (auto it = m_records.constBegin(); it != m_records.constEnd(); ++it) {
        if (!isFinal(it->info.state)) ++count;
    }
    return count;
}

QThreadPool* TaskManager::poolForKind(const QString& kind) const {
    if (kind == QLatin1String("listing")) return m_listingPool;
    if (kind == QLatin1String("preview")) return m_previewPool;
    if (kind == QLatin1String("search")) return m_searchPool;
    if (kind == QLatin1String("plugin")) return m_pluginPool;
    return m_ioPool;
}

void TaskManager::trimHistory() {
    int finishedCount = 0;
    for (const TaskId id : std::as_const(m_order)) {
        auto it = m_records.constFind(id);
        if (it != m_records.constEnd() && isFinal(it->info.state)) ++finishedCount;
    }

    int removeCount = finishedCount - kMaxFinishedTasks;
    if (removeCount <= 0) return;

    QVector<TaskId> kept;
    kept.reserve(m_order.size());
    for (const TaskId id : std::as_const(m_order)) {
        auto it = m_records.constFind(id);
        if (it != m_records.constEnd() && isFinal(it->info.state) && removeCount > 0) {
            m_records.remove(id);
            --removeCount;
            continue;
        }
        kept.push_back(id);
    }
    m_order = std::move(kept);
}

} // namespace core::tasks
