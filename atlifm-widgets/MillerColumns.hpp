#pragma once

#include <QWidget>
#include <QListView>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QSplitter>
#include <QPointer>
#include <QByteArray>
#include <QHash>
#include <QModelIndex>
#include <QItemSelection>
#include <QRubberBand>
#include <memory>
#include <filesystem>
#include <utility>
#include <vector>
#include "../atlifm-models/FileSystemModel.hpp"
#include "FileItemDelegate.hpp"
#include "PreviewPane.hpp"

namespace Models { class ArchiveModel; }

class QTimer;

namespace Widgets {

class TuiContextMenu;
class TuiPanesOverlay;
class ArchiveTreeWidget;

class MillerListView : public QListView {
    Q_OBJECT

public:
    explicit MillerListView(bool isParentPane = false, QWidget* parent = nullptr);
    void setDropTargetRow(int row);

    void updateAnchor(int row, const std::filesystem::path& path);
    int resolveAnchorRow() const;
    void clearAnchor();

signals:
    void enterDirectoryRequested(const std::filesystem::path& path);
    void openFileRequested(const std::filesystem::path& path);
    void navigateToParentRequested();
    void navigateHistoryBackRequested();
    void navigateHistoryForwardRequested();
    // средняя кнопка мыши (нажатие на колесо) по папке в списке файлов
    void openInNewTabRequested(const std::filesystem::path& path);
    void typeAheadSearchTriggered(const QString& text);
    void filterTriggered();
    void toggleSelectionRequested(int row);
    void trashRequested();
    void deletePermanentlyRequested();
    void selectionInteractionStarted();
    void contextMenuRequested(const QPoint& globalPos, const QModelIndex& index);
    void filesDropped(const std::vector<std::filesystem::path>& sources,
                      const std::filesystem::path& targetDir,
                      Qt::DropAction action,
                      Qt::KeyboardModifiers modifiers);

protected:
    void currentChanged(const QModelIndex& current, const QModelIndex& previous) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

    // пути для исходящего драга; страница архива переопределяет: пути виртуальные (архив + запись)
    virtual std::vector<std::filesystem::path> dragOutPaths(const QModelIndex& startIndex) const;

    // старт исходящего драга; страница архива переопределяет: сначала распаковка во временный кэш, потом QDrag
    virtual void startDragOut(const std::vector<std::filesystem::path>& paths);

private:
    enum class DragMode { None, BoxSelect, DragOut };

    bool m_isParentPane{false};
    DragMode m_dragMode{DragMode::None};
    bool m_clearOthersOnRelease{false};
    bool m_skipBaseRelease{false};
    bool m_preservingAnchor{false};
    QPoint m_dragStartPos;
    QPersistentModelIndex m_dragStartIndex;
    int m_selectionAnchorRow{-1};
    std::filesystem::path m_selectionAnchorPath;

    // выделение прямоугольной областью через rubberband
    QRubberBand* m_rubberBand{nullptr};
    bool m_isRubberBanding{false};
    QPoint m_rubberBandOrigin;
};

class MillerViewContainer : public QWidget {
    Q_OBJECT

public:
    explicit MillerViewContainer(QWidget* parent = nullptr);
    ~MillerViewContainer() override;

    void setDirectory(const std::filesystem::path& path);
    const std::filesystem::path& currentDirectory() const { return m_currentDir; }

    void setFilter(const QString& filter);
    void searchEverywhere(const QString& query);
    void searchContents(const QString& query);
    void clearGlobalSearch();
    bool hasGlobalSearch() const;
    void focusCenter();
    // Архив открыт в центральной области (страница поверх колонок); путь вкладки при этом не меняется
    bool isArchiveViewActive() const { return !m_archivePath.empty(); }
    const std::filesystem::path& archivePath() const { return m_archivePath; }
    void leaveArchive();
    std::vector<std::filesystem::path> selectedPaths() const;
    // все элементы текущей колонки (для fuzzy palette), в порядке отображения
    std::vector<std::filesystem::path> currentEntries() const;
    void refresh();

