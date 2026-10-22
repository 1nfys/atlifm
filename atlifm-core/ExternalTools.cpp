#include "ExternalTools.hpp"

#include <QElapsedTimer>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

namespace Core::Tools {

namespace {

QHash<QString, QString>& toolCache() {
    static QHash<QString, QString> cache;
    return cache;
}

QMutex& toolCacheMutex() {
    static QMutex mutex;
    return mutex;
}

void appendCapped(QByteArray& target, const QByteArray& chunk, qint64 limit) {
    const qint64 room = limit - target.size();
    if (room <= 0) return;
    target.append(chunk.constData(), static_cast<int>(qMin<qint64>(chunk.size(), room)));
}

} // namespace

QString findTool(const QString& name) {
    if (name.isEmpty()) return {};
    {
        QMutexLocker lock(&toolCacheMutex());
        const auto it = toolCache().constFind(name);
        if (it != toolCache().constEnd()) return it.value();
    }
    const QString path = QStandardPaths::findExecutable(name);
    QMutexLocker lock(&toolCacheMutex());
    toolCache().insert(name, path);
    return path;
}

bool hasTool(const QString& name) {
    return !findTool(name).isEmpty();
}

void clearToolCache() {
    QMutexLocker lock(&toolCacheMutex());
    toolCache().clear();
}

ProcessResult runSync(const QString& program,
                      const QStringList& arguments,
                      int timeoutMs,
                      qint64 maxOutputBytes,
                      const std::shared_ptr<std::atomic_bool>& cancelToken,
                      const QString& workingDirectory) {
    ProcessResult result;
    if (program.isEmpty()) return result;

    QProcess process;
    if (!workingDirectory.isEmpty()) process.setWorkingDirectory(workingDirectory);
    process.setProgram(program);
    process.setArguments(arguments);
    process.start();
    if (!process.waitForStarted(2000)) {
        result.error = process.errorString().toUtf8();
        return result;
    }
    result.started = true;

    QElapsedTimer clock;
    clock.start();
    bool killed = false;
    while (process.state() != QProcess::NotRunning) {
        if (cancelToken && cancelToken->load()) {
            result.cancelled = true;
            process.kill();
            killed = true;
            break;
        }
        if (timeoutMs > 0 && clock.elapsed() > timeoutMs) {
            result.timedOut = true;
            process.kill();
            killed = true;
            break;
        }
        if (process.bytesAvailable() > 0) {
            const QByteArray out = process.readAllStandardOutput();
            if (result.output.size() + out.size() > maxOutputBytes) {
                result.truncated = true;
                result.output.append(out.left(qMax<qint64>(0, maxOutputBytes - result.output.size())));
                process.kill();
                killed = true;
                break;
            }
            result.output.append(out);
            const QByteArray err = process.readAllStandardError();
            if (result.error.size() < 64 * 1024) result.error.append(err.left(64 * 1024 - result.error.size()));
        }
        process.waitForFinished(30);
    }
    if (killed) process.waitForFinished(500);
    result.finished = process.state() == QProcess::NotRunning;
    result.exitCode = result.finished ? process.exitCode() : -1;
    // процесс мог быстро упасть или завершиться так что дочитываем хвост
    const QByteArray rest = process.readAllStandardOutput();
    if (!rest.isEmpty()) {
        const qint64 room = qMax<qint64>(0, maxOutputBytes - result.output.size());
        if (rest.size() > room) {
            result.truncated = true;
            result.output.append(rest.left(static_cast<int>(room)));
        } else {
            result.output.append(rest);
        }
    }
    if (result.error.size() < 64 * 1024) {
        const QByteArray restErr = process.readAllStandardError();
        appendCapped(result.error, restErr, 64 * 1024);
    }
    return result;
}

ProcessRunner::ProcessRunner(QObject* parent)
    : QObject(parent) {
    qRegisterMetaType<Core::Tools::ProcessResult>("Core::Tools::ProcessResult");
}

ProcessRunner::~ProcessRunner() {
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished(200);
    }
}

void ProcessRunner::setProgram(const QString& program) { m_program = program; }
void ProcessRunner::setArguments(const QStringList& arguments) { m_arguments = arguments; }
void ProcessRunner::setWorkingDirectory(const QString& directory) { m_workingDirectory = directory; }
void ProcessRunner::setRecordSeparator(char separator) { m_separator = separator; }
void ProcessRunner::setMaxOutputBytes(qint64 bytes) { m_maxOutputBytes = bytes; }
void ProcessRunner::setTimeoutMs(int ms) { m_timeoutMs = ms; }

