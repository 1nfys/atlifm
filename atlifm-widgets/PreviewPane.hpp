#pragma once

#include <QWidget>
#include <QStackedWidget>
#include <QLabel>
#include <QPlainTextEdit>
#include <QImage>
#include <QSvgRenderer>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QTimer>
#include <QSet>
#include <filesystem>
#include <atomic>
#include <memory>
#include <functional>
#include <vector>

namespace Models {
class FileSystemModel;
class ArchiveModel;
}

namespace Widgets {

struct ConflictItem {
    std::filesystem::path source;
    std::filesystem::path dest;
    bool replace{true};
};

class CodeEditorWithLineNumbers;
class MillerListView;
class ArchiveTreeWidget;
class FileItemDelegate;
class InteractiveImageViewer;
class ConflictResolverWidget;
class DocumentViewer;

class PreviewPane : public QWidget {
    Q_OBJECT

public:
    explicit PreviewPane(QWidget* parent = nullptr);
    ~PreviewPane() override;

    void previewPath(const std::filesystem::path& path);
    void clearPreview();
    // фоновая предзагрузка соседних видео/PDF; не меняет текущую страницу
    void prefetchPath(const std::filesystem::path& path);

    bool archivePageActive() const;
    void clearArchiveSelection();
    void selectAllArchiveEntries();
    void selectFirstArchiveEntry();
    void selectLastArchiveEntry();
    void copyArchiveSelection();

    // выбор варианта разрешения конфликтов в правой панели
    void showConflictResolver(const std::vector<ConflictItem>& items,
                              std::function<void(const std::vector<ConflictItem>&)> onApply,
                              std::function<void()> onCancel = nullptr);

signals:
    void enterDirectoryRequested(const std::filesystem::path& path);
    // архив из листинга превью открывается в центральной колонке, как папка
    void archiveOpenRequested(const std::filesystem::path& path);
    void propertiesRequested(const std::filesystem::path& path);
    void archiveSelectionInteractionStarted();
    void messageRequested(const QString& message);
    // Open With… из меню папки в превью, полноразмерный диалог оверлеем в MainWindow
    void openWithDialogRequested(const std::filesystem::path& path);
    // Extract In… из меню папки в превью, архив фиксирован, каталог назначения выбирается оверлеем в MainWindow
    void extractInRequested(const std::filesystem::path& archivePath);
    // plugins из меню папки в превью, target это элемент под меню (пустое место
    // значит сам каталог), исполнение в MillerViewContainer (TuiOutputOverlay + refresh)
    void pluginRunRequested(const QString& pluginId,
                            const std::filesystem::path& target,
                            const std::filesystem::path& cwd);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private slots:
    void onDirContextMenu(const QPoint& globalPos, const QModelIndex& idx);

private:
    enum PageIndex {
        PageEmpty = 0,
        PageImage = 1,
        PageCode = 2,
        PageDir = 3,
        PageHex = 4,
        PageMedia = 5,
        PageConflictResolver = 6,
        PageArchive = 7,
        PageDocument = 8
    };

    void setupUi();
    // skipPlugin=true: принудительно встроенный предпросмотр после неудачного плагина, без зацикливания
    void loadPathNow(const std::filesystem::path& path, bool skipPlugin = false);

    QStackedWidget* m_stackedWidget{nullptr};

    QLabel* m_emptyLabel{nullptr};

    // интерактивный просмотр изображения: панорамирование, зум, сброс двойным кликом, перетаскивание файла наружу
    QWidget* m_imageContainer{nullptr};
    InteractiveImageViewer* m_imageViewer{nullptr};
    QLabel* m_imageInfoLabel{nullptr};
    QImage m_currentImage;

    // просмотр кода
    CodeEditorWithLineNumbers* m_codeViewer{nullptr};

    // просмотр каталога
    MillerListView* m_dirListView{nullptr};
    Models::FileSystemModel* m_dirModel{nullptr};
    FileItemDelegate* m_dirDelegate{nullptr};

    // просмотр шестнадцатеричного дампа
    QPlainTextEdit* m_hexViewer{nullptr};

    // просмотр метаданных медиа
    QPlainTextEdit* m_mediaViewer{nullptr};

    // просмотр содержимого архива: раскрытое дерево, перетаскивание с распаковкой во временный кэш
    QWidget* m_archivePage{nullptr};
    ArchiveTreeWidget* m_archiveTree{nullptr};
    Models::ArchiveModel* m_archiveModel{nullptr};

    // PDF: постраничный просмотр, страницы рендерятся по мере прокрутки
    DocumentViewer* m_documentViewer{nullptr};
    std::filesystem::path m_documentPath;
    std::shared_ptr<std::atomic_bool> m_documentCancel;
    int m_documentGeneration{0};
    int m_documentPages{-1};
    void startDocumentPreview(const std::filesystem::path& path, int pageCount);
    void requestDocumentPage(int page);
    void resetDocumentPreview();

    // виджет разрешения конфликтов
    ConflictResolverWidget* m_conflictResolver{nullptr};

    struct CachedImageEntry {
        std::filesystem::path path;
        int64_t mtimeSec{0};
        int64_t mtimeNsec{0};
        uintmax_t fileSize{0};
        QImage image;          // уменьшенная копия для кэша а не полный размер
        QSize originalSize;    // размер исходника, для догрузки при зуме
        QString info;
    };

    void performClearNow();
    void storeInImageCache(CachedImageEntry entry);
    void displayImageImmediate(const std::filesystem::path& path,
                               int64_t mtimeSec, int64_t mtimeNsec, uintmax_t fileSize,
                               const QImage& img, const QSize& originalSize, const QString& info);

    // состояние воркера для асинхронной загрузки (shared_ptr, чтобы фоновые воркеры безопасно переживали PreviewPane)
    std::filesystem::path m_activePath;
    int64_t m_activeMtimeSec{0};
    int64_t m_activeMtimeNsec{0};
    uintmax_t m_activeFileSize{0};
    std::vector<CachedImageEntry> m_imageCache;
    std::shared_ptr<std::atomic<uint64_t>> m_currentJobId{std::make_shared<std::atomic<uint64_t>>(0)};

    // debounce быстрых смен выделения (ховер DnD, резина, стрелки): грузим только
    // устоявшийся путь, иначе рваные переключения страниц дают моргание
    QTimer* m_previewDebounce{nullptr};
    std::filesystem::path m_pendingPath;
    bool m_hasPending{false};
    bool m_pendingClear{false};

    // видео/PDF: до двух фоновых предзагрузок соседних элементов
    QSet<QString> m_prefetchInFlight;
};

}
