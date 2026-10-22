#include "ArchiveModel.hpp"
#include "../atlifm-core/tasks/TaskManager.hpp"

#include <QApplication>
#include <QMetaObject>
#include <QPointer>
#include <QThreadPool>

#include <chrono>
#include <cstdint>

namespace Models {

ArchiveModel::ArchiveModel(QObject* parent)
    : QObject(parent) {
}

ArchiveModel::~ArchiveModel() {
    // фоновые результаты отбрасывает проверка generation, QPointer защищает от исчезнувшего объекта
    m_generation++;
}

void ArchiveModel::setArchive(const std::filesystem::path& archivePath) {
    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(archivePath, ec);
    const auto size = std::filesystem::file_size(archivePath, ec);
    const int64_t sigNs = ec ? -1
        : std::chrono::duration_cast<std::chrono::nanoseconds>(mtime.time_since_epoch()).count();

    // тот же неизменившийся архив не перечитываем
    if (archivePath == m_archivePath && (m_loaded || m_loading) &&
        sigNs == m_sigMtimeNs && static_cast<int64_t>(size) == m_sigSize) {
        return;
    }

    m_archivePath = archivePath;
    m_sigMtimeNs = sigNs;
    m_sigSize = ec ? -1 : static_cast<int64_t>(size);
    m_loading = true;
    m_loaded = false;
    m_truncated = false;
    m_error.clear();
    m_fileCount = 0;
    m_dirCount = 0;
    m_totalSize = 0;
    m_entries.clear();

    const uint64_t generation = ++m_generation;
    emit archiveReset();

    QPointer<ArchiveModel> self(this);
    Core::Tasks::TaskManager::instance().poolForKind(QStringLiteral("io"))->start([self, generation, archivePath]() {
        std::vector<Core::XdgUtils::ArchiveEntry> entries;
        bool truncated = false;
        QString err;
        const bool ok = Core::XdgUtils::listArchive(archivePath, entries, truncated, err);
        QMetaObject::invokeMethod(qApp, [self, generation, ok, err,
                                         entries = std::move(entries), truncated]() mutable {
            if (!self) return;
            self->applyListing(generation, ok, err, std::move(entries), truncated);
        }, Qt::QueuedConnection);
    });
}

void ArchiveModel::applyListing(uint64_t generation, bool ok, const QString& error,
                                std::vector<Core::XdgUtils::ArchiveEntry> entries, bool truncated) {
    if (generation != m_generation) return;
    m_loading = false;
    m_entries = std::move(entries);
    m_truncated = truncated;

    if (!ok) {
        m_loaded = false;
        m_error = error.isEmpty() ? QStringLiteral("unknown error") : error;
        m_fileCount = 0;
        m_dirCount = 0;
        m_totalSize = 0;
        emit loadFailed(m_error);
        return;
    }

    // одиночный поток без записей (.zst/.lz4, 7z не отдаёт path),
    // синтезируем единственный файл из имени архива
    if (m_entries.empty() && Core::XdgUtils::isSingleStreamArchive(m_archivePath)) {
        Core::XdgUtils::ArchiveEntry e;
        e.path = QString::fromUtf8(Core::XdgUtils::archiveExtractDirName(m_archivePath).string().c_str());
        e.isDir = false;
        if (!e.path.isEmpty()) m_entries.push_back(std::move(e));
    }

    m_error.clear();
    m_fileCount = 0;
    m_dirCount = 0;
    m_totalSize = 0;
    for (const auto& e : m_entries) {
        if (e.isDir) {
            ++m_dirCount;
        } else {
            ++m_fileCount;
            m_totalSize += e.size;
        }
    }

    m_loaded = true;
    emit loaded();
}

}