bool ProcessRunner::isRunning() const {
    return m_process && m_process->state() != QProcess::NotRunning;
}

void ProcessRunner::start() {
    if (isRunning()) return;

    m_result = ProcessResult{};
    m_outBuffer.clear();
    m_errBuffer.clear();
    m_finishedEmitted = false;

    m_process = new QProcess(this);
    if (!m_workingDirectory.isEmpty()) m_process->setWorkingDirectory(m_workingDirectory);
    m_process->setProgram(m_program);
    m_process->setArguments(m_arguments);

    connect(m_process, &QProcess::readyReadStandardOutput, this, &ProcessRunner::handleStdout);
    connect(m_process, &QProcess::readyReadStandardError, this, &ProcessRunner::handleStderr);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) failStart();
    });
    connect(m_process, &QProcess::started, this, [this]() { m_result.started = true; });
    connect(m_process, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        handleStdout();
        handleStderr();
        flushRecords();
        m_result.finished = status == QProcess::NormalExit;
        finishWith(status == QProcess::NormalExit ? exitCode : -1);
    });

    m_timeout = new QTimer(this);
    m_timeout->setSingleShot(true);
    connect(m_timeout, &QTimer::timeout, this, [this]() {
        if (!isRunning()) return;
        m_result.timedOut = true;
        m_process->kill();
    });
    if (m_timeoutMs > 0) m_timeout->start(m_timeoutMs);

    m_process->start();
}

void ProcessRunner::cancel() {
    if (!isRunning()) return;
    m_result.cancelled = true;
    m_process->kill();
}

void ProcessRunner::handleStdout() {
    if (!m_process) return;
    const QByteArray chunk = m_process->readAllStandardOutput();
    if (chunk.isEmpty()) return;

    if (m_result.output.size() >= m_maxOutputBytes) {
        if (!m_result.truncated) {
            m_result.truncated = true;
            m_process->kill();
        }
        return;
    }
    const qint64 room = m_maxOutputBytes - m_result.output.size();
    QByteArray part = chunk;
    if (chunk.size() > room) {
        part = chunk.left(static_cast<int>(room));
        m_result.truncated = true;
        m_process->kill();
    }
    m_result.output.append(part);
    m_outBuffer.append(part);

    if (m_separator == 0) {
        if (!m_outBuffer.isEmpty()) {
            emit recordReady(m_outBuffer);
            m_outBuffer.clear();
        }
        return;
    }

    int start = 0;
    for (int i = 0; i < m_outBuffer.size(); ++i) {
        if (m_outBuffer.at(i) != m_separator) continue;
        QByteArray record = m_outBuffer.mid(start, i - start);
        if (m_separator == '\n' && record.endsWith('\r')) record.chop(1);
        if (!record.isEmpty()) emit recordReady(record);
        start = i + 1;
    }
    if (start > 0) m_outBuffer.remove(0, start);
}

void ProcessRunner::handleStderr() {
    if (!m_process) return;
    const QByteArray chunk = m_process->readAllStandardError();
    if (chunk.isEmpty()) return;
    if (m_result.error.size() < 64 * 1024) {
        appendCapped(m_result.error, chunk, 64 * 1024);
    }
}

void ProcessRunner::flushRecords() {
    if (m_outBuffer.isEmpty()) return;
    if (m_separator == 0) {
        emit recordReady(m_outBuffer);
    } else {
        QByteArray record = m_outBuffer;
        if (m_separator == '\n' && record.endsWith('\r')) record.chop(1);
        if (!record.isEmpty()) emit recordReady(record);
    }
    m_outBuffer.clear();
}

void ProcessRunner::failStart() {
    if (m_finishedEmitted) return;
    m_result.error = m_process ? m_process->errorString().toUtf8() : QByteArray("failed to start");
    finishWith(-1);
}

void ProcessRunner::finishWith(int exitCode) {
    if (m_finishedEmitted) return;
    m_finishedEmitted = true;
    if (m_timeout) m_timeout->stop();
    m_result.exitCode = exitCode;
    emit finished(m_result);
}

} // namespace Core::Tools