    // режим выбора файла (FileChooserDialog): двойной клик/Enter по файлу отдаёт
    // сигналом fileChosenRequested, диалог сам решает (портал или «Open In»)
    void setFileChooseMode(bool enabled) { m_fileChooseMode = enabled; }
    // мультивыбор строк центральной колонки (портал multiple)
    void setMultiSelectEnabled(bool enabled);

    // триггеры контекстного меню и действий
    void openCurrent();
    void openSelectionInNewTab();
    void openWithApp(const QString& execCmd);
    void sudoOpenCurrent(const std::filesystem::path& targetDir = {});
    void clearTrashBin();
    void setSortMode(int mode);
    void toggleSortDirection();
    void resetSortToDefault();
    void showSortMenu();
    void cutSelection();
    void copySelection();
    void pasteClipboard();
    void copyPaths(int mode);
    void renameCurrent();
    // массовое переименование: результат из встроенной панели BulkRenamePanel
    void applyBulkRenames(const std::vector<std::pair<std::filesystem::path,
                                                      std::filesystem::path>>& renames);
    // операции над явными путями (результаты поиска): не зависят от выделения колонки
    void renamePath(const std::filesystem::path& path);
    void trashPaths(const std::vector<std::filesystem::path>& paths, bool checkPluginOverride = true);
    void deletePathsPermanently(const std::vector<std::filesystem::path>& paths,
                                bool checkPluginOverride = true);
    void clipboardPaths(const std::vector<std::filesystem::path>& paths, bool cut);
    void createBulkItems();
    void trashSelection();
    void deletePermanentlySelection();
    void compressSelection(const QString& format = ".tar.gz");
    // Extract Here сразу в текущую папку; In открывает выбор каталога
    void extractHere();
    void extractSelectionInto(const std::filesystem::path& destDir);
    // распаковка конкретного архива (напр. из меню папки превью) в destDir
    void extractArchiveInto(const std::filesystem::path& archive,
                            const std::filesystem::path& destDir);
    void openTerminal();
    void createNewFolder();
    void createNewFile();
    void bookmarkSelection();
    void showProperties();
    void showPropertiesFor(const std::filesystem::path& path);
    void runPluginById(const QString& pluginId); // центр: выделение + m_currentDir
    void runPluginByIdIn(const QString& pluginId,
                         const std::vector<std::filesystem::path>& selection,
                         const std::filesystem::path& cwd,
                         const QString& query = QString(),
                         bool commandOutput = false,
                         bool allowQueryPrompt = false,
                         bool preview = false);
    void runPluginCommand(const QString& pluginId, const QString& query);
    void schedulePluginPreview(const QString& pluginId, const QString& query);
    void cancelPluginPreview(bool closePage, bool focusPage = true);
    bool isPluginPreviewActive() const;
    void beginPluginPanes(const QString& title, bool preview = false);
    void appendPluginPanesOutput(const QByteArray& data);
    void appendPluginPaneOutput(int pane, const QByteArray& data);
    void finishPluginPanes(int exitCode, bool preview = false);
    void closePluginPanes(bool focusPage = true);
    bool isPluginPanesVisible() const;
    // действие плагина плюсов через хост нативных плагинов где берется выделение центра и текущая папка и сначала проверяется цепочка переопределений
    void runNativePluginAction(const QString& actionId);
    void runNativePluginCommand(const QString& actionId, const QString& query);
    // перехват действия плагином (Override = <id> в plugins/*.conf): вместо встроенной операции
    // запускает команду плагина, true = поглощено; без контекста выделение центра + текущий каталог;
    // false, если override для действия не задан (вызывает встроенное)
    bool tryPluginOverride(const QString& actionId);
    // вариант с явным контекстом: мышь/меню вкладок, пункты меню закладок
    bool tryPluginOverrideWith(const QString& actionId,
                               const std::vector<std::filesystem::path>& selection,
                               const std::filesystem::path& cwd);

