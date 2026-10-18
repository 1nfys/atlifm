#pragma once

#include <QAbstractListModel>
#include <QColor>
#include <QHash>
#include <QString>
#include <cstdint>
#include <filesystem>
#include <vector>
#include "../atlifm-core/XdgUtils.hpp"
#include "FileSystemModel.hpp"

namespace Models {

// листинг содержимого архива для правой панели превью, роли совпадают с
// FileSystemModel, делегат тот же; путь записи виртуальный: «архив\x1Fпуть
// внутри архива» (PathRole), вход в каталог это смена префикса (m_prefix)
// каталоги синтезируются по путям файлов
class ArchiveModel : public QAbstractListModel {
    Q_OBJECT

public:
    // те же id ролей, что у FileSystemModel: делегат и MillerListView читают единый набор
    enum ArchiveRoles {
        NameRole = FileSystemModel::NameRole,
        PathRole = FileSystemModel::PathRole,
        IsDirRole = FileSystemModel::IsDirRole,
        IsSymlinkRole = FileSystemModel::IsSymlinkRole,
        IsHiddenRole = FileSystemModel::IsHiddenRole,
        IsSelectedRole = FileSystemModel::IsSelectedRole,
        RawSizeRole = FileSystemModel::RawSizeRole,
        SizeRole = FileSystemModel::SizeRole,
        DateRole = FileSystemModel::DateRole,
        PermsRole = FileSystemModel::PermsRole,
        IconGlyphRole = FileSystemModel::IconGlyphRole,
        IconColorRole = FileSystemModel::IconColorRole,
        FuzzyScoreRole = FileSystemModel::FuzzyScoreRole,
        SymlinkTargetRole = FileSystemModel::SymlinkTargetRole
    };

    explicit ArchiveModel(QObject* parent = nullptr);
    ~ArchiveModel() override;

    // асинхронная загрузка через пул, повторный вызов для неизменившегося файла не перечитывает листинг
    void setArchive(const std::filesystem::path& archivePath);
    const std::filesystem::path& archivePath() const { return m_archivePath; }

    bool isLoading() const { return m_loading; }
    bool isLoaded() const { return m_loaded; }
    const QString& error() const { return m_error; }
    bool isTruncated() const { return m_truncated; }

    // виртуальное дерево архива
    void navigateInto(const QString& entryDirPath);
    bool navigateUp();
    const QString& prefix() const { return m_prefix; }
    bool atRoot() const { return m_prefix.isEmpty(); }

    // строки текущего префикса
    QString entryPathAt(int row) const;   // нормализованный путь ("" у «..»)
    QString memberPathAt(int row) const;  // имя как в архиве (для распаковки)
    bool isParentRow(int row) const;
    bool isDirAt(int row) const;

    // сводка по архиву для заголовка страницы
    int fileCount() const { return m_fileCount; }
    int dirCount() const { return m_dirCount; }
    quint64 totalSize() const { return m_totalSize; }

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void loaded();
    void loadFailed(const QString& error);
    void prefixChanged();

private:
    struct Row {
        QString name;        // отображаемое имя (у «..» «..»)
        QString entryPath;   // нормализованный путь внутри архива (без «/»)
        QString memberPath;  // исходное имя в архиве (может быть «./…»)
        quint64 size{0};
        bool isDir{false};
        QString dateText;    // dd.MM hh:mm
        QString glyph;
        QColor color;
        bool parentRow{false};
    };

    void applyListing(uint64_t generation, bool ok, const QString& error,
                      std::vector<Core::XdgUtils::ArchiveEntry> entries, bool truncated);
    void rebuildRows();

    std::filesystem::path m_archivePath;
    QString m_prefix; // "" либо "dir/" (всегда с завершающим слэшем)
    std::vector<Core::XdgUtils::ArchiveEntry> m_entries;
    std::vector<Row> m_rows;

    bool m_loading{false};
    bool m_loaded{false};
    bool m_truncated{false};
    QString m_error;

    // подпись файла (mtime/размер), setArchive для неизменившегося архива не перечитывает листинг
    int64_t m_sigMtimeNs{-1};
    int64_t m_sigSize{-1};

    int m_fileCount{0};
    int m_dirCount{0};
    quint64 m_totalSize{0};
    uint64_t m_generation{0};
};

}
