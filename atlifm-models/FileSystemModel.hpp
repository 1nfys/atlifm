#pragma once

#include <QAbstractListModel>
#include <QString>
#include <QColor>
#include <QFileSystemWatcher>
#include <QTimer>
#include <QMutex>
#include <QHash>
#include <QSet>
#include <filesystem>
#include <vector>
#include <unordered_set>
#include <atomic>
#include <memory>
#include <QVector>
#include <QPointer>

namespace Core::Tools { class ProcessRunner; }

namespace Models {

struct FileItem {
    std::filesystem::path path;
    QString filename;
    bool isDirectory{false};
    bool isSymlink{false};
    bool isHidden{false};
    bool isExecutable{false};
    bool isSelected{false};
    uintmax_t sizeBytes{0};
    QString sizeFormatted;
    QString modifiedDate;
    time_t modifiedTimestamp{0};
    QString permissions;
    QString iconGlyph;
    QColor iconColor;
    int fuzzyScore{0};
    QString symlinkTarget;
};

class FileSystemModel : public QAbstractListModel {
    Q_OBJECT

public:
    enum FileRoles {
        NameRole = Qt::DisplayRole,
        PathRole = Qt::UserRole + 1,
        IsDirRole,
        IsSymlinkRole,
        IsHiddenRole,
        IsExecutableRole,
        IsSelectedRole,
        SizeBytesRole,
        RawSizeRole = SizeBytesRole,
        SizeRole,
        DateRole,
        PermsRole,
        IconGlyphRole,
        IconColorRole,
        FuzzyScoreRole,
        SymlinkTargetRole
    };

    explicit FileSystemModel(QObject* parent = nullptr);
    ~FileSystemModel() override;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setDirectory(const std::filesystem::path& dirPath);
    const std::filesystem::path& currentDirectory() const { return m_currentDir; }

    // панель превью не рисует размер/дату/права: не тратим на них statx и форматирование
    void setLightweightScan(bool enabled) { m_lightweightScan = enabled; }
    bool lightweightScan() const { return m_lightweightScan; }

    void setFilter(const QString& filter);
    const QString& filter() const { return m_filter; }

    void searchEverywhere(const QString& query);
    void searchContents(const QString& query);
    void clearGlobalSearch();
    bool globalSearchActive() const { return m_globalSearch; }

    void setShowHidden(bool show);
    bool showHidden() const { return m_showHidden; }

    // поиск строки по пути
    int findRowByPath(const std::filesystem::path& path) const;

    // выделение
    void toggleSelection(int row);
    void setItemSelected(int row, bool selected);
    void clearSelection();
    void setSelectedPaths(const std::unordered_set<std::string>& paths);
    const std::unordered_set<std::string>& selectedPathsSet() const { return m_selectedPaths; }
    std::vector<std::filesystem::path> selectedPaths() const;

    // сортировка
    void setSortMode(int mode); // 0 = имя, 1 = размер, 2 = дата, 3 = тип
    void setSortMode(int mode, bool ascending);
    void setSortAscending(bool ascending);
    void toggleSortDirection();
    int sortMode() const { return m_sortMode; }
    bool isSortAscending() const { return m_sortAscending; }
    // дефолт направления: name и type по возрастанию, size и date по убыванию
    static bool defaultSortAscending(int mode) { return mode == 0 || mode == 3; }
    void refresh();

    static void addInProgressPath(const std::filesystem::path& p);
    static void removeInProgressPath(const std::filesystem::path& p);
    static bool isInProgress(const std::filesystem::path& p);

    // листинг одного вызова root (sudo open или мутация) кладём в общий кэш xdgutils,
    // скан читает его и не делает свой привилегированный вызов: pkexec спросил бы пароль второй раз
    void provideSudoListing(const std::filesystem::path& dir, const QString& findOutput);

    // единая точка формата find с printf для обоих вызовов и тестов:
    // экранированный ноль это escape самого find а реальный байт nul в argv невозможен
    static QString findPrintfFormat();

    bool isLoading() const { return m_isLoading; }
    int totalCount() const { return static_cast<int>(m_allItems.size()); }

signals:
    void loadingStarted();
    void loadingFinished(int totalCount);
    void directoryLoaded(const std::filesystem::path& dirPath);
    void globalSearchFinished(int totalCount);

private:
    void applyFilterAndSort();
    void applyGlobalResults(std::vector<FileItem> items);
    void stopGlobalProcess();
    // статическая: в пуле this не разыменовывается, модель может быть разрушена
    // посреди скана; qpointer используется только при доставке результата
    static void scanDirectoryAsync(std::filesystem::path dirPath, uint64_t scanId,
                                   std::shared_ptr<std::atomic<uint64_t>> scanCounter,
                                   QPointer<FileSystemModel> self, bool lightweight);

    // find дает у каталогов вес записи а реальный вес du считается в фоне и кэшируется
    void ensureDirSizesForSort();
    void applyDirSizes(const QHash<QString, qulonglong>& sizes);
    qulonglong sortSizeOf(const FileItem& item) const;
    // ключ сортировки по типу: расширение в нижнем регистре без точки
    QString sortTypeOf(const FileItem& item) const;
    QHash<QString, qulonglong> m_dirSizeCache;
    QSet<QString> m_dirSizePending;
    static void scanRecursiveAsync(std::filesystem::path dirPath, QString pattern, uint64_t scanId,
                                   std::shared_ptr<std::atomic<uint64_t>> scanCounter, bool showHidden,
                                   QPointer<FileSystemModel> self);
    static bool parseFindListing(const QString& out, const std::filesystem::path& dirPath,
                                 uint64_t scanId, const std::shared_ptr<std::atomic<uint64_t>>& scanCounter,
                                 std::vector<FileItem>& gathered);

    std::filesystem::path m_currentDir;
    QString m_filter;
    int m_sortMode{0}; // 0 = имя, 1 = размер, 2 = дата, 3 = тип
    bool m_sortAscending{true};
    bool m_showHidden{true};
    bool m_lightweightScan{false};
    bool m_globalSearch{false};
    std::shared_ptr<std::atomic_bool> m_searchCancel;
    Core::Tools::ProcessRunner* m_globalProcess{nullptr};
    QSet<QString> m_globalSeen;
    std::atomic<bool> m_isLoading{false};

    std::vector<FileItem> m_allItems;
    std::vector<FileItem> m_displayedItems;
    std::unordered_set<std::string> m_selectedPaths;

    // счётчик поколений сканов: воркеры держат копию shared_ptr, проверка
    // протухания валидна даже после разрушения модели посреди скана (без uaf)
    std::shared_ptr<std::atomic<uint64_t>> m_scanCounter = std::make_shared<std::atomic<uint64_t>>(0);

    // кэш листингов root живёт в потокобезопасном xdgutils,
    // доступ из потока gui и из воркеров скана

    // true, пока скан каталога не доставил результат: очистка фильтра
    // знает, что листинг мог остаться от прошлого каталога
    bool m_dirScanPending{false};

    QFileSystemWatcher* m_dirWatcher{nullptr};
    QTimer* m_watcherTimer{nullptr};
};

}
