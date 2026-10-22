#pragma once

// запуск внешних утилит с таймаутом и лимитом вывода

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <atomic>
#include <memory>

class QProcess;
class QTimer;

namespace Core::Tools {

struct ProcessResult {
    bool started{false};
    bool finished{false};
    bool timedOut{false};
    bool cancelled{false};
    bool truncated{false};
    int exitCode{-1};
    QByteArray output;
    QByteArray error;
};

QString findTool(const QString& name);
bool hasTool(const QString& name);
void clearToolCache();

// синхронный запуск только для фоновых воркеров а в основном потоке берем processrunner
ProcessResult runSync(const QString& program,
                      const QStringList& arguments,
                      int timeoutMs,
                      qint64 maxOutputBytes = 8 * 1024 * 1024,
                      const std::shared_ptr<std::atomic_bool>& cancelToken = {},
                      const QString& workingDirectory = QString());

class ProcessRunner : public QObject {
    Q_OBJECT

public:
    explicit ProcessRunner(QObject* parent = nullptr);
    ~ProcessRunner() override;

    void setProgram(const QString& program);
    void setArguments(const QStringList& arguments);
    void setWorkingDirectory(const QString& directory);
    // ноль это сырые куски а иначе режем по разделителю
    void setRecordSeparator(char separator);
    void setMaxOutputBytes(qint64 bytes);
    void setTimeoutMs(int ms);

    void start();
    void cancel();
    bool isRunning() const;

signals:
    void recordReady(const QByteArray& record);
    void finished(const Core::Tools::ProcessResult& result);

private:
    void handleStdout();
    void handleStderr();
    void flushRecords();
    void finishWith(int exitCode);
    void failStart();

    QString m_program;
    QStringList m_arguments;
    QString m_workingDirectory;
    char m_separator{'\n'};
    qint64 m_maxOutputBytes{8 * 1024 * 1024};
    int m_timeoutMs{30000};

    QProcess* m_process{nullptr};
    QTimer* m_timeout{nullptr};
    QByteArray m_outBuffer;
    QByteArray m_errBuffer;
    ProcessResult m_result;
    bool m_finishedEmitted{false};
};

} // namespace Core::Tools

Q_DECLARE_METATYPE(Core::Tools::ProcessResult)