    // сортировка для каждой папки отдельно (~/.config/atlifm/sorting.ini,
    // или <каталог>/sorting.ini при ATLIFM_CONFIG_DIR): папка → «режим|порядок»;
    // каждая колонка сортируется по своей папке (центр текущая, левая родителя)
    void applySortForDirectory(const std::filesystem::path& dir);
    void saveSortForCurrentDir();
    // текущее состояние центральной колонки (индикаторы/тесты)
    int centerSortMode() const { return m_centerModel ? m_centerModel->sortMode() : 0; }
    bool centerSortAscending() const { return m_centerModel ? m_centerModel->isSortAscending() : true; }
    // состояние левой (родительской) колонки: своя папка, свой режим (тесты)
    int parentSortMode() const { return m_parentModel ? m_parentModel->sortMode() : 0; }
    bool parentSortAscending() const { return m_parentModel ? m_parentModel->isSortAscending() : true; }
    int centerRowCount() const { return m_centerModel ? m_centerModel->rowCount() : 0; }
    void toggleHidden();
    void selectAll();
    void clearSelection();
    // раскладка из theme.conf: крайние колонки можно отключить, ширину задать
    void setColumnsConfig(bool showParent, bool showPreview,
                          int parentWidth, int previewWidth,
                          bool exactWidths = false);
    // пересчитать размеры после show/hide/сброса раскладки, когда QSplitter
    // уже получил финальную геометрию (иначе колонки накладываются друг на друга)
    void relayoutColumns();
    // текущее состояние для сохранения раскладки (реальные ширины QSplitter)
    bool parentColumnEnabled() const { return m_parentColumnEnabled; }
    bool previewColumnEnabled() const { return m_previewColumnEnabled; }
    int parentColumnWidth() const;
    int previewColumnWidth() const;
    // Home/End: первый/последний элемент центральной колонки
    void selectFirst();
    void selectLast();
    void selectPathAfterRefresh(const std::filesystem::path& p);
    bool selectAndFocusPath(const std::filesystem::path& p);
    std::filesystem::path computeNeighbourPath(const std::vector<std::filesystem::path>& excluding) const;
    void restoreMultiSelection(const std::unordered_set<std::string>& paths, const std::filesystem::path& focusItem);
    bool isCurrentSudo() const { return m_isCurrentSudo; }
    void setCurrentSudo(bool sudo);
    // каталог, открытый через "Sudo Open": индикатор sudo горит только внутри него
    const std::filesystem::path& sudoRoot() const { return m_sudoRoot; }
    void setSudoRoot(const std::filesystem::path& root) { m_sudoRoot = root; }

signals:
    void directoryChanged(const std::filesystem::path& path);
    void itemSelected(const std::filesystem::path& path);
    // perms права текущего элемента центральной колонки (в стиле yazi: в статусбаре, не в строках списка)
    void statusUpdated(int currentIndex, int totalItems, int selectedItems, const QString& perms);
    void messageEmitted(const QString& msg);
    void bookmarkAdded(const std::filesystem::path& path);
    void navigateHistoryBackRequested();
    void navigateHistoryForwardRequested();
    void openInNewTabRequested(const std::filesystem::path& path);
    void typeAheadSearchTriggered(const QString& text);
    void filterTriggered();
    // несколько выделенных элементов: MainWindow открывает встроенную панель
    // переименования (не отдельное окно)
    void bulkRenameRequested(const std::vector<std::filesystem::path>& paths);
    void sudoModeChanged(bool isSudo);
    // свойства (компактные): единый оверлей в MainWindow для ПКМ, Alt+Enter
    // и панели закладок; для корня диска/ФС в том же оверлее открывается Disk Usage
    void propertiesRequested(const std::filesystem::path& path);
    // Open With… полноразмерный диалог TUI в MainWindow (оверлей)
    void openWithDialogRequested(const std::filesystem::path& path);
    // Extract In… MainWindow открывает выбор каталога (как при выборе файла)
    void extractInRequested();
    // Extract In… для архива из папки превью (тот же оверлей, но распаковывается этот архив, не выделение колонок)
    void extractInPathRequested(const std::filesystem::path& archivePath);
    // режим выбора файла: двойной клик/Enter по файлу (см. setFileChooseMode)
    void fileChosenRequested(const std::filesystem::path& path);
    // архив открыт в центре / закрыт возвратом в каталог
    void archiveEntered(const std::filesystem::path& archivePath);
    void archiveLeft(const std::filesystem::path& directory);

private slots:
    void onCenterSelectionChanged(const QItemSelection& selected, const QItemSelection& deselected);
    void onCenterDoubleClicked(const QModelIndex& index);
    void onFilesDropped(const std::vector<std::filesystem::path>& sources,
                        const std::filesystem::path& targetDir,
                        Qt::DropAction action,
                        Qt::KeyboardModifiers modifiers);
    void updateCenterSelection();
    void onArchiveEntrySelected(const QString& normalizedPath, const QString& memberPath,
                                bool isDir);
    void applyResponsiveSizes();
    void updateCenterDeniedOverlay(int count);
    void positionCenterDeniedOverlay();

protected:
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void setupUi();
    void applyPluginPaneSizes();
    void activateArchiveSelection();
    void activateFileSelection();
    // открыть файл: архивы уходят в центральную страницу, остальное в приложение по умолчанию
    void openFile(const std::filesystem::path& path);
    void enterArchive(const std::filesystem::path& archivePath);
    // применяет сохранённую сортировку каталога к модели колонки; нет записи модель сохраняет текущий режим (sticky)
    void applySortToModel(const std::filesystem::path& dir, Models::FileSystemModel* model);
    // синхронизирует подсветку «открытой папки» в родительской колонке: приглушённая подсветка через делегат, current index без выделения
    void syncParentViewToCurrentDir();

