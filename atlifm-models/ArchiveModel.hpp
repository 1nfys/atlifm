#pragma once

#include <QObject>
#include <QString>
#include <cstdint>
#include <filesystem>
#include <vector>
#include "../atlifm-core/XdgUtils.hpp"

namespace Models {

// асинхронный загрузчик листинга архива, дерево строит widget
class ArchiveModel : public QObject {
    Q_OBJECT

public:
    explicit ArchiveModel(QObject* parent = nullptr);
    ~ArchiveModel() override;

    // асинхронная загрузка через пул
    void setArchive(const std::filesystem::path& archivePath);
    void clear();
    void setAllowPrompt(bool allow) { m_allowPrompt = allow; }
    bool allowPrompt() const { return m_allowPrompt; }
    const std::filesystem::path& archivePath() const { return m_archivePath; }

    bool isLoading() const { return m_loading; }
    bool isLoaded() const { return m_loaded; }
    const QString& error() const { return m_error; }
    bool isTruncated() const { return m_truncated; }
    const std::vector<Core::XdgUtils::ArchiveEntry>& entries() const { return m_entries; }

    // сводка по архиву для заголовка страницы
    int fileCount() const { return m_fileCount; }
    int dirCount() const { return m_dirCount; }
    quint64 totalSize() const { return m_totalSize; }

signals:
    // старый листинг сброшен и дерево очищается до loaded
    void archiveReset();
    void loaded();
    void loadFailed(const QString& error);

private:
    void applyListing(uint64_t generation, bool ok, const QString& error,
                      std::vector<Core::XdgUtils::ArchiveEntry> entries, bool truncated);

    std::filesystem::path m_archivePath;
    std::vector<Core::XdgUtils::ArchiveEntry> m_entries;

    bool m_loading{false};
    bool m_loaded{false};
    bool m_truncated{false};
    bool m_allowPrompt{true};
    QString m_error;

    // подпись файла mtime и размер
    int64_t m_sigMtimeNs{-1};
    int64_t m_sigSize{-1};

    int m_fileCount{0};
    int m_dirCount{0};
    quint64 m_totalSize{0};
    uint64_t m_generation{0};
};

}