    QStackedWidget* m_viewStack{nullptr};
    QSplitter* m_splitter{nullptr};
    MillerListView* m_parentView{nullptr};
    MillerListView* m_centerView{nullptr};
    PreviewPane* m_previewPane{nullptr};

    QPushButton* m_sortButton{nullptr};

    // пустое состояние центра при permission denied: подсказка поверх списка; «try to sudo open» кликабельна
    QWidget* m_centerDeniedOverlay{nullptr};
    QLabel* m_centerDeniedHint{nullptr};
    QLabel* m_centerDeniedAction{nullptr};
    bool m_centerDeniedActionClickable{true};
    // каталог, где sudo отказал листингом даже ему: оверлей «sudo can't open this folder» и не зовёт повторно
    std::filesystem::path m_sudoDeniedDir;

    Models::FileSystemModel* m_parentModel{nullptr};
    Models::FileSystemModel* m_centerModel{nullptr};

    FileItemDelegate* m_parentDelegate{nullptr};
    FileItemDelegate* m_centerDelegate{nullptr};

    std::filesystem::path m_currentDir;
    std::filesystem::path m_pathToSelectAfterRefresh;
    bool m_previewAutoHidden{false};
    bool m_parentAutoHidden{false};
    bool m_initialSizesApplied{false};
    bool m_exactInitialWidths{false};
    bool m_parentColumnEnabled{true};
    bool m_previewColumnEnabled{true};
    int m_parentColumnWidth{300};
    int m_previewColumnWidth{420};
    bool m_fileChooseMode{false};
    bool m_isCurrentSudo{false};
    std::filesystem::path m_sudoRoot;
    int m_lastKnownRow{0};
    QString m_savedCurrentPath;
    int m_pendingSelectRetries{0};
    QPointer<TuiContextMenu> m_activeContextMenu;
    bool m_archiveSelectionActive{false};

    // центральная страница архива внутри центральной колонки; m_archivePath пуст вне неё
    QStackedWidget* m_centerStack{nullptr};
    QWidget* m_archivePage{nullptr};
    ArchiveTreeWidget* m_archiveTree{nullptr};
    Models::ArchiveModel* m_archiveModel{nullptr};
    QPushButton* m_archiveBackBtn{nullptr};
    std::filesystem::path m_archivePath;
    QTimer* m_archivePreviewTimer{nullptr};
    QString m_archivePreviewPendingPath;
    QString m_archivePreviewPendingMember;
    quint64 m_archivePreviewRequest{0};
    bool m_archivePreviewBusy{false};
    bool m_archivePreviewQueued{false};

    TuiPanesOverlay* m_pluginPanes{nullptr};
    QString m_pluginPanesTitle;
    quint64 m_pluginRunGeneration{0};
    QTimer* m_pluginPreviewTimer{nullptr};
    QPointer<QObject> m_pluginPreviewProcess;
    QString m_pluginPreviewPluginId;
    QString m_pluginPreviewQuery;
    bool m_pluginPreview{false};
};

}
